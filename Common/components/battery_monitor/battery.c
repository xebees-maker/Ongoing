/**
 * @file    battery.c
 * @brief   배터리 전압/잔량(%) 측정 + USB 연결 여부 — Cntl/Sens 공용
 */

#include "battery.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t s_adc  = NULL;
static adc_cali_handle_t         s_cali = NULL;
static battery_config_t          s_cfg;

bool battery_init(const battery_config_t *cfg)
{
    s_cfg = *cfg;

    if (s_cfg.ctrl_gpio != GPIO_NUM_NC) {
        gpio_config_t gcfg = {
            .pin_bit_mask = (1ULL << s_cfg.ctrl_gpio),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&gcfg);
        gpio_set_level(s_cfg.ctrl_gpio, 0);  /* LOW: P-MOSFET ON → 분압기 활성화 */
        ESP_LOGI(TAG, "분압기 게이트 GPIO%d=LOW", s_cfg.ctrl_gpio);
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = s_cfg.adc_unit };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC init failed");
        return false;
    }
    adc_oneshot_chan_cfg_t ch_cfg = {
        .atten    = s_cfg.atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(s_adc, s_cfg.adc_channel, &ch_cfg);

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = s_cfg.adc_unit,
        .chan     = s_cfg.adc_channel,
        .atten    = s_cfg.atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC calibration unavailable — raw fallback");
        s_cali = NULL;
    }

    ESP_LOGI(TAG, "battery ADC ready  unit=%d ch=%d", s_cfg.adc_unit, s_cfg.adc_channel);
    return true;
}

int battery_read_mv(void)
{
    if (!s_adc) return 0;
    int raw = 0;
    if (adc_oneshot_read(s_adc, s_cfg.adc_channel, &raw) != ESP_OK) return 0;
    int mv = 0;
    if (s_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv);
    else        mv = (int)((float)raw * 3100.0f / 4095.0f);
    return (int)((float)mv * s_cfg.divider);
}

int battery_mv_to_pct(int vbat_mv)
{
    int pct = (int)(((float)vbat_mv - s_cfg.empty_mv) / (s_cfg.full_mv - s_cfg.empty_mv) * 100.0f + 0.5f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

int battery_read_pct(void)
{
    return battery_mv_to_pct(battery_read_mv());
}

bool battery_is_usb_connected(void)
{
    return usb_serial_jtag_is_connected();
}

void battery_set_full_mv(float full_mv)
{
    s_cfg.full_mv = full_mv;
}
