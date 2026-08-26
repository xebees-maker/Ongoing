#include "esp_now_hub.h"
#include "esp_now_photo.h"
#include "esp_now_reliable.h"
#include "esp_now_tx.h"
#include "rtc_sync.h"
#include "ui_log.h"
#include "device_config.h"
#include "battery.h"

#include <string.h>
#include <assert.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "esp_now_hub";

/* Cntl 실제 sdkconfig 기준 STA 모드+SSID/PW — 이 PC(원래 PC)의 네트워크로 복원
 * (다른 PC 세션에서 그쪽 네트워크 hkhome으로 바뀌어 커밋됨 — 2026-08-08 원복) */
#define WIFI_SSID     "iptime2.4"
#define WIFI_PASSWORD "sk1234!@#"

/* 2026-08-04 — 채널 격리 실험: Cntl을 hkhome STA에서 떼어내 순수 SoftAP 전용으로 돌려서,
 * ESP-NOW(CAM/Sens) 트래픽이 더 이상 hkhome의 실제 WiFi 트래픽과 같은 채널을 나눠 쓰지
 * 않게 함 — 오늘 겪은 청크 대량유실/DONE 유실/keepalive 연속실패가 "채널 경합" 때문인지
 * 아니면 우리 프로토콜/구현 자체의 문제인지 구분하기 위한 진단용 스위치.
 * CAM 쪽 코드는 전혀 안 건드림 — CAM은 원래도 실제 WiFi join 없이 ESP-NOW 자체 광고/
 * 채널스캔으로 Cntl을 찾으므로, Cntl이 AP든 STA든 상관없이 그대로 찾아서 페어링됨.
 * 원복하려면 이 줄만 0으로 바꾸면 됨(그 아래 CNTL_AP_SSID/PASSWORD/CHANNEL은 그때 무시됨). */
#define CNTL_WIFI_STANDALONE_AP_TEST 0
#define CNTL_AP_SSID       "Cntl-Test-AP"
#define CNTL_AP_PASSWORD   "cntltest123"
#define CNTL_AP_CHANNEL    1
#define CNTL_AP_MAX_CONN   4
/* AP 모드일 땐 STA 인터페이스가 아예 안 떠있어서, ESP-NOW 피어 등록/자기 MAC 조회를
 * WIFI_IF_STA로 하면 ESP_ERR_ESPNOW_IF로 전부 실패함(실기에서 확인 — HUB_RESET 브로드캐스트
 * 부터 막힘). 아래 CNTL_WIFI_IF로 통일해서 모드 전환 시 자동으로 맞게 함 */
#if CNTL_WIFI_STANDALONE_AP_TEST
#define CNTL_WIFI_IF WIFI_IF_AP
#else
#define CNTL_WIFI_IF WIFI_IF_STA
#endif

static const uint8_t s_broadcast_addr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static esp_now_hub_node_t s_nodes[ESP_NOW_HUB_MAX_NODES];
static int                s_node_count = 0;

/* recv_cb()는 ESP-NOW/WiFi 드라이버 태스크에서 호출되고, esp_now_hub_get_nodes()/pair()/
 * unpair()는 LVGL 워커 태스크(esp_lv_adapter)에서 호출됨 — 서로 다른 태스크가 락 없이
 * s_nodes[]/s_node_count를 동시에 건드리던 레이스가 있었음(연결 리스트가 간헐적으로 깨지고
 * 반응 없던 문제의 원인으로 추정). 아래 뮤텍스로 s_nodes[]/s_node_count 접근 전체를 보호. */
static SemaphoreHandle_t s_nodes_mutex = NULL;

/* 적응형 반응시간(2026-08-10) — 마지막 사용자 조작 시각(esp_now_photo.c의 5개 액션 함수가
 * esp_now_hub_note_user_action()으로 갱신). 전역 하나로 충분 — 지금은 CAM이 보통 1대라
 * esp_now_hub_bench_start()/apply_response_interval_sec()이 이미 쓰는 단순화와 동일 원칙.
 * 2026-08-25(CASK 재설계) — 예전엔 이 값을 별도 원샷 타이머(adaptive_deadline_timer)가
 * 소비해서 "타이머가 다 되면 모든 노드에 SLEEP_NOW를 먼저 보내러 가는" 능동적(push) 구조
 * 였음. 이제 그 판단은 CASK를 만드는 바로 그 순간(WAKE_HELLO 수신, send_cask_sleep_now()
 * 참고)에 이 타임스탬프를 확인하는 순수 반응형(pull)으로 바뀜 — 별도 타이머 자체가
 * 없어짐 */
static uint32_t s_last_user_action_ms = 0;

void esp_now_hub_note_user_action(void)
{
    s_last_user_action_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/* 2026-08-25(CASK 재설계), 2026-08-26 조건 추가(사용자 지시) — WAKE_HELLO 수신 시 CASK의
 * 마지막 단계로 호출(recv_cb 참고). sleep_sec을 실제 응답성 값으로(=진짜 딥슬립) 보내는
 * 조건은 셋 다 동시에 참일 때뿐: (1) 최근 사용자 조작이 없고(적응형 반응시간 밖) (2) 응답성
 * 자체가 0(Live 모드)이 아니고 (3) 이 노드와 진행 중인 통신(트랜잭션)이 없음. 셋 중 하나라도
 * 아니면 sleep_sec=0 — "통신 중엔 안 재운다"는 원칙(사진 요청 등 CASK "할일"의 접수 ack와
 * 진짜 완료가 다른 경우까지 포함, esp_now_photo_is_transacting_with() 참고). (2)는 별도
 * 분기 없이 else 값 자체가 0이라 자연히 흡수됨 */
static void send_cask_sleep_now(esp_now_hub_node_t *n)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t quiet_ms = now_ms - s_last_user_action_ms;
    uint32_t threshold_ms = device_config_get_adaptive_response_sec() * 1000U;
    bool transacting = esp_now_photo_is_transacting_with(n->mac);
    uint32_t sleep_sec = (quiet_ms < threshold_ms || transacting)
        ? 0 : device_config_get_response_interval_sec();

    ESP_LOGI(TAG, "SLEEP_NOW -> %s: sleep_sec=%u(조용%lums/임계값%lums/통신중=%d)",
             n->name, (unsigned)sleep_sec, (unsigned long)quiet_ms, (unsigned long)threshold_ms, (int)transacting);

    esp_now_sleep_now_t msg = {
        .version   = ESP_NOW_LINK_VERSION,
        .msg_type  = ESP_NOW_MSG_SLEEP_NOW,
        .sleep_sec = sleep_sec,
    };
    static const uint8_t s_sleep_now_ack_types[] = { ESP_NOW_MSG_SLEEP_NOW_ACK };
    esp_now_tx_enqueue(n->mac, &msg, sizeof(msg), s_sleep_now_ack_types, 1, 300, 3, "SLEEP_NOW");
}

/* 2026-08-25(CASK 재설계) — CNTL의 능동적 생존판단. 예전엔 last_seen_ms 타임아웃이
 * esp_now_hub_get_nodes()(화면 목록 필터)에서만 수동적으로 쓰여서, 실제 conn_state는
 * 안 바뀐 채 화면에서만 사라지는 어긋남이 있었음 — "CAM이 CNTL 살아있나 확인하던 걸
 * (핑퐁), CNTL이 CAM 살아있나 확인하는 걸로 뒤집는다"는 설계의 CNTL쪽 절반. 노드 종류
 * 구분 없이 전부 훑음(사용자 지시: "모든 노드는 같은 구조로 CNTL에 붙을 거라서 CNTL은
 * 하나의 타이머와 디스패처로 모두 연동") — 1초 주기, 노드 수와 무관하게 타이머 하나 */
