/**
 * @file    lv_mem_route.c
 * @brief   lv_mem_route.h 구현
 */
#include "lv_mem_route.h"

#include "esp_heap_caps.h"
#include <stdlib.h>

static volatile bool s_in_refr;   /* LVGL 태스크가 화면 갱신 중 — 그리기 스레드(swdraw)의 할당도 이 사이에만 일어남 */
static void refr_event_cb(lv_event_t *e)
{
    s_in_refr = lv_event_get_code(e) == LV_EVENT_REFR_START;
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
    lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_READY, NULL);
}
