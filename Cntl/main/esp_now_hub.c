#include "esp_now_hub.h"
#include "esp_now_photo.h"
#include "esp_now_reliable.h"
#include "esp_now_tx.h"
#include "rtc_sync.h"
#include "ui_log.h"
#include "device_config.h"

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
 * esp_now_hub_bench_start()/apply_response_interval_sec()이 이미 쓰는 단순화와 동일 원칙 */
static uint32_t s_last_user_action_ms = 0;
static esp_timer_handle_t s_adaptive_sleep_timer = NULL;

void esp_now_hub_note_user_action(void)
{
    s_last_user_action_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/* 페어링 직후 루틴 설정 핸드셰이크(SET_TIME+CAM_CONFIG_SET/ACK)가 끝날 시간 여유
 * (2026-08-10) — 정밀 추적 대신 고정 마진으로 충분(핸드셰이크는 보통 수십~수백ms) */
#define ADAPTIVE_SLEEP_PAIR_SETTLE_MS 1000
#define ADAPTIVE_SLEEP_CHECK_INTERVAL_US (500 * 1000)

/* 적응형 반응시간 조건(마지막 사용자 조작으로부터 device_config_get_adaptive_response_sec()
 * 이상 조용함)을 만족하면, 지금 라디오 레벨로 페어링 확인된 CAM 전부에게 SLEEP_NOW를 보냄
 * (2026-08-10) — CAM은 받으면 유휴여유를 기다리지 않고 곧바로 잠듦(cam_node.c 참고).
 * sleep_now_sent로 사이클당 1회만 보냄 — n->paired는 CAM이 다음 웨이크에 ADVERTISE를 다시
 * 보내야만 내려가는 필드라 딥슬립 구간 내내 true로 남아있어서, 이 가드가 없으면 이미 잠든
 * CAM에게 500ms마다 계속 허공에 재전송하게 됨(실사용 중 발견 — 사이클당 13초+ 동안 스팸) */
static void adaptive_sleep_timer_cb(void *arg)
{
    (void)arg;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t threshold_ms = device_config_get_adaptive_response_sec() * 1000U;
    if (now_ms - s_last_user_action_ms < threshold_ms) return;

    esp_now_sleep_now_t msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_SLEEP_NOW,
    };
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].kind != HUB_NODE_KIND_CAM || !s_nodes[i].paired) continue;
        if (s_nodes[i].sleep_now_sent) continue;
        if (now_ms - s_nodes[i].last_paired_ms < ADAPTIVE_SLEEP_PAIR_SETTLE_MS) continue;
        esp_err_t err = esp_now_send(s_nodes[i].mac, (const uint8_t *)&msg, sizeof(msg));
        ESP_LOGI(TAG, "SLEEP_NOW -> %s: %s", s_nodes[i].name, esp_err_to_name(err));
        s_nodes[i].sleep_now_sent = true;
    }
    xSemaphoreGive(s_nodes_mutex);
}

