#include "esp_now_node.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "status_led.h"
#include "esp_now_channelsync.h"
#include "esp_now_reliable.h"
#include "rwdt_guard.h"

/**
 * @file esp_now_node_cask.c
 * @brief SCD41 헤드리스 빌드 전용 — CAM의 CASK(WAKE_HELLO 웨이크 사이클) 구조를 그대로 이식
 *        (2026-09-05, 사용자 지시: "캐스크, esp now 등의 뼈대와 프로토콜을... 캠에서 구현된
 *        걸 가져다 써야되"). Sens도 CAM과 동일하게 매 사이클 완전히 재부팅하는 딥슬립
 *        구조로 감 — WAKE_HELLO_SENS는 매 웨이크(=매 부팅) 1회 이상, esp_now_node_report_reading()이
 *        측정값(신선하든 캐시든)과 함께 보냄(CAM의 "매 웨이크 1회 WAKE_HELLO"와 동일 리듬).
 *
 * 2026-09-05(두 번째 수정, 사용자 지시: "캠과 동일한 구조로 가라") — 애초 이식판은
 * esp_now_node_report_reading() 안에서만 "허브 알면 패스트패스 시도"를 했는데, 이 함수는
 * sens_deep_sleep_node.c의 측정+대기 루프보다 "뒤"에 불렸음(esp_now_node_init()은 스캔만
 * 시작하고 아무 것도 안 기다림) — 그래서 sens_deep_sleep_node.c가 옛날에 캠처럼 "페어링될
 * 때까지 폴링 대기"를 했을 때, 그 조건이 이 시점엔 절대 만족될 수 없었음(패스트패스 자체가
 * 아직 시도되지도 않았으므로). 캠은 esp_now_cam_init() 끝에서 esp_now_cam_reconnect()를
 * 동기 호출해서 이 문제 자체가 없음 — app_main이 그 결과(paired_now)만 보면 됨.
 * 센스는 esp_now_node_report_reading() 자체가 캠의 esp_now_cam_reconnect()와 동일한 역할
 * (패스트패스 시도 또는 폴백스캔 시작, 매 호출마다 재사용 가능)을 겸하므로, 호출부
 * (sens_deep_sleep_node.c)가 esp_now_node_init() 직후 이 함수를 한 번 동기 호출하기만 하면
 * 캠과 완전히 동일한 타이밍이 됨 — 이 파일 자체는 그 호출을 대신 해주지 않음(호출 순서는
 * app_main의 몫, 캠도 cam_node.c의 app_main이 그 순서를 정함).
 *
 * CAM의 esp_now_cam.c(cam_conn_state_t/should_advertise/on_channel_synced/recv_cb 구조,
 * esp_now_cam_try_wake_hello_fast_path)와 최대한 같은 패턴을 그대로 따름 — 프로토콜/상태
 * 판정 로직이 두 노드 종류 사이에서 어긋나면 안 되므로. 이벤트기반 재확인 신호(세마포어)/
 * 스윕완료 플래그/SLEEP_NOW 상태는 캠과 동일하게 app_main 쪽(sens_deep_sleep_node.c)이
 * 소유 — 이 파일은 esp_now_node_signal_recheck()/esp_now_node_note_sleep_now_requested()/
 * esp_now_node_note_scan_restarted()(esp_now_node.h 선언, sens_deep_sleep_node.c 구현)를
 * 상태 변화 지점마다 불러줄 뿐(캠의 esp_now_cam.c가 cam_node_*()를 부르는 것과 동일 관례).
 */

static const char *TAG = "esp_now_node_cask";

typedef enum {
    SENS_CONN_ORPHAN = 0,
    SENS_CONN_FOUND,
    SENS_CONN_PAIRED,
} sens_conn_state_t;

static sens_conn_state_t s_conn_state = SENS_CONN_ORPHAN;
static uint8_t s_hub_mac[6] = { 0 };

/* CAM의 s_wake_hub_*와 동일한 이유로 이식(esp_now_cam.c 참고) — 딥슬립 경계 너머로 "알려진
 * 허브"를 기억해서 매 웨이크(=매 부팅)마다 광고 없이 곧장 유니캐스트 재연결을 시도함 */
static RTC_DATA_ATTR uint8_t s_wake_hub_mac[6]  = { 0 };
static RTC_DATA_ATTR uint8_t s_wake_hub_channel = 0;
static RTC_DATA_ATTR bool    s_wake_hub_known   = false;

