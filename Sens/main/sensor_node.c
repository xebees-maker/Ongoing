/**
 * @file    sensor_node.c
 * @brief   LOLIN C3 Pico 헤드리스 센서 노드 — Kconfig에서 고른 센서 1개 + 배터리 + LED 2개
 *
 * sensor-c6.c(레거시 LCD 콤보 앱)의 헤드리스 버전. LVGL/화면이 없으므로 lv_timer 대신
 * esp_timer를 쓰고, DHT22+SCD41 대신 SENS_SENSOR_TYPE으로 고른 센서 하나만 다룬다.
 */

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

#include "bsp_c3_pico.h"
#include "battery.h"
#include "history_log.h"
#include "wifi_dashboard.h"
#include "esp_now_node.h"
#include "status_led.h"

#if CONFIG_SENS_SENSOR_SCD41
    #include "scd41.h"
    #define SENSOR_KIND_CURRENT  SENSOR_KIND_SCD41
    #define SENSOR_CHAN_COUNT    3
    static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
        SENSOR_CHAN_CO2_PPM, SENSOR_CHAN_TEMP_C, SENSOR_CHAN_HUMI_PCT,
    };
    /* 절전모드 1단계 — SCD41만 single-shot 듀티사이클 (다른 센서는 나중에).
     * 확정된 인터벌 후보(추후 Cntl UI 피커에서 이 중 하나로 지정): 5초/15초/1분/3분/10분,
     * 기본 15초 — SCD41 single-shot 자체 소요시간(5초)의 배수라 1초 폴링 타이머와 딱 맞음.
     * (기존 스펙엔 10초/30초였으나 5초 배수로 재조정 — project_sens_sleep_mode_spec 메모리 참고)
     * 이번 패스는 기본값 15초 고정, Cntl 설정 push 프로토콜은 다음 작업. */
    #define SCD41_CYCLE_MS        15000
    static bool     s_scd41_pending     = false;
    static uint32_t s_scd41_trigger_ms  = 0;
#elif CONFIG_SENS_SENSOR_DHT22
    #include "dht22.h"
    #define SENSOR_KIND_CURRENT  SENSOR_KIND_DHT22
    #define SENSOR_CHAN_COUNT    2
    static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
        SENSOR_CHAN_TEMP_C, SENSOR_CHAN_HUMI_PCT,
    };
#elif CONFIG_SENS_SENSOR_SHT45
    #include "sht4x.h"
    #define SENSOR_KIND_CURRENT  SENSOR_KIND_SHT45
    #define SENSOR_CHAN_COUNT    2
    static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
        SENSOR_CHAN_TEMP_C, SENSOR_CHAN_HUMI_PCT,
    };
#elif CONFIG_SENS_SENSOR_SHT40
    #include "sht4x.h"
    #define SENSOR_KIND_CURRENT  SENSOR_KIND_SHT40
    #define SENSOR_CHAN_COUNT    2
    static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
        SENSOR_CHAN_TEMP_C, SENSOR_CHAN_HUMI_PCT,
    };
#else
    #error "SENS_SENSOR_TYPE을 골라야 함 (idf.py menuconfig > Sens Sensor Node)"
#endif

static const char *TAG = "sensor_node";

#define SAMPLE_MS         3000    /* DHT22 최소 호출 간격(2초) 대비 여유 — 모든 센서 타입 공용 */
#define HISTORY_TIMER_MS  (HISTORY_TICK_SEC * 1000)
#define SENSOR_STALE_MS   20000U  /* 마지막 성공 읽기 이후 이만큼 지나야 "값 없음"으로 표시 */

#define BATT_SAMPLE_COUNT  11   /* Cntl ui_dashboard.c/sensor-c6.c와 동일 이동평균 윈도우 */
#define BATT_TREND_SAMPLES      20
#define BATT_PLATEAU_BAND_MV    6
#define BATT_PLATEAU_MIN_MV     3900.0f
#define BATT_FULL_MV_DEFAULT    4020.0f
#define NVS_NS_BATTCAL           "battcal"
#define NVS_KEY_FULL_MV          "full_mv"

static float    s_last_val[SENSOR_CHAN_COUNT] = { 0 };
static bool     s_display_ok  = false;
static uint32_t s_last_ok_ms  = 0;  /* 0 = 부팅 후 아직 한 번도 성공 안 함 */