static esp_timer_handle_t s_liveness_sweep_timer = NULL;
#define LIVENESS_SWEEP_INTERVAL_US (1 * 1000 * 1000)

static void liveness_sweep_cb(void *arg)
{
    (void)arg;
    uint32_t now_ms     = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t timeout_ms = esp_now_hub_node_timeout_ms();
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < s_node_count; i++) {
        esp_now_hub_node_t *n = &s_nodes[i];
        if (n->conn_state == NODE_CONN_PAIRED && now_ms - n->last_seen_ms > timeout_ms) {
            ESP_LOGW(TAG, "%s 무응답(%us 이상) — PAIRED에서 강등", n->name, (unsigned)(timeout_ms / 1000));
            n->conn_state = NODE_CONN_ORPHAN;
        }
    }
    xSemaphoreGive(s_nodes_mutex);
}

static hub_node_kind_t classify_name(const char *name)
{
    /* 2026-08-22 — CAM 기본 이름 "Cam-XXXXXX" -> "CXXXXXX"로 축약(전력로그 폭 문제,
     * esp_now_cam.c:resolve_name 참고) — Sens는 "Sens-"라 'C'로 시작 안 해서 안전 */
    if (strncmp(name, "C", 1) == 0)      return HUB_NODE_KIND_CAM;
    if (strncmp(name, "Sens-", 5) == 0) return HUB_NODE_KIND_SENS;
    return HUB_NODE_KIND_UNKNOWN;
}

static esp_now_hub_node_t *find_or_add_node(const uint8_t *mac)
{
    for (int i = 0; i < s_node_count; i++) {
        if (memcmp(s_nodes[i].mac, mac, 6) == 0) return &s_nodes[i];
    }
    if (s_node_count < ESP_NOW_HUB_MAX_NODES) {
        esp_now_hub_node_t *n = &s_nodes[s_node_count++];
        memcpy(n->mac, mac, 6);
        return n;
    }
    return NULL;  /* 테이블 가득 — 새 노드 무시 */
}

static esp_now_hub_node_t *find_node(const uint8_t *mac)
{
    for (int i = 0; i < s_node_count; i++) {
        if (memcmp(s_nodes[i].mac, mac, 6) == 0) return &s_nodes[i];
    }
    return NULL;
}

/* 2026-08-26(사용자 지시) — 노드별 사용자 액션 대기 큐. esp_now_photo.c의 5개 액션 함수가
 * 여기 넣기만 하고, 실제로 언제 내보낼지는 WAKE_HELLO 핸들러(아래)가 CASK의 "할일" 단계로
 * 중앙집중 판단함 */
void esp_now_hub_queue_action(const uint8_t *mac, const void *req, size_t req_len,
                               const uint8_t *ack_types, size_t ack_types_count,
                               uint32_t timeout_ms, int max_attempts, const char *what)
{
    if (req_len > ESP_NOW_HUB_PENDING_ACTION_MAX_LEN) {
        ESP_LOGE(TAG, "액션 큐잉 실패(%s) — 페이로드 %u > %u", what, (unsigned)req_len,
                 (unsigned)ESP_NOW_HUB_PENDING_ACTION_MAX_LEN);
        return;
    }
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    esp_now_hub_node_t *n = find_or_add_node(mac);
    if (!n) {
        xSemaphoreGive(s_nodes_mutex);
        ESP_LOGW(TAG, "액션 큐잉 실패(%s) — 노드 테이블 가득", what);
        return;
    }
    /* 링버퍼 가득 — 가장 오래된(head) 항목을 밀어내고 새로 넣음(무한 적체 방지, 사용자
     * 조작이 큐 용량보다 훨씬 빠르게 쌓이는 건 비정상 상황이라 오래된 것부터 버리는 게 맞음) */
    int idx;
    if (n->action_queue_count < ESP_NOW_HUB_PENDING_ACTION_QUEUE_DEPTH) {
        idx = (n->action_queue_head + n->action_queue_count) % ESP_NOW_HUB_PENDING_ACTION_QUEUE_DEPTH;
        n->action_queue_count++;
    } else {
        idx = n->action_queue_head;
        n->action_queue_head = (n->action_queue_head + 1) % ESP_NOW_HUB_PENDING_ACTION_QUEUE_DEPTH;
        ESP_LOGW(TAG, "%s 액션 큐 가득 — 가장 오래된 항목 버림", n->name);
    }
    esp_now_hub_pending_action_t *slot = &n->action_queue[idx];
    memcpy(slot->req, req, req_len);
    slot->req_len         = req_len;
    slot->ack_types       = ack_types;
    slot->ack_types_count = ack_types_count;
    slot->timeout_ms      = timeout_ms;
    slot->max_attempts    = max_attempts;
    slot->what            = what;
    xSemaphoreGive(s_nodes_mutex);
    ESP_LOGI(TAG, "액션 큐잉됨(%s) — %s 대기열 %d개", what, n->name, n->action_queue_count);
}

/* CASK "할일" 단계에서 호출(s_nodes_mutex를 이미 쥔 상태로 불림) — 있으면 하나 꺼내고 true,
 * 없으면 false. 실제 전송은 호출부(WAKE_HELLO 핸들러)가 esp_now_tx_enqueue()로 함 */
static bool dequeue_pending_action_locked(esp_now_hub_node_t *n, esp_now_hub_pending_action_t *out)
{
    if (n->action_queue_count == 0) return false;
    *out = n->action_queue[n->action_queue_head];
    n->action_queue_head = (n->action_queue_head + 1) % ESP_NOW_HUB_PENDING_ACTION_QUEUE_DEPTH;
    n->action_queue_count--;
    return true;
}

static void add_peer_if_needed(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx   = CNTL_WIFI_IF;
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        ESP_LOGW(TAG, "피어 등록 실패");
        return;
    }
    /* 2026-08-08 — CAM 쪽과 짝맞춤(esp_now_cam.c 동일 주석 참고). 이 함수는 CAM/Sens 등
     * 실제 노드 MAC에만 불림(브로드캐스트 주소는 여기 안 옴) — 안전. */
    esp_now_rate_config_t rate_cfg = { .phymode = WIFI_PHY_MODE_HT20, .rate = WIFI_PHY_RATE_MCS0_LGI, .ersu = false, .dcm = false };
    esp_err_t rate_err = esp_now_set_peer_rate_config(mac, &rate_cfg);
    ESP_LOGI(TAG, "피어 레이트 설정(MCS0/HT20) -> %s", esp_err_to_name(rate_err));
}

static volatile hub_config_apply_stage_t s_config_apply_stage = HUB_CONFIG_APPLY_IDLE;

hub_config_apply_stage_t esp_now_hub_get_config_apply_stage(void) { return s_config_apply_stage; }
void esp_now_hub_config_apply_stage_clear(void) { s_config_apply_stage = HUB_CONFIG_APPLY_IDLE; }

/* 2026-08-08 — device_config(Cntl이 소유하는 CAM 설정)의 "현재 값"을 mac 하나에 그대로
 * 밀어줌. 페어링 시 자동 전송(recv_cb의 PAIR_ACK 핸들러)과, 설정탭 Apply 버튼
 * (esp_now_hub_apply_*) 둘 다 이 함수 하나로 통일 — "지금 저장된 값을 보낸다"는 의미가
 * 완전히 같으므로 재사용. s_config_apply_stage는 Apply 버튼 진행팝업용(페어링 자동전송
 * 때도 갱신되긴 하지만 그때는 아무도 안 봄 — 무해) */
