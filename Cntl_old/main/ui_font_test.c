/**
 * @file    ui_font_test.c
 * @brief   폰트 테스트 탭 — 12 / 18 / 30pt
 */

#include "ui_font_test.h"
#include "ui_common.h"

void ui_create_font_test(lv_obj_t *parent)
{
    typedef struct { uint8_t size; const char *label; } FontItem;
    const FontItem items[] = {
        { 12, "홍길동 시작 종료 12pt" },
        { 18, "홍길동 시작 종료 18pt" },
        { 30, "홍길동 시작 종료 30pt" },
    };
    const int count = sizeof(items) / sizeof(items[0]);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 4, 0);
    lv_obj_set_style_pad_all(parent, 8, 0);

    for (int i = 0; i < count; i++) {
        lv_obj_t *lbl = lv_label_create(parent);
        lv_label_set_text(lbl, items[i].label);
        lv_obj_set_style_text_font(lbl, ui_font_get(items[i].size), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_TEXT_UI), 0);
    }
}