static int   s_last_batt_pct = 0;
static bool  s_last_batt_ok  = false;
static bool  s_last_powered  = false;

static int s_batt_queue[BATT_SAMPLE_COUNT];
static int s_batt_queue_head  = 0;
static int s_batt_queue_count = 0;

static int s_batt_trend[BATT_TREND_SAMPLES];
static int s_batt_trend_head  = 0;
static int s_batt_trend_count = 0;

static float s_full_mv           = BATT_FULL_MV_DEFAULT;
static int   s_full_mv_persisted = 0;

static adc_oneshot_unit_handle_t s_vin_adc = NULL;

static bool sensor_init(void)
{
#if CONFIG_SENS_SENSOR_SCD41
    if (!scd41_init(BSP_C3_I2C_PORT, BSP_C3_I2C_SDA, BSP_C3_I2C_SCL)) return false;
    /* scd41_init()은 항상 continuous 모드로 시작함 — single-shot 듀티사이클을 쓰려면
     * 꺼야 함(둘이 동시에 돌면 안 됨). NACK이어도 무시: 이미 정지 상태였다는 뜻일 뿐. */
    scd41_stop_periodic_measurement();
    return true;
#elif CONFIG_SENS_SENSOR_DHT22
    dht22_init(BSP_C3_DHT22_PIN);
    return true;
#else  /* SHT45 / SHT40 */
    return sht4x_init(BSP_C3_I2C_PORT, BSP_C3_I2C_SDA, BSP_C3_I2C_SCL);
#endif
}

static bool sensor_read(float out[SENSOR_CHAN_COUNT])
{
#if CONFIG_SENS_SENSOR_SCD41
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_scd41_pending) {
        if (s_scd41_trigger_ms == 0 || (now_ms - s_scd41_trigger_ms) >= SCD41_CYCLE_MS) {
            if (scd41_trigger_single_shot()) {
                s_scd41_pending    = true;
                s_scd41_trigger_ms = now_ms;
            }
        }
        return false;  /* 이번 tick엔 새 값 없음 — 측정 중이거나 아직 트리거할 때가 아님 */
    }

    int co2 = 0;
    float t = 0.0f, h = 0.0f;
    bool read_ok = false;
    if (!scd41_poll_single_shot(&co2, &t, &h, &read_ok)) {
        return false;  /* 아직 측정 중 */
    }
    s_scd41_pending = false;  /* 이번 사이클 종료(성공/실패 무관) — 다음 tick에 새로 트리거 판단 */
    if (read_ok) { out[0] = (float)co2; out[1] = t; out[2] = h; }
    return read_ok;
#elif CONFIG_SENS_SENSOR_DHT22
    return dht22_read(&out[0], &out[1]);
#else  /* SHT45 / SHT40 */
    return sht4x_read(&out[0], &out[1]);
#endif
}

/* J3 헤더 "V"(VIN) 핀을 외부 100k+100k 분압해서 GPIO1로 읽음 — USB 미연결 시
 * Q1(P-FET)이 VBAT을 거의 그대로 통과, USB 연결 시 D1을 거친 VBUS(~4.6~4.7V)로
 * 전환되므로 배터리 완충 전압보다 확실히 높은 지점에 임계값을 둔다. */
static bool vin_indicates_usb(void)
{
    if (!s_vin_adc) return false;
    int raw = 0;
    if (adc_oneshot_read(s_vin_adc, BSP_C3_VIN_ADC_CHANNEL, &raw) != ESP_OK) return false;
    int mv = (int)((float)raw * 3100.0f / 4095.0f);
    return mv >= BSP_C3_VIN_PRESENT_MV;
}

/* sensor-c6.c의 maybe_learn_full_mv()와 동일 로직 — 전원 공급 중 전압이 오래 평탄하면
 * 그 지점을 "이번 배터리의 완충 전압"으로 학습해서 NVS에 저장 */
