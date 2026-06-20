/**
 * @file    ui_main.c
 * @brief   탭뷰 생성 및 각 탭 UI 초기화
 */

#include "ui_main.h"
#include "ui_common.h"
#include "ui_dashboard.h"
#include "ui_timer.h"
#include "ui_input.h"
#include "ui_screen.h"
#include "ui_nodes.h"

static void tab_changed_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    ui_input_set_active_tab((int)lv_tabview_get_tab_active(tv));
}

void ui_init(void)
{
    ui_input_init();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, UI_ALIGN_OFFSET_NONE);

    lv_obj_t *tabview = lv_tabview_create(screen);
    lv_tabview_set_tab_bar_position(tabview, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tabview, TABVIEW_BAR_H);
    lv_obj_set_size(tabview, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(tabview, LV_ALIGN_TOP_LEFT, UI_ALIGN_OFFSET_NONE, UI_ALIGN_OFFSET_NONE);

    lv_obj_t *tab_dashboard = lv_tabview_add_tab(tabview, STR_TAB_DASHBOARD);
    lv_obj_t *tab_timer     = lv_tabview_add_tab(tabview, STR_TAB_TIMER);
    lv_obj_t *tab_nodes     = lv_tabview_add_tab(tabview, STR_TAB_NODES);

    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_style_text_font(tab_bar, UI_FONT_12, LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(tab_bar, UI_FONT_12, LV_STATE_CHECKED);

    lv_obj_t *content = lv_tabview_get_content(tabview);
    lv_obj_set_style_bg_color(content, lv_color_hex(COLOR_BG), UI_ALIGN_OFFSET_NONE);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, UI_ALIGN_OFFSET_NONE);

    lv_obj_add_event_cb(tabview, tab_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    ui_create_dashboard(tab_dashboard);
    ui_create_timer(tab_timer);
    ui_create_nodes(tab_nodes);

    ui_screen_init();
}
