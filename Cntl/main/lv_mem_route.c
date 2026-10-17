/**
 * @file    lv_mem_route.c
 * @brief   lv_mem_route.h 구현
 */
#include "lv_mem_route.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdlib.h>

static const char *TAG = "LVMEM";

static volatile bool s_in_refr;   /* LVGL 태스크가 화면 갱신 중 — 그리기 스레드(swdraw)의 할당도 이 사이에만 일어남 */
static int64_t s_refr_t0;

/* 10초 창 — 실제로 그린 갱신(1ms 넘게 걸린 것)만 셈 */
static int64_t s_win_start;
static uint32_t s_win_n;
static int64_t s_win_sum, s_win_max;

static void refr_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int64_t now = esp_timer_get_time();
    if (code == LV_EVENT_REFR_START) {
        s_refr_t0 = now;
        s_in_refr = true;
        return;
    }
    s_in_refr = false;
    int64_t d = now - s_refr_t0;
    if (d > 1000) {
        s_win_n++;
        s_win_sum += d;
        if (d > s_win_max) s_win_max = d;
    }
    if (now - s_win_start >= 10 * 1000000LL) {
        if (s_win_n) {
            ESP_LOGW(TAG, "LVRENDER %u refreshes avg %lld us max %lld us | internal free %u", (unsigned)s_win_n,
                     s_win_sum / s_win_n, s_win_max, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
        s_win_start = now;
        s_win_n = 0;
        s_win_sum = s_win_max = 0;
    }
}

/* LVGL의 C 라이브러리 할당(lv_mem_core_clib.c의 malloc/realloc)을 링커로 가로챔(main/CMakeLists.txt --wrap). 갱신 중이면 예전과
 * 똑같이 malloc(작은 것은 내부 RAM), 그 밖이면 PSRAM 우선(없으면 내부 RAM). 해제는 그대로 free(어느 쪽이든 됨) */
void *__wrap_lv_malloc_core(size_t size);
void *__wrap_lv_realloc_core(void *p, size_t new_size);

void *__wrap_lv_malloc_core(size_t size)
{
    if (s_in_refr) return malloc(size);
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *__wrap_lv_realloc_core(void *p, size_t new_size)
{
    if (s_in_refr) return realloc(p, new_size);
    return heap_caps_realloc_prefer(p, new_size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void lv_mem_route_attach(lv_display_t *disp)
{
    s_win_start = esp_timer_get_time();
    lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_READY, NULL);
}