static hub_node_kind_t classify_name(const char *name)
{
    if (strncmp(name, "Cam-", 4) == 0)  return HUB_NODE_KIND_CAM;
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
    };
    static const uint8_t s_config_ack_types[] = { ESP_NOW_MSG_CAM_CONFIG_ACK };
    s_config_apply_stage = HUB_CONFIG_APPLY_SENT;
    esp_now_tx_enqueue(mac, &cfg, sizeof(cfg), s_config_ack_types, 1, 800, 3, "CAM 설정");
    ESP_LOGI(TAG, "CAM_CONFIG_SET -> 촬영주기=%us 응답성=%us 큐잉됨",
             (unsigned)cfg.capture_interval_sec, (unsigned)cfg.response_interval_sec);
}

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
        bool was_paired = n->paired;
        bool was_user_unpaired = n->user_unpaired;
        bool ever_paired = n->ever_paired;
        n->paired = false;
        memcpy(n->name, msg->name, sizeof(n->name));
        n->name[ESP_NOW_LINK_NAME_LEN - 1] = '\0';
        n->kind = classify_name(n->name);
        n->last_seen_ms = now_ms;
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
        if (ever_paired && !was_user_unpaired) {
            esp_now_hub_pair(mac_copy);
        }

        /* 사람이 페어링을 누르기 전이라도 즉시 응답 — 채널 스캔 중인 노드가 Cntl을
         * 찾았다는 걸 알고 이 채널에 고정하기 위한 용도(정식 페어링과 별개) */
        add_peer_if_needed(info->src_addr);
        esp_now_advertise_ack_t ack = {
            .version  = ESP_NOW_LINK_VERSION,
            .msg_type = ESP_NOW_MSG_ADVERTISE_ACK,
        };
        esp_wifi_get_mac(CNTL_WIFI_IF, ack.hub_mac);
        esp_now_send(info->src_addr, (const uint8_t *)&ack, sizeof(ack));

    } else if (msg_type == ESP_NOW_MSG_PAIR_ACK) {
        if (len < (int)sizeof(esp_now_pair_ack_t)) return;

        xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
        esp_now_hub_node_t *n = find_node(info->src_addr);
        bool became_paired = false;
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
            bool was_paired = n->paired;
            if (!n->user_unpaired) {
                n->paired = true;
                n->ever_paired = true;  /* 2026-08-10 — 한 번 세팅되면 이번 부팅 세션 내내
                                            유지, ADVERTISE 핸들러의 자동 재페어링 판단에 씀 */
                n->last_paired_ms = now_ms;  /* esp_now_hub_is_reconnect_stuck()의 기준시각 */
                n->sleep_now_sent = false;  /* 새 웨이크 사이클 시작 — 이번 사이클에 한 번은
                                                다시 보낼 수 있어야 함 */
                became_paired = !was_paired;
                strncpy(name_copy, n->name, sizeof(name_copy) - 1);
            }
        }
        xSemaphoreGive(s_nodes_mutex);
        if (became_paired) {
            ESP_LOGI(TAG, "페어링 완료: %s", name_copy);
            /* 2026-08-10 — 방금 막 깨어나 페어링된 시점을 "마지막 사용자 조작"으로도 침 —
             * 안 그러면 이 타임스탬프는 세션 최초 1회(자동 목록조회)에만 갱신되고 그 뒤로는
             * 영원히 그대로라, Cntl이 계속 켜져 있는 한 두 번째 페어링부터는 "마지막 조작 이후
             * 경과시간"이 이미 적응형 임계값을 훌쩍 넘겨 있어서 adaptive_sleep_timer_cb가
             * 페어링 직후(설정간격 500ms 이내) 곧바로 SLEEP_NOW를 쏴버림 — 매 웨이크마다 사용자
             * 반응 시간이 사실상 0이 되는 버그(실사용 중 발견, 3006 반복 원인) */
            esp_now_hub_note_user_action();
            /* CAM/Sens는 자체 RTC가 없어서 페어링될 때마다 Cntl 시각을 알려줌 — 이게
             * 없으면 CAM의 시계가 부팅 시각(1970-01-01 근처)에 멈춰있어서 사진 파일명
             * (촬영시각 유닉스 타임스탬프)이 전부 1월 1일로 찍힘(2026-08-01 실기에서 확인) */
            esp_now_set_time_t set_time = {
                .version   = ESP_NOW_LINK_VERSION,
                .msg_type  = ESP_NOW_MSG_SET_TIME,
                .unix_time = rtc_sync_get_unix_time(),
            };
            esp_err_t err = esp_now_send(info->src_addr, (const uint8_t *)&set_time, sizeof(set_time));
            ESP_LOGI(TAG, "SET_TIME(%u) 전송: %s", (unsigned)set_time.unix_time, esp_err_to_name(err));

            /* 2026-08-08 설계 — CAM/SENS는 설정을 로컬에 저장하지 않으므로, 페어링될
             * 때마다 Cntl이 기억하고 있는 현재 설정값을 매번 다시 밀어줌(SET_TIME과 같은
             * 타이밍) — 그래야 재부팅한 CAM이 Kconfig 기본값이 아니라 사용자가 마지막으로
             * Apply한 값으로 곧바로 동작함 */
            if (kind_copy == HUB_NODE_KIND_CAM) {
                push_cam_config_to(info->src_addr);
            }
        }

    } else if (msg_type == ESP_NOW_MSG_CHANNEL_PING) {
        /* 채널 추적(Layer 0) 생존확인(2026-08-04) — esp_now_channelsync.h 설계 참고. 페어링
         * 여부와 무관하게 항상 즉시 응답함(생존확인은 페어링 승인과 별개 개념) — 노드가 아직
         * 모르는 상대여도(테이블에 없어도) PONG은 보내야 노드 쪽 채널 동기화가 유지됨.
         * 아는 노드면 last_seen_ms도 갱신 — 예전엔 PAIR_ACK(keepalive)가 이 역할을 했는데
         * 이제 그건 진짜 페어링 확인 전용으로 되돌아갔으므로 이 PING이 그 자리를 대신함 */
        if (len < (int)sizeof(esp_now_channel_ping_t)) return;

        xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
        esp_now_hub_node_t *n = find_node(info->src_addr);
        if (n) n->last_seen_ms = now_ms;
        xSemaphoreGive(s_nodes_mutex);

        esp_now_channel_pong_t pong = {
            .version  = ESP_NOW_LINK_VERSION,
            .msg_type = ESP_NOW_MSG_CHANNEL_PONG,
        };
        esp_now_send(info->src_addr, (const uint8_t *)&pong, sizeof(pong));

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
               msg_type == ESP_NOW_MSG_PHOTO_LIST_ENTRY || msg_type == ESP_NOW_MSG_PHOTO_LIST_DONE ||
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

    } else if (msg_type == ESP_NOW_MSG_DEEP_SLEEP_STATS) {
        /* CHANNEL_PING과 같은 성격 — 노드가 그냥 페어링 완료 직후 1회 보내기만 함(ACK 없음).
         * 페어링 안 된 노드도 테이블엔 있을 수 있어서(발견됨~페어링 사이) find_node로 없으면
         * 조용히 버림 — CAM이 아직 안 붙은 CNTL에도 브로드캐스트할 이유가 없어서 이 경우는
         * 실제로는 거의 안 옴(esp_now_cam.c가 페어링 완료 직후에만 보냄).
         * ds_cycle_count/ds_total_asleep_sec/ds_rwdt_catch_count는 CAM이 안 보내는 값 —
         * CAM은 매 사이클 완전 재부팅이라 스스로 누적을 못 하므로, Cntl이 리포트를 받을
         * 때마다 직접 누적함(2026-08-10) */
        if (len < (int)sizeof(esp_now_deep_sleep_stats_t)) return;
        const esp_now_deep_sleep_stats_t *stats = (const esp_now_deep_sleep_stats_t *)data;

        xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
        esp_now_hub_node_t *n = find_node(info->src_addr);
        if (n) {
            n->last_seen_ms               = now_ms;
            n->has_deepsleep_stats        = true;
            n->ds_cycle_count++;
            n->ds_total_asleep_sec       += stats->sleep_interval_sec;
            if (stats->wake_reason == CAM_WAKE_REASON_RWDT) n->ds_rwdt_catch_count++;
            n->ds_last_wake_reason        = stats->wake_reason;
            n->ds_last_awake_uptime_ms    = stats->awake_uptime_ms;
            n->ds_last_sleep_interval_sec = stats->sleep_interval_sec;
        }
        xSemaphoreGive(s_nodes_mutex);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
#if !CNTL_WIFI_STANDALONE_AP_TEST
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 연결 끊김 — 재시도");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP 받음: " IPSTR, IP2STR(&evt->ip_info.ip));
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

    wifi_bringup();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_broadcast_addr, sizeof(peer.peer_addr));
    peer.ifidx   = CNTL_WIFI_IF;
    peer.channel = 0;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /* Cntl 재부팅 신호 브로드캐스트(2026-08-02, 타이밍 재수정 2026-08-03) — CAM/Sens가
     * 소프트리셋 전에 이미 페어링돼 있었다면 Cntl의 노드 테이블은 방금 비워졌는데도 그쪽은
     * 여전히 자기가 페어링된 줄 알고 ADVERTISE 없이 keepalive만 보냄(esp_now_link.h의
     * ESP_NOW_MSG_HUB_RESET 주석 참고) — 그 상태론 재광고를 스스로 트리거할 방법이 없어서
     * Cntl이 먼저 알려줘야 함.
     * 원래 IP_EVENT_STA_GOT_IP(공유기 완전 연결 시점)에서 보냈는데, 이게 사용자 실제
     * 워크플로우("항상 소프트리셋 -> 연결 -> 목록 -> 사진")와 충돌하는 레이스가 있었음
     * (2026-08-03 실기에서 확인): 공유기 재연결이 느릴 때(인증 backoff 등, 실기에서
     * "Association refused... comeback time" 확인된 적 있음) 사용자가 CAM과 방금 새로
     * 페어링을 끝낸 뒤에야 이 브로드캐스트가 뒤늦게 나가서, 이번 부팅에서 막 정상적으로
     * 맺어진 페어링을 "예전 거"로 오인하고 걷어차버림(사용자 지적: "연결됨으로 상태가
     * 바뀌고... 목록을 가져왔잖아, 연결 됐던 거 아냐?"). 지금은 여기, ESP-NOW 자체가 막
     * 켜진 시점(공유기 연결 여부와 무관, 부팅 극초반)에 보내서 사람이 화면을 터치해서
     * 페어링할 수 있는 어떤 시점보다도 반드시 먼저 나가도록 함 — 802.11 채널 고정 자체는
     * 공유기 인증 절차 초반(esp_wifi_connect 시작 시점)에 이미 이뤄지므로 채널 문제도
     * 없음 */
    esp_now_hub_reset_t reset_msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_HUB_RESET,
    };
    esp_err_t reset_err = esp_now_send(s_broadcast_addr, (const uint8_t *)&reset_msg, sizeof(reset_msg));
    ESP_LOGI(TAG, "HUB_RESET 브로드캐스트: %s", esp_err_to_name(reset_err));

    const esp_timer_create_args_t adaptive_sleep_args = {
        .callback = adaptive_sleep_timer_cb, .name = "adaptive_sleep",
    };
    esp_timer_create(&adaptive_sleep_args, &s_adaptive_sleep_timer);
    esp_timer_start_periodic(s_adaptive_sleep_timer, ADAPTIVE_SLEEP_CHECK_INTERVAL_US);

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
    if (n && n->ever_paired && !n->paired && (now_ms - n->last_paired_ms) > timeout_ms) {
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
    bool radio_paired = n && n->paired;
    xSemaphoreGive(s_nodes_mutex);

    if (!ever_paired || esp_now_hub_is_reconnect_stuck(mac)) return HUB_CONN_STATE_WAITING;
    return radio_paired ? HUB_CONN_STATE_ACTIVE : HUB_CONN_STATE_PAIRED;
}

