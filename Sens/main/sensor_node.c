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

static adc_oneshot_unit_handle_t s_vbus_adc = NULL;

static bool sensor_init(void)
{
#if CONFIG_SENS_SENSOR_SCD41
    return scd41_init(BSP_C3_I2C_PORT, BSP_C3_I2C_SDA, BSP_C3_I2C_SCL);
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
    int co2 = 0;
    float t = 0.0f, h = 0.0f;
    bool ok = scd41_read(&co2, &t, &h);
    if (ok) { out[0] = (float)co2; out[1] = t; out[2] = h; }
    return ok;
#elif CONFIG_SENS_SENSOR_DHT22
    return dht22_read(&out[0], &out[1]);
#else  /* SHT45 / SHT40 */
    return sht4x_read(&out[0], &out[1]);
#endif
}

static bool vbus_is_present(void)
{
    if (!s_vbus_adc) return false;
    int raw = 0;
    if (adc_oneshot_read(s_vbus_adc, BSP_C3_VBUS_ADC_CHANNEL, &raw) != ESP_OK) return false;
    int mv = (int)((float)raw * 3100.0f / 4095.0f);
    return mv >= BSP_C3_VBUS_PRESENT_MV;
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
    bool powered = vbus_is_present();
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

    /* 센서 이상(stale)이면 배터리 표시 대신 Blue LED로 "주의" 신호를 잠깐 대신 보여줌 —
     * 센서가 복구되면 자동으로 배터리 패턴으로 되돌아감 */
    if (!s_display_ok) {
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

    s_vbus_adc = battery_get_adc_handle();
    if (s_vbus_adc) {
        adc_oneshot_chan_cfg_t vbus_ch_cfg = {
            .atten    = BSP_C3_VBUS_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_oneshot_config_channel(s_vbus_adc, BSP_C3_VBUS_ADC_CHANNEL, &vbus_ch_cfg);
    }

    status_led_init(BSP_C3_LED_BLUE);
    status_led_set_pattern(BSP_C3_LED_BLUE, LED_PATTERN_OFF);  /* 첫 배터리 샘플 전까지 */

    wifi_dashboard_init();
    esp_now_node_set_status_led(BSP_C3_LED_GREEN);
    esp_now_node_init();

    const esp_timer_create_args_t sample_args = { .callback = sample_cb, .name = "sample" };
    esp_timer_handle_t sample_timer;
    ESP_ERROR_CHECK(esp_timer_create(&sample_args, &sample_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(sample_timer, SAMPLE_MS * 1000));

    const esp_timer_create_args_t history_args = { .callback = history_tick_cb, .name = "history_tick" };
    esp_timer_handle_t history_timer;
    ESP_ERROR_CHECK(esp_timer_create(&history_args, &history_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(history_timer, HISTORY_TIMER_MS * 1000));

    ESP_LOGI(TAG, "헤드리스 센서 노드 준비 완료 (kind=%d, channels=%d)", SENSOR_KIND_CURRENT, SENSOR_CHAN_COUNT);
}
