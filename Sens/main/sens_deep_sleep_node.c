/**
 * @file    sens_deep_sleep_node.c
 * @brief   SCD41 헤드리스 Sens 노드 — CAM(cam_node.c)의 웨이크/CASK/딥슬립 구조를 그대로
 *          이식(2026-09-05, 사용자 지시: "캠에서 구현된 걸 가져다 써야되").
 *
 * 예전 sensor_node.c(계속실행+라이트슬립)에서 재사용 범위로 명시된 것만 옮겨옴 — 센서
 * 연동(SCD41), 배터리 잔량 체크, LED 제어. 네트워크/웹(wifi_dashboard.c)과 history_log는
 * 이번 범위에서 제외(사용자 지시). 통신/전원관리 뼈대는 esp_now_node_cask.c/rwdt_guard.c를
 * 통해 CAM 것을 그대로 씀.
 *
 * 2026-09-05(두 번째 수정, 사용자 지시: "캠과 동일한 구조로 가라") — 이 파일이 캠의
 * cam_node.c와 대칭되는 자리 — CASK 이벤트기반 대기(세마포어/스윕완료/SLEEP_NOW 상태)를
 * 여기서 소유(esp_now_node_cask.c는 esp_now_cam.c와 대칭, 연결 상태만 다룸). 측정 "시도"는
 * 캠의 주기촬영처럼 네트워크/캐스크와 완전히 독립적으로 하드웨어 초기화 직후 곧장 하되,
 * 실제 SCD41 트리거는 게이팅됨(사용자 지시: "전력소모 큰 센서가 있어서... 측정주기에만
 * 측정해야되") — 마지막 실측정 이후 실제 경과시간이 측정주기 이상일 때만 재측정, 아니면
 * 캐시값 재사용. 값은 RTC 메모리에 캐시해 딥슬립 넘어서도 유지, 측정ID(measurement_id)를
 * 새로 측정 성공할 때마다 증가시켜 캐스크(WAKE_HELLO_SENS)에 실어보냄(사용자 지시: "측정
 * 값이 없으면 그냥 웨헬 보내고, 있으면 보내도록", "콘이 달라지지 않은 측정값을 처리할
 * 필요 없도록"). 딥슬립 실제 시간은 캠과 동일하게 SLEEP_NOW.sleep_sec 그 자체를 그대로 씀
 * (esp_now_node_get_last_sleep_sec()) — 콘 쪽(esp_now_hub.c의 send_cask_sleep_now())이
 * MIN(응답성, 이 센스의 측정주기)을 계산해서 보내므로, 응답성<측정주기일 때는 측정주기보다
 * 자주 깨지만(콘의 제어 기회 확보용) 위 게이팅 덕분에 그 웨이크들에서 재측정하지는 않음.
 * 센서종류/채널구성(sensor_kind/chan_type)은 매 캐스크가 아니라 페어링(PAIR_ACK) 때 1회만
 * 보냄(esp_now_link.h의 esp_now_pair_ack_t 참고) — esp_now_node_init()에 그때 한 번 넘김.
 */

#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "bsp_c3_pico.h"
#include "battery.h"
#include "esp_now_node.h"
#include "esp_now_channelsync.h"
#include "status_led.h"
#include "rwdt_guard.h"
#include "scd41.h"
#include "led_strip.h"

static const char *TAG = "sens_deep_sleep_node";

#define SENSOR_KIND_CURRENT  SENSOR_KIND_SCD41
#define SENSOR_CHAN_COUNT    3
static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
    SENSOR_CHAN_CO2_PPM, SENSOR_CHAN_TEMP_C, SENSOR_CHAN_HUMI_PCT,
};

#define BATT_PLATEAU_MIN_MV     3900.0f
#define BATT_FULL_MV_DEFAULT    4020.0f
#define NVS_NS_BATTCAL           "battcal"
#define NVS_KEY_FULL_MV          "full_mv"

/* SCD41 single-shot 측정 소요시간(~5초) 대비 여유 — 트리거 후 이 안에 완료 안 되면 실패로
 * 처리(cam_node.c류 "무한정 안 기다린다" 원칙과 동일). 2026-09-05 — 센서마다 측정 소요시간이
 * 다름(사용자 지시: 온습도~100ms, CO2~1초, 암모니아~3초) — 이 값은 SCD41 전용이고, 다른
 * 센서의 딥슬립 포팅이 나중에 필요해지면 그 센서 고유 타임아웃으로 따로 정의해야 함(지금은
 * SCD41 헤드리스 빌드만 범위) */
#define SCD41_MEASURE_POLL_MS    200
#define SCD41_MEASURE_TIMEOUT_MS 6000

/* CAM의 cam_node.c와 동일 상수(esp_now_link.h의 ESP_NOW_NODE_UNPAIRED_RETRY_SEC 공용값 재사용) */
#define UNPAIRED_BACKOFF_SHORT_UNTIL_SEC   60          /* 1분까지: 짧게(3초) */
#define UNPAIRED_BACKOFF_MID_SEC           10          /* 1~11분: 10초 */
#define UNPAIRED_BACKOFF_MID_UNTIL_SEC     (60 + 600)
#define UNPAIRED_BACKOFF_LONG_SEC          30          /* 11분 이후: 30초 */

