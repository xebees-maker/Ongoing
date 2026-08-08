/**
 * @file    ui_nodes.c
 * @brief   노드 탭 — ESP-NOW 노드 목록 (1초 갱신). 미페어링 노드를 탭하면 페어링을 시도한다.
 */

#include "ui_nodes.h"
#include "ui_common.h"
#include "esp_now_hub.h"
#include "esp_timer.h"
#include <string.h>

#define NODE_LIST_REFRESH_MS 1000

static lv_obj_t *s_list      = NULL;
static lv_obj_t *s_net_label = NULL;
static uint8_t   s_btn_macs[ESP_NOW_HUB_MAX_NODES][6];

static void node_btn_clicked_cb(lv_event_t *e)
{
    uint8_t *mac = (uint8_t *)lv_event_get_user_data(e);
    esp_now_hub_pair(mac);
}

static void node_list_refresh_cb(lv_timer_t *t)
{
    (void)t;

    char status[40];
    esp_now_hub_get_net_status(status, sizeof(status));
    lv_label_set_text(s_net_label, status);

    esp_now_hub_node_t nodes[ESP_NOW_HUB_MAX_NODES];
    int count = esp_now_hub_get_nodes(nodes, ESP_NOW_HUB_MAX_NODES);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    lv_obj_clean(s_list);

    if (count == 0) {
        lv_obj_t *btn = lv_list_add_button(s_list, LV_SYMBOL_REFRESH, STR_NODES_EMPTY);
        lv_obj_set_style_text_font(btn, UI_FONT_12, 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        uint32_t freshness_ms = nodes[i].paired ? nodes[i].last_data_ms : nodes[i].last_seen_ms;
        char ago[16];
        lv_snprintf(ago, sizeof(ago), STR_NODES_AGO_FMT,
                    (unsigned long)((now_ms - freshness_ms) / 1000));

        char label[48];
        lv_snprintf(label, sizeof(label), "%s%s  -  %s",
                    nodes[i].name, nodes[i].paired ? " (페어링됨)" : "", ago);

        lv_obj_t *btn = lv_list_add_button(s_list, nodes[i].paired ? LV_SYMBOL_OK : LV_SYMBOL_WIFI, label);
        lv_obj_set_style_text_font(btn, UI_FONT_12, 0);

        memcpy(s_btn_macs[i], nodes[i].mac, 6);
        lv_obj_add_event_cb(btn, node_btn_clicked_cb, LV_EVENT_CLICKED, s_btn_macs[i]);
    }
}

void ui_create_nodes(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_net_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_net_label, UI_FONT_12, 0);
    lv_obj_set_style_text_color(s_net_label, lv_color_hex(COLOR_TEXT_UI), 0);
    lv_label_set_text(s_net_label, "");
    lv_obj_align(s_net_label, LV_ALIGN_TOP_MID, 0, 4);

    s_list = lv_list_create(parent);
    lv_obj_set_size(s_list, LV_PCT(100), LV_PCT(100) - 24);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *btn = lv_list_add_button(s_list, LV_SYMBOL_REFRESH, STR_NODES_EMPTY);
    lv_obj_set_style_text_font(btn, UI_FONT_12, 0);

    lv_timer_create(node_list_refresh_cb, NODE_LIST_REFRESH_MS, NULL);
}