static void maybe_learn_full_mv(bool powered, int avg_mv)
{
    if (!powered) return;
    if (s_batt_trend_count < BATT_TREND_SAMPLES) return;
    if ((float)avg_mv < BATT_PLATEAU_MIN_MV) return;

    int mn = s_batt_trend[0], mx = s_batt_trend[0];
    for (int i = 1; i < BATT_TREND_SAMPLES; i++) {
        if (s_batt_trend[i] < mn) mn = s_batt_trend[i];
        if (s_batt_trend[i] > mx) mx = s_batt_trend[i];
    }
    if (mx - mn > BATT_PLATEAU_BAND_MV) return;
    if (avg_mv == (int)s_full_mv) return;

    s_full_mv = (float)avg_mv;
    battery_set_full_mv(s_full_mv);
    ESP_LOGI(TAG, "배터리 완충 전압 학습: %d mV", avg_mv);

    if (avg_mv != s_full_mv_persisted) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_BATTCAL, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, NVS_KEY_FULL_MV, avg_mv);
            nvs_commit(h);
            nvs_close(h);
            s_full_mv_persisted = avg_mv;
        }
    }
}

static led_pattern_t batt_pct_to_led_pattern(int pct)
{
    /* 60~100%는 배터리가 정상일 때 가장 오래 유지되는 상태라 계속 켜짐 대신 heartbeat로 —
     * Green LED의 paired 상태와 같은 이유(전력) */
    if (pct >= 60) return LED_PATTERN_HEARTBEAT;
    if (pct >= 20) return LED_PATTERN_HEARTBEAT_FAST;
    return LED_PATTERN_HEARTBEAT_URGENT;
}

static void sample_cb(void *arg)
{
    (void)arg;

    float vals[SENSOR_CHAN_COUNT];
    bool ok = sensor_read(vals);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (ok) {
        for (int i = 0; i < SENSOR_CHAN_COUNT; i++) s_last_val[i] = vals[i];
        s_last_ok_ms = now_ms;
    }
    s_display_ok = (s_last_ok_ms != 0) && (now_ms - s_last_ok_ms) < SENSOR_STALE_MS;

    int batt_mv = battery_read_mv();
    s_last_batt_ok = (batt_mv > 0);
    bool powered = vin_indicates_usb();
    s_last_powered = powered;

    if (s_last_batt_ok) {
        s_batt_queue[s_batt_queue_head] = batt_mv;
        s_batt_queue_head = (s_batt_queue_head + 1) % BATT_SAMPLE_COUNT;
        if (s_batt_queue_count < BATT_SAMPLE_COUNT) s_batt_queue_count++;

        int sum = 0;
        for (int i = 0; i < s_batt_queue_count; i++) sum += s_batt_queue[i];
        int avg_mv = sum / s_batt_queue_count;

        s_batt_trend[s_batt_trend_head] = avg_mv;
        s_batt_trend_head = (s_batt_trend_head + 1) % BATT_TREND_SAMPLES;
        if (s_batt_trend_count < BATT_TREND_SAMPLES) s_batt_trend_count++;

        maybe_learn_full_mv(powered, avg_mv);
        s_last_batt_pct = battery_mv_to_pct(avg_mv);
    }

    /* 충전 중(USB 전원)이 최우선 — 센서 이상이어도 지금 당장 배선/센서를 만지고 있을
     * 가능성이 높은 상태라 "충전 중"이 더 유용한 정보. 그다음 센서 이상(stale), 마지막
     * 배터리% — 센서가 복구되거나 충전이 끝나면 자동으로 하위 패턴으로 되돌아감 */
    if (powered) {
        status_led_set_pattern(BSP_C3_LED_BLUE, LED_PATTERN_ON);
    } else if (!s_display_ok) {
        status_led_set_pattern(BSP_C3_LED_BLUE, LED_PATTERN_BURST_TRIPLE);
    } else if (s_last_batt_ok) {
        status_led_set_pattern(BSP_C3_LED_BLUE, batt_pct_to_led_pattern(s_last_batt_pct));
    }

    uint8_t chan_ok[SENSOR_CHAN_COUNT];
    for (int i = 0; i < SENSOR_CHAN_COUNT; i++) chan_ok[i] = s_display_ok;

    esp_now_node_set_readings(SENSOR_KIND_CURRENT, SENSOR_CHAN_COUNT, s_chan_types, chan_ok, s_last_val,
                               s_last_batt_ok, s_last_batt_pct, s_last_powered);
    wifi_dashboard_set_readings(SENSOR_KIND_CURRENT, SENSOR_CHAN_COUNT, s_chan_types, chan_ok, s_last_val,
                                 s_last_batt_pct, s_last_batt_ok, s_last_powered);
}

