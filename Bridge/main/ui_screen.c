#include "ui_screen.h"

#include "lvgl.h"
#include "esp_lv_adapter.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_screen";

static lv_obj_t *s_mem_label;
static lv_obj_t *s_wireless_log;
static lv_obj_t *s_can_log;

/* 로그창 하나가 무한정 커지지 않게 텍스트 길이 상한(화면 표시용일 뿐 — CAN 큐 자체의
 * "절대 드롭 안 함" 정책과는 별개 개념) */
#define UI_SCREEN_LOG_MAX_CHARS 4000

static void mem_update_task(void *arg);

static lv_obj_t *make_log_box(lv_obj_t *parent, lv_align_t align, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, w, h);
    lv_obj_align(ta, align, 0, 40);  /* 메모리 라벨(높이 약 40) 아래 */
    lv_textarea_set_max_length(ta, UI_SCREEN_LOG_MAX_CHARS);
    lv_obj_add_state(ta, LV_STATE_DISABLED);  /* 편집/커서 불필요, 순수 표시용 */
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    return ta;
}

void ui_screen_init(void)
{
    lv_obj_t *scr = lv_screen_active();

    s_mem_label = lv_label_create(scr);
    lv_obj_set_width(s_mem_label, LV_PCT(100));
    lv_obj_align(s_mem_label, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_text_font(s_mem_label, &lv_font_montserrat_18, 0);
    lv_label_set_text(s_mem_label, "MEM: --");

    lv_coord_t screen_w = lv_obj_get_width(scr);
    lv_coord_t screen_h = lv_obj_get_height(scr);
    lv_coord_t half_w = (screen_w / 2) - 4;
    lv_coord_t log_h = screen_h - 48;

    s_wireless_log = make_log_box(scr, LV_ALIGN_TOP_LEFT, half_w, log_h);
    s_can_log = make_log_box(scr, LV_ALIGN_TOP_RIGHT, half_w, log_h);

    ESP_LOGI(TAG, "브 화면 구성됨(메모리 표시 + 무선/CAN 로그창 2개)");

    xTaskCreate(mem_update_task, "ui_mem", 3072, NULL, 5, NULL);
}

static void log_append(lv_obj_t *box, const char *fmt, va_list args)
{
    char line[192];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n < 0) return;
    size_t len = strnlen(line, sizeof(line));
    if (len == 0 || line[len - 1] != '\n') {
        if (len < sizeof(line) - 1) { line[len] = '\n'; line[len + 1] = '\0'; }
    }

    if (esp_lv_adapter_lock(pdMS_TO_TICKS(200)) == ESP_OK) {
        /* 상한을 넘기면 오래된 것부터 잘라냄(화면 표시 목적일 뿐, CAN 큐와 무관) */
        if (lv_textarea_get_text(box) && strlen(lv_textarea_get_text(box)) + strlen(line) > UI_SCREEN_LOG_MAX_CHARS) {
            lv_textarea_set_text(box, "");
        }
        lv_textarea_add_text(box, line);
        esp_lv_adapter_unlock();
    }
}

void ui_screen_log_wireless(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_append(s_wireless_log, fmt, args);
    va_end(args);
}

void ui_screen_log_can(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_append(s_can_log, fmt, args);
    va_end(args);
}

static void mem_update_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (esp_lv_adapter_lock(pdMS_TO_TICKS(200)) == ESP_OK) {
            lv_label_set_text_fmt(s_mem_label, "MEM: I=%u P=%u",
                                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            esp_lv_adapter_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ui_screen_start_mem_task(void)
{
    xTaskCreate(mem_update_task, "ui_mem", 3072, NULL, 5, NULL);
}