static void push_cam_config_to(const uint8_t *mac)
{
    esp_now_cam_config_t cfg = {
        .version                = ESP_NOW_LINK_VERSION,
        .msg_type               = ESP_NOW_MSG_CAM_CONFIG_SET,
        .wb_mode                = CAM_WB_AUTO,
        .capture_interval_sec   = device_config_get_cam_capture_interval_sec(),
        .response_interval_sec  = device_config_get_response_interval_sec(),
        .agc_enable             = device_config_get_agc_enable() ? 1 : 0,
        .aec_enable             = device_config_get_aec_enable() ? 1 : 0,
        .xclk_mhz               = device_config_get_xclk_mhz(),
        .nack_max_rounds        = device_config_get_nack_max_rounds(),
        /* 2026-08-25(CASK 재설계) — 예전 별도 SET_TIME 메시지를 대체. CONFIG가 이미 매
         * 사이클 무조건 나가니 몇 바이트 더 싣는 게 별도 왕복 하나를 통째로 없애는 것보다
         * 쌈(esp_now_link.h의 esp_now_cam_config_t.unix_time 주석 참고) */
        .unix_time              = rtc_sync_get_unix_time(),
    };
    static const uint8_t s_config_ack_types[] = { ESP_NOW_MSG_CAM_CONFIG_ACK };
    s_config_apply_stage = HUB_CONFIG_APPLY_SENT;
    esp_now_tx_enqueue(mac, &cfg, sizeof(cfg), s_config_ack_types, 1, 800, 3, "CAM 설정");
    ESP_LOGI(TAG, "CAM_CONFIG_SET -> 촬영주기=%us 응답성=%us AGC=%d AEC=%d XCLK=%uMHz NACK라운드=%u 큐잉됨",
             (unsigned)cfg.capture_interval_sec, (unsigned)cfg.response_interval_sec,
             (int)cfg.agc_enable, (int)cfg.aec_enable, (unsigned)cfg.xclk_mhz,
             (unsigned)cfg.nack_max_rounds);
}

/* recv_cb(ADVERTISE 핸들러)가 먼저 쓰고 실제 정의는 파일 뒤쪽(esp_now_hub_request_pair
 * 근처)에 있음 — 전방 선언 */
