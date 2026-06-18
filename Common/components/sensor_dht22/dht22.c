/**
 * @file    dht22.c
 * @brief   DHT22(AM2302) 온습도 센서 — 단일 GPIO bit-bang 드라이버
 */

#include "dht22.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DHT22_TIMEOUT_US   100

static const char *TAG = "dht22";
static gpio_num_t s_pin = GPIO_NUM_NC;

/* level이 될 때까지 대기, timeout_us 안에 안 되면 false */
static bool wait_level(int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(s_pin) != level) {
        if (esp_timer_get_time() - start > timeout_us) return false;
    }
    return true;
}

void dht22_init(gpio_num_t data_gpio)
{
    s_pin = data_gpio;
    gpio_reset_pin(s_pin);  /* IOMUX를 GPIO 기능으로 강제 전환 (스트래핑 등 잔여 기능 해제) */
    gpio_set_direction(s_pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(s_pin, GPIO_PULLUP_ONLY);
}

bool dht22_read(float *temperature, float *humidity)
{
    if (s_pin == GPIO_NUM_NC) return false;

    uint8_t data[5] = { 0 };

    int fail_step = -1;  /* -1=성공, 0~2=응답 단계, 3~42=비트 인덱스+3 */

    /* 시작 신호: >=1ms LOW → 짧은 HIGH → 입력 전환
     * 호스트가 라인을 놓아준 시점부터 응답 polling까지 인터럽트에 의해
     * 끼어들 틈이 생기면 DHT22의 짧은 응답 윈도우를 통째로 놓친다.
     * 그래서 시작 신호 단계부터 인터럽트를 꺼서 끝까지 한 번에 처리한다.
     * (vTaskDelay는 인터럽트/스케줄러가 필요하므로 busy-wait로 대체) */
    portDISABLE_INTERRUPTS();

    gpio_set_direction(s_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(s_pin, 0);
    esp_rom_delay_us(2000);
    gpio_set_level(s_pin, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(s_pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(s_pin, GPIO_PULLUP_ONLY);

    /* 센서 응답: LOW 80us → HIGH 80us */
    if (!wait_level(0, DHT22_TIMEOUT_US)) { fail_step = 0; goto fail; }
    if (!wait_level(1, DHT22_TIMEOUT_US)) { fail_step = 1; goto fail; }
    if (!wait_level(0, DHT22_TIMEOUT_US)) { fail_step = 2; goto fail; }

    /* 40비트 데이터: 비트마다 LOW 시작 후 HIGH 구간 길이로 0/1 판별 */
    for (int i = 0; i < 40; i++) {
        if (!wait_level(1, DHT22_TIMEOUT_US)) { fail_step = 3 + i; goto fail; }
        int64_t t0 = esp_timer_get_time();
        if (!wait_level(0, DHT22_TIMEOUT_US)) { fail_step = 3 + i; goto fail; }
        int64_t high_us = esp_timer_get_time() - t0;

        data[i / 8] <<= 1;
        if (high_us > 40) data[i / 8] |= 1;
    }

    portENABLE_INTERRUPTS();

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        ESP_LOGW(TAG, "checksum 불일치: data=%02X %02X %02X %02X chk=%02X (계산값 %02X)",
                 data[0], data[1], data[2], data[3], data[4], checksum);
        return false;
    }

    uint16_t hum_raw  = ((uint16_t)data[0] << 8) | data[1];
    uint16_t temp_raw = ((uint16_t)data[2] << 8) | data[3];
    bool     negative = temp_raw & 0x8000;
    temp_raw &= 0x7FFF;

    if (humidity)    *humidity    = hum_raw / 10.0f;
    if (temperature) *temperature = (negative ? -1.0f : 1.0f) * (temp_raw / 10.0f);

    return true;

fail:
    portENABLE_INTERRUPTS();
    if (fail_step <= 2) {
        ESP_LOGW(TAG, "응답 신호 단계 %d에서 타임아웃 (0=초기LOW못봄,1=HIGH못봄,2=시작비트못봄) — 센서 응답 자체가 없음", fail_step);
    } else {
        ESP_LOGW(TAG, "비트 %d 읽다가 타임아웃 (총 40비트 중)", fail_step - 3);
    }
    return false;
}