/* CASK 왕복 전체(CONFIG 800ms×3 + 할일없음 500ms×3 + SLEEP_NOW 300ms×3 = 4.8s) 대비 여유 —
 * 캠의 CASK_TIMEOUT_MS(15초, 사진목록 등 무거운 할일 포함)보다 짧음(센스는 그런 무거운
 * 할일이 없음) */
#define SENS_CASK_TIMEOUT_MS      8000
/* Live 모드(SLEEP_NOW.sleep_sec==0) 재체크인 페이싱 — 캠의 CASK_LIVE_PACE_MS와 동일 값 */
#define SENS_CASK_LIVE_PACE_MS    1000

static float s_full_mv           = BATT_FULL_MV_DEFAULT;
static int   s_full_mv_persisted = 0;
static adc_oneshot_unit_handle_t s_vin_adc = NULL;

/* 2026-09-05 — 측정값 캐시(딥슬립 경계 넘어 유지) + 측정ID(캠의 s_wake_hub_* RTC_DATA_ATTR
 * 패턴과 동일 이유). 0=한 번도 측정 성공한 적 없음 — 이때만 웨헬이 "빈" 값(chan_ok=false)을
 * 실어보냄(사용자 설계: "캐스크에 측정 값이 없으면 그냥 웨헬 보내고") */
static RTC_DATA_ATTR uint32_t s_measurement_id                    = 0;
static RTC_DATA_ATTR float    s_cached_vals[SENSOR_CHAN_COUNT]     = { 0 };
static RTC_DATA_ATTR uint8_t  s_cached_chan_ok[SENSOR_CHAN_COUNT]  = { 0 };
static RTC_DATA_ATTR uint32_t s_unpaired_backoff_elapsed_sec       = 0;

/* 2026-09-05(사용자 지시) — 전력소모가 큰 센서가 있어서 "깰 때마다"가 아니라 "실제 측정
 * 주기만큼 지났을 때만" 재측정해야 함(응답성<측정주기일 때 측정주기보다 자주 깨는 건
 * 콘의 제어 기회 확보용일 뿐, 그때마다 재측정하면 의미없이 전력만 씀). 이 값은 "마지막
 * 실측정 이후 실제로 몇 초가 지났는지"의 누적치 — 매 사이클 끝에 이번에 실제로 잠들
 * sleep_sec만큼 더해두고, 실측정에 성공하면 0으로 리셋. RTC_DATA_ATTR로 딥슬립 경계
 * 너머 유지해야 다음 부팅(=다음 웨이크) 때도 정확함 */
static RTC_DATA_ATTR uint32_t s_seconds_since_last_measurement    = 0;

/* 2026-09-05 — CAM의 cam_node.c 이벤트기반 대기 루프 이식(s_wake_recheck_sem/s_sweep_completed/
 * s_sleep_now_requested). esp_now_node_cask.c(연결 상태 전담, esp_now_cam.c와 대칭)가
 * esp_now_node_signal_recheck()/esp_now_node_note_sleep_now_requested()/
 * esp_now_node_note_scan_restarted()(esp_now_node.h 선언)로 이 상태를 갱신함 */
static SemaphoreHandle_t s_wake_recheck_sem  = NULL;
static volatile bool     s_sweep_completed   = false;
static volatile bool     s_sleep_now_requested = false;

void esp_now_node_signal_recheck(void)
{
    if (s_wake_recheck_sem) xSemaphoreGive(s_wake_recheck_sem);
}

void esp_now_node_note_sleep_now_requested(uint32_t sleep_sec)
{
    (void)sleep_sec;  /* 실제 딥슬립 시간엔 안 씀(esp_now_node.h의 get_sample_interval_sec
                         주석 참고) — 여기선 "SLEEP_NOW 왔다"는 신호만 필요 */
    s_sleep_now_requested = true;
    esp_now_node_signal_recheck();
}

void esp_now_node_reset_sleep_now_state(void)
{
    s_sleep_now_requested = false;
}

void esp_now_node_note_scan_restarted(void)
{
    s_sweep_completed = false;
}

/* 채널스캔 스윕 한 바퀴 완료 훅(esp_now_channelsync_set_event_hooks 참고) — CAM은 스피커
 * 알림용 훅을 통해 간접적으로 이 상태를 세우지만(cam_speaker 의존), 센스는 스피커가 없으니
 * 이 컴포넌트에 직접 등록 */
static void on_scan_sweep_done(void)
{
    s_sweep_completed = true;
    esp_now_node_signal_recheck();
}

static uint32_t next_unpaired_retry_sleep_sec(void)
{
    if (s_unpaired_backoff_elapsed_sec < UNPAIRED_BACKOFF_SHORT_UNTIL_SEC) return ESP_NOW_NODE_UNPAIRED_RETRY_SEC;
    if (s_unpaired_backoff_elapsed_sec < UNPAIRED_BACKOFF_MID_UNTIL_SEC) return UNPAIRED_BACKOFF_MID_SEC;
    return UNPAIRED_BACKOFF_LONG_SEC;
}

static bool vin_indicates_usb(void)
{
    if (!s_vin_adc) return false;
    int raw = 0;
    if (adc_oneshot_read(s_vin_adc, BSP_C3_VIN_ADC_CHANNEL, &raw) != ESP_OK) return false;
    int mv = (int)((float)raw * 3100.0f / 4095.0f);
    return mv >= BSP_C3_VIN_PRESENT_MV;
}

