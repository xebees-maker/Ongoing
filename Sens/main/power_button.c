/**
 * @file    power_button.c
 * @brief   power_button.h 구현 — 2026-11-13
 */
#include "power_button.h"
#include "bsp_c3_pico.h"
#include "rgb_status.h"
#include "rwdt_guard.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "PWR_BTN";

#define PIN            BSP_C3_POWER_BUTTON_PIN
#define OFF_MAGIC      0x0FF5E115u
#define DEBOUNCE_MS    50      /* 떨림 거르기(사용자 결정 11-13) — 이만큼 같은 레벨이어야 누름·놓음으로 봄 */

/* 끄기가 걸림 — 끄기는 다시 시작(esp_restart)을 거치므로 RTC_DATA(다시 시작하면 초기화됨)가 아니라 NOINIT. 전원을 넣으면 쓰레기 값이라 표시값으로 확인 */
static RTC_NOINIT_ATTR uint32_t s_off_magic;
/* 이번 잠에서 깰 시각(RTC 시계) — 짧게 눌러 깼을 때 그때까지 다시 잠 */
static RTC_DATA_ATTR uint64_t s_wake_due_rtc_us = 0;

static SemaphoreHandle_t s_edge = NULL;

static bool pressed(void)
{
    return gpio_get_level(PIN) == 0;
}

/* 눌린 채로 간 시간(ms), limit_ms에서 멈춤 — 부팅 때만(감시 태스크 전). DEBOUNCE_MS보다 짧게 떨어진 건 떨림으로 보고 계속 셈 */
static uint32_t held_ms(uint32_t limit_ms)
{
    uint32_t ms = 0, up_ms = 0;
    while (ms < limit_ms && up_ms < DEBOUNCE_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        ms += 10;
        up_ms = pressed() ? 0 : up_ms + 10;
    }
    return ms;
}

/* 놓을 때까지(DEBOUNCE_MS 동안 계속 떨어져 있음) — 누른 채로 잠들면 바로 다시 깸 */
static void wait_release(void)
{
    uint32_t up_ms = 0;
    while (up_ms < DEBOUNCE_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        up_ms = pressed() ? 0 : up_ms + 10;
    }
}

/* 엣지 뒤 떨림이 가라앉은 레벨 — 그동안 들어온 엣지는 버림 */
static bool settled_pressed(void)
{
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    xSemaphoreTake(s_edge, 0);
    return pressed();
}

static void request_off(void)
{
    ESP_LOGW(TAG, "Long press - power off");
    rgb_status_blink(RGB_STATUS_RED, 5);
    s_off_magic = OFF_MAGIC;
    wait_release();
    rgb_status_flush(2000);
    esp_restart();   /* 전원 켤 때와 같은 센서 초기화를 거쳐 power_button_off_if_requested()에서 잠 */
}

static void sleep_until(uint64_t sleep_us)
{
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (sleep_us) esp_sleep_enable_timer_wakeup(sleep_us);
    esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

static void IRAM_ATTR on_edge(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_edge, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/* 깨어 있는 동안 — 눌림(엣지, 떨림 거른 뒤)이 오면 3초 안에 놓는지 봄. 놓지 않으면 끔 */
static void monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_edge, portMAX_DELAY);
        TickType_t start = xTaskGetTickCount();
        if (!settled_pressed()) continue;
        bool released = false;
        for (;;) {
            TickType_t el = xTaskGetTickCount() - start;
            if (el >= pdMS_TO_TICKS(POWER_BUTTON_LONG_MS)) break;
            if (xSemaphoreTake(s_edge, pdMS_TO_TICKS(POWER_BUTTON_LONG_MS) - el) == pdTRUE && !settled_pressed()) {
                released = true;
                break;
            }
        }
        if (!released && pressed()) request_off();
        if (released) {
            ESP_LOGI(TAG, "Short press - on");
            rgb_status_blink(RGB_STATUS_GREEN, 3);
        }
    }
}

static void monitor_start(void)
{
    s_edge = xSemaphoreCreateBinary();
    if (!s_edge) return;
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(e)); return; }
    gpio_set_intr_type(PIN, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN, on_edge, NULL);
    xTaskCreate(monitor_task, "pwr_btn", 2560, NULL, 5, NULL);
}

void power_button_boot(void)
{
    /* 2026-11-13 — 잠들기 전 IDF가 깨우기 핀에 건 hold(RTC 핀이라 RTC hold)는 깨어나도 풀리지 않음 — 풀고 설정 */
    gpio_hold_dis(PIN);
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);

    esp_reset_reason_t rr = esp_reset_reason();
    bool button_wake = rr == ESP_RST_DEEPSLEEP && (esp_sleep_get_wakeup_causes() & (1U << ESP_SLEEP_WAKEUP_GPIO));
    if (s_off_magic == OFF_MAGIC && rr != ESP_RST_SW && rr != ESP_RST_DEEPSLEEP) s_off_magic = 0;   /* 전원을 새로 넣음 등 — 켬 */

    if (s_off_magic == OFF_MAGIC) {
        if (!button_wake) return;   /* 끄기 다시 시작 — 센서 초기화 뒤 power_button_off_if_requested()에서 잠 */
        s_off_magic = 0;
        ESP_LOGW(TAG, "Button press - power on");
        rgb_status_blink(RGB_STATUS_GREEN, 3);
    } else if (button_wake) {
        if (held_ms(POWER_BUTTON_LONG_MS) >= POWER_BUTTON_LONG_MS) request_off();
        rgb_status_blink(RGB_STATUS_GREEN, 3);   /* 켜져 있음 */
        int64_t left = (int64_t)s_wake_due_rtc_us - (int64_t)esp_rtc_get_time_us();
        if (s_wake_due_rtc_us && left > 1000000) {   /* 1초도 안 남았으면 그냥 이번에 깨어 있음 */
            wait_release();
            rgb_status_flush(1500);
            ESP_LOGI(TAG, "Short press while asleep - back to sleep for %lldms", (long long)(left / 1000));
            sleep_until((uint64_t)left);   /* RWDT는 원래 잠의 예산 그대로(이 함수는 rwdt_guard_arm 앞) */
        }
    }
    monitor_start();
}

void power_button_off_if_requested(void)
{
    if (s_off_magic != OFF_MAGIC) return;
    wait_release();
    rgb_status_flush(2000);
    rwdt_guard_disarm();   /* 꺼진 동안 RTC 워치독이 리셋하면 켜져 버림 */
    ESP_LOGW(TAG, "Powered off - deep sleep until button press");
    sleep_until(0);
}

void power_button_prepare_sleep(uint64_t sleep_us)
{
    s_wake_due_rtc_us = esp_rtc_get_time_us() + sleep_us;
    esp_err_t e = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (e != ESP_OK) ESP_LOGE(TAG, "button wake not set: %s", esp_err_to_name(e));
}
