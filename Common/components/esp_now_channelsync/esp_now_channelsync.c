#include "esp_now_channelsync.h"
#include "esp_now_link.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "esp_now_chsync";

/* 2026-08-23 — 공유 상태(s_scan_channel/s_synced/s_pong_pending/s_ping_fail_count/타이머들)
 * 보호용 뮤텍스. esp_timer 콜백(scan/ping/rest)은 전부 esp_timer 전용 태스크 하나에서
 * 순차 실행되지만, esp_now_channelsync_on_recv()는 ESP-NOW 수신 태스크(별도 태스크,
 * ESP32-S3는 듀얼코어라 진짜 동시 실행 가능)에서 돈다 — 실기에서 확인된 레이스(예:
 * ADVERTISE_ACK 처리 도중 scan_timer_cb가 끼어들어 채널을 또 옮기는 것) 근본 원인.
 * enter_unsynced()/send_advertise_on_current_channel() 등 내부 헬퍼는 락을 직접 잡지
 * 않음 — 전부 이미 락을 쥔 호출부(아래 각 진입점)에서만 불림 */
static SemaphoreHandle_t s_state_mutex = NULL;

/* 2026-08-23 — 부가 이벤트 훅(esp_now_channelsync.h 참고, 하드웨어 전용 컴포넌트와의
 * 결합을 피하기 위한 느슨한 연결) */
static esp_now_channelsync_event_cb_t s_on_channel_scanned = NULL;
static esp_now_channelsync_event_cb_t s_on_advertise_sent  = NULL;
static esp_now_channelsync_event_cb_t s_on_scan_sweep_done = NULL;
static esp_now_channelsync_event_cb_t s_on_ping_sent       = NULL;
static esp_now_channelsync_event_cb_t s_on_pong_received   = NULL;

/* 2026-08-23 — 광고 전송 직전 게이트(esp_now_channelsync.h 참고, 방어적 이중 확인) */
static esp_now_channelsync_should_advertise_cb_t s_should_advertise_cb = NULL;

void esp_now_channelsync_set_should_advertise_cb(esp_now_channelsync_should_advertise_cb_t cb)
{
    s_should_advertise_cb = cb;
}

void esp_now_channelsync_set_event_hooks(esp_now_channelsync_event_cb_t on_channel_scanned,
                                          esp_now_channelsync_event_cb_t on_advertise_sent,
                                          esp_now_channelsync_event_cb_t on_scan_sweep_done,
                                          esp_now_channelsync_event_cb_t on_ping_sent,
                                          esp_now_channelsync_event_cb_t on_pong_received)
{
    s_on_channel_scanned = on_channel_scanned;
    s_on_advertise_sent  = on_advertise_sent;
    s_on_scan_sweep_done = on_scan_sweep_done;
    s_on_ping_sent       = on_ping_sent;
    s_on_pong_received   = on_pong_received;
}

#define SCAN_DWELL_US        (300 * 1000)
#define SCAN_CHANNEL_MIN     1
#define SCAN_CHANNEL_MAX     13

/* 마지막으로 동기화 성공했던 채널(2026-08-10) — RTC 슬로우메모리(딥슬립 중에도 유지됨,
 * 전원이 완전히 끊기거나 RWDT의 RESET_RTC 액션이 아닌 한 살아있음)에 저장해뒀다가 다음
 * 웨이크 때 그 채널부터 먼저 시도. CAM Deep Sleep 전환 후 매 웨이크마다 채널스캔을 처음부터
 * 다시 해야 했는데, 공유기(허브의 실제 채널)는 보통 세션 내내 안 바뀌므로 대부분의 사이클은
 * 이 값 하나로 거의 즉시 동기화됨 — 값이 틀렸으면(채널이 실제로 바뀌었으면) 기존처럼
 * 전체 스캔으로 자연히 폴백됨(증가/랩어라운드 로직이 시작점과 무관하게 전체 범위를 도니까).
 * 설정값이 아니라 순수 성능 힌트라 잘못돼도 안전(최악의 경우 예전과 동일한 전체 스캔) */
static RTC_DATA_ATTR uint8_t s_last_synced_channel = 0;  /* 0 = 유효한 값 없음(최초 부팅) */