/* sensor_node.c의 maybe_learn_full_mv()와 같은 목적이지만 단순화(2026-09-05) — 예전 버전은
 * "계속실행" 전제로 20샘플 추세밴드를 봤는데, 딥슬립에선 매 부팅이 곧 새 판독 하나뿐이라
 * 그 창 자체가 없음. 이번 판독이 USB 전원 중이고 완충값 부근이면 그대로 학습 */
static void maybe_learn_full_mv(bool powered, int mv)
{
    if (!powered) return;
    if ((float)mv < BATT_PLATEAU_MIN_MV) return;
    if (mv == (int)s_full_mv) return;

    s_full_mv = (float)mv;
    battery_set_full_mv(s_full_mv);
    ESP_LOGI(TAG, "배터리 완충 전압 학습: %d mV", mv);

    if (mv != s_full_mv_persisted) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_BATTCAL, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, NVS_KEY_FULL_MV, mv);
            nvs_commit(h);
            nvs_close(h);
            s_full_mv_persisted = mv;
        }
    }
}

static led_pattern_t batt_pct_to_led_pattern(int pct)
{
    if (pct >= 60) return LED_PATTERN_HEARTBEAT;
    if (pct >= 20) return LED_PATTERN_HEARTBEAT_FAST;
    return LED_PATTERN_HEARTBEAT_URGENT;
}

/* 2026-09-06(사용자 지시: "잘 되면 대치하려고 해") — 온보드 WS2812(GPIO7, bsp_c3_pico.h
 * 주석 확인) 하드웨어 검증용 임시 테스트. 원색 3개(빨/초/파) + 혼합색 3개(보라=빨+파,
 * 노랑=빨+초, 청록=초+파) 1초 간격 6가지 — "혼합도 되는지 확인" 목적. 이 실험이 잘 되면
 * 기존 단색 상태 LED(HEARTBEAT/BLINK_FAST 등)를 이걸로 대치할 예정 — 지금은 순수 시각
 * 확인용 임시 코드, 실제 상태 매핑 아님 */
#define WS2812_TEST_GPIO       7
#define WS2812_TEST_BRIGHTNESS 32  /* 0~255, 눈부심 방지로 낮게 */

/* 2026-09-06(사용자 지시: "코드에도 정의해") — 빨/초/파/보/노/청/백 색 이름과 RGB 조합을
 * 코드로 고정. 나중에 실제 상태(HEARTBEAT/BLINK_FAST 등 대치)를 이 색 중 하나로 매핑할
 * 때 이 enum/테이블을 그대로 재사용하면 됨 — 순서는 실기로 확인한 시각 테스트 순서와 동일 */
typedef enum {
    WS2812_COLOR_RED,     /* 빨 */
    WS2812_COLOR_GREEN,   /* 초 */
    WS2812_COLOR_BLUE,    /* 파 */
    WS2812_COLOR_PURPLE,  /* 보 = 빨+파 */
    WS2812_COLOR_YELLOW,  /* 노 = 빨+초 */
    WS2812_COLOR_CYAN,    /* 청 = 초+파 */
    WS2812_COLOR_WHITE,   /* 백 = 빨+초+파 */
    WS2812_COLOR_COUNT,
} ws2812_color_t;

static const struct { uint8_t r, g, b; const char *name; } s_ws2812_colors[WS2812_COLOR_COUNT] = {
    [WS2812_COLOR_RED]    = { WS2812_TEST_BRIGHTNESS, 0, 0, "빨강" },
    [WS2812_COLOR_GREEN]  = { 0, WS2812_TEST_BRIGHTNESS, 0, "초록" },
    [WS2812_COLOR_BLUE]   = { 0, 0, WS2812_TEST_BRIGHTNESS, "파랑" },
    [WS2812_COLOR_PURPLE] = { WS2812_TEST_BRIGHTNESS, 0, WS2812_TEST_BRIGHTNESS, "보라" },
    [WS2812_COLOR_YELLOW] = { WS2812_TEST_BRIGHTNESS, WS2812_TEST_BRIGHTNESS, 0, "노랑" },
    [WS2812_COLOR_CYAN]   = { 0, WS2812_TEST_BRIGHTNESS, WS2812_TEST_BRIGHTNESS, "청록" },
    [WS2812_COLOR_WHITE]  = { WS2812_TEST_BRIGHTNESS, WS2812_TEST_BRIGHTNESS, WS2812_TEST_BRIGHTNESS, "백색" },
};

/* 상태 매핑 코드가 재사용할 헬퍼 — 색 이름(enum)만 넘기면 실제 픽셀 설정+refresh까지 함 */
static void ws2812_set_color(led_strip_handle_t strip, ws2812_color_t color)
{
    led_strip_set_pixel(strip, 0, s_ws2812_colors[color].r, s_ws2812_colors[color].g, s_ws2812_colors[color].b);
    led_strip_refresh(strip);
}

/* 2026-09-06(사용자 지시) — 측정 시작/완료/실패를 이 WS2812로 눈에 보이게 표시(SCD41 판독
 * 실패 진단용). 핸들은 최초 호출 때 한 번만 만들고 계속 재사용 */
static led_strip_handle_t s_ws2812_strip = NULL;