static char s_name[ESP_NOW_LINK_NAME_LEN] = "";
static uint8_t s_mac[6] = { 0 };
static gpio_num_t s_led_pin = GPIO_NUM_NC;

static bool     s_channelsync_initialized = false;
static RTC_DATA_ATTR uint32_t s_sample_interval_sec = 15;  /* Cntl의 SENS_CONFIG_SET이 갱신 —
                                                 기본값은 sensor_node.c의 기존 SCD41_CYCLE_MS와
                                                 동일. 실제 딥슬립 시간은 캠과 동일하게 SLEEP_NOW.
                                                 sleep_sec 그대로 씀(esp_now_node_get_last_
                                                 sleep_sec() 참고) — 이 값은 WAKE_HELLO_SENS의
                                                 sleep_interval_sec 필드로 보고되어(콘이
                                                 MIN(응답성,이 값)을 계산하는 재료) 쓰이는
                                                 것과는 별개로, sens_deep_sleep_node.c가 "실측정을
                                                 할 때가 됐는지"를 판단하는 기준으로도 씀
                                                 (esp_now_node_get_sample_interval_sec() 참고) —
                                                 그래서 딥슬립 경계 너머 유지돼야 함(RTC_DATA_ATTR,
                                                 안 그러면 매 부팅 15로 리셋되어 그 판단이
                                                 이번 사이클의 SENS_CONFIG_SET을 받기도 전엔
                                                 항상 부정확함) */
static uint32_t s_last_sleep_sec = 0;
static uint32_t s_last_report_ms = 0;

/* 2026-09-05(사용자 지시로 재설계) — 센서종류/채널구성은 페어링 완료(PAIR_ACK) 순간에만
 * 실어보냄(esp_now_pair_ack_t 참고) — 매 캐스크(WAKE_HELLO_SENS)마다 다시 보내지 않음.
 * esp_now_node_init()이 받아서 여기 저장, PAIR_ACK 전송 시(recv_cb의 PAIR_REQUEST 분기)
 * 채워 씀. 빌드 시 하드코딩된 값(자동판별 안 함, 사용자 지시) — 부팅 내내 안 바뀜 */
static sensor_kind_t s_sensor_kind = SENSOR_KIND_UNKNOWN;
static uint8_t       s_chan_count  = 0;
static uint8_t       s_chan_type[ESP_NOW_MAX_CHANNELS] = { 0 };

/* CAM의 cam_node.c capture_wake_reason() 이식(2026-09-05) — 매 부팅 1회, WiFi/센서 초기화
 * "전"에 esp_now_node_capture_wake_info()로 채움. actual_last_sleep_sec은 딥슬립 경계를
 * 넘겨야 해서 RTC_DATA_ATTR, wake_reason은 이번 부팅 안에서만 쓰이므로 일반 static으로 충분 */
static uint8_t              s_wake_reason = 0;  /* cam_wake_reason_t */
static RTC_DATA_ATTR time_t s_sleep_entry_unix_time = 0;
static RTC_DATA_ATTR uint32_t s_last_actual_sleep_sec = 0;

void esp_now_node_capture_wake_info(void)
{
    /* CAM의 cam_node.c capture_wake_reason()과 동일 판정(esp_now_link.h의 cam_wake_reason_t:
     * POWERON=0/TIMER=1/RWDT=2/OTHER=3 재사용) */
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_DEEPSLEEP) {
        uint32_t causes = esp_sleep_get_wakeup_causes();
        s_wake_reason = (causes & (1U << ESP_SLEEP_WAKEUP_TIMER)) ? 1 : 3;
    } else if (rr == ESP_RST_WDT) {
        s_wake_reason = 2;
    } else if (rr == ESP_RST_POWERON) {
        s_wake_reason = 0;
    } else {
        s_wake_reason = 3;
    }

    /* 실측 — 타이머 웨이크일 때만 의미 있음(POWERON/RWDT는 실제로 안 잤으므로 0).
     * s_sleep_entry_unix_time==0이면 이번이 첫 사이클(재플래시 등으로 RTC가 막 초기화됨)이라
     * 비교 기준이 없으므로 건너뜀 */
    if (s_wake_reason == 1 && s_sleep_entry_unix_time != 0) {
        time_t now = 0;
        time(&now);
        s_last_actual_sleep_sec = (now > s_sleep_entry_unix_time)
                                       ? (uint32_t)(now - s_sleep_entry_unix_time) : 0;
    } else {
        s_last_actual_sleep_sec = 0;
    }
    ESP_LOGI(TAG, "웨이크 원인 판정: reset_reason=%d -> wake_reason=%u 직전 실제 수면=%us",
             rr, (unsigned)s_wake_reason, (unsigned)s_last_actual_sleep_sec);
}

