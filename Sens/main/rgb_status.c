/**
 * @file    rgb_status.c
 * @brief   rgb_status.h 구현 — esp_timer 한 번짜리를 다시 걸며 켜고 끔(LED 갱신은 타이머 태스크에서만)
 */
#include "rgb_status.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rgb_status";

#define RGB_GPIO        7      /* LOLIN C3 Pico 내장 WS2812(sch_c3_pico U3, DI = IO7) */
#define RGB_BRIGHTNESS  32     /* 0~255, 눈부심 방지 */
#define RGB_ON_MS       100
#define RGB_OFF_MS      150
#define RGB_SEARCH_MS   100    /* 찾는 중 깜빡임 켜짐·꺼짐 */
#define RGB_QUEUE       8

typedef struct { uint8_t color; uint8_t times; } rgb_req_t;

static led_strip_handle_t s_strip = NULL;
static esp_timer_handle_t s_tmr = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static rgb_req_t s_q[RGB_QUEUE];
static uint8_t s_qh = 0, s_qn = 0;          /* 줄 머리·개수 */
static uint8_t s_cur_color = 0;
static int s_cur_phases = 0;                 /* 남은 켜짐/꺼짐 단계 수 */
static bool s_search = false, s_search_on = false;
static volatile bool s_running = false;      /* 타이머가 걸려 있음 */

static void led_set(int color, bool on)
{
    if (!s_strip) return;
    if (!on) { led_strip_clear(s_strip); led_strip_refresh(s_strip); return; }
    uint8_t r = (color == RGB_STATUS_RED || color == RGB_STATUS_YELLOW) ? RGB_BRIGHTNESS : 0;
    uint8_t g = (color == RGB_STATUS_GREEN || color == RGB_STATUS_YELLOW) ? RGB_BRIGHTNESS : 0;
    uint8_t b = color == RGB_STATUS_BLUE ? RGB_BRIGHTNESS : 0;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void step(void *arg)
{
    (void)arg;
    int color = -1;
    bool on = false;
    uint32_t next_ms = 0;
    taskENTER_CRITICAL(&s_mux);
    if (s_cur_phases == 0 && s_qn > 0) {   /* 다음 요청 */
        s_cur_color = s_q[s_qh].color;
        s_cur_phases = s_q[s_qh].times * 2;
        s_qh = (uint8_t)((s_qh + 1) % RGB_QUEUE);
        s_qn--;
        s_search_on = false;
    }
    if (s_cur_phases > 0) {
        on = (s_cur_phases % 2) == 0;      /* 짝수 = 켜짐 단계 */
        color = s_cur_color;
        next_ms = on ? RGB_ON_MS : RGB_OFF_MS;
        s_cur_phases--;
    } else if (s_search) {
        s_search_on = !s_search_on;
        on = s_search_on;
        color = RGB_STATUS_BLUE;
        next_ms = RGB_SEARCH_MS;
    } else {
        s_running = false;                  /* 할 일 없음 — 끄고 멈춤 */
    }
    taskEXIT_CRITICAL(&s_mux);
    led_set(color, on);
    if (next_ms) esp_timer_start_once(s_tmr, (uint64_t)next_ms * 1000);
}

static bool init_once(void)
{
    if (s_tmr) return true;
    led_strip_config_t cfg = {
        .strip_gpio_num = RGB_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,   /* 이 보드는 RGB 순서(2026-09-06 실기 확인) */
    };
    led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&cfg, &rmt, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 init failed - no status LED");
        s_strip = NULL;
    }
    const esp_timer_create_args_t ta = { .callback = step, .name = "rgb_status" };
    if (esp_timer_create(&ta, &s_tmr) != ESP_OK) { s_tmr = NULL; return false; }
    return true;
}

static void kick(void)
{
    bool start = false;
    taskENTER_CRITICAL(&s_mux);
    if (!s_running) { s_running = true; start = true; }
    taskEXIT_CRITICAL(&s_mux);
    if (start) esp_timer_start_once(s_tmr, 1000);
}

void rgb_status_blink(rgb_status_color_t color, int times)
{
    if (times <= 0 || !init_once()) return;
    taskENTER_CRITICAL(&s_mux);
    if (s_qn < RGB_QUEUE) {
        s_q[(s_qh + s_qn) % RGB_QUEUE] = (rgb_req_t){ (uint8_t)color, (uint8_t)times };
        s_qn++;
    }
    taskEXIT_CRITICAL(&s_mux);
    kick();
}

void rgb_status_search(bool on)
{
    if (!init_once()) return;
    taskENTER_CRITICAL(&s_mux);
    s_search = on;
    taskEXIT_CRITICAL(&s_mux);
    if (on) kick();
}

void rgb_status_flush(uint32_t max_ms)
{
    if (!s_tmr) return;
    taskENTER_CRITICAL(&s_mux);
    s_search = false;
    taskEXIT_CRITICAL(&s_mux);
    for (uint32_t waited = 0; s_running && waited < max_ms; waited += 10) vTaskDelay(pdMS_TO_TICKS(10));
    esp_timer_stop(s_tmr);
    s_running = false;
    led_set(-1, false);
}