static void esp_now_hub_pair(const uint8_t *mac);

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 2) return;
    uint8_t msg_type = data[1];
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* Reliable 모드(Layer 1) 요청/응답 매칭 — 지금 esp_now_tx 태스크가 기다리는 요청이 있고
     * 이 메시지가 그 응답이면 대기 태스크를 깨움. 그 외엔 조용히 무시하고 리턴하므로 아래
     * 기존 dispatch와 안전하게 병행됨(esp_now_channelsync_on_recv()와 동일 패턴, 2026-08-05) */
    esp_now_reliable_on_recv(msg_type, info ? info->src_addr : NULL, data, len);

    if (msg_type == ESP_NOW_MSG_ADVERTISE) {
        if (len < (int)sizeof(esp_now_advertise_t)) return;
        const esp_now_advertise_t *msg = (const esp_now_advertise_t *)data;
        if (msg->version != ESP_NOW_LINK_VERSION) return;

        xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
        esp_now_hub_node_t *n = find_or_add_node(info->src_addr);
        if (!n) {
            xSemaphoreGive(s_nodes_mutex);
            ESP_LOGW(TAG, "노드 테이블 가득 — %s 무시", msg->name);
            return;
        }
        /* desync 감지/복구(2026-08-04, 실기에서 발견) — ADVERTISE는 노드가 페어링 전(광고/
         * 채널스캔) 상태일 때만 보내는 메시지. 그런데 Cntl은 한번 paired=true가 되면 그
         * 이후로 ADVERTISE를 다시 받아도 이 필드를 절대 안 건드렸음 — 노드가 HUB_RESET
         * 수신이나 SEND_FAIL_THRESHOLD 등으로 스스로 재광고 모드에 들어가도 Cntl 화면은
         * 계속 "연결됨"으로 남아있는 상태(desync)가 됨. 30분 처리량 벤치마크 도중 CAM이
         * 실제로는 전혀 안 붙어있는데 화면은 연결됨으로 표시된 채였던 게 이 버그였음
         * (사용자가 화면과 실제 통신 상태가 다르다고 지적, "페어링 확인 공통모듈"을
         * 만들라고 지시). ADVERTISE 수신 자체가 "이 노드는 지금 언페어링 상태"라는
         * 확실한 증거이므로, 여기서 바로 paired를 내림 */
        /* 2026-08-24(사용자 지시로 시간기반 폐기) — 예전엔 "낡은 광고" 레이스를 시간창으로
         * 걸렀는데, 그 근본원인(캠의 scan_timer가 페어링 완료 후에도 미처 못 멈추고 광고를
         * 내보내는 것)을 오늘 캠 쪽에서 직접 고쳤음(광고 송신 함수가 실제 전송 직전에 상태를
         * 매번 새로 확인 — esp_now_channelsync.c의 send_advertise_on_current_channel() 참고).
         * 그래서 이제 여기서 낡은 광고를 걸러낼 필요 자체가 없어짐 — 매 ADVERTISE를 그대로
         * 신뢰하고 처리함 */
        bool was_paired = (n->conn_state == NODE_CONN_PAIRED);
        bool was_user_unpaired = n->user_unpaired;
        bool ever_paired = n->ever_paired;
        /* 2026-08-24 — 사용자가 "연결"을 눌러 대기 중이면, 지금 이 광고를 실제로 받은 이
         * 순간(채널이 같다는 게 방금 증명됨)에 소비 — esp_now_hub_request_pair() 참고.
         * 시간 대신 "광고 수신으로 트리거된 시도 횟수"로 3번까지만 허용 */
        bool user_pair_wanted = n->user_pair_wanted;
        if (user_pair_wanted) {
            if (n->pair_req_attempts_left > 0) n->pair_req_attempts_left--;
            if (n->pair_req_attempts_left == 0) n->user_pair_wanted = false;  /* 이번이 마지막 시도 */
        }
        memcpy(n->name, msg->name, sizeof(n->name));
        n->name[ESP_NOW_LINK_NAME_LEN - 1] = '\0';
        n->kind = classify_name(n->name);
        n->last_seen_ms = now_ms;
        n->conn_state = NODE_CONN_ORPHAN;
        char name_copy[ESP_NOW_LINK_NAME_LEN];
        strncpy(name_copy, n->name, sizeof(name_copy) - 1);
        name_copy[sizeof(name_copy) - 1] = '\0';
        uint8_t mac_copy[6];
        memcpy(mac_copy, info->src_addr, sizeof(mac_copy));
        xSemaphoreGive(s_nodes_mutex);
        if (was_paired) {
            ESP_LOGW(TAG, "%s가 재광고 시작 — 페어링 끊김으로 판단(desync 복구)", name_copy);
        }
        /* 자동 재페어링(2026-08-10, CAM Deep Sleep 전환) — CAM은 매 딥슬립 웨이크마다 완전
         * 재부팅(상태 없음)되므로 ADVERTISE 수신이 곧 "새 사이클 시작"임. was_paired(방금
         * 막 페어링이 풀린 순간)만 보고 재연결을 시도하면, 그 시도 자체가 무선 유실 등으로
         * 실패했을 때(실기에서 확인됨 — "5회 시도 모두 무응답") 이후 어떤 ADVERTISE가 와도
         * 다시 시도할 계기가 없어 영구히 "연결 대기 중"에 멈춰버림. ever_paired(이번 Cntl
         * 부팅 세션에서 한 번이라도 페어링 성공했는가, esp_now_hub_node_t 참고)를 대신
         * 써서 — 성공/실패와 무관하게 이 노드가 다시 광고할 때마다(즉 매 웨이크 사이클마다)
         * 재연결을 계속 재시도함. 사용자가 명시적으로 연결 해제한 노드(user_unpaired)는
         * 그대로 안 건드림 — 다음에 사용자가 직접 다시 붙여야 함(esp_now_hub_unpair의
         * 원래 의도 유지) */
        /* 2026-08-24(사용자 지시) — was_paired(Cntl 자신이 방금까지 PAIRED로 알고 있었음)는
         * user_unpaired 이력과 무관하게 무조건 재페어링을 트리거함. Cntl이 스스로 PAIRED로
         * 인지 중이었다는 사실 자체가 "지금 이 노드와 연결 상태를 유지하고 싶다"는 가장 강한
         * 증거인데, 과거 언젠가의 user_unpaired 이력(예: 다른 세션/재시작 이전)이 이걸 막아
         * 캠은 계속 미페어링으로 광고만 반복하고 Cntl은 재요청을 안 보내는 교착이 실기로
         * 발생했음(사용자 지적) */
        if (was_paired || (ever_paired && !was_user_unpaired) || user_pair_wanted) {
            esp_now_hub_pair(mac_copy);
        }

        /* 2026-08-24(사용자 지시) — user_pair_wanted로 인해 이번에 PAIR_REQUEST를 보냈으면
         * ADVERTISE_ACK는 안 보냄(같은 광고에 두 메시지를 겹쳐 보낼 이유가 없음 — PAIR_REQUEST가
         * 채널스캔 확인 목적까지 포함함). 그 외의 경우(사람이 아직 연결 안 누른, 순수 발견
         * 단계)에만 채널 확인용 ACK를 보냄 */
        if (!user_pair_wanted) {
            /* 사람이 페어링을 누르기 전이라도 즉시 응답 — 채널 스캔 중인 노드가 Cntl을
             * 찾았다는 걸 알고 이 채널에 고정하기 위한 용도(정식 페어링과 별개) */
            add_peer_if_needed(info->src_addr);
            esp_now_advertise_ack_t ack = {
                .version      = ESP_NOW_LINK_VERSION,
                .msg_type     = ESP_NOW_MSG_ADVERTISE_ACK,
            };
            esp_wifi_get_mac(CNTL_WIFI_IF, ack.hub_mac);
            esp_now_send(info->src_addr, (const uint8_t *)&ack, sizeof(ack));
        }

    } else if (msg_type == ESP_NOW_MSG_PAIR_ACK) {
        if (len < (int)sizeof(esp_now_pair_ack_t)) return;

        xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
        esp_now_hub_node_t *n = find_node(info->src_addr);
        bool became_paired = false;
        bool first_ever_pairing = false;  /* 2026-08-10 — 이번 부팅 세션에서 이 노드와 "정말
                                              처음" 붙는 순간만 true. 매 재페어링(단순 생존확인
                                              사이클)과 구분하기 위함 — 아래 became_paired 블록
                                              참고 */
        char name_copy[ESP_NOW_LINK_NAME_LEN] = { 0 };
        hub_node_kind_t kind_copy = HUB_NODE_KIND_UNKNOWN;
        if (n) {
            kind_copy = n->kind;
            /* 생존 신호(last_seen_ms)는 항상 갱신 — 페어링 후엔 CAM/SENS가 ADVERTISE를
             * 끊고 이 PAIR_ACK(keepalive)로만 살아있음을 알리는 것으로 보이는데, 이걸
             * user_unpaired로 걸러버리면(예전 버그) 연결 해제 직후부터 생존 신호 자체가
             * 안 갱신돼서 5초 타임아웃 뒤 리스트에서 완전히 사라져버림 */
            n->last_seen_ms = now_ms;
            /* 재연결(paired=true로 되돌리는 것)만 user_unpaired로 막음 — 사용자가
             * esp_now_hub_pair()를 다시 불러야(리스트에서 다시 연결 허용) 풀림.
             * became_paired는 "방금 막 페어링됨(false->true 전환)"일 때만 true여야 함 —
             * 예전엔 n->paired가 이미 true였어도 PAIR_ACK(1초 keepalive)가 올 때마다 매번
             * true로 잡혀서, 페어링된 CAM/Sens에 SET_TIME을 1초마다 무한정 계속 보내고
             * 있었음(2026-08-03, CAM 시리얼 로그로 발견 — 8초 사이에 SET_TIME이 9번 옴) */
            bool was_paired = (n->conn_state == NODE_CONN_PAIRED);
            bool was_ever_paired = n->ever_paired;
            n->pair_req_attempts_left = 0;  /* 성사됐으니 남은 시도 카운트 해제 */
            n->user_pair_wanted = false;
            if (!n->user_unpaired) {
                n->conn_state = NODE_CONN_PAIRED;
                n->ever_paired = true;  /* 2026-08-10 — 한 번 세팅되면 이번 부팅 세션 내내
                                            유지, ADVERTISE 핸들러의 자동 재페어링 판단에 씀 */
                n->last_paired_ms = now_ms;  /* esp_now_hub_is_reconnect_stuck()의 기준시각 */
                became_paired = !was_paired;
                first_ever_pairing = became_paired && !was_ever_paired;
                strncpy(name_copy, n->name, sizeof(name_copy) - 1);
            }
        }
        xSemaphoreGive(s_nodes_mutex);
        if (became_paired) {
            ESP_LOGI(TAG, "페어링 완료: %s", name_copy);
            /* 2026-08-10 — "최초 페어링"과 "단순 생존확인 재페어링"을 구분(사용자 지적으로
             * 재설계). 처음엔 모든 became_paired에서 이 리셋을 했는데, 그러면 페어링(=CAM이
             * "할 일 있어요?" 확인하러 온 것뿐, 진짜 사용자 조작 아님) 자체가 매 사이클
             * "방금 조작 있었음"으로 잡혀서, 할 일이 전혀 없어도 매번 적응형 시간을 전부
             * 소모하고서야 재웠음(불필요하게 사이클당 최대 적응형시간만큼 더 깨어있었음,
             * 실사용 중 -mm:ss 타임스탬프 분석으로 발견). first_ever_pairing(이번 세션 이
             * 노드와 정말 처음 붙는 순간)에만 리셋해서 최초 연결 직후엔 반응시간을 주고,
             * 그 이후 순수 생존확인 사이클은 리셋 안 해서 할 일 없으면 곧바로 재울 수 있게 함 */
            if (first_ever_pairing) esp_now_hub_note_user_action();
            /* 2026-08-25(CASK 재설계) — 예전엔 여기서 SET_TIME을 별도 reliable 요청으로
             * 보냈는데, 그 unix_time을 이제 push_cam_config_to()의 CAM_CONFIG_SET에 실어
             * 보냄(esp_now_link.h의 esp_now_cam_config_t.unix_time 참고) — CAM/Sens는 자체
             * RTC가 없어서 이게 없으면 시계가 부팅 시각(1970-01-01 근처)에 멈춰있음.
             * 2026-08-08 설계 — CAM/SENS는 설정을 로컬에 저장하지 않으므로, 페어링될
             * 때마다 Cntl이 기억하고 있는 현재 설정값을 매번 다시 밀어줌(SET_TIME과 같은
             * 타이밍) — 그래야 재부팅한 CAM이 Kconfig 기본값이 아니라 사용자가 마지막으로
             * Apply한 값으로 곧바로 동작함 */
            if (kind_copy == HUB_NODE_KIND_CAM) {
                push_cam_config_to(info->src_addr);
            }
        }

    } else if (msg_type == ESP_NOW_MSG_WAKE_HELLO) {
        /* 2026-08-25(CASK 재설계) — 알려진 CNTL로 곧장 체크인하는 캠의 "저 왔어요". 이걸
         * 페어드로 인지하지 않으면(모르거나, 사용자가 언페어했거나, 이번 부팅 세션에
         * 아직 한 번도 안 붙어봤으면) 조용히 무시 — 새 메시지 타입 없이, 캠 쪽 자기
         * 재시도(3회)+폴백(전체 스캔)이 알아서 처리하게 둠(사용자 설계: "CNTL이 씹으면,
         * 캠은 반응없음과 동일한 경로로 초기화") */
        if (len < (int)sizeof(esp_now_wake_hello_t)) return;
        const esp_now_wake_hello_t *hello = (const esp_now_wake_hello_t *)data;

        xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
        esp_now_hub_node_t *n = find_node(info->src_addr);
        if (!n || !n->ever_paired || n->user_unpaired) {
            xSemaphoreGive(s_nodes_mutex);
            return;
        }
        n->last_seen_ms    = now_ms;
        n->conn_state      = NODE_CONN_PAIRED;
        n->last_paired_ms  = now_ms;
        n->has_deepsleep_stats        = true;
        n->ds_cycle_count++;
        if (hello->wake_reason == CAM_WAKE_REASON_RWDT) n->ds_rwdt_catch_count++;
        n->ds_last_wake_reason         = hello->wake_reason;
        n->ds_last_awake_uptime_ms     = hello->awake_uptime_ms;
        n->ds_last_sleep_interval_sec  = hello->sleep_interval_sec;
        n->ds_last_actual_sleep_sec    = hello->actual_last_sleep_sec;
        /* 2026-08-22 — 배터리 잔량. %%는 여기서 공용 커브(battery_mv_to_pct(), Sens와 동일
         * 상수)로 계산 — CAM은 mV까지만 보냄 */
        n->battery_adc_raw = hello->battery_adc_raw;
        n->battery_mv      = hello->battery_mv;
        n->battery_pct     = (uint8_t)battery_mv_to_pct(hello->battery_mv);
        ESP_LOGI(TAG, "WAKE_HELLO <- %s: 사이클#%lu wake=%u batt_raw=%u batt_mv=%u batt_pct=%u",
                 n->name, (unsigned long)n->ds_cycle_count, (unsigned)hello->wake_reason,
                 (unsigned)n->battery_adc_raw, (unsigned)n->battery_mv, (unsigned)n->battery_pct);

        /* WAKE_HELLO_ACK — 캠이 보낸 esp_now_reliable_request()의 응답 leg 그 자체(별도로
         * 또 reliable을 감싸지 않음, SET_TIME_ACK/CAPTURE_STATUS_ACK와 동일 관례) */
        add_peer_if_needed(info->src_addr);
        esp_now_wake_hello_ack_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_WAKE_HELLO_ACK };
        esp_now_send(info->src_addr, (const uint8_t *)&ack, sizeof(ack));

        /* CASK — 항상 고정 3단계(2026-08-26, 사용자 지시로 재설계): CONFIG(항상 먼저, 캠이
         * 설정을 로컬에 저장 안 하므로 매번 필요) -> 할일(대기 큐에 있으면 그것, 없으면
         * 명시적 CASK_WORK_NONE) -> SLEEP_NOW(항상 마지막). 패킷 수가 매번 똑같아야 캠이
         * "이번엔 할일 메시지가 오는지 안 오는지" 추측할 필요가 없어짐(esp_now_hub_queue_action
         * 주석 참고) */
        if (n->kind == HUB_NODE_KIND_CAM) {
            push_cam_config_to(info->src_addr);
        }
        esp_now_hub_pending_action_t action;
        if (dequeue_pending_action_locked(n, &action)) {
            esp_now_tx_enqueue(info->src_addr, action.req, action.req_len,
                                action.ack_types, action.ack_types_count,
                                action.timeout_ms, action.max_attempts, action.what);
            ESP_LOGI(TAG, "CASK 할일 -> %s: %s (대기열 %d개 남음)", n->name, action.what, n->action_queue_count);
        } else {
            esp_now_cask_work_none_t none = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_CASK_WORK_NONE };
            static const uint8_t s_work_none_ack_types[] = { ESP_NOW_MSG_CASK_WORK_NONE_ACK };
            esp_now_tx_enqueue(info->src_addr, &none, sizeof(none), s_work_none_ack_types, 1, 500, 3, "할일없음");
        }
        send_cask_sleep_now(n);
        xSemaphoreGive(s_nodes_mutex);

    } else if (msg_type == ESP_NOW_MSG_BENCH_BLAST) {
        /* 처리량+유실률 벤치마크 수신(2026-08-04, 1시간 연속 실행 지원으로 재설계) — CAM이
         * seq를 0부터 순서대로 붙여 보내므로(esp_now_cam.c의 run_bench_blast), 여기서
         * "다음에 와야 할 seq"와 실제 도착한 seq를 비교하면 로컬 큐 상태와 무관한 진짜
         * 종단간(무선 구간) 패킷 유실을 셀 수 있음 — CAM 쪽 NO_MEM 재시도 집계와는 별개로,
         * 이게 esp_now_reliable 설계에 실제로 필요한 "물리 ACK로 못 잡아내는 유실률" 숫자.
         * 1초마다 로그를 남기면 1시간에 3600줄이라 너무 많아서(사용자 지적) 30초 구간
         * 집계로 줄임 — 구간 통계 리셋, 세션 전체 누적은 별도로 유지해서 seq==0(새 세션
         * 시작)이 다시 오면 그때 세션 총계를 한 번 찍고 리셋 */
        if (len < (int)sizeof(esp_now_bench_blast_t)) return;
        const esp_now_bench_blast_t *msg = (const esp_now_bench_blast_t *)data;

        static bool     s_bench_active = false;
        static uint32_t s_bench_expected_seq = 0;
        static int64_t  s_bench_session_start_us = 0;
        static uint64_t s_bench_session_bytes = 0;
        static uint32_t s_bench_session_pkts  = 0;
        static uint32_t s_bench_session_lost  = 0;
        /* 30초 구간 집계용 */
        static uint64_t s_bench_win_bytes = 0;
        static uint32_t s_bench_win_pkts  = 0;
        static uint32_t s_bench_win_lost  = 0;
        static int64_t  s_bench_win_start_us = 0;

        int64_t now_us = esp_timer_get_time();

        if (msg->seq == 0 && s_bench_active && s_bench_expected_seq > 1) {
            /* 새 세션 시작(진짜 재시작 — ESP-NOW 물리계층 중복수신으로 seq=0이 또 온
             * 것과 구분하려고 expected_seq>1일 때만 "이전 세션 종료"로 인정) */
            uint32_t total = s_bench_session_pkts + s_bench_session_lost;
            double loss_pct = total > 0 ? (100.0 * s_bench_session_lost / total) : 0.0;
            double sec = (now_us - s_bench_session_start_us) / 1e6;
            ESP_LOGI(TAG, "BENCH 세션 종료: %.1fs간 수신%u개/유실%u개(유실율%.2f%%), 평균%.1fKB/s",
                     sec, (unsigned)s_bench_session_pkts, (unsigned)s_bench_session_lost, loss_pct,
                     sec > 0 ? (s_bench_session_bytes / 1024.0) / sec : 0.0);
            s_bench_active = false;
        }
        if (!s_bench_active) {
            s_bench_active = true;
            s_bench_expected_seq = 0;
            s_bench_session_start_us = now_us;
            s_bench_session_bytes = 0;
            s_bench_session_pkts  = 0;
            s_bench_session_lost  = 0;
            s_bench_win_bytes = 0;
            s_bench_win_pkts  = 0;
            s_bench_win_lost  = 0;
            s_bench_win_start_us = now_us;
        }

        if (msg->seq > s_bench_expected_seq) {
            uint32_t gap = msg->seq - s_bench_expected_seq;
            s_bench_session_lost += gap;
            s_bench_win_lost += gap;
        }
        if (msg->seq >= s_bench_expected_seq) {
            s_bench_expected_seq = msg->seq + 1;
        }

        s_bench_session_bytes += (uint32_t)len;
        s_bench_session_pkts++;
        s_bench_win_bytes += (uint32_t)len;
        s_bench_win_pkts++;

        int64_t win_elapsed_us = now_us - s_bench_win_start_us;
        if (win_elapsed_us >= 30 * 1000 * 1000) {
            double sec = win_elapsed_us / 1e6;
            uint32_t win_total = s_bench_win_pkts + s_bench_win_lost;
            double loss_pct = win_total > 0 ? (100.0 * s_bench_win_lost / win_total) : 0.0;
            ESP_LOGI(TAG, "BENCH 수신 중간집계(%.0fs 경과): %.1fKB/s, 수신%u개/유실%u개(유실율%.2f%%)",
                     (now_us - s_bench_session_start_us) / 1e6,
                     (s_bench_win_bytes / 1024.0) / sec,
                     (unsigned)s_bench_win_pkts, (unsigned)s_bench_win_lost, loss_pct);
            s_bench_win_bytes = 0;
            s_bench_win_pkts  = 0;
            s_bench_win_lost  = 0;
            s_bench_win_start_us = now_us;
        }

    } else if (msg_type == ESP_NOW_MSG_PHOTO_META || msg_type == ESP_NOW_MSG_PHOTO_CHUNK ||
               msg_type == ESP_NOW_MSG_PHOTO_DONE || msg_type == ESP_NOW_MSG_CAPTURE_STATUS ||
               msg_type == ESP_NOW_MSG_PHOTO_LIST_COUNT || msg_type == ESP_NOW_MSG_PHOTO_LIST_BATCH ||
               msg_type == ESP_NOW_MSG_PHOTO_LIST_DONE ||
               msg_type == ESP_NOW_MSG_PHOTO_DELETE_ACK || msg_type == ESP_NOW_MSG_PHOTO_DELETE_ALL_ACK ||
               msg_type == ESP_NOW_MSG_PHOTO_WINDOW_STATUS_REQUEST) {
        /* ESP-NOW는 recv_cb를 하나만 등록할 수 있어서, 사진 관련 프로토콜(전송/목록/삭제/
         * 지금촬영 진행상태) 처리는 전부 esp_now_photo.c로 넘김 */
        esp_now_photo_on_recv(msg_type, info ? info->src_addr : NULL, data, len);

    } else if (msg_type == ESP_NOW_MSG_CAM_CONFIG_ACK) {
        /* esp_now_reliable_on_recv(위)가 이미 esp_now_tx 태스크를 깨워서 재시도 루프를
         * 끝내지만, 그건 esp_now_tx 모듈 내부 상태일 뿐이라 UI(설정탭 Apply 진행팝업)가
         * 볼 방법이 없음 — capture_stage와 같은 이유로 여기서 별도 폴링 상태를 갱신 */
        if (len < (int)sizeof(esp_now_cam_config_ack_t)) return;
        s_config_apply_stage = HUB_CONFIG_APPLY_ACKED;

    }
}