/* 기본 500ms마다 PING 왕복 확인, 연속 3회 무응답이면 동기화 끊김으로 판정. 매 라운드는
 * "직전 PING의 PONG이 왔는가"만 검사하고 바로 다음 PING을 보내는 단순한 구조라 별도의 응답
 * 타임아웃 타이머가 필요 없음(위 헤더의 설계 원칙 참고 — 이 루프는 다른 무엇으로도 멈추지
 * 않음).
 * 2026-08-08 — 배터리 노드(CAM)의 절전 설정(esp_now_cam_config_t.response_interval_sec)에
 * 맞춰 이 주기를 늘릴 수 있게 상수를 변수로 바꿈(esp_now_channelsync_set_ping_interval_ms
 * 참고) — 상시전원 노드는 기본값 그대로 둬도 됨 */
#define PING_INTERVAL_US_DEFAULT (500 * 1000)
#define PING_FAIL_THRESHOLD  3

static uint32_t s_ping_interval_us = PING_INTERVAL_US_DEFAULT;

static const uint8_t s_broadcast_addr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static char    s_node_name[ESP_NOW_LINK_NAME_LEN] = "";
static uint8_t s_node_mac[6] = { 0 };

static esp_now_channelsync_on_synced_cb_t    s_on_synced    = NULL;
static esp_now_channelsync_on_lost_sync_cb_t s_on_lost_sync = NULL;

static volatile bool s_synced       = false;
static uint8_t        s_scan_channel = SCAN_CHANNEL_MIN;
static uint8_t         s_hub_mac[6]  = { 0 };

/* 2026-08-23 — Cntl의 부팅 세션 식별자(esp_now_link.h hub_boot_id 필드 참고). 매
 * ADVERTISE_ACK/PONG마다 같이 옴 — 값이 바뀌면 "Cntl이 리붓해서 날 잊었다"는 뜻이므로,
 * HUB_RESET 브로드캐스트(수신 불확실, 재전송 없음)를 기다리지 않고 이미 신뢰성 있는
 * PING/PONG 왕복만으로 능동적으로 재광고를 트리거함. CAML에서 검증 후 이식
 * (project_caml_bat_en_latch_missing_resolved 메모리 참고) */
static uint32_t s_hub_boot_id       = 0;
static bool     s_hub_boot_id_known = false;

static esp_timer_handle_t s_scan_timer = NULL;  /* UNSYNCED에서만 동작 */
static esp_timer_handle_t s_ping_timer = NULL;  /* SYNCED에서만 동작 */
static volatile bool s_pong_pending    = false; /* 직전 PING에 대한 PONG을 아직 못 받음 */
static int            s_ping_fail_count = 0;

/* 2026-08-23 — 스윕 백오프(CAML에서 라이트슬립 도입에 맞춰 설계, 딥슬립 복귀 후에도 유효한
 * 일반 절전 설계라 함께 이식). 미페어링 상태에서 포기하는 대신, 채널 스윕(1~13번 한 바퀴,
 * 300ms×13=3.9초) 자체는 항상 빠르게 유지하고("사람이 기다릴 때 늦게 찾으면 안 됨"), 대신
 * 스윕과 스윕 사이 쉬는 시간을 검색이 길어질수록 늘려서 절전함:
 *   0~1분: 안 쉬고 연속 스윕
 *   1분~11분: 스윕 사이 10초 휴식
 *   11분 이후 계속: 스윕 사이 30초 휴식
 * s_rest_timer는 원샷(one-shot) — 스윕 한 바퀴(채널 랩어라운드) 끝날 때만 켜고, 끝나면
 * scan_timer를 다시 주기 재개시킴 */
static esp_timer_handle_t s_rest_timer      = NULL;
static int64_t            s_unsynced_start_us = 0;

#define BACKOFF_CONTINUOUS_DURATION_US   (60LL * 1000 * 1000)         /* 0~1분: 연속 스윕 */
#define BACKOFF_MID_DURATION_US          (10LL * 60 * 1000 * 1000)    /* 다음 10분 */
#define BACKOFF_MID_REST_US              (10LL * 1000 * 1000)         /* 스윕 사이 10초 */
#define BACKOFF_LONG_REST_US             (30LL * 1000 * 1000)         /* 그 이후 계속 30초 */