static void ws2812_init_once(void)
{
    if (s_ws2812_strip) return;
    led_strip_config_t strip_config = {
        .strip_gpio_num        = WS2812_TEST_GPIO,
        .max_leds              = 1,
        .led_model             = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,  /* 이 보드는 GRB 아니라
                                                                          RGB 순서(실기 확인) */
    };
    led_strip_rmt_config_t rmt_config = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_ws2812_strip) != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 초기화 실패 — 측정 표시 LED 없이 진행");
        s_ws2812_strip = NULL;
    }
}

static void ws2812_off(void)
{
    if (!s_ws2812_strip) return;
    led_strip_clear(s_ws2812_strip);
    led_strip_refresh(s_ws2812_strip);
}

/* 한 번 켰다가 on_ms 뒤에 끔 */
static void ws2812_flash(ws2812_color_t color, uint32_t on_ms)
{
    ws2812_init_once();
    if (!s_ws2812_strip) return;
    ws2812_set_color(s_ws2812_strip, color);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    ws2812_off();
}

/* on_ms 켬 -> on_ms 끔을 times번 반복(측정 실패 표시: 보라 2회 깜빡임) */
static void ws2812_blink(ws2812_color_t color, uint32_t on_ms, int times)
{
    ws2812_init_once();
    if (!s_ws2812_strip) return;
    for (int i = 0; i < times; i++) {
        ws2812_set_color(s_ws2812_strip, color);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        ws2812_off();
        if (i + 1 < times) vTaskDelay(pdMS_TO_TICKS(on_ms));
    }
}

/* 2026-09-06(사용자 지시) — CONFIG_PM_ENABLE=y + FREERTOS_USE_TICKLESS_IDLE=y(자동
 * 라이트슬립)가 켜져 있어서, SCD41 측정 폴링 중(vTaskDelay 사이사이) 라이트슬립에 들어갔다
 * 나왔다 하면서 I2C 상태가 깨질 가능성 — 측정 시작~완료까지는 라이트슬립 자체를 못 하게
 * 락을 잡음(이 락은 esp_deep_sleep_start()의 진짜 딥슬립과는 무관, 자동 라이트슬립만 막음) */
static esp_pm_lock_handle_t s_no_light_sleep_lock = NULL;

static void pm_lock_no_light_sleep_acquire(void)
{
    if (!s_no_light_sleep_lock) {
        if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "sens_measure", &s_no_light_sleep_lock) != ESP_OK) {
            ESP_LOGW(TAG, "PM 락 생성 실패 — 라이트슬립 방지 없이 측정 진행");
            return;
        }
    }
    esp_pm_lock_acquire(s_no_light_sleep_lock);
}

static void pm_lock_no_light_sleep_release(void)
{
    if (s_no_light_sleep_lock) esp_pm_lock_release(s_no_light_sleep_lock);
}

/* SCD41 single-shot 판독 — 트리거 후 최대 SCD41_MEASURE_TIMEOUT_MS까지 블로킹 폴링.
 * 예전 sensor_node.c는 이걸 여러 esp_timer 틱에 걸쳐 논블로킹으로 했는데(계속실행 전제),
 * 딥슬립은 부팅마다 한 번뿐이라 "다음 틱"이 없음 — CAM의 촬영 대기 패턴과 동일하게
 * 한 부팅 안에서 블로킹으로 끝냄(scd41.c의 trigger/poll API 자체는 안 건드림) */
static bool measure_scd41(float out[SENSOR_CHAN_COUNT])
{
    if (!scd41_trigger_single_shot()) return false;

    uint32_t waited_ms = 0;
    while (waited_ms < SCD41_MEASURE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(SCD41_MEASURE_POLL_MS));
        waited_ms += SCD41_MEASURE_POLL_MS;
        int co2 = 0;
        float t = 0.0f, h = 0.0f;
        bool ok = false;
        if (scd41_poll_single_shot(&co2, &t, &h, &ok)) {
            if (ok) { out[0] = (float)co2; out[1] = t; out[2] = h; }
            return ok;
        }
    }
    ESP_LOGW(TAG, "SCD41 측정 타임아웃(%ums)", (unsigned)SCD41_MEASURE_TIMEOUT_MS);
    return false;
}

/* 2026-09-06(사용자 지시) — 측정 시도 한 번(노랑 시작 표시 -> 트리거+폴링 -> 청록/보라
 * 결과 표시)을 통째로 감싼 헬퍼. 파워사이클 후 첫 워밍업 측정도 이 함수로 똑같이
 * 표시하고(사용자 지시: 구분 없이 LED 표시), 실측정도 이 함수로 함 — 한 사이클 안에서
 * 최대 두 번(워밍업+실측정) 불릴 수 있어서 elapsed는 호출부가 계속 누적하도록
 * 포인터로 받음 */
static bool attempt_one_scd41_measurement(float out[SENSOR_CHAN_COUNT], uint32_t *accum_elapsed_ms)
{
    ws2812_flash(WS2812_COLOR_YELLOW, 100);
    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    pm_lock_no_light_sleep_acquire();
    bool ok = measure_scd41(out);
    pm_lock_no_light_sleep_release();
    *accum_elapsed_ms += (uint32_t)(esp_timer_get_time() / 1000) - start_ms;
    if (ok) {
        ws2812_flash(WS2812_COLOR_CYAN, 200);
    } else {
        ws2812_blink(WS2812_COLOR_PURPLE, 100, 2);
    }
    return ok;
}