void esp_now_hub_pair(const uint8_t *mac)
{
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    esp_now_hub_node_t *n = find_node(mac);
    bool ok = (n && !n->paired);
    char name_copy[ESP_NOW_LINK_NAME_LEN] = { 0 };
    if (ok) {
        n->user_unpaired = false;  /* 사용자가 다시 연결을 시도하는 것 — keepalive 무시 플래그 해제 */
        strncpy(name_copy, n->name, sizeof(name_copy) - 1);
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
    /* 이 호출은 대부분 CAM Deep Sleep의 ADVERTISE 핸들러가 매 사이클 자동으로 거는 백그라운드
     * 재연결 시도임(2026-08-10) — CAM이 아직 채널동기화 중이라 첫 시도가 무응답으로 실패하는
     * 게 실기에서 흔했고(뒤이은 재시도가 곧 성공), 이건 connectionless 프로토콜에서 정상
     * 범위. esp_now_tx.c가 이제 무응답이어도 UI 에러를 안 띄우므로(진단 로그만) 여기서
     * 따로 신경 안 써도 됨(사용자 지적으로 esp_now_tx.c 자체를 그렇게 정리함) */
    esp_now_tx_enqueue(mac, &req, sizeof(req), s_pair_ack_types, 1, 500, 5, "페어링");
    ESP_LOGI(TAG, "PAIR_REQUEST -> %s 큐잉됨", name_copy);
}

void esp_now_hub_bench_start(uint16_t duration_sec, uint8_t mode)
{
    uint8_t target_mac[6] = { 0 };
    bool found = false;
    char name_copy[ESP_NOW_LINK_NAME_LEN] = { 0 };

    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].kind == HUB_NODE_KIND_CAM && s_nodes[i].paired) {
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

bool esp_now_hub_apply_response_interval_sec(uint32_t sec)
{
    device_config_set_response_interval_sec(sec);

    /* 시스템 공통 설정이므로 지금 페어링된 CAM 전부에게 다시 보냄(esp_now_hub_bench_start의
     * "페어링된 노드 순회" 패턴과 동일) — SENS는 아직 CAM_CONFIG_SET을 이해 못 하므로 CAM만 */
    uint8_t targets[ESP_NOW_HUB_MAX_NODES][6];
    int target_count = 0;
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].kind == HUB_NODE_KIND_CAM && s_nodes[i].paired) {
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
        n->paired = false;
        n->user_unpaired = true;  /* CAM/SENS의 keepalive(PAIR_ACK 재전송)로 조용히 재연결되는 것 방지 */
    }
    xSemaphoreGive(s_nodes_mutex);
    if (!n) return;

    /* 피어를 지우기 전에 먼저 UNPAIR을 보내야 함(피어 삭제 후엔 유니캐스트가 안 나감) —
     * 이게 없으면 노드가 자기 상태를 몰라서 계속 keepalive를 보냄(실기로 확인된 문제) */
    esp_now_unpair_t msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_UNPAIR,
    };
    esp_err_t err = esp_now_send(mac, (const uint8_t *)&msg, sizeof(msg));
    ESP_LOGI(TAG, "UNPAIR -> %s: %s", name_copy, esp_err_to_name(err));

    if (esp_now_is_peer_exist(mac)) {
        esp_now_del_peer(mac);
    }
    ESP_LOGI(TAG, "연결 해제: %s", name_copy);
}