/* 리턴 0 = 쉬지 말고 바로 다음 스윕(연속 구간), 그 외 = 이만큼(us) 쉬고 다음 스윕 */
static int64_t sweep_rest_us(void)
{
    int64_t elapsed_us = esp_timer_get_time() - s_unsynced_start_us;
    if (elapsed_us < BACKOFF_CONTINUOUS_DURATION_US) return 0;
    if (elapsed_us < BACKOFF_CONTINUOUS_DURATION_US + BACKOFF_MID_DURATION_US) return BACKOFF_MID_REST_US;
    return BACKOFF_LONG_REST_US;
}

static void add_peer_if_needed(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

/* 2026-08-23(사용자 지시: "분리를 하더라도 인텔리전스는 한 곳에 있어야지") — "허용됐는가"를
 * 실제로 계산하는 로직은 이 함수 하나뿐. scan_timer_cb/enter_unsynced는 각자 자기 흐름에서
 * 이 함수를 한 번만 불러서 판단하고, 그 결과를 그대로 씀(같은 시점에 또 물어보지 않음) —
 * 콜백이 없으면(등록 안 됨) 기존 동작대로 항상 허용 */
static bool should_advertise_now(void)
{
    return !s_should_advertise_cb || s_should_advertise_cb();
}

/* scan_timer_cb와 enter_unsynced 둘 다 씀(2026-08-10, 아래 참고) — 지금 s_scan_channel에서
 * 광고 1통 보냄 */
/* 2026-08-23(사용자 지시) — 판단(should_advertise)과 실행(이 함수)은 분리하되, 판단은 호출부에서
 * "한 번만" 하고 여기엔 그 결과를 물어보지 않고 그대로 실행만 함(같은 판단을 여러 번 하면
 * 안 됨 — 흐름과 실행 사이에 시차가 생겨 로그/소리와 실제 수행이 어긋날 여지가 생김).
 * 그래서 이 함수는 호출됐다는 것 자체가 "이미 허용됐다"는 뜻 — 호출부(scan_timer_cb/
 * enter_unsynced)가 그 판단 결과를 보고 부를지 말지 결정함 */
static void send_advertise_on_current_channel(void)
{
    /* 2026-08-24(사용자 지시: "호출쪽이 아니라 반드시 실제 광고 송출하는 함수에서 체크해야
     * 되") — 호출부(enter_unsynced/scan_timer_cb)가 미리 계산해둔 판단값을 그대로 믿지 않고,
     * 진짜 전송 직전인 여기서 상태를 다시 확인함. 이유: enter_unsynced()는 PING 실패/PONG
     * boot_id 불일치로 인한 lost_sync 콜백보다 먼저 불리는데, 그 콜백(esp_now_cam.c의
     * on_channel_lost_sync)이 상태변수를 ORPHAN으로 바꾸는 건 그 다음 줄이라 — enter_unsynced()
     * 시점엔 상태가 아직 낡은 값(PAIRED)이라 판단이 어긋날 수 있었음(실기로 발견). 여기서
     * 다시 확인하면 호출 시점과 무관하게 항상 최신 상태로 게이트됨 */
    if (!should_advertise_now()) {
        return;
    }

    /* 채널 스캔 훅 — 원래 scan_timer_cb/enter_unsynced에 따로 있었는데, 이 함수가 항상
     * 그 직후에 불려서 사실상 같은 순간이므로 여기로 합침(사용자 지시) */
    ESP_LOGI(TAG, "채널 스캔 (CH%d)", s_scan_channel);
    if (s_on_channel_scanned) s_on_channel_scanned();

    esp_now_advertise_t msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_ADVERTISE,
    };
    memcpy(msg.name, s_node_name, sizeof(msg.name));
    memcpy(msg.mac, s_node_mac, sizeof(msg.mac));
    esp_err_t err = esp_now_send(s_broadcast_addr, (const uint8_t *)&msg, sizeof(msg));
    /* 2026-08-24(사용자 지시: "실제 송출될 때만 소리가 나도록") — esp_now_send()의 동기
     * 리턴값(err)은 "로컬 큐에 접수됐다"는 뜻일 뿐 진짜 무선 송출 완료가 아님(그건 비동기
     * send_cb로 나중에 옴 — esp_now_channelsync_notify_advertise_send_done() 참고). 여기선
     * 큐잉 자체가 실패한 경우만 경고 로그 — "전송됨" 로그/소리는 절대 여기서 안 냄 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADVERTISE 전송 실패 (큐잉, CH%d): %s", s_scan_channel, esp_err_to_name(err));
    }
}

static void enter_unsynced(void)
{
    s_synced = false;
    if (s_ping_timer) esp_timer_stop(s_ping_timer);
    if (s_rest_timer) esp_timer_stop(s_rest_timer);  /* 이전 검색의 휴식이 남아있으면 정리 */
    s_unsynced_start_us = esp_timer_get_time();       /* 스윕 백오프 기준시각 리셋 */
    s_pong_pending    = false;
    s_ping_fail_count = 0;

    /* 2026-08-10 — 마지막으로 성공했던 채널이 있으면 거기서 시작(위 s_last_synced_channel
     * 주석 참고). 증가/랩어라운드(scan_timer_cb)는 시작점과 무관하게 전체 1~13 범위를
     * 도니까, 값이 틀려도(허브가 채널을 바꿨어도) 안전하게 전체 스캔으로 이어짐 */
    s_scan_channel = (s_last_synced_channel >= SCAN_CHANNEL_MIN && s_last_synced_channel <= SCAN_CHANNEL_MAX)
                      ? s_last_synced_channel : SCAN_CHANNEL_MIN;
    esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);

    /* 2026-08-24(사용자 지시로 변경) — 판단을 여기서 미리 계산해 아래 두 곳에 나눠 쓰던 방식은
     * PING 실패 콜백 순서 문제로 낡은 값을 쓰는 버그가 있었음(위 send_advertise_on_current_channel()
     * 주석 참고 — 그 함수가 실제 전송 직전에 항상 새로 확인함). 이제 그 함수 자신이 매번
     * 신선하게 게이트하므로, 여기서는 무조건 시도/무조건 타이머 재시작해도 안전함 — 상태가
     * 허용 안 하면 그 함수가 알아서 거부하고, scan_timer_cb 맨 위 자가치유 게이트가 다음 틱에
     * 스스로 멈춤. 예전처럼 여기서 미리 막아버리면 그 뒤로 아무도 타이머를 다시 안 돌려서
     * 영구 정지하는 문제가 있었음(실기로 발견) */
    {
        /* 2026-08-10 — 시작 채널에서도 즉시 광고 1번 보냄(CAM Deep Sleep 실기 테스트에서 발견:
         * 예전엔 scan_timer_cb가 "채널 증가 후 광고"만 해서, 시작 채널(SCAN_CHANNEL_MIN=1)
         * 자체에서는 스캔이 한 바퀴(최대 3.9초) 다 돌아야만 광고가 나갔음 — 허브가 하필 채널 1에
         * 있으면 매번 최악의 경우를 맞는 구조적 편향이 있었음. 이 즉시 전송으로 그 편향을 없앰:
         * 허브가 지금 CAM이 있는 채널에 이미 있으면 거의 즉시 찾고, 아니면 기존처럼 스캔이
         * 이어감(최악값은 그대로 유지, 더 나빠지지 않음) */
        send_advertise_on_current_channel();
    }

    if (s_scan_timer) {
        esp_timer_stop(s_scan_timer);
        esp_timer_start_periodic(s_scan_timer, SCAN_DWELL_US);
    }
}