/* 2026-09-06(사용자 지시 정정: "연결 전에 측정하지 말아", "광고를 하는 상황에서는
 * 측정하지 않는다") — 예전엔 캠의 "캐스크 타이밍과 무관하게 측정"을 센스에도 그대로
 * 확장해서 페어링 여부와 무관하게 부팅 직후 곧장 측정했는데, 이건 잘못된 확장이었음.
 * 캠은 로컬저장이 있어 연결 안 돼도 촬영 자체는 가치가 있지만, 센스 값은 보낼 곳(콘)이
 * 없으면 측정 자체가 무의미 — 그래서 이 함수는 반드시 "실제로 페어링된 게 확인된 뒤"에만
 * app_main에서 호출해야 함(광고/스캔 중엔 호출 안 함). 측정주기 게이팅(due_for_measurement)
 * 자체는 그대로 유지 */
static void do_gated_measurement_once(uint32_t *measurement_elapsed_ms)
{
    uint32_t measure_period_sec = esp_now_node_get_sample_interval_sec();
    bool due_for_measurement = (s_measurement_id == 0) ||
                               (s_seconds_since_last_measurement >= measure_period_sec);
    if (due_for_measurement) {
        bool is_first_ever = (s_measurement_id == 0);
        float fresh_vals[SENSOR_CHAN_COUNT] = { 0 };
        bool fresh_ok = attempt_one_scd41_measurement(fresh_vals, measurement_elapsed_ms);

        if (fresh_ok && is_first_ever) {
            /* 2026-09-06(Sensirion 공식 문서: "파워사이클 후 첫 싱글샷 결과는 항상 버려야
             * 안정화됨") — 진짜 최초(측정ID==0)일 때만 해당, 딥슬립 웨이크는 센서 자체
             * 전원이 안 끊기므로 매번 적용 안 함. 워밍업 결과는 버리고 곧장 한 번 더
             * 측정해서 그 결과를 진짜 첫 값(측정ID=1)으로 씀 — 사용자 지시로 이 워밍업도
             * 진짜 측정과 동일하게 LED로 표시(구분 없음) */
            ESP_LOGI(TAG, "SCD41 워밍업 측정 완료(버림, 파워사이클 후 첫 값) — 실제 측정 재시도");
            fresh_ok = attempt_one_scd41_measurement(fresh_vals, measurement_elapsed_ms);
        }

        if (fresh_ok) {
            memcpy(s_cached_vals, fresh_vals, sizeof(fresh_vals));
            for (int i = 0; i < SENSOR_CHAN_COUNT; i++) s_cached_chan_ok[i] = 1;
            s_measurement_id++;
            s_seconds_since_last_measurement = 0;
            /* %f 안 씀(newlib-nano 미지원 — feedback_lvgl_no_percent_f 관례) — 정수부/소수부
             * 수동 분리(format_battery_display()와 동일 패턴) */
            int temp_x10 = (int)(s_cached_vals[1] * 10.0f + 0.5f);
            int humi_x10 = (int)(s_cached_vals[2] * 10.0f + 0.5f);
            ESP_LOGI(TAG, "SCD41MARK 측정 성공 — 측정ID=%u co2=%d temp=%d.%d humi=%d.%d",
                     (unsigned)s_measurement_id, (int)s_cached_vals[0],
                     temp_x10 / 10, temp_x10 % 10, humi_x10 / 10, humi_x10 % 10);
        } else {
            ESP_LOGW(TAG, "SCD41MARK 판독 실패 — 직전 캐시값(측정ID=%u) 재사용", (unsigned)s_measurement_id);
        }
    } else {
        ESP_LOGI(TAG, "측정주기(%us) 미도달(경과 %us) — 재측정 생략, 캐시값(측정ID=%u) 재사용",
                 (unsigned)measure_period_sec, (unsigned)s_seconds_since_last_measurement,
                 (unsigned)s_measurement_id);
    }
    ESP_LOGW(TAG, "MEASCHK due=%d id=%u elapsed=%us period=%us reset_reason=%d",
             (int)due_for_measurement, (unsigned)s_measurement_id,
             (unsigned)s_seconds_since_last_measurement, (unsigned)measure_period_sec,
             (int)esp_reset_reason());
}

