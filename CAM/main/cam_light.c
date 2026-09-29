/**
 * @file    cam_light.c
 * @brief   cam_light.h 구현 — GPIO44 = LED용 MOSFET(IRLZ44N) 게이트
 *
 * 2026-09-29(사용자 배선) — 스키매틱(ESP32-S3-CAM-XXXX): 두 번째 SH1.0 4핀 단자(J11)의 ESP_RXD가 U0RXD = GPIO44에
 * 직결. 콘솔은 USB-Serial-JTAG라 UART0은 안 씀 — GPIO44를 일반 출력(푸시풀 3.3V)으로 써서 게이트를 바로 구동.
 * 이 선은 SD 슬롯 D0에도 이어져 있고 10kΩ 풀업(R31~R36)이 있어서, 전원이 들어온 뒤 펌웨어가 LOW로 쓰기 전까지는
 * HIGH(LED 켜짐) — 그래서 app_main 맨 앞에서 초기화함.
 * (PCF8574 방식은 HIGH 출력이 약해 FET를 확실히 켜지 못해 버림)
 */
#include "cam_light.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "SYS";

#define CAM_LIGHT_GPIO GPIO_NUM_44

static bool s_ready = false;
static bool s_on = false;

bool cam_light_init(void)
{
    gpio_set_level(CAM_LIGHT_GPIO, 0);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CAM_LIGHT_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Capture light: GPIO%d config failed", CAM_LIGHT_GPIO);
        return false;
    }
    gpio_set_level(CAM_LIGHT_GPIO, 0);
    s_ready = true;
    s_on = false;
    ESP_LOGI(TAG, "Capture light: GPIO%d, off", CAM_LIGHT_GPIO);
    return true;
}

void cam_light_set(bool on)
{
    if (!s_ready) return;
    gpio_set_level(CAM_LIGHT_GPIO, on ? 1 : 0);
    if (on != s_on) ESP_LOGD(TAG, "Capture light %s", on ? "on" : "off");
    s_on = on;
}

bool cam_light_present(void)
{
    return s_ready;
}