static void scan_timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    /* 2026-08-23(사용자 지시) — 방어적 자가치유: 이 틱 자체가 "안 돌아야 하는 상태"에서
     * 불렸다면(예: notify_paired()의 esp_timer_stop이 무슨 이유로 안 먹혔거나 레이스로
     * 이미 지나간 뒤였던 경우) 여기서 즉시 스스로 멈추고 아무 것도(채널훅/광고) 안 함 —
     * "상태가 출력을 직접 결정"을 이 진입점에서도 관철 */
    if (!should_advertise_now()) {
        if (s_scan_timer) esp_timer_stop(s_scan_timer);
        xSemaphoreGive(s_state_mutex);
        return;
    }

    /* 채널 이동을 먼저 하고 그 채널에서 광고를 보냄(2026-08-04 — 순서를 반대로 하면, 광고를
     * 보낸 그 채널에서 상대의 응답을 들을 시간이 없어서 매번 다음 채널로 잘못 락되는 레이스가
     * 있었음, 실기로 재현/수정 검증됨). 이 채널에 다음 타이머까지 계속 머무르므로 응답이
     * 도착할 시간이 충분함 */
    s_scan_channel++;
    bool wrapped = false;
    if (s_scan_channel > SCAN_CHANNEL_MAX) {
        s_scan_channel = SCAN_CHANNEL_MIN;
        wrapped = true;
    }
    esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);
    /* 2026-08-23(사용자 지시) — 채널스캔 훅/로그는 send_advertise_on_current_channel() 안으로
     * 합쳐짐 — 여기선 그 함수를 부르기만 함(로그와 실제 수행이 어긋나지 않도록 한 곳에만 둠) */
    send_advertise_on_current_channel();

    if (!wrapped) {
        xSemaphoreGive(s_state_mutex);
        return;
    }

    /* 2026-08-23 — 훅은 "쉬기로 정했을 때"가 아니라 "한 바퀴 다 돌았을 때" 무조건 불러야 함.
     * CAM의 Deep Sleep 모델(esp_now_cam.c의 s_sweep_completed)이 이 훅으로 "스윕 끝났으니
     * 자도 됨"을 판정하는데, 아래 백오프(sweep_rest_us==0 구간, 연속 스윕 60초)에 가려
     * 훅이 안 불리면 그 60초 동안 캠이 잠들 판단 자체를 못 해서 계속 깨있는 버그가 났었음(실기로
     * 확인: 1분 깨있고 3초 자고 반복). 아래 백오프 로직 자체는 CAML의 계속 켜있는 채로 도는
     * 모델에는 여전히 유효하므로 그대로 둠 — 훅 호출 위치만 분리 */
    if (s_on_scan_sweep_done) s_on_scan_sweep_done();

    /* 스윕 백오프: 랩어라운드 이후에도 계속 켜있을 경우(CAML)에만 쉴지 판단. 스윕 중간엔
     * 원래대로 계속 300ms 주기 유지(사람이 기다릴 때 늦게 찾지 않도록) */
    int64_t rest_us = sweep_rest_us();
    if (rest_us == 0) {
        xSemaphoreGive(s_state_mutex);
        return;  /* 연속 스윕 구간 — 안 쉬고 계속 */
    }
    ESP_LOGI(TAG, "SCAN 스윕 완료 — %lld us 휴식", (long long)rest_us);
    esp_timer_stop(s_scan_timer);
    if (s_rest_timer) esp_timer_start_once(s_rest_timer, rest_us);
    xSemaphoreGive(s_state_mutex);
}

