#include "esp_now_hub.h"
#include "esp_now_photo.h"
#include "esp_now_reliable.h"
#include "esp_now_tx.h"
#include "rtc_sync.h"
#include "ui_log.h"

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

/* Cntl 실제 sdkconfig 기준 STA 모드+SSID/PW — 4단계 테스트 때와 동일 */
#define WIFI_SSID     "hkhome"
#define WIFI_PASSWORD "mama0709!"

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
    }
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
        n->paired = false;
        memcpy(n->name, msg->name, sizeof(n->name));
        n->name[ESP_NOW_LINK_NAME_LEN - 1] = '\0';
        n->kind = classify_name(n->name);
        n->last_seen_ms = now_ms;
        char name_copy[ESP_NOW_LINK_NAME_LEN];
        strncpy(name_copy, n->name, sizeof(name_copy) - 1);
        name_copy[sizeof(name_copy) - 1] = '\0';
        xSemaphoreGive(s_nodes_mutex);
        if (was_paired) {
            ESP_LOGW(TAG, "%s가 재광고 시작 — 페어링 끊김으로 판단(desync 복구)", name_copy);
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
        if (n) {
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
                became_paired = !was_paired;
                strncpy(name_copy, n->name, sizeof(name_copy) - 1);
            }
        }
        xSemaphoreGive(s_nodes_mutex);
        if (became_paired) {
            ESP_LOGI(TAG, "페어링 완료: %s", name_copy);
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

    ESP_LOGI(TAG, "ESP-NOW 허브 시작됨 (STA)");
}

uint8_t esp_now_hub_get_wifi_channel(void)
{
    uint8_t channel = 0;
    wifi_second_chan_t second_chan;
    esp_wifi_get_channel(&channel, &second_chan);
    return channel;
}

int esp_now_hub_get_nodes(hub_node_kind_t kind, esp_now_hub_node_t *out, int max)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    int count = 0;
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < s_node_count && count < max; i++) {
        if (kind != HUB_NODE_KIND_UNKNOWN && s_nodes[i].kind != kind) continue;
        if (now_ms - s_nodes[i].last_seen_ms > ESP_NOW_HUB_NODE_TIMEOUT_MS) continue;
        out[count++] = s_nodes[i];
    }
    xSemaphoreGive(s_nodes_mutex);
    return count;
}

bool esp_now_hub_is_paired(const uint8_t *mac)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool paired = false;
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    esp_now_hub_node_t *n = find_node(mac);
    if (n && n->paired && (now_ms - n->last_seen_ms) <= ESP_NOW_HUB_NODE_TIMEOUT_MS) {
        paired = true;
    }
    xSemaphoreGive(s_nodes_mutex);
    return paired;
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
