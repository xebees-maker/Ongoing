/**
 * @file    ui_dashboard.c
 * @brief   종합 탭 — 3단 레이아웃 (40 : 40 : 20)
 *          1단: 대기 온도 (DHT22)
 *          2단: 대기 습도 (DHT22)
 *          3단: 배터리/전원 상태
 */

#include "ui_dashboard.h"
#include "ui_common.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "dht22.h"
#include "battery.h"

static const char *TAG = "ui_dash";

/* ── 레이아웃 ── */
#define CONTENT_H   (BSP_LCD_V_RES - TABVIEW_BAR_H)
#define SEC1_H      (CONTENT_H * 40 / 100)
#define SEC2_H      (CONTENT_H * 40 / 100)
#define SEC3_H      (CONTENT_H - SEC1_H - SEC2_H)

/* ── 배터리 아이콘 치수 ── */
#define BAT_W       50
#define BAT_H       20
#define BAT_TIP_W    5
#define BAT_TIP_H   10
#define BAT_BORDER   2
#define BAT_PAD      2

/* ── 색상 ── */
#define CLR_SEC3        0x1A1A2E
#define CLR_SEC2        0xEEEEEE
#define CLR_BAT_BORDER  0xCCCCCC
#define CLR_BAT_GREEN   0x2ECC71
#define CLR_BAT_ORANGE  0xF39C12
#define CLR_BAT_RED     0xE74C3C
#define CLR_TEXT_LIGHT  0xFFFFFF
#define CLR_CHG         0x2ECC71
#define CLR_TITLE       0x000000
#define CLR_TEMP        0xE74C3C
#define CLR_HUMI        0x3498DB

#define DHT_SAMPLE_MS   1000     /* 실험 단계: 1초 (정식 운영 시 2초 이상 권장) */

/* ── ADC / GPIO ── */
#define BAT_CTRL_PIN     GPIO_NUM_14    /* P-MOSFET 게이트: LOW=ON → 분압기 활성화 */
#define BAT_ADC_UNIT     ADC_UNIT_2
#define BAT_ADC_CH       ADC_CHANNEL_1  /* GPIO12 = ADC2_CH1 (BAT_ADC) */
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_12
#define BAT_SAMPLE_MS        1000       /* 샘플 주기: 1 Hz */
#define BAT_SAMPLE_COUNT       11       /* 순환 버퍼: 11개 */
#define BAT_GAUGE_EVERY        10       /* 게이지 갱신: 10샘플(10초)마다 */
#define VBAT_DIV              3.0f     /* R19=200K, R21=100K */
#define VBAT_FULL_MV          4200.0f
#define VBAT_EMPTY_MV         3300.0f

static lv_obj_t *s_fill     = NULL;
static lv_obj_t *s_pct_lbl  = NULL;
static lv_obj_t *s_chg_icon = NULL;
static lv_obj_t *s_temp_value = NULL;  /* 1단: 온도 값 표시 */
static lv_obj_t *s_humi_value = NULL;  /* 2단: 습도 값 표시 */
static int s_queue[BAT_SAMPLE_COUNT];  /* 순환 버퍼 — battery_read_mv() 값(mV) 보관 */
static int s_queue_head  = 0;          /* 다음 쓸 위치 */
static int s_queue_count = 0;          /* 현재 보관 개수 */
static int  s_sample_n    = 0;          /* 게이지 갱신 카운터 */
static int  s_all_peak    = 0;          /* 누적 최고 peak (리셋 없음) */
static int  s_last_pct    = -1;         /* 마지막 계산된 pct (표시 갱신용) */
static bool s_usb_prev    = false;      /* 이전 USB 연결 상태 */

/* ════════════════════════════════════════════════════════════
 * 표시 갱신
 * ════════════════════════════════════════════════════════════ */