/* esp_deep_sleep_start() 직전에 sensor_node.c가 호출 — CAM의 s_sleep_entry_unix_time과
 * 동일 이유(다음 부팅에서 실제로 잔 시간을 계산하는 기준점) */
void esp_now_node_note_sleep_entry(void)
{
    time(&s_sleep_entry_unix_time);
}

static void set_led(led_pattern_t pattern)
{
    if (s_led_pin == GPIO_NUM_NC) return;
    status_led_set_pattern(s_led_pin, pattern);
}

/* esp_now_cam.c의 should_advertise()와 동일 이유 — PAIRED가 아닐 때만 광고 */
static bool should_advertise(void)
{
    return s_conn_state == SENS_CONN_ORPHAN || s_conn_state == SENS_CONN_FOUND;
}

static const char *conn_state_name(sens_conn_state_t s)
{
    switch (s) {
        case SENS_CONN_ORPHAN: return "ORPHAN";
        case SENS_CONN_FOUND:  return "FOUND";
        case SENS_CONN_PAIRED: return "PAIRED";
    }
    return "?";
}

static void on_channel_synced(uint8_t channel, const uint8_t *hub_mac)
{
    (void)channel; (void)hub_mac;
    if (s_conn_state == SENS_CONN_ORPHAN) {
        s_conn_state = SENS_CONN_FOUND;
        ESP_LOGI(TAG, "[STATE] -> %s", conn_state_name(s_conn_state));
    }
    set_led(LED_PATTERN_BLINK_FAST);
}

static const uint8_t s_broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* CAM의 send_cb와 동일 원칙 — 생존판정은 WAKE_HELLO_SENS의 reliable 요청/응답이 전담,
 * 물리계층 ACK는 광고 송출 완료 통보(채널싱크용)로만 씀 */
static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    if (info && info->des_addr && memcmp(info->des_addr, s_broadcast_mac, sizeof(s_broadcast_mac)) == 0) {
        if (status == ESP_NOW_SEND_SUCCESS) {
            esp_now_channelsync_notify_advertise_send_done();
        }
    }
}

static void resolve_name(void)
{
    /* 2026-09-05(사용자 지시: "캠에서 구현된 걸 가져다 써야되") — CAM처럼 항상 STA만. 배터리
     * 딥슬립 노드가 자기 AP를 호스팅할 이유가 없음(짧은 웨이크 창 안에 붙을 클라이언트가
     * 없음) — 예전 SENS_WIFI_MODE_STA/AP 선택지는 wifi_dashboard.c(제거됨) 전용이었음 */
    esp_wifi_get_mac(WIFI_IF_STA, s_mac);

#if defined(CONFIG_SENS_NODE_NAME)
    if (strlen(CONFIG_SENS_NODE_NAME) > 0) {
        snprintf(s_name, sizeof(s_name), "%s", CONFIG_SENS_NODE_NAME);
        return;
    }
#endif
    snprintf(s_name, sizeof(s_name), "Sens-%02X%02X", s_mac[4], s_mac[5]);
}

/* CAM의 esp_now_cam_reconnect() 폴백과 동일 — 이미 시작했으면 재개, 아니면 최초 시작
 * (esp_now_cam.c의 s_channelsync_initialized와 동일 패턴). 스윕 재시작 기록도 캠과 동일하게
 * 여기서 무효화(esp_now_node_note_scan_restarted(), app_main 소유 — cam_node_note_scan_
 * restarted()와 동일 호출 위치) */