/* 스윕 사이 휴식 종료 시 호출(원샷). 채널/광고는 손 안 댐 — 방금 랩어라운드 시점에 이미
 * 시작채널(MIN)에서 광고를 보냈고 그 채널에 계속 머물러 있었으므로, 늦게 온 응답도 그대로
 * 들을 수 있음. 그냥 주기타이머만 재개하면 다음 틱부터 다음 채널로 스윕 이어감 */
static void rest_timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    /* 2026-08-24 — enter_unsynced()와 같은 이유로 무조건 재개(실제 게이트는
     * send_advertise_on_current_channel() 자신이 매번 새로 함) */
    if (s_scan_timer) {
        esp_timer_start_periodic(s_scan_timer, SCAN_DWELL_US);
    }
    xSemaphoreGive(s_state_mutex);
}

static void ping_timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_pong_pending) {
        s_ping_fail_count++;
        ESP_LOGW(TAG, "PING 무응답(연속 %d회)", s_ping_fail_count);
#if 0  /* 2026-08-24(사용자 지시, 임시 테스트) — PING 실패로 인한 재스캔 트리거만 잠깐
        * 막아서, "페어링 후 무한 광고"가 이 경로 때문인지 격리 확인. 실패 로그(위)는
        * 그대로 남김 — 진짜로 실패하는지는 계속 관찰 가능. 확인 끝나면 원복해야 함 */
        if (s_ping_fail_count >= PING_FAIL_THRESHOLD) {
            ESP_LOGW(TAG, "채널 동기화 끊김 — 재스캔 시작");
            enter_unsynced();
            if (s_on_lost_sync) s_on_lost_sync();
            xSemaphoreGive(s_state_mutex);
            return;  /* enter_unsynced()가 이미 ping_timer를 멈춤 */
        }
#endif
    }
    esp_now_channel_ping_t ping = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_CHANNEL_PING,
    };
    esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&ping, sizeof(ping));
    if (s_on_ping_sent) s_on_ping_sent();
    if (err == ESP_ERR_ESPNOW_NO_MEM) {
        /* 로컬 송신큐 포화 — PING이 아예 안 나간 것뿐, 채널/상대 문제와 무관함(2026-08-05,
         * 실기에서 발견: 사진 청크를 한꺼번에 많이 보내는 중에 이 로컬 포화가 PING까지
         * 밀어내서 "동기화 끊김"으로 오판 — 실제로 CAM이 전송 중간에 채널을 이탈해서
         * Cntl의 DONE_ACK가 도달 못 하고 3006으로 이어짐). "무응답"과 다른 원인이므로
         * 실패로 안 세고 이번 사이클은 그냥 건너뜀(pending도 안 켬 — 다음 틱에 새로 시도) */
        s_pong_pending = false;
        xSemaphoreGive(s_state_mutex);
        return;
    }
    s_pong_pending = true;
    xSemaphoreGive(s_state_mutex);
}