static void bat_display_update(int pct)
{
    bool charging = battery_is_usb_connected();

    if (pct < 0) lv_label_set_text(s_pct_lbl, "--%");
    else         lv_label_set_text_fmt(s_pct_lbl, "%d%%", pct);

    if (charging) lv_obj_remove_flag(s_chg_icon, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag(s_chg_icon,    LV_OBJ_FLAG_HIDDEN);

    lv_color_t col = (charging || pct > 50) ? lv_color_hex(CLR_BAT_GREEN)  :
                     (pct > 20)             ? lv_color_hex(CLR_BAT_ORANGE) :
                                              lv_color_hex(CLR_BAT_RED);
    lv_obj_set_style_bg_color(s_fill, col, LV_STATE_DEFAULT);

    if (pct >= 0) {
        int max_w = BAT_W - BAT_BORDER * 2 - BAT_PAD * 2;
        int w     = max_w * pct / 100;
        lv_obj_set_width(s_fill, w > 0 ? w : 1);
    }
}

/* ════════════════════════════════════════════════════════════
 * 게이지 즉시 갱신 (이동 평균 기반)
 * ════════════════════════════════════════════════════════════ */
static void bat_gauge_refresh(void)
{
    if (s_queue_count == 0) return;

    int sum = 0;
    for (int i = 0; i < s_queue_count; i++) sum += s_queue[i];
    int vbat_avg_mv = sum / s_queue_count;

    bool usb = battery_is_usb_connected();
    int  pct = battery_mv_to_pct(vbat_avg_mv);

    ESP_LOGI(TAG, "%s  vbat_avg=%dmV  pct=%d%%", usb ? "USB" : "BAT", vbat_avg_mv, pct);
    s_last_pct = pct;
    bat_display_update(pct);
}

/* ════════════════════════════════════════════════════════════
 * 배터리 샘플 콜백 (1 Hz)
 *   - 매 샘플: 순환 버퍼(11개) 저장
 *   - USB 상태 변경 시: 게이지 즉시 갱신
 *   - 10초마다: 게이지 정기 갱신
 * ════════════════════════════════════════════════════════════ */
static void bat_sample_cb(lv_timer_t *t)
{
    (void)t;

    int vbat_mv = battery_read_mv();
    if (vbat_mv == 0) return;

    /* 순환 버퍼에 저장 + 누적 max 갱신 */
    s_queue[s_queue_head] = vbat_mv;
    s_queue_head  = (s_queue_head + 1) % BAT_SAMPLE_COUNT;
    if (s_queue_count < BAT_SAMPLE_COUNT) s_queue_count++;
    if (vbat_mv > s_all_peak) s_all_peak = vbat_mv;
    s_sample_n++;

    /* 최초 샘플: 즉시 게이지 갱신 */
    if (s_last_pct < 0) {
        bat_gauge_refresh();
        return;
    }

    /* USB 연결/해제: pct 즉시 재계산 + 전체 갱신 */
    bool usb_now = battery_is_usb_connected();
    if (usb_now != s_usb_prev) {
        s_usb_prev = usb_now;
        bat_gauge_refresh();
        return;
    }

    /* 배터리 게이지 pct 재계산: 10초마다 */
    if (s_sample_n >= BAT_GAUGE_EVERY) {
        s_sample_n = 0;
        bat_gauge_refresh();
        return;
    }

    /* 충전 아이콘·색상: 1초마다 즉시 반영 (N<10 일반 틱) */
    if (s_last_pct >= 0) bat_display_update(s_last_pct);
}

/* ════════════════════════════════════════════════════════════
 * 배터리 위젯
 * ════════════════════════════════════════════════════════════ */
static void create_battery_widget(lv_obj_t *parent)
{
    /* 수평 행 — 부모 중앙 */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(row);

    /* ⚡ 충전 아이콘 (montserrat 심볼 폰트) */
    s_chg_icon = lv_label_create(row);
    lv_label_set_text(s_chg_icon, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(s_chg_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_chg_icon, lv_color_hex(CLR_CHG), 0);
    lv_obj_add_flag(s_chg_icon, LV_OBJ_FLAG_HIDDEN);

    /* 배터리 본체 + 단자 래퍼 */
    lv_obj_t *bwrap = lv_obj_create(row);
    lv_obj_remove_style_all(bwrap);
    lv_obj_set_size(bwrap, BAT_W + BAT_TIP_W + 1, BAT_H);
    lv_obj_clear_flag(bwrap, LV_OBJ_FLAG_SCROLLABLE);

    /* 외곽 틀 */
    lv_obj_t *body = lv_obj_create(bwrap);
    lv_obj_set_size(body, BAT_W, BAT_H);
    lv_obj_align(body, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(CLR_BAT_BORDER), 0);
    lv_obj_set_style_border_width(body, BAT_BORDER, 0);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_set_style_pad_all(body, BAT_PAD, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* 채움 바 */
    s_fill = lv_obj_create(body);
    lv_obj_set_size(s_fill, 0, LV_PCT(100));
    lv_obj_align(s_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_fill, lv_color_hex(CLR_BAT_GREEN), 0);
    lv_obj_set_style_bg_opa(s_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_fill, 0, 0);
    lv_obj_set_style_radius(s_fill, 2, 0);
    lv_obj_clear_flag(s_fill, LV_OBJ_FLAG_SCROLLABLE);

    /* 단자 범프 */
    lv_obj_t *tip = lv_obj_create(bwrap);
    lv_obj_set_size(tip, BAT_TIP_W, BAT_TIP_H);
    lv_obj_align(tip, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(tip, lv_color_hex(CLR_BAT_BORDER), 0);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tip, 0, 0);
    lv_obj_set_style_radius(tip, 2, 0);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);

    /* 퍼센트 — 12pt */
    s_pct_lbl = lv_label_create(row);
    lv_label_set_text(s_pct_lbl, "--%");
    lv_obj_set_style_text_font(s_pct_lbl, UI_FONT_12, 0);
    lv_obj_set_style_text_color(s_pct_lbl, lv_color_hex(CLR_TEXT_LIGHT), 0);
}

/* ════════════════════════════════════════════════════════════
 * 섹션 컨테이너 헬퍼
 * ════════════════════════════════════════════════════════════ */
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
 * DHT22 샘플 콜백
 * ════════════════════════════════════════════════════════════ */
static void dht_sample_cb(lv_timer_t *t)
{
    (void)t;
    float temp = 0.0f, humi = 0.0f;

    if (dht22_read(&temp, &humi)) {
        /* LVGL 빌트인 sprintf는 %f 미지원 → 정수 연산으로 직접 포맷 */
        int t10 = (int)(temp * 10.0f + (temp >= 0.0f ? 0.5f : -0.5f));
        bool t_neg = t10 < 0;
        if (t_neg) t10 = -t10;
        lv_label_set_text_fmt(s_temp_value, "%s%d.%d °C",
                              t_neg ? "-" : "", t10 / 10, t10 % 10);

        int h = (int)(humi + 0.5f);
        lv_label_set_text_fmt(s_humi_value, "%d %%", h);
    } else {
        lv_label_set_text(s_temp_value, "센서 없음");
        lv_label_set_text(s_humi_value, "센서 없음");
    }

    lv_obj_align(s_temp_value, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(s_humi_value, LV_ALIGN_CENTER, 0, 0);
}

/* ════════════════════════════════════════════════════════════
 * 공개 API
 * ════════════════════════════════════════════════════════════ */
void ui_create_dashboard(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* 1단 — 온도 */
    lv_obj_t *sec1 = make_section(parent, 0, SEC1_H, COLOR_BG);
    s_temp_value = make_sensor_value(sec1, "대기 온도", CLR_TEMP);

    /* 2단 — 습도 */
    lv_obj_t *sec2 = make_section(parent, SEC1_H, SEC2_H, CLR_SEC2);
    s_humi_value = make_sensor_value(sec2, "대기 습도", CLR_HUMI);

    /* 3단 — 배터리/전원 */
    lv_obj_t *sec3 = make_section(parent, SEC1_H + SEC2_H, SEC3_H, CLR_SEC3);
    create_battery_widget(sec3);

    /* GPIO12 내부 pull-up 명시적 해제 (ADC2_CH1 입력 안정화) */
    gpio_set_pull_mode(GPIO_NUM_12, GPIO_FLOATING);

    battery_config_t batt_cfg = {
        .adc_unit    = BAT_ADC_UNIT,
        .adc_channel = BAT_ADC_CH,
        .atten       = BAT_ADC_ATTEN,
        .divider     = VBAT_DIV,
        .full_mv     = VBAT_FULL_MV,
        .empty_mv    = VBAT_EMPTY_MV,
        .ctrl_gpio   = BAT_CTRL_PIN,
    };
    battery_init(&batt_cfg);
    lv_timer_create(bat_sample_cb, BAT_SAMPLE_MS, NULL);

    dht22_init(GPIO_NUM_1);
    lv_timer_create(dht_sample_cb, DHT_SAMPLE_MS, NULL);
}