static void start_or_resume_scan(void)
{
    if (!s_channelsync_initialized) {
        s_channelsync_initialized = true;
        esp_now_channelsync_init(s_name, s_mac, on_channel_synced);
        esp_now_channelsync_set_should_advertise_cb(should_advertise);
    } else {
        esp_now_channelsync_resume_scan();
    }
    esp_now_node_note_scan_restarted();
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 2) return;
    uint8_t msg_type = data[1];

    esp_now_channelsync_on_recv(info, msg_type, data, len);
    esp_now_reliable_on_recv(msg_type, info ? info->src_addr : NULL, data, len);

    if (msg_type == ESP_NOW_MSG_UNPAIR) {
        if (s_conn_state != SENS_CONN_PAIRED || len < (int)sizeof(esp_now_unpair_t)) return;
        if (memcmp(info->src_addr, s_hub_mac, sizeof(s_hub_mac)) != 0) return;
        ESP_LOGI(TAG, "Cntl이 연결 해제함");
        s_conn_state = SENS_CONN_ORPHAN;
        s_wake_hub_known = false;
        ESP_LOGI(TAG, "[STATE] -> %s (unpair)", conn_state_name(s_conn_state));
        set_led(LED_PATTERN_BLINK_FAST);
        esp_now_unpair_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_UNPAIR_ACK };
        esp_now_send(info->src_addr, (const uint8_t *)&ack, sizeof(ack));
        start_or_resume_scan();
        esp_now_node_signal_recheck();
        return;
    }

    if (msg_type == ESP_NOW_MSG_CASK_WORK_NONE) {
        if (s_conn_state != SENS_CONN_PAIRED || len < (int)sizeof(esp_now_cask_work_none_t)) return;
        esp_now_cask_work_none_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_CASK_WORK_NONE_ACK };
        esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
        return;
    }

    if (msg_type == ESP_NOW_MSG_SENS_CONFIG_SET) {
        if (s_conn_state != SENS_CONN_PAIRED || len < (int)sizeof(esp_now_sens_config_t)) return;
        const esp_now_sens_config_t *cfg = (const esp_now_sens_config_t *)data;
        if (cfg->sample_interval_sec != 0) {
            s_sample_interval_sec = cfg->sample_interval_sec;
            /* cam_node_set_response_interval_sec()과 동일 위치/이유 — 실제 샘플주기가
             * 확정되는 순간 RWDT 예산을 그 값 기준으로 재무장 */
            rwdt_guard_arm(s_sample_interval_sec + CONFIG_SENS_DEEPSLEEP_AWAKE_MARGIN_SEC);
        }
        if (cfg->unix_time != 0) {
            struct timeval tv = { .tv_sec = (time_t)cfg->unix_time, .tv_usec = 0 };
            settimeofday(&tv, NULL);
        }
        ESP_LOGI(TAG, "SENS_CONFIG_SET 수신: sample_interval_sec=%u", (unsigned)s_sample_interval_sec);
        esp_now_sens_config_ack_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_SENS_CONFIG_ACK, .success = 1 };
        esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
        return;
    }

    if (msg_type == ESP_NOW_MSG_SLEEP_NOW) {
        if (s_conn_state != SENS_CONN_PAIRED || len < (int)sizeof(esp_now_sleep_now_t)) return;
        const esp_now_sleep_now_t *msg = (const esp_now_sleep_now_t *)data;
        s_last_sleep_sec = msg->sleep_sec;
        ESP_LOGI(TAG, "SLEEP_NOW 수신(sleep_sec=%u)", (unsigned)msg->sleep_sec);
        esp_now_sleep_now_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_SLEEP_NOW_ACK };
        esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
        /* 캠의 cam_node_note_sleep_now_requested()와 동일 — app_main의 이벤트기반 대기 루프를
         * 즉시 깨움(sens_deep_sleep_node.c 구현) */
        esp_now_node_note_sleep_now_requested(msg->sleep_sec);
        return;
    }

    /* 폴백 경로(전체 채널스캔 이후 정식 페어링) — WAKE_HELLO_SENS 유니캐스트 패스트패스가
     * 아직 없거나(최초 페어링 전) 실패한 경우, 예전과 동일하게 광고->PAIR_REQUEST로 붙음 */
    if (s_conn_state == SENS_CONN_PAIRED || msg_type != ESP_NOW_MSG_PAIR_REQUEST) return;
    if (len < (int)sizeof(esp_now_pair_request_t)) return;
    const esp_now_pair_request_t *req = (const esp_now_pair_request_t *)data;

    memcpy(s_hub_mac, req->hub_mac, sizeof(s_hub_mac));

    s_conn_state = SENS_CONN_PAIRED;
    ESP_LOGI(TAG, "[STATE] -> %s (pair_request)", conn_state_name(s_conn_state));
    esp_now_channelsync_notify_paired();
    set_led(LED_PATTERN_HEARTBEAT);

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_hub_mac, sizeof(peer.peer_addr));
    peer.ifidx = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(s_hub_mac)) {
        esp_now_add_peer(&peer);
    }

    /* 2026-09-05(캠의 순서 버그 수정과 동일 조치) — CNTL이 PAIR_ACK 수신 직후 곧바로
     * CONFIG+SLEEP_NOW를 보내므로, 전송 직전에 미리 대기 상태를 깨끗하게 함 */
    esp_now_node_reset_sleep_now_state();
    esp_now_pair_ack_t ack = {
        .version     = ESP_NOW_LINK_VERSION,
        .msg_type    = ESP_NOW_MSG_PAIR_ACK,
        .sensor_kind = (uint8_t)s_sensor_kind,
        .chan_count  = s_chan_count,
    };
    memcpy(ack.node_mac, s_mac, sizeof(ack.node_mac));
    memcpy(ack.chan_type, s_chan_type, s_chan_count);
    esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "페어링됨: hub " MACSTR ", PAIR_ACK %s", MAC2STR(s_hub_mac), esp_err_to_name(err));

    /* "졸업" — 다음 딥슬립 웨이크가 곧장 유니캐스트로 재연결(WAKE_HELLO_SENS 패스트패스)
     * 시도할 수 있게 미리 기억(esp_now_cam.c와 동일 이유) */
    uint8_t current_channel = 0;
    wifi_second_chan_t second_chan;
    esp_wifi_get_channel(&current_channel, &second_chan);
    memcpy(s_wake_hub_mac, s_hub_mac, sizeof(s_wake_hub_mac));
    s_wake_hub_channel = current_channel;
    s_wake_hub_known   = true;

    /* 캠의 esp_now_cam.c:162-164(페어링 상태 변화)와 동일 — app_main의 이벤트기반 대기
     * 루프를 즉시 깨움(스윕 끝나기 전에 이미 페어링됐을 수 있으므로) */
    esp_now_node_signal_recheck();
}

