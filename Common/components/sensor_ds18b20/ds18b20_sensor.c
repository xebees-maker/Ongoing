/**
 * @file    ds18b20_sensor.c
 * @brief   ds18b20_sensor.h 구현
 */
#include "ds18b20_sensor.h"

#include "onewire_bus.h"
#include "ds18b20.h"
#include "esp_log.h"

static const char *TAG = "SENS";

static onewire_bus_handle_t s_bus = NULL;
static ds18b20_device_handle_t s_dev = NULL;

/* 버스에 센서가 하나뿐이라 ROM 검색 없이(skip ROM) 바로 장치로 잡음 */
static bool attach_device(void)
{
    if (s_dev) return true;
    ds18b20_config_t cfg = {};  /* 0.4.0에선 필드 없는 빈 구조체 */
    if (ds18b20_new_device_from_bus(s_bus, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return false;
    }
    return true;
}

bool ds18b20_sensor_init(gpio_num_t data_gpio)
{
    onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = data_gpio,
        .flags.en_pull_up = false,  /* 중간 보드에 풀업이 있음(사용자 확인) */
    };
    onewire_bus_rmt_config_t rmt_cfg = {
        .max_rx_bytes = 10,  /* 스크래치패드 9바이트 + 여유 */
    };
    if (onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "DS18B20 1-Wire bus create failed (GPIO%d)", data_gpio);
        s_bus = NULL;
        return false;
    }
    if (!attach_device()) {
        ESP_LOGW(TAG, "DS18B20 not found on GPIO%d", data_gpio);
        return false;
    }
    ESP_LOGI(TAG, "DS18B20 1-Wire OK  DATA=%d", data_gpio);
    return true;
}

ds18b20_sensor_result_t ds18b20_sensor_read(float *temp_c)
{
    if (!s_bus) return DS18B20_SENSOR_ERR_READ;
    if (!attach_device()) return DS18B20_SENSOR_ERR_NO_DEVICE;

    esp_err_t err = ds18b20_trigger_temperature_conversion(s_dev);
    if (err == ESP_OK) err = ds18b20_get_temperature(s_dev, temp_c);
    if (err == ESP_ERR_NOT_FOUND) return DS18B20_SENSOR_ERR_NO_DEVICE;
    if (err == ESP_ERR_INVALID_CRC) return DS18B20_SENSOR_ERR_CRC;
    if (err != ESP_OK) return DS18B20_SENSOR_ERR_READ;
    /* 85.0°C는 전원 투입 직후 스크래치패드 기본값 — 변환이 실제로 안 됐을 때(전원 부족 등) 그대로 읽힘.
     * Agar 온도로는 나올 수 없는 값이라 실패로 처리 */
    if (*temp_c == 85.0f) return DS18B20_SENSOR_ERR_READ;
    return DS18B20_SENSOR_OK;
}
