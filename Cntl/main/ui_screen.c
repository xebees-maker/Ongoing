/**
 * @file    ui_screen.c
 * @brief   화면 자동 꺼짐/켜짐 관리
 *
 *  - USB 연결 중: 항상 켜짐
 *  - 배터리 모드: 10초 무터치 → 꺼짐, 터치 → 켜짐 (웨이크업 터치는 소비)
 */

#include "ui_screen.h"
#include "ui_input.h"
#include "bsp_ws_1_47.h"
#include "driver/usb_serial_jtag.h"
#include "lvgl.h"

#define SCREEN_IDLE_TIMEOUT_MS  10000U
#define SCREEN_CHECK_PERIOD_MS    500U

static bool     s_screen_off    = false;
static uint32_t s_last_input_ms = 0;

/* global handler — priority 0 (최우선) */
static bool screen_input_handler(bool pressed, lv_point_t pt)
{
    (void)pt;
    if (!pressed) return false;

    s_last_input_ms = lv_tick_get();

    if (s_screen_off) {
        bsp_display_set_brightness(BSP_LCD_BRIGHTNESS_DEFAULT);
        s_screen_off = false;
        return true;  /* 웨이크업 터치 소비 */
    }
    return false;
}

/* 500ms 타이머: 타임아웃 및 USB 복구 체크 */
static void screen_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (usb_serial_jtag_is_connected()) {
        if (s_screen_off) {
            bsp_display_set_brightness(BSP_LCD_BRIGHTNESS_DEFAULT);
            s_screen_off = false;
        }
        return;
    }

    if (!s_screen_off) {
        uint32_t idle_ms = lv_tick_get() - s_last_input_ms;
        if (idle_ms >= SCREEN_IDLE_TIMEOUT_MS) {
            bsp_display_set_brightness(BSP_LCD_BRIGHTNESS_OFF);
            s_screen_off = true;
        }
    }
}

void ui_screen_init(void)
{
    s_last_input_ms = lv_tick_get();
    ui_input_add_global(screen_input_handler, 0);
    lv_timer_create(screen_timer_cb, SCREEN_CHECK_PERIOD_MS, NULL);
}