static void history_tick_cb(void *arg)
{
    (void)arg;

    if (s_display_ok) {
        for (int i = 0; i < SENSOR_CHAN_COUNT; i++) {
            history_log_record((history_metric_t)(HISTORY_METRIC_CH0 + i), s_last_val[i]);
        }
    }
    if (s_last_batt_ok) {
        history_log_record(HISTORY_METRIC_BATT_PCT, (float)s_last_batt_pct);
    }
    history_log_tick_commit();
}

void app_main(void)
{
#if CONFIG_SENS_SENSOR_SCD41
    /* Automatic Light Sleep — PM 서브시스템이 FreeRTOS idle 구간마다 알아서 light sleep에
     * 들어갔다 나옴. 기존 esp_timer 콜백(샘플링/ESP-NOW/LED heartbeat) 구조는 안 건드림 —
     * DFS(주파수 스케일링)는 안 하고 light sleep만 켬(min==max). 다른 센서 빌드는 이번
     * 패스 범위 밖이라 그대로 둠. */
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 160,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));
#endif

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
            ESP_LOGI(TAG, "저장된 배터리 완충 전압 불러옴: %d mV", (int)learned_mv);
        }
        nvs_close(batt_nvs);
    }

    ESP_ERROR_CHECK(bsp_c3_pico_init());

    history_log_init();
#if CONFIG_SENS_SENSOR_SCD41
    history_log_set_scale(HISTORY_METRIC_CH0, 1.0f);  /* CH0=CO2 — *10이면 int16 범위 초과 */
#endif
    if (history_log_now() < 1700000000) {  /* 2023년 이전 = 아직 시각 미주입 */
        struct tm seed_tm = {
            .tm_year = 2026 - 1900, .tm_mon = 5, .tm_mday = 18,
            .tm_hour = 12, .tm_min = 0, .tm_sec = 0,
        };
        history_log_set_time(mktime(&seed_tm));
    }

    bool sensor_ok = sensor_init();
    if (!sensor_ok) {
        ESP_LOGW(TAG, "센서 초기화 실패 — 연결 확인 필요(계속 재시도됨)");
    }

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

    status_led_init(BSP_C3_LED_BLUE);
    status_led_set_pattern(BSP_C3_LED_BLUE, LED_PATTERN_OFF);  /* 첫 배터리 샘플 전까지 */

    wifi_dashboard_init();
    esp_now_node_set_status_led(BSP_C3_LED_GREEN);
    esp_now_node_init();
#if CONFIG_SENS_SENSOR_SCD41
    /* 센서를 15초에 한 번만 재는데 라디오가 1초마다 계속 쏘면 절전 효과가 없음 —
     * SCD41 절전모드 경로만 전송 주기를 샘플 주기(SCD41_CYCLE_MS)에 맞춤 */
    esp_now_node_set_data_period_ms(SCD41_CYCLE_MS);
#endif

    const esp_timer_create_args_t sample_args = { .callback = sample_cb, .name = "sample" };
    esp_timer_handle_t sample_timer;
    ESP_ERROR_CHECK(esp_timer_create(&sample_args, &sample_timer));
#if CONFIG_SENS_SENSOR_SCD41
    /* SCD41의 single-shot 트리거/폴링 상태머신이 1초 단위 배수(5초 측정시간, 15초 주기)로
     * 설계돼 있어서, 공용 SAMPLE_MS(3000, DHT22 등 다른 센서용)보다 촘촘한 1초 틱이 필요 —
     * 안 그러면 3초 배수가 아닌 경계에서 최대 2초까지 밀리는 오차가 생김(실측 확인됨) */
    ESP_ERROR_CHECK(esp_timer_start_periodic(sample_timer, 1000 * 1000));
#else
    ESP_ERROR_CHECK(esp_timer_start_periodic(sample_timer, SAMPLE_MS * 1000));
#endif

    const esp_timer_create_args_t history_args = { .callback = history_tick_cb, .name = "history_tick" };
    esp_timer_handle_t history_timer;
    ESP_ERROR_CHECK(esp_timer_create(&history_args, &history_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(history_timer, HISTORY_TIMER_MS * 1000));

    ESP_LOGI(TAG, "헤드리스 센서 노드 준비 완료 (kind=%d, channels=%d)", SENSOR_KIND_CURRENT, SENSOR_CHAN_COUNT);
}
