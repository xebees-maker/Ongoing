/**
 * @file    cam_light.c
 * @brief   cam_light.h 구현 — PCF8574AT P0 = LED 신호
 */
#include "cam_light.h"
#include "bsp_esp32s3_cam.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "SYS";

/* A2~A0 = GND. 칩 표기는 PCF8574AT로 보였지만 실제 버스 스캔(2026-09-29)에서 0x20으로 응답 — PCF8574(T) 주소표
 * (0x20~0x27). 이 버스엔 이미 ES8311(0x18), CH32V003(0x24), 카메라(0x3C)가 있어서 0x24(A2만 HIGH)는 쓰면 안 됨.
 * 스캔에서 0x40도 응답했는데 정체는 미확인 */
#define CAM_LIGHT_PCF8574_ADDR 0x20
#define CAM_LIGHT_PIN_MASK     0x01  /* P0 */
#define CAM_LIGHT_I2C_TIMEOUT_MS 50

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_on = false;

/* PCF8574는 레지스터 없이 바이트 하나를 쓰면 P7..P0에 그대로 나감(1 = 약한 풀업 HIGH, 0 = LOW). 안 쓰는 핀은
 * 전원 기본값과 같은 1로 둠 */
static bool write_outputs(bool on)
{
    uint8_t out = (uint8_t)(0xFF & ~CAM_LIGHT_PIN_MASK) | (on ? CAM_LIGHT_PIN_MASK : 0);
    return i2c_master_transmit(s_dev, &out, 1, CAM_LIGHT_I2C_TIMEOUT_MS) == ESP_OK;
}

bool cam_light_init(void)
{
    i2c_master_bus_handle_t bus = bsp_esp32s3_cam_get_i2c_bus();
    if (!bus) return false;
    if (i2c_master_probe(bus, CAM_LIGHT_PCF8574_ADDR, CAM_LIGHT_I2C_TIMEOUT_MS) != ESP_OK) {
        /* 배선/주소 확인용 — 버스에서 응답하는 주소를 전부 남김(원래 있는 것: 0x18 ES8311, 0x24 CH32V003, 카메라 0x3C는
         * 센서가 대기(PWDN) 중이라 안 보일 수 있음) */
        char found[96] = "";
        int len = 0;
        for (uint16_t a = 0x08; a < 0x78 && len < (int)sizeof(found) - 6; a++) {
            if (i2c_master_probe(bus, a, CAM_LIGHT_I2C_TIMEOUT_MS) == ESP_OK) {
                len += snprintf(found + len, sizeof(found) - len, " 0x%02X", a);
            }
        }
        ESP_LOGI(TAG, "Capture light: no PCF8574 at 0x%02X - running without light (bus:%s)", CAM_LIGHT_PCF8574_ADDR,
                 len ? found : " none");
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CAM_LIGHT_PCF8574_ADDR,
        .scl_speed_hz    = BSP_CAM_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        ESP_LOGW(TAG, "Capture light: PCF8574 add failed");
        return false;
    }
    bool ok = write_outputs(false);
    s_on = false;
    ESP_LOGI(TAG, "Capture light: PCF8574 at 0x%02X, P0 off (%s)", CAM_LIGHT_PCF8574_ADDR, ok ? "ok" : "write failed");
    return true;
}

void cam_light_set(bool on)
{
    if (!s_dev) return;
    if (!write_outputs(on)) {
        ESP_LOGW(TAG, "Capture light: %s write failed", on ? "on" : "off");
        return;
    }
    if (on != s_on) ESP_LOGD(TAG, "Capture light %s", on ? "on" : "off");
    s_on = on;
}

bool cam_light_present(void)
{
    return s_dev != NULL;
}
