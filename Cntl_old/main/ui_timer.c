/**
 * @file    ui_timer.c
 * @brief   Timer 탭 UI 구현
 */

#include "ui_timer.h"
#include "ui_common.h"

static const char *TAG = "ui_timer";

static lv_obj_t   *s_spinbox   = NULL;
static lv_obj_t   *s_arc       = NULL;
static lv_obj_t   *s_arc_label = NULL;
static lv_timer_t *s_timer     = NULL;

/* ════════════════════════════════════════════════════════════
 * Modal
 * ════════════════════════════════════════════════════════════ */
static void cb_modal_ok(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(btn)));
}

static void show_modal(void)
{
    /* 오버레이 */
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(COLOR_MODAL_BG), UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_bg_opa(overlay, UI_OPA_HALF, UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_border_width(overlay, UI_BORDER_NONE, UI_ALIGN_OFFSET_NONE);
    lv_obj_align(overlay, LV_ALIGN_CENTER, UI_ALIGN_OFFSET_NONE, UI_ALIGN_OFFSET_NONE);

    /* 박스 */
    lv_obj_t *box = lv_obj_create(overlay);
    lv_obj_set_size(box, MODAL_BOX_W, MODAL_BOX_H);
    lv_obj_align(box, LV_ALIGN_CENTER, UI_ALIGN_OFFSET_NONE, UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_bg_color(box, lv_color_hex(COLOR_MODAL_BOX), UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_radius(box, MODAL_BOX_RADIUS, UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_border_width(box, UI_BORDER_NONE, UI_ALIGN_OFFSET_NONE);

    /* 메시지 */
    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, STR_MODAL_MSG);
    lv_obj_set_style_text_font(msg, UI_FONT_12, UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT_UI), UI_ALIGN_OFFSET_NONE);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, UI_ALIGN_OFFSET_NONE, MODAL_MSG_OFFSET_Y);

    /* OK 버튼 */
    lv_obj_t *btn_ok = lv_button_create(box);
    lv_obj_set_size(btn_ok, BTN_OK_W, BTN_OK_H);
    lv_obj_align(btn_ok, LV_ALIGN_BOTTOM_MID, UI_ALIGN_OFFSET_NONE, BTN_OK_OFFSET_Y);
    lv_obj_t *lbl_ok = lv_label_create(btn_ok);
    lv_label_set_text(lbl_ok, STR_BTN_OK);
    lv_obj_set_style_text_font(lbl_ok, UI_FONT_12, UI_ALIGN_OFFSET_NONE);
    lv_obj_center(lbl_ok);
    ui_apply_btn_style(btn_ok, lbl_ok);
    lv_obj_add_event_cb(btn_ok, cb_modal_ok, LV_EVENT_CLICKED, NULL);
}

/* ════════════════════════════════════════════════════════════
 * 타이머 tick
 * ════════════════════════════════════════════════════════════ */
static void cb_timer_tick(lv_timer_t *t)
{
    int32_t val = lv_spinbox_get_value(s_spinbox);

    if (val <= 0) {
        lv_timer_pause(s_timer);
        show_modal();
        return;
    }

    lv_spinbox_decrement(s_spinbox);
    val--;
    lv_arc_set_value(s_arc, (int16_t)val);
    lv_label_set_text_fmt(s_arc_label, "%d", (int)val);
}

/* ════════════════════════════════════════════════════════════
 * 이벤트 콜백 — Start / Stop / +/-
 * ════════════════════════════════════════════════════════════ */
static void cb_start(lv_event_t *e)
{
    ESP_LOGI(TAG, "Start pressed, val=%d", (int)lv_spinbox_get_value(s_spinbox));
    if (lv_spinbox_get_value(s_spinbox) <= 0) return;
    if (s_timer == NULL) {
        s_timer = lv_timer_create(cb_timer_tick, TIMER_INTERVAL_MS, NULL);
        ESP_LOGI(TAG, "Timer created");
    } else {
        lv_timer_resume(s_timer);
        ESP_LOGI(TAG, "Timer resumed");
    }
}

static void cb_stop(lv_event_t *e)
{
    if (s_timer) lv_timer_pause(s_timer);
}

static void cb_spinbox_inc(lv_event_t *e)
{
    lv_spinbox_increment(s_spinbox);
    int32_t val = lv_spinbox_get_value(s_spinbox);
    lv_arc_set_value(s_arc, (int16_t)val);
    lv_label_set_text_fmt(s_arc_label, "%d", (int)val);
}

static void cb_spinbox_dec(lv_event_t *e)
{
    lv_spinbox_decrement(s_spinbox);
    int32_t val = lv_spinbox_get_value(s_spinbox);
    lv_arc_set_value(s_arc, (int16_t)val);
    lv_label_set_text_fmt(s_arc_label, "%d", (int)val);
}

/* ════════════════════════════════════════════════════════════
 * UI 생성
 * ════════════════════════════════════════════════════════════ */
void ui_create_timer(lv_obj_t *parent)
{
    /* 제목 */
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, STR_TITLE_TIMER);
    lv_obj_set_style_text_font(title, UI_FONT_12, UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_UI), UI_ALIGN_OFFSET_NONE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, UI_ALIGN_OFFSET_NONE, UI_TITLE_OFFSET_Y);

    /* Arc */
    s_arc = lv_arc_create(parent);
    lv_obj_set_size(s_arc, ARC_SIZE, ARC_SIZE);
    lv_arc_set_range(s_arc, ARC_VAL_MIN, ARC_VAL_MAX);
    lv_arc_set_value(s_arc, ARC_VAL_INIT);
    lv_arc_set_bg_angles(s_arc, ARC_ANGLE_START, ARC_ANGLE_END);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, UI_ALIGN_OFFSET_NONE, UI_ARC_OFFSET_Y);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    /* Arc 중앙 숫자 */
    s_arc_label = lv_label_create(parent);
    lv_label_set_text(s_arc_label, "1");
    lv_obj_set_style_text_font(s_arc_label, UI_FONT_12, UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_text_color(s_arc_label, lv_color_hex(COLOR_TEXT_UI), UI_ALIGN_OFFSET_NONE);
    lv_obj_align_to(s_arc_label, s_arc, LV_ALIGN_CENTER, UI_ALIGN_OFFSET_NONE, UI_ALIGN_OFFSET_NONE);

    /* - 버튼 */
    lv_obj_t *btn_dec = lv_button_create(parent);
    lv_obj_set_size(btn_dec, BTN_PLUS_MINUS_SIZE, BTN_PLUS_MINUS_SIZE);
    lv_obj_align(btn_dec, LV_ALIGN_BOTTOM_MID, -BTN_PLUS_MINUS_OFFSET_X, BTN_PLUS_MINUS_OFFSET_Y);
    lv_obj_t *lbl_dec = lv_label_create(btn_dec);
    lv_label_set_text(lbl_dec, LV_SYMBOL_MINUS);
    lv_obj_center(lbl_dec);
    ui_apply_btn_style(btn_dec, lbl_dec);
    lv_obj_add_event_cb(btn_dec, cb_spinbox_dec, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_dec, cb_spinbox_dec, LV_EVENT_LONG_PRESSED_REPEAT, NULL);

    /* Spinbox */
    s_spinbox = lv_spinbox_create(parent);
    lv_spinbox_set_range(s_spinbox, SPINBOX_VAL_MIN, SPINBOX_VAL_MAX);
    lv_spinbox_set_value(s_spinbox, SPINBOX_VAL_INIT);
    lv_spinbox_set_digit_format(s_spinbox, SPINBOX_DIGITS, UI_ALIGN_OFFSET_NONE);
    lv_obj_set_size(s_spinbox, SPINBOX_W, SPINBOX_H);
    lv_obj_align(s_spinbox, LV_ALIGN_BOTTOM_MID, UI_ALIGN_OFFSET_NONE, BTN_PLUS_MINUS_OFFSET_Y);
    lv_obj_set_style_text_font(s_spinbox, UI_FONT_12, UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_text_color(s_spinbox, lv_color_hex(COLOR_TEXT_UI), UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_bg_color(s_spinbox, lv_color_hex(COLOR_BG), UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_bg_opa(s_spinbox, LV_OPA_COVER, UI_ALIGN_OFFSET_NONE);

    /* + 버튼 */
    lv_obj_t *btn_inc = lv_button_create(parent);
    lv_obj_set_size(btn_inc, BTN_PLUS_MINUS_SIZE, BTN_PLUS_MINUS_SIZE);
    lv_obj_align(btn_inc, LV_ALIGN_BOTTOM_MID, BTN_PLUS_MINUS_OFFSET_X, BTN_PLUS_MINUS_OFFSET_Y);
    lv_obj_t *lbl_inc = lv_label_create(btn_inc);
    lv_label_set_text(lbl_inc, LV_SYMBOL_PLUS);
    lv_obj_center(lbl_inc);
    ui_apply_btn_style(btn_inc, lbl_inc);
    lv_obj_add_event_cb(btn_inc, cb_spinbox_inc, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_inc, cb_spinbox_inc, LV_EVENT_LONG_PRESSED_REPEAT, NULL);

    /* Start 버튼 */
    lv_obj_t *btn_start = lv_button_create(parent);
    lv_obj_set_size(btn_start, BTN_START_STOP_W, BTN_START_STOP_H);
    lv_obj_align(btn_start, LV_ALIGN_BOTTOM_MID, BTN_START_OFFSET_X, BTN_BOTTOM_OFFSET_Y);
    lv_obj_t *lbl_start = lv_label_create(btn_start);
    lv_label_set_text(lbl_start, STR_BTN_START);
    lv_obj_set_style_text_font(lbl_start, UI_FONT_18, UI_ALIGN_OFFSET_NONE);
    lv_obj_set_width(lbl_start, LV_SIZE_CONTENT);
    lv_obj_center(lbl_start);
    ui_apply_btn_style(btn_start, lbl_start);
    lv_obj_add_event_cb(btn_start, cb_start, LV_EVENT_CLICKED, NULL);

    /* Stop 버튼 */
    lv_obj_t *btn_stop = lv_button_create(parent);
    lv_obj_set_size(btn_stop, BTN_START_STOP_W, BTN_START_STOP_H);
    lv_obj_align(btn_stop, LV_ALIGN_BOTTOM_MID, BTN_STOP_OFFSET_X, BTN_BOTTOM_OFFSET_Y);
    lv_obj_t *lbl_stop = lv_label_create(btn_stop);
    lv_label_set_text(lbl_stop, STR_BTN_STOP);
    lv_obj_set_style_text_font(lbl_stop, UI_FONT_12, UI_ALIGN_OFFSET_NONE);
    lv_obj_center(lbl_stop);
    ui_apply_btn_style(btn_stop, lbl_stop);
    lv_obj_add_event_cb(btn_stop, cb_stop, LV_EVENT_CLICKED, NULL);
}