/* 2026-08-21 — 상황판 요약 맨 윗줄에 웹 대시보드 URL 표시용(사용자 지시). IP 못 받은
 * 상태(빈 문자열)면 ui_main.c가 URL 자체를 숨김 */
static char s_own_ip_str[16] = "";

const char *esp_now_hub_get_own_ip_str(void)
{
    return s_own_ip_str;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
#if !CNTL_WIFI_STANDALONE_AP_TEST
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi 연결 끊김(reason=%d, rssi=%d) — 재시도",
                 disc ? disc->reason : -1, disc ? disc->rssi : 0);
        s_own_ip_str[0] = '\0';  /* IP 무효화 — 재연결해서 새 IP 받을 때까지 URL 숨김 */
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP 받음: " IPSTR, IP2STR(&evt->ip_info.ip));
        /* 2026-08-21 — 상황판 요약에 웹 대시보드 접속 URL을 보여주기 위해 저장(사용자 지시) */
        snprintf(s_own_ip_str, sizeof(s_own_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
    }
#else
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "AP: 클라이언트 접속 " MACSTR, MAC2STR(evt->mac));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "AP: 클라이언트 접속해제 " MACSTR, MAC2STR(evt->mac));
    }
#endif
}

static void wifi_bringup(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

#if !CNTL_WIFI_STANDALONE_AP_TEST
    esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
#else
    /* 채널 격리 실험(위 CNTL_WIFI_STANDALONE_AP_TEST 주석 참고) — hkhome STA 연결 없이
     * Cntl 혼자만의 SoftAP로. CAM/Sens용 ESP-NOW 채널을 Cntl이 완전히 독점하게 됨 */
    esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.ap.ssid, CNTL_AP_SSID, sizeof(wifi_cfg.ap.ssid) - 1);
    wifi_cfg.ap.ssid_len = strlen(CNTL_AP_SSID);
    strncpy((char *)wifi_cfg.ap.password, CNTL_AP_PASSWORD, sizeof(wifi_cfg.ap.password) - 1);
    wifi_cfg.ap.channel = CNTL_AP_CHANNEL;
    wifi_cfg.ap.max_connection = CNTL_AP_MAX_CONN;
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP 시작: SSID=%s CH=%d (채널 격리 실험 모드)", CNTL_AP_SSID, CNTL_AP_CHANNEL);
#endif

    /* WiFi 절전(모뎀 슬립) 끔(2026-08-02) — 부팅 로그에 "wifi:pm start, type: 1"이 찍혀서
     * 기본값(절전 켜짐)으로 동작 중이었음을 확인. 절전 중엔 라디오가 공유기 비콘 주기에
     * 맞춰서만 깨어나는데, CAM이 보내는 ESP-NOW 청크는 그 주기와 무관하게 아무 때나
     * 도착해서 라디오가 자는 타이밍에 온 청크를 놓침 — 사진 가져오기 청크 누락(3002)의
     * 실제 원인으로 추정(실기에서 확인된 증상: 느리고 ETA가 들쭉날쭉하다 결국 실패).
     * Cntl은 상시전원(배터리 아님)이라 절전을 꺼도 손해가 없음 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void esp_now_hub_init(void)
{
    /* recv_cb가 등록되기 전에 먼저 만들어둬야 함 — 등록 직후부터 다른 태스크에서
     * s_nodes[]를 건드릴 수 있음 */
    s_nodes_mutex = xSemaphoreCreateMutex();
    assert(s_nodes_mutex != NULL);

    /* 2026-08-22 — Cntl 자신은 배터리가 없지만, CAM/Sens가 보고해오는 mV값을 %%로 바꾸려면
     * battery_mv_to_pct()의 커브 상수(full/empty_mv)가 세팅돼있어야 함(battery_init()을 안
     * 부르면 0으로 나누기가 됨) — Sens의 기본값(sensor_node.c/sensor-c6.c의
     * BATT_FULL_MV_DEFAULT/empty_mv)과 동일하게 맞춤. 실제 ADC는 안 건드림(battery_set_curve) */
    battery_set_curve(4020.0f, 3300.0f);

    wifi_bringup();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_broadcast_addr, sizeof(peer.peer_addr));
    peer.ifidx   = CNTL_WIFI_IF;
    peer.channel = 0;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /* 2026-08-25(CASK 재설계) — 예전엔 여기서 HUB_RESET을 브로드캐스트해서, Cntl이 재부팅해도
     * "아직 자기가 페어링된 줄 아는" 노드들을 강제로 재광고시켰음. 이제는 모든 재연결이
     * 노드(캠) 주도라 이게 불필요해짐 — 재부팅한 Cntl은 다음 WAKE_HELLO를 그냥 "이 캠을
     * 모르는 CNTL"로 조용히 무시하고, 캠은 자기 재시도+폴백으로 알아서 다시 찾아옴 */

    /* 2026-08-25(CASK 재설계) — CNTL의 능동적 생존판단 스윕(send_cask_sleep_now 위쪽 참고).
     * 노드 수와 무관하게 1초 주기 타이머 하나 */
    const esp_timer_create_args_t sweep_args = {
        .callback = liveness_sweep_cb, .name = "liveness_sweep",
    };
    esp_timer_create(&sweep_args, &s_liveness_sweep_timer);
    esp_timer_start_periodic(s_liveness_sweep_timer, LIVENESS_SWEEP_INTERVAL_US);

    ESP_LOGI(TAG, "ESP-NOW 허브 시작됨 (STA)");
}

