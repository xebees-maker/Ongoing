/**
 * @file    sensor-c6.c
 * @brief   ESP32-C6 센서 노드 — DHT22 + SCD41 (브링업 단계, 영문 라벨)
 */

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_c6.h"
#include "dht22.h"
#include "scd41.h"

static const char *TAG = "sensor-c6";

#define DHT22_PIN        GPIO_NUM_8
#define SCD41_I2C_PORT   1
#define SCD41_SDA_PIN    GPIO_NUM_6
#define SCD41_SCL_PIN    GPIO_NUM_7

#define SAMPLE_MS        3000   /* DHT22 최소 호출 간격 2초 이상 — 여유 두고 3초 (진단용) */

static lv_obj_t *s_temp_lbl     = NULL;
static lv_obj_t *s_humi_lbl     = NULL;
static lv_obj_t *s_co2_lbl      = NULL;
static lv_obj_t *s_scd_temp_lbl = NULL;
static lv_obj_t *s_scd_humi_lbl = NULL;
static lv_obj_t *s_batt_lbl     = NULL;

static adc_oneshot_unit_handle_t s_adc  = NULL;
static adc_cali_handle_t         s_cali = NULL;

static void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t cfg = { .unit_id = BSP_BATTERY_ADC_UNIT };
    if (adc_oneshot_new_unit(&cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC init failed");
        return;
    }
    adc_oneshot_chan_cfg_t ch_cfg = {
        .atten    = BSP_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(s_adc, BSP_BATTERY_ADC_CHANNEL, &ch_cfg);

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BSP_BATTERY_ADC_UNIT,
        .chan     = BSP_BATTERY_ADC_CHANNEL,
        .atten    = BSP_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC calibration unavailable — raw fallback");
        s_cali = NULL;
    }
}

static int battery_read_mv(void)
{
    if (!s_adc) return 0;
    int raw = 0;
    if (adc_oneshot_read(s_adc, BSP_BATTERY_ADC_CHANNEL, &raw) != ESP_OK) return 0;
    int mv = 0;
    if (s_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv);
    else        mv = (int)((float)raw * 3100.0f / 4095.0f);
    return (int)(mv * BSP_BATTERY_DIV);
}

static void sample_cb(lv_timer_t *t)
{
    (void)t;

    float temp = 0.0f, humi = 0.0f;
    if (dht22_read(&temp, &humi)) {
        int t10 = (int)(temp * 10.0f + (temp >= 0.0f ? 0.5f : -0.5f));
        bool t_neg = t10 < 0;
        if (t_neg) t10 = -t10;
        lv_label_set_text_fmt(s_temp_lbl, "TEMP  %s%d.%d C", t_neg ? "-" : "", t10 / 10, t10 % 10);
        lv_label_set_text_fmt(s_humi_lbl, "HUMI  %d %%", (int)(humi + 0.5f));
    } else {
        lv_label_set_text(s_temp_lbl, "TEMP  --");
        lv_label_set_text(s_humi_lbl, "HUMI  --");
    }

    int co2 = 0;
    float scd_temp = 0.0f, scd_humi = 0.0f;
    if (scd41_read(&co2, &scd_temp, &scd_humi)) {
        lv_label_set_text_fmt(s_co2_lbl, "CO2   %d ppm", co2);

        int st10 = (int)(scd_temp * 10.0f + (scd_temp >= 0.0f ? 0.5f : -0.5f));
        bool st_neg = st10 < 0;
        if (st_neg) st10 = -st10;
        lv_label_set_text_fmt(s_scd_temp_lbl, "SCD-T %s%d.%d C", st_neg ? "-" : "", st10 / 10, st10 % 10);
        lv_label_set_text_fmt(s_scd_humi_lbl, "SCD-H %d %%", (int)(scd_humi + 0.5f));
    }

    int batt_mv = battery_read_mv();
    lv_label_set_text_fmt(s_batt_lbl, "BATT  %d mV", batt_mv);
}

static lv_obj_t *make_row(lv_obj_t *parent, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "--");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 8, y);
    return lbl;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(bsp_board_init());

    dht22_init(DHT22_PIN);
    bool scd41_ok = scd41_init(SCD41_I2C_PORT, SCD41_SDA_PIN, SCD41_SCL_PIN);
    battery_init();

    if (bsp_lvgl_lock(BSP_MUTEX_WAIT_DEFAULT)) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x1A1A2E), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

        lv_obj_t *title = lv_label_create(scr);
        lv_label_set_text(title, "C6 Sensor Node");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        s_temp_lbl     = make_row(scr, 50);
        s_humi_lbl     = make_row(scr, 90);
        s_co2_lbl      = make_row(scr, 130);
        s_scd_temp_lbl = make_row(scr, 170);
        s_scd_humi_lbl = make_row(scr, 210);
        s_batt_lbl     = make_row(scr, 250);

        lv_obj_set_style_text_color(s_temp_lbl,     lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(s_humi_lbl,     lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(s_co2_lbl,      lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(s_scd_temp_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(s_scd_humi_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(s_batt_lbl,     lv_color_hex(0xFFFFFF), 0);

        if (!scd41_ok) {
            lv_label_set_text(s_co2_lbl, "CO2   no sensor");
        }

        lv_timer_create(sample_cb, SAMPLE_MS, NULL);
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "C6 sensor node ready");
}
