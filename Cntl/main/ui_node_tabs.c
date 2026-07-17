/**
 * @file    ui_node_tabs.c
 * @brief   가변 탭 — "센서 요약"(페어링 노드 전체 max/min/avg) + 페어링 노드별 탭.
 *          LVGL 탭 삭제 API가 없어 탭은 추가만 되고, 언페어/오프라인 시 제거되지 않는다.
 */

#include "ui_node_tabs.h"
#include "ui_common.h"
#include "esp_now_hub.h"
#include <string.h>

#define NODE_TABS_REFRESH_MS 1000

typedef struct {
    uint8_t   mac[6];
    bool      used;
    lv_obj_t *lbl_chan[ESP_NOW_MAX_CHANNELS];
    lv_obj_t *lbl_batt;
} node_tab_t;

static lv_obj_t  *s_tabview = NULL;
static node_tab_t  s_node_tabs[ESP_NOW_HUB_MAX_NODES];

static lv_obj_t *s_sum_count_lbl = NULL;
static lv_obj_t *s_sum_chan_lbl[SENSOR_CHAN_TYPE_COUNT] = { NULL };  /* index = sensor_channel_type_t */
static lv_obj_t *s_sum_batt_lbl  = NULL;

typedef struct { const char *name; const char *unit; bool decimal; } chan_meta_t;

/* SENSOR_CHAN_NONE(집계 대상 아님)도 안전하게 처리하도록 default 포함 */
static chan_meta_t chan_meta(uint8_t type)
{
    switch (type) {
        case SENSOR_CHAN_TEMP_C:  return (chan_meta_t){ "Temp", "C",   true  };
        case SENSOR_CHAN_HUMI_PCT:return (chan_meta_t){ "Humi", "%",   false };
        case SENSOR_CHAN_CO2_PPM: return (chan_meta_t){ "CO2",  "ppm", false };
        default:                  return (chan_meta_t){ "?",    "",    false };
    }
}

/* LVGL 내장 lv_snprintf는 %f를 지원하지 않음(Sens sensor-c6.c와 동일 이슈) — 정수 연산으로 직접 포맷 */
static void split_tenths(float v, bool *neg, int *whole, int *frac)
{
    int t10 = (int)(v * 10.0f + (v >= 0.0f ? 0.5f : -0.5f));
    *neg = t10 < 0;
    if (*neg) t10 = -t10;
    *whole = t10 / 10;
    *frac  = t10 % 10;
}

static lv_obj_t *create_value_label(lv_obj_t *parent, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, UI_FONT_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_TEXT_UI), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 8, y);
    return lbl;
}

/* lv_tabview_add_tab()은 탭바/콘텐츠 자식 개수를 바꿔서 LV_EVENT_LAYOUT_CHANGED를
 * 일으키고, 그게 다시 lv_obj_scroll_to_x()를 호출한다 — 사용자가 한창 스와이프(드래그)
 * 중일 때 이게 끼어들면 LVGL의 인덱브 스크롤 상태가 깨져서 화면이 멈추고 터치가
 * 먹통이 된다(실기에서 확인됨). 그래서 드래그 중엔 새 탭 생성을 한 틱 미룬다. */
static bool indev_is_dragging(void)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || lv_indev_get_state(indev) != LV_INDEV_STATE_PRESSED) return false;
    return lv_indev_get_scroll_obj(indev) != NULL;
}

static node_tab_t *find_node_tab(const uint8_t *mac)
{
    for (int i = 0; i < ESP_NOW_HUB_MAX_NODES; i++) {
        if (s_node_tabs[i].used && memcmp(s_node_tabs[i].mac, mac, 6) == 0) {
            return &s_node_tabs[i];
        }
    }
    return NULL;
}

static node_tab_t *create_node_tab(const esp_now_hub_node_t *node)
{
    for (int i = 0; i < ESP_NOW_HUB_MAX_NODES; i++) {
        if (!s_node_tabs[i].used) {
            lv_obj_t *tab = lv_tabview_add_tab(s_tabview, node->name);
            lv_obj_set_style_pad_all(tab, 0, 0);
            lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

            s_node_tabs[i].used = true;
            memcpy(s_node_tabs[i].mac, node->mac, 6);
            for (int c = 0; c < ESP_NOW_MAX_CHANNELS; c++) {
                s_node_tabs[i].lbl_chan[c] = create_value_label(tab, 4 + c * 24);
            }
            s_node_tabs[i].lbl_batt = create_value_label(tab, 4 + ESP_NOW_MAX_CHANNELS * 24);
            return &s_node_tabs[i];
        }
    }
    return NULL;  /* 탭 슬롯 가득 — 이번 범위에서는 그냥 무시 */
}

