/**
 * @file    sht4x.c
 * @brief   SHT45/SHT40 온습도 센서 — I2C 드라이버 (Sensirion 프로토콜)
 */

#include "sht4x.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "sht4x";

#define CMD_MEASURE_HIGH_PRECISION  0xFD
#define I2C_TIMEOUT_MS              1000
#define MEASURE_DELAY_MS            10   /* 고정밀 모드 최대 측정 시간(8.3ms) + 여유 */

static i2c_master_dev_handle_t s_dev = NULL;
static i2c_master_bus_handle_t s_bus = NULL;

/* Sensirion CRC8: poly 0x31, init 0xFF (scd41.c와 동일 규격) */
static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

bool sht4x_init(int i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .i2c_port              = i2c_port,
        .scl_io_num            = scl_gpio,
        .sda_io_num            = sda_gpio,
        .glitch_ignore_cnt     = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed (SDA=%d SCL=%d)", sda_gpio, scl_gpio);
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SHT4X_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed");
        return false;
    }

    ESP_LOGI(TAG, "SHT4x I2C OK  SDA=%d SCL=%d", sda_gpio, scl_gpio);
    return true;
}

bool sht4x_read(float *temperature, float *humidity)
{
    if (!s_dev) return false;

    uint8_t cmd = CMD_MEASURE_HIGH_PRECISION;
    esp_err_t err = i2c_master_transmit(s_dev, &cmd, sizeof(cmd), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "measure cmd 전송 실패: %s", esp_err_to_name(err));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(MEASURE_DELAY_MS));

    uint8_t resp[6] = { 0 };
    err = i2c_master_receive(s_dev, resp, sizeof(resp), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "측정값 수신 실패: %s", esp_err_to_name(err));
        return false;
    }

    if (crc8(&resp[0], 2) != resp[2] || crc8(&resp[3], 2) != resp[5]) {
        ESP_LOGW(TAG, "CRC 불일치");
        return false;
    }

    uint16_t temp_raw = ((uint16_t)resp[0] << 8) | resp[1];
    uint16_t humi_raw = ((uint16_t)resp[3] << 8) | resp[4];

    if (temperature) *temperature = -45.0f + 175.0f * ((float)temp_raw / 65535.0f);
    if (humidity) {
        float rh = -6.0f + 125.0f * ((float)humi_raw / 65535.0f);
        if (rh < 0.0f) rh = 0.0f;
        if (rh > 100.0f) rh = 100.0f;
        *humidity = rh;
    }

    return true;
}
