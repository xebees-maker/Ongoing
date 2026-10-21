/**
 * @file    cam_light.c
 * @brief   cam_light.h 구현 — GPIO44 = LED용 MOSFET(IRLZ44N) 게이트
 *
 * 2026-09-29(사용자 배선) — 스키매틱(ESP32-S3-CAM-XXXX): 두 번째 SH1.0 4핀 단자(J11)의 ESP_RXD가 U0RXD = GPIO44에
 * 직결. 콘솔은 USB-Serial-JTAG라 UART0은 안 씀 — GPIO44를 일반 출력(푸시풀 3.3V)으로 써서 게이트를 바로 구동.
 * 이 선은 SD 슬롯 D0에도 이어져 있고 10kΩ 풀업(R31~R36)이 있어서, 전원이 들어온 뒤 펌웨어가 LOW로 쓰기 전까지는
 * HIGH(LED 켜짐) — 그래서 app_main 맨 앞에서 초기화함.
 * (PCF8574 방식은 HIGH 출력이 약해 FET를 확실히 켜지 못해 버림)
 *
 * 2026-10-19(사용자 배선 변경 — 깰 때마다 약 1초 켜짐) — 풀업 때문에 부팅·딥슬립·깨는 순간 GPIO44가 HIGH라, LOW에서 켜지게 반전:
 * GPIO44 → Q1(IRLZ44N) G, Q1 D → Q2(LED FET) G + 1MΩ 풀업(3V3), Q1 S → GND. GPIO44 HIGH = Q1 켜짐 = Q2 꺼짐 = LED 꺼짐.
 * 그래서 끔 = HIGH(풀업과 같은 값 — 자는 동안 핀을 놓아도 꺼진 채, 전류는 1MΩ 3.3µA뿐), 켬 = LOW
 */
#include "cam_light.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "SYS";

#define CAM_LIGHT_GPIO GPIO_NUM_44

static bool s_ready = false;
static bool s_on = false;
static int64_t s_on_since_us = 0;   /* 2026-10-20(진단, 임시) — 아래 LEDTIME */

/* 2026-10-19 — LOW에서 켜짐(파일 머리 주석) */
#define CAM_LIGHT_LEVEL_ON  0
#define CAM_LIGHT_LEVEL_OFF 1

bool cam_light_init(void)
{
    gpio_set_level(CAM_LIGHT_GPIO, CAM_LIGHT_LEVEL_OFF);
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
    gpio_set_level(CAM_LIGHT_GPIO, CAM_LIGHT_LEVEL_OFF);
    s_ready = true;
    s_on = false;
    ESP_LOGI(TAG, "Capture light: GPIO%d, off", CAM_LIGHT_GPIO);
    return true;
}

void cam_light_set(bool on)
{
    if (!s_ready) return;
    gpio_set_level(CAM_LIGHT_GPIO, on ? CAM_LIGHT_LEVEL_ON : CAM_LIGHT_LEVEL_OFF);
    if (on != s_on) ESP_LOGD(TAG, "Capture light %s", on ? "on" : "off");
    /* 2026-10-20(진단, 임시 — 밤 시험: LED 켜짐 시간 줄이기·실패 시 안전장치) — 켜져 있던 시간을 남김(깨어난 뒤 몇 ms에 켰는지 포함) */
    if (on && !s_on) s_on_since_us = esp_timer_get_time();
    if (!on && s_on) ESP_LOGI(TAG, "LEDTIME on %lldms (from uptime %lldms)", (long long)((esp_timer_get_time() - s_on_since_us) / 1000),
                              (long long)(s_on_since_us / 1000));
    s_on = on;
}

bool cam_light_present(void)
{
    return s_ready;
}