uint8_t esp_now_hub_get_wifi_channel(void)
{
    uint8_t channel = 0;
    wifi_second_chan_t second_chan;
    esp_wifi_get_channel(&channel, &second_chan);
    return channel;
}

/* 2026-08-08 재설계 — 응답성이 시스템 전체 공통 설정(device_config)이 되면서, 이 타임아웃도
 * 노드별이 아니라 그 값 하나로 전체 계산. CAM 쪽 "끊김 판정"(esp_now_channelsync의
 * PING_FAIL_THRESHOLD=3회 연속 무응답)이 이제 응답성*3만큼 걸리므로, Cntl이 그보다 먼저/
 * 빠듯하게 "무응답=이상"으로 판단하면 CAM이 정상적으로 뜸하게 확인하는 중인데도 매번
 * 오탐(false 끊김)이 남 — CAM 쪽 판정+한 번 더 재시도할 시간까지 여유있게 감안한 배수 */
#define HUB_NODE_TIMEOUT_MARGIN_MULT 6

uint32_t esp_now_hub_node_timeout_ms(void)
{
    uint32_t response_sec = device_config_get_response_interval_sec();
    if (response_sec == 0) return ESP_NOW_HUB_NODE_TIMEOUT_MS;
    uint32_t computed = response_sec * 1000U * HUB_NODE_TIMEOUT_MARGIN_MULT;
    return computed > ESP_NOW_HUB_NODE_TIMEOUT_MS ? computed : ESP_NOW_HUB_NODE_TIMEOUT_MS;
}

