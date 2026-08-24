#include "io_expander_ch32v003.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ch32v003";

static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_output_shadow = 0;  /* 레지스터는 write-only 느낌이라 마지막 출력값을 들고 있음 */

static esp_err_t write_reg(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = { reg, data };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 0xffff);
}

esp_err_t ch32v003_init(i2c_master_bus_handle_t i2c_bus)
{
    const i2c_device_config_t dev_cfg = {
        .device_address = CH32V003_I2C_ADDR,
        .scl_speed_hz   = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_dev),
                         TAG, "add I2C device failed");

    /* 전부 출력 모드(0xFF) — Waveshare 예제와 동일 */
    ESP_RETURN_ON_ERROR(write_reg(CH32V003_REG_MODE, 0xFF), TAG, "mode reg write failed");
    s_output_shadow = 0;
    return ESP_OK;
}

void ch32v003_set_output(uint8_t pin, bool level)
{
    if (pin > 7) {
        ESP_LOGE(TAG, "set_output: invalid pin %d", pin);
        return;
    }
    if (level) {
        s_output_shadow |= (uint8_t)(1u << pin);
    } else {
        s_output_shadow &= (uint8_t)~(1u << pin);
    }
    esp_err_t err = write_reg(CH32V003_REG_OUTPUT, s_output_shadow);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_output(pin=%d): %s", pin, esp_err_to_name(err));
    }
}

bool ch32v003_get_input(uint8_t pin)
{
    if (pin > 7) {
        ESP_LOGE(TAG, "get_input: invalid pin %d", pin);
        return false;
    }
    uint8_t value = 0;
    esp_err_t err = i2c_master_transmit_receive(s_dev, (uint8_t[]) { CH32V003_REG_INPUT }, 1,
                                                 &value, 1, 0xffff);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_input(pin=%d): %s", pin, esp_err_to_name(err));
        return false;
    }
    return (value >> pin) & 0x01;
}

esp_err_t ch32v003_get_adc(uint16_t *out_raw)
{
    uint8_t buf[2] = { 0 };
    esp_err_t err = i2c_master_transmit_receive(s_dev, (uint8_t[]) { CH32V003_REG_ADC }, 1,
                                                 buf, sizeof(buf), 0xffff);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_adc: %s", esp_err_to_name(err));
        return err;
    }
    /* 리틀엔디안(저바이트 먼저) — Waveshare DEV_I2C_Read_Word() 확인값 */
    *out_raw = (uint16_t)(buf[0] | (buf[1] << 8));
    return ESP_OK;
}