void esp_now_channelsync_init(const char *node_name, const uint8_t *node_mac,
                               esp_now_channelsync_on_synced_cb_t on_synced,
                               esp_now_channelsync_on_lost_sync_cb_t on_lost_sync)
{
    strncpy(s_node_name, node_name, sizeof(s_node_name) - 1);
    memcpy(s_node_mac, node_mac, sizeof(s_node_mac));
    s_on_synced    = on_synced;
    s_on_lost_sync = on_lost_sync;

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_broadcast_addr, sizeof(peer.peer_addr));
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(s_broadcast_addr)) esp_now_add_peer(&peer);

    /* 2026-08-22(CAML에서 검증) — skip_unhandled_events는 여기 셋(scan/ping/rest)엔 쓰면 안
     * 됨: ESP-IDF 소스로 확인(esp_timer.h의 esp_timer_get_next_alarm_for_wake_up() 설명 +
     * esp_pm/pm_impl.c가 실제로 그 함수로 자동 라이트슬립 기상 시각을 계산) — 이 플래그를
     * 켜면 "가끔 늦게 불려도 됨"이 아니라 "이 타이머의 예정 시각은 라이트슬립을 깨울 이유에서
     * 아예 제외"가 됨. 이 셋은 전부 자기 스케줄대로 CPU를 깨워야만 역할(광고 송신/PING 송신/
     * 스윕 재개)을 하므로 붙이면 배터리(라이트슬립 실제 진입 시)에서 광고 자체가 멈추는
     * 버그가 됨(실기로 확인) — 딥슬립 복귀와 무관하게 앞으로도 유효한 주의사항이라 주석 유지 */
    const esp_timer_create_args_t scan_args = { .callback = scan_timer_cb, .name = "chsync_scan" };
    esp_timer_create(&scan_args, &s_scan_timer);

    const esp_timer_create_args_t ping_args = { .callback = ping_timer_cb, .name = "chsync_ping" };
    esp_timer_create(&ping_args, &s_ping_timer);

    const esp_timer_create_args_t rest_args = { .callback = rest_timer_cb, .name = "chsync_rest" };
    esp_timer_create(&rest_args, &s_rest_timer);

    s_state_mutex = xSemaphoreCreateMutex();
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    enter_unsynced();
    xSemaphoreGive(s_state_mutex);
}