int esp_now_hub_get_nodes(hub_node_kind_t kind, esp_now_hub_node_t *out, int max)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t timeout_ms = esp_now_hub_node_timeout_ms();
    int count = 0;
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < s_node_count && count < max; i++) {
        if (kind != HUB_NODE_KIND_UNKNOWN && s_nodes[i].kind != kind) continue;
        if (now_ms - s_nodes[i].last_seen_ms > timeout_ms) continue;
        out[count++] = s_nodes[i];
    }
    xSemaphoreGive(s_nodes_mutex);
    return count;
}

bool esp_now_hub_is_reconnect_stuck(const uint8_t *mac)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t timeout_ms = esp_now_hub_node_timeout_ms();
    bool stuck = false;
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    esp_now_hub_node_t *n = find_node(mac);
    if (n && n->ever_paired && n->conn_state != NODE_CONN_PAIRED && (now_ms - n->last_paired_ms) > timeout_ms) {
        stuck = true;
    }
    xSemaphoreGive(s_nodes_mutex);
    return stuck;
}

hub_conn_state_t esp_now_hub_get_conn_state(const uint8_t *mac)
{
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    esp_now_hub_node_t *n = find_node(mac);
    bool ever_paired = n && n->ever_paired;
    bool radio_paired = n && (n->conn_state == NODE_CONN_PAIRED);
    bool user_unpaired = n && n->user_unpaired;
    xSemaphoreGive(s_nodes_mutex);

    /* 2026-08-21 — user_unpaired(사용자가 명시적으로 "연결해제"를 누른 상태)는
     * is_reconnect_stuck()의 타임아웃(last_paired_ms 기준, "갑자기 조용해진 비정상 상황"을
     * 감지하기 위한 값)을 기다릴 필요 없이 곧장 WAITING으로 — 의도적 연결해제와 예기치 못한
     * 끊김은 성격이 다른데 같은 타임아웃으로 취급하고 있었음(실기에서 확인: 연결해제 버튼을
     * 눌러도 화면이 몇~수십 초 동안 "연결됨"으로 남아있던 원인) */
    if (!ever_paired || user_unpaired || esp_now_hub_is_reconnect_stuck(mac)) return HUB_CONN_STATE_WAITING;
    return radio_paired ? HUB_CONN_STATE_ACTIVE : HUB_CONN_STATE_PAIRED;
}

#define PAIR_REQUEST_RETRY_TIMEOUT_MS 500
#define PAIR_REQUEST_RETRY_ATTEMPTS   \
    ((ESP_NOW_NODE_UNPAIRED_RETRY_SEC * 1000 * 2) / PAIR_REQUEST_RETRY_TIMEOUT_MS + 1)

/* 2026-08-24 — 실제 PAIR_REQUEST 전송(내부 전용, static). 예전엔 이게 공개 API였고 UI가
 * 직접 불렀는데, 그러면 Cntl의 고정채널과 캠의 스캔채널이 우연히 겹치길 바라며 독립 재시도
 * 버스트를 쏘는 구조적 도박이었음(사용자 지적: "원론적으로 해결"). 이제 이 함수는 채널이
 * 이미 맞다고 증명된 순간(=이 노드의 ADVERTISE를 방금 수신한 시점)에만 호출됨 —
 * esp_now_hub_request_pair()(사용자 버튼)와 자동 재페어링 둘 다 여기로 수렴 */
static void esp_now_hub_pair(const uint8_t *mac)
{
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    esp_now_hub_node_t *n = find_node(mac);
    bool ok = (n && n->conn_state != NODE_CONN_PAIRED);
    char name_copy[ESP_NOW_LINK_NAME_LEN] = { 0 };
    if (ok) {
        n->user_unpaired = false;  /* 사용자가 다시 연결을 시도하는 것 — keepalive 무시 플래그 해제 */
        strncpy(name_copy, n->name, sizeof(name_copy) - 1);
        /* 2026-08-24 — 시간 기반 대기창 폐기. PAIR_PENDING 표시는 UI 상태 문구용으로만 남김
         * (esp_now_hub_get_conn_state() 등). "몇 번 더 시도할지"는 이제 호출부(ADVERTISE
         * 핸들러)의 pair_req_attempts_left가 담당함 */
        n->conn_state = NODE_CONN_PAIR_PENDING;
    }
    xSemaphoreGive(s_nodes_mutex);
    if (!ok) return;

    add_peer_if_needed(mac);

    uint8_t hub_mac[6];
    esp_wifi_get_mac(CNTL_WIFI_IF, hub_mac);

    esp_now_pair_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PAIR_REQUEST,
    };
    memcpy(req.hub_mac, hub_mac, sizeof(req.hub_mac));
    /* 2026-08-05 Layer 1 — esp_now_tx 태스크로 큐잉(reliable_request로 PAIR_ACK을 기다리며
     * 재시도). 이 함수 자체는 그대로 즉시 리턴(UI 안 얼어붙음, 기존과 동일한 UX) — 실제
     * 페어링 상태 반영은 지금처럼 recv_cb의 PAIR_ACK 핸들러가 그대로 담당함(esp_now_tx는
     * 상태를 안 건드리는 순수 전송 스케줄러, esp_now_tx.h 참고) */
    static const uint8_t s_pair_ack_types[] = { ESP_NOW_MSG_PAIR_ACK };
    /* 2026-08-21 나이키스트 재설계(사용자 지시) — 노드(CAM/Sens)는 페어링 전엔 응답성
     * 설정과 무관하게 "짧게 깨서 시도, 안 되면 짧게 자고 재시도"를 반복함
     * (ESP_NOW_NODE_UNPAIRED_RETRY_SEC, esp_now_link.h 공용 헤더 — CAM/Sens 둘 다 이 값을
     * 그대로 씀). 이 켜짐/꺼짐 주기보다 CNTL의 재시도 구간이 짧으면, 사용자가 버튼을 한 번
     * 누른 시점이 하필 노드가 자고 있는 위상과 겹쳐서 그 판 전체가 허사가 됨(재시도는 매번
     * 500ms 간격이라 촘촘하지만, 전체 구간이 짧으면 위상을 못 잡음) — 안전한 재시도 구간은
     * 이 값의 최소 2배여야 위상과 무관하게 반드시 한 번은 깨어있는 구간과 겹침(나이키스트
     * 원칙과 동일한 이유: 표본 "간격"이 아니라 표본을 "충분히 오래" 걸쳐야 함).
     * 부팅/WiFi초기화 오버헤드(~1.8초, 이름 붙은 상수 없이 그때그때 걸리는 시간이라
     * 하드코딩하면 휴리스틱이 됨, 사용자 지적)는 이 공식에 안 넣음 — 6초(3초×2)가 실측
     * 죽는시간(~4.8초)보다 이미 크므로 별도로 안 넣어도 안전마진 안에 들어옴 */
    esp_now_tx_enqueue(mac, &req, sizeof(req), s_pair_ack_types, 1,
                        PAIR_REQUEST_RETRY_TIMEOUT_MS, PAIR_REQUEST_RETRY_ATTEMPTS, "페어링");
    ESP_LOGI(TAG, "PAIR_REQUEST -> %s 큐잉됨", name_copy);
}