bool esp_now_node_report_reading(uint8_t chan_count, const uint8_t *chan_ok,
                                  const float *chan_val, uint32_t measurement_id,
                                  uint16_t battery_adc_raw, uint16_t battery_mv)
{
    if (chan_count > ESP_NOW_MAX_CHANNELS) chan_count = ESP_NOW_MAX_CHANNELS;

    /* CAM의 esp_now_cam_try_wake_hello_fast_path()/esp_now_cam_reconnect() 이식 — 이 함수
     * 하나가 그 둘의 역할을 겸함(캠도 reconnect()가 fast path를 감싸는 동일 구조). 아직
     * PAIRED가 아니면(딥슬립 후 매 부팅은 항상 이 상태로 시작) 알려진 허브(RTC 기억)가
     * 있을 때만 광고 없이 곧장 유니캐스트로 시도 — 성공하면 아래에서 PAIRED로 승격됨.
     * 알려진 허브 자체가 없으면(최초 페어링 전) 보낼 것도 없이 그냥 버림 — 채널스캔은
     * esp_now_node_init()이 이미 시작해뒀음. 호출부(sens_deep_sleep_node.c)가
     * esp_now_node_init() 직후 이 함수를 한 번 동기 호출하는 것 자체가 캠의
     * "esp_now_cam_init() 끝에서 esp_now_cam_reconnect() 호출"과 동일 타이밍 —
     * 그래서 그 뒤에 오는 esp_now_node_is_paired() 확인이 항상 정확함(대기 루프가
     * 절대 만족 안 되는 조건을 기다리는 문제 자체가 없어짐) */
    if (s_conn_state != SENS_CONN_PAIRED) {
        if (!s_wake_hub_known) {
            start_or_resume_scan();  /* 최초 페어링 전 — 스캔이 아직 안 돌고 있으면 시작 */
            return false;
        }
        esp_wifi_set_channel(s_wake_hub_channel, WIFI_SECOND_CHAN_NONE);
        memcpy(s_hub_mac, s_wake_hub_mac, sizeof(s_hub_mac));
        esp_now_peer_info_t peer = { 0 };
        memcpy(peer.peer_addr, s_hub_mac, sizeof(peer.peer_addr));
        peer.ifidx = WIFI_IF_STA;
        peer.channel = 0;
        peer.encrypt = false;
        if (!esp_now_is_peer_exist(s_hub_mac)) esp_now_add_peer(&peer);
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    esp_now_wake_hello_sens_t hello = {
        .version               = ESP_NOW_LINK_VERSION,
        .msg_type              = ESP_NOW_MSG_WAKE_HELLO_SENS,
        .wake_reason           = s_wake_reason,
        .awake_uptime_ms       = now_ms - s_last_report_ms,
        .sleep_interval_sec    = s_sample_interval_sec,
        .actual_last_sleep_sec = s_last_actual_sleep_sec,
        .battery_adc_raw       = battery_adc_raw,
        .battery_mv            = battery_mv,
        .measurement_id        = measurement_id,
    };
    memcpy(hello.chan_ok,  chan_ok,  chan_count * sizeof(chan_ok[0]));
    memcpy(hello.chan_val, chan_val, chan_count * sizeof(chan_val[0]));
    s_last_report_ms = now_ms;

    /* 캠의 순서 버그 수정과 동일 조치(esp_now_cam_try_wake_hello_fast_path() 참고) — 응답이
     * 도착하기 전에 미리 대기 상태를 깨끗하게 함 */
    esp_now_node_reset_sleep_now_state();

    static const uint8_t s_wake_hello_sens_ack_types[] = { ESP_NOW_MSG_WAKE_HELLO_SENS_ACK };
    esp_now_wake_hello_sens_ack_t ack;
    esp_err_t err = esp_now_reliable_request(s_hub_mac, &hello, sizeof(hello),
                                              s_wake_hello_sens_ack_types, 1,
                                              200, 3,  /* CAM의 WAKE_HELLO와 동일 값(esp_now_cam.c
                                                          2026-09-05 수정과 짝) */
                                              &ack, sizeof(ack), NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WAKE_HELLO_SENS 무응답(3회) — 폴백 스캔으로 전환");
        s_conn_state = SENS_CONN_ORPHAN;
        ESP_LOGI(TAG, "[STATE] -> %s (wake_hello_fail)", conn_state_name(s_conn_state));
        set_led(LED_PATTERN_BLINK_FAST);
        start_or_resume_scan();
        return false;
    }

    /* notify_paired()는 channelsync가 아직 init 전이어도 안전(내부 null-check, esp_now_cam.c와
     * 동일 관례) */
    esp_now_channelsync_notify_paired();
    s_conn_state = SENS_CONN_PAIRED;
    ESP_LOGI(TAG, "[STATE] -> %s (wake_hello)", conn_state_name(s_conn_state));
    set_led(LED_PATTERN_HEARTBEAT);
    return true;
}

uint32_t esp_now_node_get_last_sleep_sec(void)
{
    return s_last_sleep_sec;
}

uint32_t esp_now_node_get_sample_interval_sec(void)
{
    return s_sample_interval_sec;
}

void esp_now_node_set_status_led(gpio_num_t pin)
{
    s_led_pin = pin;
    status_led_init(pin);
}

void esp_now_node_init(sensor_kind_t sensor_kind, uint8_t chan_count, const uint8_t *chan_type)
{
    s_sensor_kind = sensor_kind;
    s_chan_count  = (chan_count > ESP_NOW_MAX_CHANNELS) ? ESP_NOW_MAX_CHANNELS : chan_count;
    memcpy(s_chan_type, chan_type, s_chan_count);

    resolve_name();
    ESP_LOGI(TAG, "노드 이름: %s (MAC " MACSTR ")", s_name, MAC2STR(s_mac));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    /* 2026-09-05 — 허브를 몰라서(최초 페어링 전) 스캔부터 시작해야 하는 경우만 여기서
     * 미리 시작. 허브를 이미 알면(딥슬립 복귀) 여기선 아무 것도 안 함 — 호출부
     * (sens_deep_sleep_node.c)가 이 함수 직후 esp_now_node_report_reading()을 동기 호출해서
     * 캠의 esp_now_cam_init()->esp_now_cam_reconnect() 순서를 그대로 재현함(위 파일 헤더
     * 주석 참고) */
    if (!s_wake_hub_known) {
        start_or_resume_scan();
    }
    set_led(LED_PATTERN_BLINK_FAST);
}

const char *esp_now_node_get_name(void)
{
    return s_name;
}

bool esp_now_node_is_paired(void)
{
    return s_conn_state == SENS_CONN_PAIRED;
}
