/**
 * @file    ui_gas.c
 * @brief   가스 탭 — 3단 레이아웃 (50 : 25 : 25)
 *          1단: CO2 (SCD41, ppm)
 *          2단: 온도 (SCD41 내장, DHT22와 비교용)
 *          3단: 습도 (SCD41 내장, DHT22와 비교용)
 */

#include "ui_gas.h"
#include "ui_common.h"
#include "scd41.h"

#define GAS_SAMPLE_MS   1000     /* SCD41 자체 갱신 주기는 5초, 폴링만 1초 */

/* ── 레이아웃 ── */
#define CONTENT_H   (BSP_LCD_V_RES - TABVIEW_BAR_H)
#define SEC1_H      (CONTENT_H * 50 / 100)
#define SEC2_H      (CONTENT_H * 25 / 100)
#define SEC3_H      (CONTENT_H - SEC1_H - SEC2_H)

/* ── 색상 ── */
#define CLR_SEC2    0xEEEEEE
#define CLR_TITLE   0x000000
#define CLR_CO2     0x16A085
#define CLR_TEMP    0xE74C3C
#define CLR_HUMI    0x3498DB

static lv_obj_t *s_co2_value  = NULL;
static lv_obj_t *s_temp_value = NULL;
static lv_obj_t *s_humi_value = NULL;

static lv_obj_t *make_section(lv_obj_t *parent, int y, int h, uint32_t bg)
{
    lv_obj_t *sec = lv_obj_create(parent);
    lv_obj_set_size(sec, BSP_LCD_H_RES, h);
    lv_obj_set_pos(sec, 0, y);
    lv_obj_set_style_bg_color(sec, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(sec, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sec, 0, 0);
    lv_obj_set_style_radius(sec, 0, 0);
    lv_obj_set_style_pad_all(sec, 0, 0);
    lv_obj_clear_flag(sec, LV_OBJ_FLAG_SCROLLABLE);
    return sec;
}

/* 좌상단(Top+12pt) 제목(12pt) + 단 높이 중앙에 큰 글씨(30pt, 가운데정렬) 값 라벨 */
static lv_obj_t *make_sensor_value(lv_obj_t *sec, const char *title, uint32_t value_color)
{
    lv_obj_t *title_lbl = lv_label_create(sec);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, UI_FONT_12, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(CLR_TITLE), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 4, 12);

    lv_obj_t *value_lbl = lv_label_create(sec);
    lv_label_set_text(value_lbl, "센서 없음");
    lv_obj_set_style_text_font(value_lbl, UI_FONT_30, 0);
    lv_obj_set_style_text_color(value_lbl, lv_color_hex(value_color), 0);
    lv_obj_align(value_lbl, LV_ALIGN_CENTER, 0, 0);

    return value_lbl;
}

/* ════════════════════════════════════════════════════════════
 * SCD41 샘플 콜백
 * ════════════════════════════════════════════════════════════ */
static void gas_sample_cb(lv_timer_t *t)
{
    (void)t;
    int co2 = 0;
    float temp = 0.0f, humi = 0.0f;

    if (!scd41_read(&co2, &temp, &humi)) return;  /* 아직 새 데이터 없음 */

    lv_label_set_text_fmt(s_co2_value, "%d ppm", co2);

    /* LVGL 빌트인 sprintf는 %f 미지원 → 정수 연산으로 직접 포맷 */
    int t10 = (int)(temp * 10.0f + (temp >= 0.0f ? 0.5f : -0.5f));
    bool t_neg = t10 < 0;
    if (t_neg) t10 = -t10;
    lv_label_set_text_fmt(s_temp_value, "%s%d.%d °C",
                          t_neg ? "-" : "", t10 / 10, t10 % 10);

    int h = (int)(humi + 0.5f);
    lv_label_set_text_fmt(s_humi_value, "%d %%", h);

    lv_obj_align(s_co2_value, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(s_temp_value, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(s_humi_value, LV_ALIGN_CENTER, 0, 0);
}

void ui_create_gas(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* 1단 — CO2 (50%) */
    lv_obj_t *sec1 = make_section(parent, 0, SEC1_H, COLOR_BG);
    s_co2_value = make_sensor_value(sec1, "CO2", CLR_CO2);

    /* 2단 — 온도 (25%) */
    lv_obj_t *sec2 = make_section(parent, SEC1_H, SEC2_H, CLR_SEC2);
    s_temp_value = make_sensor_value(sec2, "온도", CLR_TEMP);

    /* 3단 — 습도 (25%) */
    lv_obj_t *sec3 = make_section(parent, SEC1_H + SEC2_H, SEC3_H, COLOR_BG);
    s_humi_value = make_sensor_value(sec3, "습도", CLR_HUMI);

    if (scd41_init(1, GPIO_NUM_4, GPIO_NUM_3)) {
        lv_timer_create(gas_sample_cb, GAS_SAMPLE_MS, NULL);
    }
}