void esp_now_channelsync_on_recv(const esp_now_recv_info_t *info, uint8_t msg_type,
                                  const uint8_t *data, int len)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (!s_synced && msg_type == ESP_NOW_MSG_ADVERTISE_ACK) {
        if (len < (int)sizeof(esp_now_advertise_ack_t)) {
            ESP_LOGW(TAG, "ADVERTISE_ACK 길이 부족(len=%d) — 무시", len);
            goto done;
        }
        const esp_now_advertise_ack_t *ack = (const esp_now_advertise_ack_t *)data;

        /* s_scan_channel을 그대로 믿으면 안 됨 — scan_timer_cb(별도 타이머 콜백)가 이 콜백과
         * 다른 시점에 채널을 바꿀 수 있어서, 실제로 이 응답을 수신한 채널은
         * info->rx_ctrl->channel로 확인하는 게 정확함(2026-08-02, 실기에서 발견된 레이스 —
         * 이제 뮤텍스로 근본 원인도 막혔지만, "어느 채널에서 받았는지"는 여전히 이렇게 확인) */
        uint8_t actual_channel = (info && info->rx_ctrl) ? info->rx_ctrl->channel : s_scan_channel;

        memcpy(s_hub_mac, ack->hub_mac, sizeof(s_hub_mac));
        add_peer_if_needed(s_hub_mac);
        s_hub_boot_id       = ack->hub_boot_id;
        s_hub_boot_id_known = true;

        s_scan_channel = actual_channel;
        esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);
        /* 2026-08-23(사용자 지시) — ACK 받았다고 스캔(scan_timer)을 멈추면 안 됨. 페어링은
         * 여기서 끝나는 게 아니라 별도의 PAIR_REQUEST/PAIR_ACK가 와야 완료되고, CNTL이 여러
         * 대일 수도 있어서 다른 채널에 있는 CNTL도 계속 발견 기회를 줘야 함 — 채널 하나에
         * 묶여서 광고를 끊으면 (a) 그 PAIR_REQUEST가 늦게 오면 페어링 창구 자체가 사라지고
         * (b) 다른 CNTL은 영원히 못 찾음. 그래서 scan_timer는 페어링 전까지 계속 돔 —
         * s_last_synced_channel만 힌트로 남기고(다음 웨이크 fast-path), 이번 웨이크의 스캔은
         * 안 멈춤 */
        if (s_rest_timer) esp_timer_stop(s_rest_timer);  /* 스윕 휴식 중에 동기화된 경우 대비 */
        s_last_synced_channel = actual_channel;  /* 다음 웨이크가 여기서부터 먼저 시도 */

        s_synced          = true;
        s_ping_fail_count = 0;
        s_pong_pending    = false;
        /* 2026-08-23(사용자 지시) — PING은 여기서 더 이상 자동 시작 안 함. 채널 스캔이 계속
         * 도는 동안(위 주석 참고) PING을 켜면 라디오가 계속 다른 채널로 옮겨가서 PING이
         * 구조적으로 실패하는 무한루프가 됨(실기로 발견) — 실제 페어링 완료 시
         * esp_now_channelsync_notify_paired()가 스캔을 멈추고 PING을 시작함 */

        ESP_LOGI(TAG, "채널 동기화됨(CH%d)", s_scan_channel);
        if (s_on_synced) s_on_synced(s_scan_channel, s_hub_mac);
        goto done;
    }

    if (s_synced && msg_type == ESP_NOW_MSG_CHANNEL_PONG) {
        if (!info || memcmp(info->src_addr, s_hub_mac, sizeof(s_hub_mac)) != 0) goto done;
        if (len < (int)sizeof(esp_now_channel_pong_t)) {
            ESP_LOGW(TAG, "PONG 길이 부족(len=%d) — 무시", len);
            goto done;
        }
        if (s_on_pong_received) s_on_pong_received();
        s_pong_pending    = false;
        s_ping_fail_count = 0;

        /* 2026-08-23 — boot_id 대조(위 s_hub_boot_id 주석 참고). PING/PONG 왕복은 캠이
         * 스스로 깨어있는 순간에 하는 거라 HUB_RESET 브로드캐스트보다 훨씬 신뢰성 있음 —
         * 이걸로 "Cntl이 리붓해서 날 잊었다"를 능동적으로, 확실하게 잡아냄 */
        const esp_now_channel_pong_t *pong = (const esp_now_channel_pong_t *)data;
        if (s_hub_boot_id_known && pong->hub_boot_id != s_hub_boot_id) {
            ESP_LOGW(TAG, "Cntl boot_id 변경 감지(0x%08lx -> 0x%08lx) — Cntl이 리붓함, 재광고 시작",
                     (unsigned long)s_hub_boot_id, (unsigned long)pong->hub_boot_id);
            enter_unsynced();
            if (s_on_lost_sync) s_on_lost_sync();
            goto done;
        }
        goto done;
    }