void app_main(void)
{
    /* 최우선 — WiFi/센서 초기화보다 먼저(cam_node.c의 capture_wake_reason() 호출 순서와
     * 동일 이유, esp_now_node_cask.c 참고) */
    esp_now_node_capture_wake_info();

    /* 초기 보수적 예산(첫 부팅엔 아직 CNTL의 실제 샘플주기를 모름) — SENS_CONFIG_SET 수신
     * 시 esp_now_node_cask.c가 실제 값으로 재무장함(cam_node_set_response_interval_sec()과
     * 동일 패턴) */
    rwdt_guard_arm(15 + CONFIG_SENS_DEEPSLEEP_AWAKE_MARGIN_SEC);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_handle_t batt_nvs;
    if (nvs_open(NVS_NS_BATTCAL, NVS_READONLY, &batt_nvs) == ESP_OK) {
        int32_t learned_mv = 0;
        if (nvs_get_i32(batt_nvs, NVS_KEY_FULL_MV, &learned_mv) == ESP_OK) {
            s_full_mv           = (float)learned_mv;
            s_full_mv_persisted = learned_mv;
        }
        nvs_close(batt_nvs);
    }

    ESP_ERROR_CHECK(bsp_c3_pico_init());

    /* 2026-09-06(사용자 지시로 재확인) — single-shot만 쓰는 경로는 continuous(periodic)
     * 모드를 아예 시작하지도 않으므로, "혹시 periodic이 켜져있을까봐 방어적으로 끄는" 절차
     * 자체가 필요 없음(Sensirion 공식 문서: measure_single_shot은 start_periodic_measurement의
     * 대안이지 같이 쓰는 게 아니고, wake_up도 power_down을 실제로 건 적이 있을 때만 필요한데
     * 이 경로는 power_down을 안 씀). STOP_PERIODIC 자체는 없앴지만, 안정화 지연은 그 명령과
     * 무관하게 별도로 필요함 — Sensirion 공식 데이터시트: "전원 인가 후 idle 상태 진입까지
     * 1000ms 필요, 그래야 명령을 받을 준비가 됨"(싱글샷이든 뭐든 첫 명령 자체에 적용되는
     * 일반 요구사항). 예전엔 컨티뉴어스 왕복(약 7~9초)이 우연히 이 요구사항을 넘겨서
     * 가려주고 있었을 뿐 — 그 왕복을 없앤 지금은 이 지연을 그 목적 그대로 명시적으로 둠.
     * I2C 버스만 지금 준비해두고, 실제 측정은 아래에서 페어링 확인 후로 미룸(사용자 지시:
     * "연결 전에 측정하지 말아" — 캠의 "캐스크 타이밍과 무관"을 센스에 잘못 그대로 확장 적용한
     * 실수를 정정. 캠은 로컬저장이 있어 연결 여부와 무관하게 촬영 가치가 있지만, 센스 값은
     * 보낼 곳이 없으면 측정 자체가 무의미) */
    if (!scd41_init_single_shot(BSP_C3_I2C_PORT, BSP_C3_I2C_SDA, BSP_C3_I2C_SCL)) {
        ESP_LOGW(TAG, "SCD41 초기화 실패 — 연결 확인 필요(다음 사이클에 재시도)");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));  /* 싱글샷용 전원안정화 지연(위 주석 참고) */

    uint32_t measurement_elapsed_ms = 0;  /* 2026-09-06(사용자 지시) — 이번 사이클에 측정으로
                                            * 쓴 실제 시간, 나중에 딥슬립 시간에서 뺄 기준.
                                            * 실제 측정은 페어링 확인 후(아래) 일어남 */

    /* 2026-09-06 버그수정(사용자 보고: "배터리 값은 0.00 V 로 나와") — CASK 재작성(2026-09-05)
     * 때 이 초기화 블록 자체를 실수로 빠뜨림. battery_read_mv()는 battery_init()이 세팅하는
     * 내부 ADC 핸들(s_adc)이 없으면 그냥 0을 반환하므로(battery.c), 호출은 계속 됐지만 항상
     * 0이 나왔던 것 — 매번 "0.00 V"로 보인 원인 */
    battery_config_t batt_cfg = {
        .adc_unit    = BSP_C3_BATTERY_ADC_UNIT,
        .adc_channel = BSP_C3_BATTERY_ADC_CHANNEL,
        .atten       = BSP_C3_BATTERY_ADC_ATTEN,
        .divider     = BSP_C3_BATTERY_DIV,
        .full_mv     = s_full_mv,
        .empty_mv    = 3300.0f,
        .ctrl_gpio   = GPIO_NUM_NC,
    };
    battery_init(&batt_cfg);

    s_vin_adc = battery_get_adc_handle();
    if (s_vin_adc) {
        adc_oneshot_chan_cfg_t vin_ch_cfg = {
            .atten    = BSP_C3_VIN_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_oneshot_config_channel(s_vin_adc, BSP_C3_VIN_ADC_CHANNEL, &vin_ch_cfg);
    }

    int batt_mv = battery_read_mv();
    bool batt_ok = (batt_mv > 0);
    bool powered = vin_indicates_usb();
    if (batt_ok) maybe_learn_full_mv(powered, batt_mv);
    int batt_pct = batt_ok ? battery_mv_to_pct(batt_mv) : 0;

    /* 충전 중(USB 전원)이 최우선 — sensor_node.c와 동일 우선순위. "판독 실패" LED는
     * 이번 부팅 판독이 아니라 "한 번도 측정 성공한 적 없음"(measurement_id==0) 기준 —
     * 캐시된 값이 있으면 이번에 재측정 실패해도 굳이 경보 패턴까지는 안 씀 */
    if (powered) {
        status_led_init(BSP_C3_LED_BLUE);
        status_led_set_pattern(BSP_C3_LED_BLUE, LED_PATTERN_ON);
    } else if (s_measurement_id == 0) {
        status_led_init(BSP_C3_LED_BLUE);
        status_led_set_pattern(BSP_C3_LED_BLUE, LED_PATTERN_BURST_TRIPLE);
    } else if (batt_ok) {
        status_led_init(BSP_C3_LED_BLUE);
        status_led_set_pattern(BSP_C3_LED_BLUE, batt_pct_to_led_pattern(batt_pct));
    }

    uint16_t batt_mv_u16 = (batt_ok && batt_mv < 65536) ? (uint16_t)batt_mv : 0;

    /* 최소 WiFi 브링업(cam_node.c와 완전히 동일 시퀀스) — SSID 접속/HTTP서버 없음,
     * ESP-NOW만을 위한 라디오 초기화 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "WiFi 모뎀슬립 설정: %s", esp_err_to_name(ps_err));

    /* 2026-09-05 — 캠은 스피커 이벤트훅을 통해 간접 연결하지만(cam_speaker 의존), 센스는
     * 스피커가 없으니 채널스캔 스윕완료 훅에 직접 연결 */
    esp_now_channelsync_set_event_hooks(NULL, NULL, NULL, on_scan_sweep_done);
    s_wake_recheck_sem = xSemaphoreCreateBinary();

    esp_now_node_set_status_led(BSP_C3_LED_GREEN);
    esp_now_node_init(SENSOR_KIND_CURRENT, SENSOR_CHAN_COUNT, s_chan_types);

    ESP_LOGI(TAG, "헤드리스 Sens 노드 준비 완료 (kind=%d, channels=%d, 측정ID=%u)",
             SENSOR_KIND_CURRENT, SENSOR_CHAN_COUNT, (unsigned)s_measurement_id);

    /* CAM의 app_main 웨이크 루프와 완전히 동일 구조(2026-09-05 재수정, cam_node.c 참고) —
     * esp_now_node_init() 직후 esp_now_node_report_reading()을 한 번 동기 호출하는 것 자체가
     * 캠의 "esp_now_cam_init() 끝에서 esp_now_cam_reconnect() 호출"과 동일 타이밍(esp_now_
     * node_cask.c 파일 헤더 주석 참고) — 그래서 이 결과(paired_now)가 항상 정확함 */
    bool paired_now = esp_now_node_report_reading(SENSOR_CHAN_COUNT, s_cached_chan_ok, s_cached_vals,
                                                   s_measurement_id, 0, batt_mv_u16);
    uint32_t sleep_sec = ESP_NOW_NODE_UNPAIRED_RETRY_SEC;
    /* 2026-09-06(사용자 지시) — Live 모드(sleep_sec==0)로 오래 깨있는 동안에도 측정주기가
     * 되면 재측정해야 함(캠의 독립적 주기촬영과 동일 원칙) — 이 기준점을 실제로 측정할
     * 때마다(워밍업 포함, do_gated_measurement_once 안에서) 갱신하고, 여기서부터는 Live
     * 루프 안에서 "이 기준점 이후 실제로 깨있던 시간"을 재보고 판단. 최초값은 아래에서
     * 실제로 측정이 일어난 시점에 다시 잡음(그 전엔 의미 없는 기준점) */
    uint32_t live_awake_baseline_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool measured_this_boot = false;  /* 2026-09-06(사용자 지시) — 페어링 확인 전엔 측정 안 함,
                                          확인되면 딱 한 번만 이 부팅의 최초 측정을 함 */

    if (paired_now) {
        do_gated_measurement_once(&measurement_elapsed_ms);
        measured_this_boot = true;
        live_awake_baseline_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }

    for (;;) {
        if (!paired_now) {
            /* 알려진 허브가 없었거나 패스트패스가 실패해서 지금 폴백 스캔 중(백그라운드 —
             * esp_now_node_report_reading()의 폴백 분기 참고) — PAIR_REQUEST가 비동기로
             * 도착할 때까지 이벤트 기반 대기, 스윕 한 바퀴 다 돌 때까지만. 이 대기 동안은
             * 광고/스캔 상태라 측정 안 함(사용자 지시) */
            while (!esp_now_node_is_paired() && !s_sweep_completed) {
                xSemaphoreTake(s_wake_recheck_sem, pdMS_TO_TICKS(1000));
            }
            if (!esp_now_node_is_paired()) {
                sleep_sec = next_unpaired_retry_sleep_sec();
                s_unpaired_backoff_elapsed_sec += sleep_sec;
                ESP_LOGW(TAG, "폴백 스윕 완료 — Cntl 못 찾음, %us 후 재시도", (unsigned)sleep_sec);
                break;
            }
        }
        s_unpaired_backoff_elapsed_sec = 0;  /* 페어링 성공 — 백오프 리셋 */

        if (!measured_this_boot) {
            /* 폴백 스캔으로 방금 막 페어링됨 — 이제서야 이번 부팅의 최초 측정 수행 */
            do_gated_measurement_once(&measurement_elapsed_ms);
            measured_this_boot = true;
            live_awake_baseline_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }

        /* CASK 대기 — CONFIG부터 SLEEP_NOW까지, 이벤트 기반으로 기다리되 SENS_CASK_TIMEOUT_MS
         * 전체 상한을 둠(캠과 동일 원칙 — "WAKE_HELLO 성공 판정을 CASK 전체로 넓힌 것") */
        uint32_t cask_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool cask_timed_out = false;
        while (!s_sleep_now_requested) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms - cask_start_ms >= SENS_CASK_TIMEOUT_MS) {
                ESP_LOGW(TAG, "CASK 미완주(%ums 경과, SLEEP_NOW 못 받음) — WAKE_HELLO_SENS부터 재시도",
                         (unsigned)SENS_CASK_TIMEOUT_MS);
                cask_timed_out = true;
                break;
            }
            xSemaphoreTake(s_wake_recheck_sem, pdMS_TO_TICKS(1000));
        }

        if (cask_timed_out) {
            paired_now = esp_now_node_report_reading(SENSOR_CHAN_COUNT, s_cached_chan_ok, s_cached_vals,
                                                       s_measurement_id, 0, batt_mv_u16);
            continue;
        }

        /* 2026-09-05(사용자 지시로 정정) — 딥슬립 실제 시간은 캠과 동일하게 SLEEP_NOW.
         * sleep_sec 그 자체를 그대로 씀(노드 쪽은 특별취급 없음). "센스는 자기 측정주기마다
         * 깨야 한다"는 요구는 콘 쪽(esp_now_hub.c의 send_cask_sleep_now())이 이 노드의
         * 측정주기를 기준값으로 써서 sleep_sec을 계산하는 것으로 충족됨 — 그래서 여기서
         * 받는 값이 곧 측정주기이고, 별도로 로컬 값을 다시 끼워 넣지 않음(그래야 실수로
         * "실제 잠든 시간 > 측정주기"가 되는 경로 자체가 없어짐) */
        if (esp_now_node_get_last_sleep_sec() != 0) {
            sleep_sec = esp_now_node_get_last_sleep_sec();
            break;
        }

        /* 2026-09-06(사용자 지시) — Live로 오래 깨있는 동안 측정주기가 되면 재측정. "이번
         * 부팅 들어와서 실제로 깨있던 시간"(live_awake_baseline_ms 기준)을 마지막 실측정
         * 이후 경과시간에 더해서 판단 — 딥슬립을 안 거치므로 s_seconds_since_last_measurement
         * 자체는 그대로 두고(다음에 진짜 잘 때를 위해), 여기서는 로컬 변수로만 따짐 */
        uint32_t live_now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t live_awake_elapsed_sec = (live_now_ms - live_awake_baseline_ms) / 1000;
        uint32_t live_measure_period_sec = esp_now_node_get_sample_interval_sec();
        if (s_seconds_since_last_measurement + live_awake_elapsed_sec >= live_measure_period_sec) {
            float fresh_vals[SENSOR_CHAN_COUNT] = { 0 };
            if (attempt_one_scd41_measurement(fresh_vals, &measurement_elapsed_ms)) {
                memcpy(s_cached_vals, fresh_vals, sizeof(fresh_vals));
                for (int i = 0; i < SENSOR_CHAN_COUNT; i++) s_cached_chan_ok[i] = 1;
                s_measurement_id++;
                s_seconds_since_last_measurement = 0;
                live_awake_baseline_ms = (uint32_t)(esp_timer_get_time() / 1000);
            } else {
                ESP_LOGW(TAG, "SCD41 판독 실패(Live 재측정) — 직전 캐시값(측정ID=%u) 재사용",
                         (unsigned)s_measurement_id);
                /* 기준점을 안 옮겨서 다음 Live 반복에서 곧바로 다시 재시도됨 */
            }
        }

        /* Live 루프 — 이번 CASK가 걸린 시간이 페이싱 기준보다 짧으면 나머지를 채워 대기 */
        uint32_t elapsed_ms = (uint32_t)(esp_timer_get_time() / 1000) - cask_start_ms;
        if (elapsed_ms < SENS_CASK_LIVE_PACE_MS) {
            vTaskDelay(pdMS_TO_TICKS(SENS_CASK_LIVE_PACE_MS - elapsed_ms));
        }
        paired_now = esp_now_node_report_reading(SENSOR_CHAN_COUNT, s_cached_chan_ok, s_cached_vals,
                                                   s_measurement_id, 0, batt_mv_u16);
    }

    /* 이번에 실제로 잠들 시간만큼 "마지막 실측정 이후 경과시간"에 더해둠 — 다음 부팅에서
     * 위 due_for_measurement 판단의 기준이 됨(사용자 지시: 측정주기 게이팅). 이 누적은
     * CNTL이 준 "원래 자야 할 시간"(sleep_sec) 기준 그대로 — 아래 실제 딥슬립 시간 보정과는
     * 별개(측정주기 게이팅은 "예산 하나를 다 썼다"는 의미라 원래 예산으로 유지) */
    s_seconds_since_last_measurement += sleep_sec;

    /* 2026-09-06(사용자 지시) — "총 잠들어야 하는 시간에서 측정에 소요된 시간을 뺀 시간만큼만
     * 잠들도록" — 이 센서는 측정 자체가 오래 걸려서(수 초), 측정시간을 안 빼면 전체 주기가
     * (측정시간 + sleep_sec)로 밀림. 측정을 안 한 사이클(due_for_measurement==false)은
     * measurement_elapsed_ms==0이라 자연히 그대로 sleep_sec만큼 잠 */
    uint32_t measurement_elapsed_sec = measurement_elapsed_ms / 1000;
    uint32_t actual_sleep_sec = (sleep_sec > measurement_elapsed_sec)
                                     ? sleep_sec - measurement_elapsed_sec : 0;

    esp_now_node_note_sleep_entry();
    ESP_LOGI(TAG, "딥슬립 진입: %us 후 웨이크(원래 %us, 측정에 %us 씀)",
             (unsigned)actual_sleep_sec, (unsigned)sleep_sec, (unsigned)measurement_elapsed_sec);
    esp_sleep_enable_timer_wakeup((uint64_t)actual_sleep_sec * 1000000ULL);
    esp_deep_sleep_start();
}
