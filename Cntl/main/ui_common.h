/**
 * @file    ui_common.h
 * @brief   UI 파일 공통 include 묶음
 */
#ifndef UI_COMMON_H
#define UI_COMMON_H

#include "lvgl.h"
#include "bsp_ws_1_47.h"
#include "esp_log.h"
#include "ui_styles.h"
#include "ui_palette.h"
#include "ui_font.h"

/* ════════════════════════════════════════════════════════════
 * 폰트 매크로
 * ════════════════════════════════════════════════════════════ */
#define UI_FONT_12  (ui_font_get(12))
#define UI_FONT_18  (ui_font_get(18))
#define UI_FONT_30  (ui_font_get(30))

/* ════════════════════════════════════════════════════════════
 * 버튼 스타일 헬퍼 — 기본 + 눌림 반전 한번에 적용
 * ════════════════════════════════════════════════════════════ */
static inline void ui_apply_btn_style(lv_obj_t *btn, lv_obj_t *label)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_BTN_NORMAL), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_BTN_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_BTN), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_BTN_PRESSED), LV_STATE_PRESSED);
}

#endif /* UI_COMMON_H */