/* 2026-08-24(사용자 지시) — "연결" 버튼의 새 진입점. 여기선 아무것도 안 보내고 플래그만
 * 세움 — 실제 전송은 ADVERTISE 핸들러가 이 노드의 광고를 실제로 받는 순간에 esp_now_hub_pair()
 * 를 부르면서 함(그 순간 채널이 같다는 게 이미 증명됨) */
void esp_now_hub_request_pair(const uint8_t *mac)
{
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    esp_now_hub_node_t *n = find_node(mac);
    char name_copy[ESP_NOW_LINK_NAME_LEN] = { 0 };
    bool ok = (n && n->conn_state != NODE_CONN_PAIRED);
    if (ok) {
        n->user_unpaired = false;
        n->user_pair_wanted = true;
        n->pair_req_attempts_left = 3;  /* 2026-08-24 — 광고 수신 트리거 3회까지 */
        strncpy(name_copy, n->name, sizeof(name_copy) - 1);
    }
    xSemaphoreGive(s_nodes_mutex);
    if (ok) {
        ESP_LOGI(TAG, "%s 연결 요청 — 다음 ADVERTISE 수신 시 PAIR_REQUEST 전송 예정", name_copy);
    }
}

void esp_now_hub_bench_start(uint16_t duration_sec, uint8_t mode)
{
    uint8_t target_mac[6] = { 0 };
    bool found = false;
    char name_copy[ESP_NOW_LINK_NAME_LEN] = { 0 };

    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].kind == HUB_NODE_KIND_CAM && s_nodes[i].conn_state == NODE_CONN_PAIRED) {
            memcpy(target_mac, s_nodes[i].mac, sizeof(target_mac));
            strncpy(name_copy, s_nodes[i].name, sizeof(name_copy) - 1);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_nodes_mutex);

    if (!found) {
        ESP_LOGW(TAG, "벤치마크: 페어링된 CAM 없음");
        return;
    }

    esp_now_bench_start_t msg = {
        .version      = ESP_NOW_LINK_VERSION,
        .msg_type     = ESP_NOW_MSG_BENCH_START,
        .duration_sec = duration_sec,
        .mode         = mode,
    };
    esp_err_t err = esp_now_send(target_mac, (const uint8_t *)&msg, sizeof(msg));
    ESP_LOGI(TAG, "BENCH_START(mode=%u, %u초) -> %s: %s", mode, duration_sec, name_copy, esp_err_to_name(err));
}

void esp_now_hub_apply_cam_capture_interval_sec(const uint8_t *mac, uint32_t sec)
{
    device_config_set_cam_capture_interval_sec(sec);
    push_cam_config_to(mac);
}

/* 2026-08-21 — AGC/AEC On/Off(세로줄 노이즈 진단용), 촬영주기와 같은 카메라별 설정 패턴 */
void esp_now_hub_apply_cam_agc_enable(const uint8_t *mac, bool enable)
{
    device_config_set_agc_enable(enable);
    push_cam_config_to(mac);
}

void esp_now_hub_apply_cam_aec_enable(const uint8_t *mac, bool enable)
{
    device_config_set_aec_enable(enable);
    push_cam_config_to(mac);
}

void esp_now_hub_apply_cam_xclk_mhz(const uint8_t *mac, uint8_t mhz)
{
    device_config_set_xclk_mhz(mhz);
    push_cam_config_to(mac);
}

bool esp_now_hub_apply_response_interval_sec(uint32_t sec)
{
    device_config_set_response_interval_sec(sec);

    /* 시스템 공통 설정이므로 지금 페어링된 CAM 전부에게 다시 보냄(esp_now_hub_bench_start의
     * "페어링된 노드 순회" 패턴과 동일) — SENS는 아직 CAM_CONFIG_SET을 이해 못 하므로 CAM만 */
    uint8_t targets[ESP_NOW_HUB_MAX_NODES][6];
    int target_count = 0;
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].kind == HUB_NODE_KIND_CAM && s_nodes[i].conn_state == NODE_CONN_PAIRED) {
            memcpy(targets[target_count++], s_nodes[i].mac, 6);
        }
    }
    xSemaphoreGive(s_nodes_mutex);

    for (int i = 0; i < target_count; i++) {
        push_cam_config_to(targets[i]);
    }
    return target_count > 0;
}

void esp_now_hub_unpair(const uint8_t *mac)
{
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    esp_now_hub_node_t *n = find_node(mac);
    char name_copy[ESP_NOW_LINK_NAME_LEN] = { 0 };
    if (n) {
        strncpy(name_copy, n->name, sizeof(name_copy) - 1);
        n->conn_state = NODE_CONN_ORPHAN;
        n->user_unpaired = true;  /* CAM/SENS의 keepalive(PAIR_ACK 재전송)로 조용히 재연결되는 것 방지 */
    }
    xSemaphoreGive(s_nodes_mutex);
    if (!n) return;

    /* 2026-08-25(CASK 재설계, 사용자 지시) — UNPAIR은 사용자의 실시간 조작이라 즉각·확실히
     * 반영돼야 해서 reliable 스택으로 승격(예전엔 raw esp_now_send라, 유실되면 캠은 영원히
     * 자기가 여전히 페어드인 줄 알았음 — 알려진 결함이었음). esp_now_tx_enqueue()는
     * 비동기(큐잉만 하고 바로 리턴)라, 예전처럼 이 자리에서 곧바로 esp_now_del_peer()를
     * 부르면 실제 전송(및 재시도)이 일어나기도 전에 피어가 사라져서 전송 자체가 실패하는
     * 레이스가 생김 — 그래서 피어는 이제 안 지움(등록된 채로 남아도 기능상 무해, 최대
     * ESP_NOW_HUB_MAX_NODES개뿐이라 자리 걱정도 없음) */
    esp_now_unpair_t msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_UNPAIR,
    };
    static const uint8_t s_unpair_ack_types[] = { ESP_NOW_MSG_UNPAIR_ACK };
    esp_now_tx_enqueue(mac, &msg, sizeof(msg), s_unpair_ack_types, 1, 300, 3, "UNPAIR");
    ESP_LOGI(TAG, "연결 해제: %s (UNPAIR 큐잉됨)", name_copy);
}