done:
    xSemaphoreGive(s_state_mutex);
}

void esp_now_channelsync_notify_paired(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    /* 이제서야 채널에 눌러앉음(스캔 종료) — 그 전까지는 다른 CNTL도 찾을 기회를 주려고 계속
     * 돌던 것(위 ADVERTISE_ACK 핸들러 주석 참고). scan_timer를 멈춰도 should_advertise_now()가
     * 이제 PAIRED를 보고할 테니, 설령 이 타이머가 무슨 이유로든 다시 돌더라도
     * scan_timer_cb/rest_timer_cb의 자가치유 게이트가 다음 틱에 바로 다시 잡아 멈춤 */
    if (s_scan_timer) esp_timer_stop(s_scan_timer);
    if (s_rest_timer) esp_timer_stop(s_rest_timer);
    s_ping_fail_count = 0;
    s_pong_pending    = false;
    if (s_ping_timer) esp_timer_start_periodic(s_ping_timer, s_ping_interval_us);
    ESP_LOGI(TAG, "페어링 완료 — 스캔 잠금, PING 시작(CH%d)", s_scan_channel);
    xSemaphoreGive(s_state_mutex);
}

/* 2026-08-24 — 호출부(esp_now_cam.c 등)의 send_cb가 브로드캐스트 목적지+성공 상태를 확인하고
 * 불러줌 — 여기서만 "전송됨" 로그와 소리 훅을 냄(진짜 무선 송출 완료 시점, 위
 * send_advertise_on_current_channel() 주석 참고). 뮤텍스 불필요 — s_on_advertise_sent는
 * 부팅 시 한 번만 설정되고 이후 안 바뀜, 다른 공유 상태를 안 건드림 */
void esp_now_channelsync_notify_advertise_send_done(void)
{
    ESP_LOGI(TAG, "ADVERTISE 전송됨(무선 송출 완료 확인)");
    if (s_on_advertise_sent) s_on_advertise_sent();
}

void esp_now_channelsync_notify_alive(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_synced) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    /* PONG을 실제로 받은 것과 동일하게 취급 — 위 헤더 설명 참고. 지금 막 딴 왕복(예:
     * DONE_ACK)이 성공했다는 건 이 순간 채널이 멀쩡하다는 증거이므로, 마침 큐에 밀려
     * 대기 중이던 PING이 뒤늦게 실패로 잡히더라도 그건 이미 낡은 판단임 */
    s_pong_pending    = false;
    s_ping_fail_count = 0;
    xSemaphoreGive(s_state_mutex);
}

bool esp_now_channelsync_is_synced(void)
{
    return s_synced;
}

void esp_now_channelsync_set_ping_interval_ms(uint32_t interval_ms)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (interval_ms == 0) interval_ms = PING_INTERVAL_US_DEFAULT / 1000;
    s_ping_interval_us = interval_ms * 1000;
    /* SYNCED 상태에서 바로 반영 — 다음 PONG/재동기화 때 새 주기로 자동 반영되는 걸 기다리지
     * 않고, 지금 도는 타이머를 새 주기로 즉시 재시작(연속 실패 카운트는 유지, 방금 막 살아있음
     * 확인이 끊긴 건 아니므로 리셋할 이유 없음) */
    if (s_synced && s_ping_timer) {
        esp_timer_stop(s_ping_timer);
        esp_timer_start_periodic(s_ping_timer, s_ping_interval_us);
    }
    xSemaphoreGive(s_state_mutex);
}