static void update_node_tab(node_tab_t *nt, const esp_now_hub_node_t *node)
{
    for (int i = 0; i < ESP_NOW_MAX_CHANNELS; i++) {
        if (i >= node->chan_count) {
            lv_label_set_text(nt->lbl_chan[i], "");
            continue;
        }
        chan_meta_t meta = chan_meta(node->chan_type[i]);
        if (!node->chan_ok[i]) {
            lv_label_set_text_fmt(nt->lbl_chan[i], "%s  --", meta.name);
        } else if (meta.decimal) {
            bool neg; int whole, frac;
            split_tenths(node->chan_val[i], &neg, &whole, &frac);
            lv_label_set_text_fmt(nt->lbl_chan[i], "%s  %s%d.%d %s",
                                   meta.name, neg ? "-" : "", whole, frac, meta.unit);
        } else {
            lv_label_set_text_fmt(nt->lbl_chan[i], "%s  %d %s",
                                   meta.name, (int)(node->chan_val[i] + 0.5f), meta.unit);
        }
    }

    if (node->batt_ok) {
        lv_label_set_text_fmt(nt->lbl_batt, "BATT  %d%%  %s", node->batt_pct, node->powered ? "USB" : "BAT");
    } else {
        lv_label_set_text(nt->lbl_batt, "BATT  --");
    }
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;

    esp_now_hub_summary_t sum;
    esp_now_hub_get_summary(&sum);
    lv_label_set_text_fmt(s_sum_count_lbl, "페어링된 노드: %d", sum.count);

    for (uint8_t type = SENSOR_CHAN_TEMP_C; type < SENSOR_CHAN_TYPE_COUNT; type++) {
        lv_obj_t *lbl = s_sum_chan_lbl[type];
        if (!lbl) continue;
        chan_meta_t meta = chan_meta(type);
        const esp_now_hub_channel_agg_t *agg = &sum.by_type[type];
        if (sum.count == 0 || !agg->has_data) {
            lv_label_set_text_fmt(lbl, "%s  --", meta.name);
        } else if (meta.decimal) {
            bool min_neg, max_neg, avg_neg;
            int min_w, min_f, max_w, max_f, avg_w, avg_f;
            split_tenths(agg->val_min, &min_neg, &min_w, &min_f);
            split_tenths(agg->val_max, &max_neg, &max_w, &max_f);
            split_tenths(agg->val_avg, &avg_neg, &avg_w, &avg_f);
            lv_label_set_text_fmt(lbl, "%s  %s%d.%d~%s%d.%d %s (평균 %s%d.%d)",
                                   meta.name,
                                   min_neg ? "-" : "", min_w, min_f,
                                   max_neg ? "-" : "", max_w, max_f, meta.unit,
                                   avg_neg ? "-" : "", avg_w, avg_f);
        } else {
            lv_label_set_text_fmt(lbl, "%s  %d~%d %s (평균 %d)",
                                   meta.name, (int)agg->val_min, (int)agg->val_max, meta.unit,
                                   (int)(agg->val_avg + 0.5f));
        }
    }

    if (sum.count > 0) {
        lv_label_set_text_fmt(s_sum_batt_lbl, "BATT  %d~%d%% (평균 %d%%)",
                               sum.batt_pct_min, sum.batt_pct_max, (int)(sum.batt_pct_avg + 0.5f));
    } else {
        lv_label_set_text(s_sum_batt_lbl, "BATT  --");
    }

    esp_now_hub_node_t nodes[ESP_NOW_HUB_MAX_NODES];
    int count = esp_now_hub_get_nodes(nodes, ESP_NOW_HUB_MAX_NODES);
    for (int i = 0; i < count; i++) {
        if (!nodes[i].paired) continue;

        node_tab_t *nt = find_node_tab(nodes[i].mac);
        if (!nt) {
            if (indev_is_dragging()) continue; /* 스와이프 중 — 다음 틱에 재시도 */
            nt = create_node_tab(&nodes[i]);
        }
        if (nt) update_node_tab(nt, &nodes[i]);
    }
}

void ui_node_tabs_init(lv_obj_t *tabview)
{
    s_tabview = tabview;
    memset(s_node_tabs, 0, sizeof(s_node_tabs));

    lv_obj_t *summary_tab = lv_tabview_add_tab(tabview, STR_TAB_SENSOR_SUMMARY);
    lv_obj_set_style_pad_all(summary_tab, 0, 0);
    lv_obj_clear_flag(summary_tab, LV_OBJ_FLAG_SCROLLABLE);

    s_sum_count_lbl = create_value_label(summary_tab, 4);
    int y = 28;
    for (uint8_t type = SENSOR_CHAN_TEMP_C; type < SENSOR_CHAN_TYPE_COUNT; type++) {
        s_sum_chan_lbl[type] = create_value_label(summary_tab, y);
        y += 24;
    }
    s_sum_batt_lbl = create_value_label(summary_tab, y);

    lv_timer_create(refresh_cb, NODE_TABS_REFRESH_MS, NULL);
}
