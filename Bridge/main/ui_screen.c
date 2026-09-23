#include "ui_screen.h"
#include "esp_now_link.h"
#include "esp_now.h"

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

void ui_screen_mac6(const uint8_t mac[6], char out[7])
{
    snprintf(out, 7, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

const char *ui_screen_err_short(esp_err_t err)
{
    static char buf[24];
    if (err == ESP_OK) return "OK";
    const char *name = esp_err_to_name(err);
    if (strncmp(name, "ESP_ERR_", 8) == 0) {
        snprintf(buf, sizeof(buf), "ERR-%s", name + 8);
        return buf;
    }
    if (strcmp(name, "ESP_FAIL") == 0) return "ERR-FAIL";
    return name;  /* 매칭 안 되는 드문 케이스는 원본 그대로(길어도 정보는 유지) */
}

const char *ui_screen_msg_type_name(uint8_t msg_type)
{
    /* 지금 페어링 흐름(광고->ACK->연결)에서 실제로 보이는 것부터 — 나머지(캐스크/사진전송
     * 등)는 필요해지면 추가 */
    switch (msg_type) {
        case ESP_NOW_MSG_ADVERTISE:        return "ADVERTISE";
        case ESP_NOW_MSG_PAIR_REQUEST:     return "PAIR_REQ";
        case ESP_NOW_MSG_PAIR_ACK:         return "PAIR_ACK";
        case ESP_NOW_MSG_ADVERTISE_ACK:    return "ADV_ACK";
        case ESP_NOW_MSG_WAKE_HELLO:       return "WAKE_HELLO";
        case ESP_NOW_MSG_WAKE_HELLO_ACK:   return "WAKE_ACK";
        case ESP_NOW_MSG_UNPAIR:           return "UNPAIR";
        case ESP_NOW_MSG_UNPAIR_ACK:       return "UNPAIR_ACK";
        default: {
            static char buf[12];
            snprintf(buf, sizeof(buf), "#%u", msg_type);
            return buf;
        }
    }
}

const char *ui_screen_result_code(esp_err_t err)
{
    static char buf[12];
    switch (err) {
        case ESP_OK:                    return "OK";
        case ESP_ERR_TIMEOUT:           return "TO";
        case ESP_ERR_ESPNOW_NOT_FOUND:  return "NP";
        case ESP_FAIL:                  return "FL";
        case ESP_ERR_NO_MEM:            return "NM";
        case ESP_ERR_INVALID_ARG:       return "BA";
        case ESP_ERR_INVALID_STATE:     return "BS";
        default:
            snprintf(buf, sizeof(buf), "E%d", (int)err);
            return buf;
    }
}

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
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_18, 0);
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

    static StaticTask_t s_mem_tcb;
    StackType_t *mem_stack = (StackType_t *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    xTaskCreateStatic(mem_update_task, "ui_mem", 3072 / sizeof(StackType_t), NULL, 5, mem_stack, &s_mem_tcb);
}

static void log_append(lv_obj_t *box, const char *fmt, va_list args)
{
    /* 2026-09-23(사용자 지시 — 양쪽 창 로그를 순서대로 대조하기 위한 공통 순번) — 두 창
     * 공통 카운터, 00~99 순환. 순수 표시용 디버그 보조라 동시성 보호는 안 둠(드물게 겹쳐도
     * 화면 번호 하나 스킵/중복되는 정도, 기능엔 영향 없음) */
    static uint32_t s_seq = 0;
    char line[192];
    int prefix_n = snprintf(line, sizeof(line), "[%02u] ", (unsigned)(s_seq++ % 100));
    if (prefix_n < 0 || (size_t)prefix_n >= sizeof(line)) return;
    int n = vsnprintf(line + prefix_n, sizeof(line) - (size_t)prefix_n, fmt, args);
    if (n < 0) return;
    /* 2026-09-23(디버깅용) — 화면에만 찍히고 시리얼엔 전혀 안 남아서 원격으로 확인이
     * 불가능했음(사용자는 화면으로 보지만 Claude는 시리얼로만 봄) — 둘 다 남김 */
    ESP_LOGI(box == s_wireless_log ? "wireless" : "can_ui", "%s", line);
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

/* 콘의 format_bytes_human과 동일 원칙(LVGL lv_label_set_text_fmt는 %f를 지원 안 함 —
 * feedback_lvgl_no_percent_f 메모리) — 일반 C snprintf(%f 가능)로 먼저 문자열을 만들고,
 * 그 결과를 %s로 라벨에 넣음. 사용자 지시: KB 고정 단위, 소수점 2자리 */
static void format_kb(uint32_t bytes, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%.2fKB", (double)bytes / 1024.0);
}

static void mem_update_task(void *arg)
{
    (void)arg;
    char mem_i[24], mem_p[24];
    for (;;) {
        format_kb((uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), mem_i, sizeof(mem_i));
        format_kb((uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), mem_p, sizeof(mem_p));
        if (esp_lv_adapter_lock(pdMS_TO_TICKS(200)) == ESP_OK) {
            lv_label_set_text_fmt(s_mem_label, "MEM: I=%s P=%s", mem_i, mem_p);
            esp_lv_adapter_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
