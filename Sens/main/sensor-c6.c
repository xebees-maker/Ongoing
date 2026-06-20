/**
 * @file    sensor-c6.c
 * @brief   ESP32-C6 센서 노드 — DHT22 + SCD41 (브링업 단계, 영문 라벨)
 */

#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <stdio.h>

#include "bsp_c6.h"
#include "dht22.h"
#include "scd41.h"
#include "battery.h"
#include "history_log.h"
#include "wifi_dashboard.h"
#include "esp_now_node.h"

static const char *TAG = "sensor-c6";

#define DHT22_PIN        GPIO_NUM_8
#define SCD41_I2C_PORT   1
#define SCD41_SDA_PIN    GPIO_NUM_6
#define SCD41_SCL_PIN    GPIO_NUM_7

/* VBUS 분압(100k+100k, H1 VBUS → IO5) — 실측: USB 연결 시 ADC핀 ~2.3V, 배터리 단독 시 ~0V */
#define VBUS_SENSE_PIN      GPIO_NUM_5
#define VBUS_SENSE_CHANNEL  ADC_CHANNEL_5
#define VBUS_SENSE_ATTEN    ADC_ATTEN_DB_12
#define VBUS_PRESENT_MV     1200   /* 실측 0V(미연결)/2.3V(연결) 중간보다 한참 아래 — 충분한 마진 */

#define SAMPLE_MS        3000   /* DHT22 최소 호출 간격 2초 이상 — 여유 두고 3초 (진단용) */
#define HISTORY_TIMER_MS (HISTORY_TICK_SEC * 1000)

#define SCREEN_IDLE_TIMEOUT_MS  10000U
#define SCREEN_CHECK_PERIOD_MS    500U

#define SCD_STALE_MS  20000U   /* scd41.c 내부 워치독(16초)보다 살짝 길게 — 복구 중을 오류로 오인하지 않도록 */

#define BATT_SAMPLE_COUNT  11  /* Cntl ui_dashboard.c와 동일한 이동평균 윈도우 — ADC 노이즈로 인한 출렁임 완화 */

/* 배터리마다 충전 IC가 충전을 종료시키는 실제 완충 전압이 다름(실측: 4.02V / 4.08V) —
 * 고정값으로는 한쪽이 항상 100%를 못 찍으므로, 전원 공급 중에 추세가 평탄해지는 지점을
 * "이번 배터리의 완충 전압"으로 학습해서 NVS에 저장하고 다음 부팅에도 재사용한다. */
#define BATT_TREND_SAMPLES      20       /* SAMPLE_MS(3초) * 20 = 약 1분 창 — 완충 전압 평탄 구간 학습용 */
#define BATT_PLATEAU_BAND_MV    6        /* 추세 창 내 최대-최소가 이 안이면 "평탄"으로 판단 */
#define BATT_PLATEAU_MIN_MV     3900.0f  /* 평탄해도 이 전압 미만이면 중간 방전 구간 평탄으로 보고 학습 안 함 */
#define BATT_FULL_MV_DEFAULT    4020.0f  /* NVS에 학습값이 없을 때(최초 부팅) 쓰는 초기값 */
#define NVS_NS_BATTCAL          "battcal"
#define NVS_KEY_FULL_MV         "full_mv"

static lv_obj_t *s_temp_lbl     = NULL;
static lv_obj_t *s_humi_lbl     = NULL;
static lv_obj_t *s_co2_lbl      = NULL;
static lv_obj_t *s_scd_temp_lbl = NULL;
static lv_obj_t *s_scd_humi_lbl = NULL;
static lv_obj_t *s_batt_lbl     = NULL;

static lv_obj_t *s_main_scr     = NULL;
static lv_obj_t *s_detail_scr   = NULL;
static lv_obj_t *s_detail_title = NULL;
static lv_obj_t *s_detail_list  = NULL;

/* sample_cb(3초)이 갱신하고 history_tick_cb가 소비하는 최신값 캐시 */
static float s_last_dht_temp = 0.0f, s_last_dht_humi = 0.0f;
static bool  s_last_dht_ok   = false;
static int   s_last_co2      = 0;
static float s_last_scd_temp = 0.0f, s_last_scd_humi = 0.0f;
static bool  s_last_scd_ok   = false;
static int   s_last_batt_pct = 0;
static bool  s_last_batt_ok  = false;
static bool  s_last_powered  = false;
static uint32_t s_scd_last_ok_ms = 0;  /* 0 = 부팅 후 아직 한 번도 성공 안 함 */

static int s_batt_queue[BATT_SAMPLE_COUNT];  /* 순환 버퍼 — battery_read_mv() 값(mV) 보관 */
static int s_batt_queue_head  = 0;
static int s_batt_queue_count = 0;

static adc_oneshot_unit_handle_t s_vbus_adc = NULL;

static int s_batt_trend[BATT_TREND_SAMPLES];  /* 순환 버퍼 — avg_mv(이동평균) 완충 전압 학습용 */
static int s_batt_trend_head  = 0;
static int s_batt_trend_count = 0;

static float s_full_mv             = BATT_FULL_MV_DEFAULT;  /* 학습된(또는 기본) 완충 전압 */
static int   s_full_mv_persisted   = 0;                     /* NVS에 마지막으로 쓴 값 — 중복 쓰기 방지 */

static bool     s_screen_off    = false;
static uint32_t s_last_input_ms = 0;

static const char *metric_name(history_metric_t m)
{
    switch (m) {
        case HISTORY_METRIC_DHT_TEMP: return "DH Temp";
        case HISTORY_METRIC_DHT_HUMI: return "DH Humi";
        case HISTORY_METRIC_SCD_CO2:  return "SCD CO2";
        case HISTORY_METRIC_SCD_TEMP: return "SCD TEMP";
        case HISTORY_METRIC_SCD_HUMI: return "SCD-Humi";
        case HISTORY_METRIC_BATT_PCT: return "BATT";
        default: return "?";
    }
}

static const char *metric_unit(history_metric_t m)
{
    switch (m) {
        case HISTORY_METRIC_DHT_TEMP:
        case HISTORY_METRIC_SCD_TEMP: return "C";
        case HISTORY_METRIC_DHT_HUMI:
        case HISTORY_METRIC_SCD_HUMI:
        case HISTORY_METRIC_BATT_PCT: return "%";
        case HISTORY_METRIC_SCD_CO2:  return "ppm";
        default: return "";
    }
}

static void format_ts(time_t t, char *buf, size_t buflen)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, buflen, "%m/%d %H:%M", &tmv);
}

/* LVGL 내장 lv_snprintf는 %f를 지원하지 않음 — 정수 연산으로 직접 포맷 */
static void format_val1(float v, char *buf, size_t buflen)
{
    int v10 = (int)(v * 10.0f + (v >= 0.0f ? 0.5f : -0.5f));
    bool neg = v10 < 0;
    if (neg) v10 = -v10;
    snprintf(buf, buflen, "%s%d.%d", neg ? "-" : "", v10 / 10, v10 % 10);
}

static void back_btn_clicked_cb(lv_event_t *e)
{
    (void)e;
    lv_screen_load(s_main_scr);
}

/* 눌렀을 때 배경/글자색 반전 — 탭 가능한 라벨 공통 시각 피드백 */
static void apply_press_invert(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(obj, lv_color_hex(0x1A1A2E), LV_STATE_PRESSED);
}

static void open_detail_screen(history_metric_t metric)
{
    static const struct { history_window_t w; const char *label; } windows[HISTORY_WINDOW_COUNT] = {
        { HISTORY_WINDOW_8H,    "Last 8 hours" },
        { HISTORY_WINDOW_DAY,   "Last 1 day"   },
        { HISTORY_WINDOW_WEEK,  "Last 1 week"  },
        { HISTORY_WINDOW_MONTH, "Last 1 month" },
    };

    lv_label_set_text(s_detail_title, metric_name(metric));
    lv_obj_clean(s_detail_list);

    for (int i = 0; i < HISTORY_WINDOW_COUNT; i++) {
        history_stats_t st;
        bool ok = history_log_get_stats(metric, windows[i].w, &st);

        lv_obj_t *card = lv_obj_create(s_detail_list);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(card, 4, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x16213E), 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *hdr = lv_label_create(card);
        lv_obj_set_width(hdr, LV_PCT(100));
        lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hdr, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(hdr, windows[i].label);

        if (!ok || !st.valid) {
            lv_obj_t *na = lv_label_create(card);
            lv_obj_set_style_text_font(na, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(na, lv_color_hex(0xAAAAAA), 0);
            lv_label_set_text(na, " no data yet");
            continue;
        }

        char vbuf[16];
        char tbuf[24];

        format_val1(st.avg_val, vbuf, sizeof(vbuf));
        lv_obj_t *avg = lv_label_create(card);
        lv_obj_set_style_text_font(avg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(avg, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text_fmt(avg, " A %s %s", vbuf, metric_unit(metric));

        format_val1(st.min_val, vbuf, sizeof(vbuf));
        format_ts(st.min_time, tbuf, sizeof(tbuf));
        lv_obj_t *minl = lv_label_create(card);
        lv_obj_set_style_text_font(minl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(minl, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text_fmt(minl, " N %s @ %s", vbuf, tbuf);

        format_val1(st.max_val, vbuf, sizeof(vbuf));
        format_ts(st.max_time, tbuf, sizeof(tbuf));
        lv_obj_t *maxl = lv_label_create(card);
        lv_obj_set_style_text_font(maxl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(maxl, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text_fmt(maxl, " X %s @ %s", vbuf, tbuf);
    }

    lv_screen_load(s_detail_scr);
}

static void label_clicked_cb(lv_event_t *e)
{
    history_metric_t metric = (history_metric_t)(intptr_t)lv_event_get_user_data(e);
    open_detail_screen(metric);
}

static void build_detail_screen(void)
{
    s_detail_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_detail_scr, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(s_detail_scr, LV_OPA_COVER, 0);

    lv_obj_t *back_lbl = lv_label_create(s_detail_scr);
    lv_label_set_text(back_lbl, "< Back");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_size(back_lbl, 70, 28);
    lv_obj_align(back_lbl, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_add_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_lbl, back_btn_clicked_cb, LV_EVENT_CLICKED, NULL);
    apply_press_invert(back_lbl);

    s_detail_title = lv_label_create(s_detail_scr);
    lv_obj_set_style_text_font(s_detail_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_detail_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_detail_title, LV_ALIGN_TOP_MID, 0, 40);

    s_detail_list = lv_obj_create(s_detail_scr);
    lv_obj_set_size(s_detail_list, BSP_LCD_H_RES, BSP_LCD_V_RES - 70);
    lv_obj_align(s_detail_list, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_flex_flow(s_detail_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_detail_list, 6, 0);
    lv_obj_set_style_bg_opa(s_detail_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_detail_list, 0, 0);
}

/* VBUS 분압 직접 측정 — usb_serial_jtag_is_connected()와 달리 데이터선 없는
 * 단순 충전기도 감지함 (H1 VBUS → 100k/100k 분압 → IO5) */
static bool vbus_is_present(void)
{
    if (!s_vbus_adc) return false;
    int raw = 0;
    if (adc_oneshot_read(s_vbus_adc, VBUS_SENSE_CHANNEL, &raw) != ESP_OK) return false;
    int mv = (int)((float)raw * 3100.0f / 4095.0f);
    return mv >= VBUS_PRESENT_MV;
}

/* 전원 공급 중에 추세 창(약 1분) 전체가 평탄하고 충분히 높은 전압이면 "이번 배터리는
 * 여기서 완충됨"으로 보고 학습한다. NVS에는 값이 바뀐 경우에만 써서 마모를 줄인다. */
static void maybe_learn_full_mv(bool powered, int avg_mv)
{
    if (!powered) return;
    if (s_batt_trend_count < BATT_TREND_SAMPLES) return;
    if ((float)avg_mv < BATT_PLATEAU_MIN_MV) return;

    int mn = s_batt_trend[0], mx = s_batt_trend[0];
    for (int i = 1; i < BATT_TREND_SAMPLES; i++) {
        if (s_batt_trend[i] < mn) mn = s_batt_trend[i];
        if (s_batt_trend[i] > mx) mx = s_batt_trend[i];
    }
    if (mx - mn > BATT_PLATEAU_BAND_MV) return;  /* 아직 충전 중 — 평탄하지 않음 */

    if (avg_mv == (int)s_full_mv) return;  /* 이미 이 값으로 학습됨 */

    s_full_mv = (float)avg_mv;
    battery_set_full_mv(s_full_mv);
    ESP_LOGI(TAG, "배터리 완충 전압 학습: %d mV", avg_mv);

    if (avg_mv != s_full_mv_persisted) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_BATTCAL, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, NVS_KEY_FULL_MV, avg_mv);
            nvs_commit(h);
            nvs_close(h);
            s_full_mv_persisted = avg_mv;
        }
    }
}

static void sample_cb(lv_timer_t *t)
{
    (void)t;

    float temp = 0.0f, humi = 0.0f;
    s_last_dht_ok = dht22_read(&temp, &humi);
    if (s_last_dht_ok) {
        s_last_dht_temp = temp;
        s_last_dht_humi = humi;

        int t10 = (int)(temp * 10.0f + (temp >= 0.0f ? 0.5f : -0.5f));
        bool t_neg = t10 < 0;
        if (t_neg) t10 = -t10;
        lv_label_set_text_fmt(s_temp_lbl, "DH Temp  %s%d.%d C", t_neg ? "-" : "", t10 / 10, t10 % 10);
        lv_label_set_text_fmt(s_humi_lbl, "DH Humi  %d %%", (int)(humi + 0.5f));
    } else {
        lv_label_set_text(s_temp_lbl, "DH Temp  --");
        lv_label_set_text(s_humi_lbl, "DH Humi  --");
    }

    int co2 = 0;
    float scd_temp = 0.0f, scd_humi = 0.0f;
    s_last_scd_ok = scd41_read(&co2, &scd_temp, &scd_humi);
    if (s_last_scd_ok) {
        s_scd_last_ok_ms = lv_tick_get();
        s_last_co2      = co2;
        s_last_scd_temp = scd_temp;
        s_last_scd_humi = scd_humi;

        lv_label_set_text_fmt(s_co2_lbl, "SCD CO2   %d ppm", co2);

        int st10 = (int)(scd_temp * 10.0f + (scd_temp >= 0.0f ? 0.5f : -0.5f));
        bool st_neg = st10 < 0;
        if (st_neg) st10 = -st10;
        lv_label_set_text_fmt(s_scd_temp_lbl, "SCD TEMP %s%d.%d C", st_neg ? "-" : "", st10 / 10, st10 % 10);
        lv_label_set_text_fmt(s_scd_humi_lbl, "SCD-Humi %d %%", (int)(scd_humi + 0.5f));
    } else if (s_scd_last_ok_ms != 0 && (lv_tick_get() - s_scd_last_ok_ms) >= SCD_STALE_MS) {
        /* 정상적인 '아직 준비 안 됨'(5초 주기 vs 3초 폴링)을 오류로 오인하지 않도록
         * 일정 시간 이상 성공이 없을 때만 stale로 표시 — 그 전엔 마지막 값 유지 */
        lv_label_set_text(s_co2_lbl,      "SCD CO2   stale");
        lv_label_set_text(s_scd_temp_lbl, "SCD TEMP  stale");
        lv_label_set_text(s_scd_humi_lbl, "SCD-Humi  stale");
    }

    int batt_mv = battery_read_mv();
    s_last_batt_ok = (batt_mv > 0);
    bool powered = vbus_is_present();
    if (s_last_batt_ok) {
        s_batt_queue[s_batt_queue_head] = batt_mv;
        s_batt_queue_head = (s_batt_queue_head + 1) % BATT_SAMPLE_COUNT;
        if (s_batt_queue_count < BATT_SAMPLE_COUNT) s_batt_queue_count++;

        int sum = 0;
        for (int i = 0; i < s_batt_queue_count; i++) sum += s_batt_queue[i];
        int avg_mv = sum / s_batt_queue_count;

        s_batt_trend[s_batt_trend_head] = avg_mv;
        s_batt_trend_head = (s_batt_trend_head + 1) % BATT_TREND_SAMPLES;
        if (s_batt_trend_count < BATT_TREND_SAMPLES) s_batt_trend_count++;

        maybe_learn_full_mv(powered, avg_mv);
        s_last_batt_pct = battery_mv_to_pct(avg_mv);
        s_last_powered  = powered;
        lv_label_set_text_fmt(s_batt_lbl, "BATT  %d%%  %s", s_last_batt_pct, powered ? "USB" : "BAT");
    } else {
        lv_label_set_text(s_batt_lbl, "BATT  --");
    }

    /* LCD는 SCD41 실패 시 stale 타임아웃 전까지 마지막 값을 그대로 보여줌 — 웹도 동일하게,
     * 이번 사이클의 성공/실패(s_last_scd_ok)가 아니라 "아직 stale은 아님"을 ok로 보냄
     * (5초 주기 센서를 3초마다 폴링하니 매 사이클 실패가 정상이라 그대로 보내면 깜빡임) */
    bool scd_display_ok = s_scd_last_ok_ms != 0 &&
                           (lv_tick_get() - s_scd_last_ok_ms) < SCD_STALE_MS;

    wifi_dashboard_set_readings(s_last_dht_temp, s_last_dht_humi, s_last_dht_ok,
                                 s_last_co2, s_last_scd_temp, s_last_scd_humi, scd_display_ok,
                                 s_last_batt_pct, s_last_batt_ok, s_last_powered);
}

static void history_tick_cb(lv_timer_t *t)
{
    (void)t;

    if (s_last_dht_ok) {
        history_log_record(HISTORY_METRIC_DHT_TEMP, s_last_dht_temp);
        history_log_record(HISTORY_METRIC_DHT_HUMI, s_last_dht_humi);
    }
    if (s_last_scd_ok) {
        history_log_record(HISTORY_METRIC_SCD_CO2,  (float)s_last_co2);
        history_log_record(HISTORY_METRIC_SCD_TEMP, s_last_scd_temp);
        history_log_record(HISTORY_METRIC_SCD_HUMI, s_last_scd_humi);
    }
    if (s_last_batt_ok) {
        history_log_record(HISTORY_METRIC_BATT_PCT, (float)s_last_batt_pct);
    }

    history_log_tick_commit();
}

/* USB 연결 중: 항상 켜짐 / 배터리 모드: 10초 무터치 → 꺼짐, 터치 → 켜짐(웨이크업 제스처는 소비) */
static bool s_suppress_touch = false;  /* 웨이크업을 트리거한 제스처 — 손을 뗄 때까지 전부 흡수 */

static bool screen_touch_hook(bool pressed, lv_point_t pt)
{
    (void)pt;

    if (pressed) {
        s_last_input_ms = lv_tick_get();

        if (s_screen_off) {
            bsp_display_set_brightness(BSP_LCD_BRIGHTNESS_DEFAULT);
            s_screen_off = false;
            s_suppress_touch = true;
        }

        return s_suppress_touch;
    }

    /* released */
    if (s_suppress_touch) {
        s_suppress_touch = false;
        return true;  /* release까지 흡수해야 LVGL이 클릭으로 인식하지 않음 */
    }
    return false;
}

static void screen_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (vbus_is_present()) {
        /* USB(충전기 포함) 연결 중에는 유휴 시계를 계속 리셋 — 뽑는 순간 그동안 쌓인
         * 유휴시간으로 즉시 타임아웃되는 것을 방지 (Cntl ui_screen.c에서 발견된 버그의 수정판) */
        s_last_input_ms = lv_tick_get();
        if (s_screen_off) {
            bsp_display_set_brightness(BSP_LCD_BRIGHTNESS_DEFAULT);
            s_screen_off = false;
        }
        return;
    }

    if (!s_screen_off) {
        uint32_t idle_ms = lv_tick_get() - s_last_input_ms;
        if (idle_ms >= SCREEN_IDLE_TIMEOUT_MS) {
            bsp_display_set_brightness(BSP_LCD_BRIGHTNESS_OFF);
            s_screen_off = true;
        }
    }
}

static lv_obj_t *make_row(lv_obj_t *parent, int y, history_metric_t metric)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "--");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    /* 탭 인식 영역을 텍스트 길이가 아니라 행 전체로 넓혀서 터치 반응성을 높임 */
    lv_obj_set_size(lbl, BSP_LCD_H_RES - 16, 28);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl, label_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)metric);
    apply_press_invert(lbl);
    return lbl;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_handle_t batt_nvs;
    if (nvs_open(NVS_NS_BATTCAL, NVS_READONLY, &batt_nvs) == ESP_OK) {
        int32_t learned_mv = 0;
        if (nvs_get_i32(batt_nvs, NVS_KEY_FULL_MV, &learned_mv) == ESP_OK) {
            s_full_mv           = (float)learned_mv;
            s_full_mv_persisted = learned_mv;
            ESP_LOGI(TAG, "저장된 배터리 완충 전압 불러옴: %d mV", (int)learned_mv);
        }
        nvs_close(batt_nvs);
    }

    ESP_ERROR_CHECK(bsp_board_init());

    history_log_init();
    if (history_log_now() < 1700000000) {  /* 2023년 이전 = 아직 시각 미주입 */
        struct tm seed_tm = {
            .tm_year = 2026 - 1900, .tm_mon = 5, .tm_mday = 18,
            .tm_hour = 12, .tm_min = 0, .tm_sec = 0,
        };
        history_log_set_time(mktime(&seed_tm));
    }

    dht22_init(DHT22_PIN);
    bool scd41_ok = scd41_init(SCD41_I2C_PORT, SCD41_SDA_PIN, SCD41_SCL_PIN);

    battery_config_t batt_cfg = {
        .adc_unit    = BSP_BATTERY_ADC_UNIT,
        .adc_channel = BSP_BATTERY_ADC_CHANNEL,
        .atten       = BSP_BATTERY_ADC_ATTEN,
        .divider     = BSP_BATTERY_DIV,
        .full_mv     = s_full_mv,  /* 기본값 또는 NVS에서 불러온 학습값 — 배터리마다 실제 완충
                                     * 전압이 다름(실측: 4.02V/4.08V), maybe_learn_full_mv()가 계속 갱신 */
        .empty_mv    = 3300.0f,
        .ctrl_gpio   = GPIO_NUM_NC,
    };
    battery_init(&batt_cfg);

    s_vbus_adc = battery_get_adc_handle();
    if (s_vbus_adc) {
        adc_oneshot_chan_cfg_t vbus_ch_cfg = {
            .atten    = VBUS_SENSE_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_oneshot_config_channel(s_vbus_adc, VBUS_SENSE_CHANNEL, &vbus_ch_cfg);
    }

    wifi_dashboard_init();
    esp_now_node_init();

    if (bsp_lvgl_lock(BSP_MUTEX_WAIT_DEFAULT)) {
        s_main_scr = lv_screen_active();
        lv_obj_set_style_bg_color(s_main_scr, lv_color_hex(0x1A1A2E), 0);
        lv_obj_set_style_bg_opa(s_main_scr, LV_OPA_COVER, 0);

        lv_obj_t *title = lv_label_create(s_main_scr);
        lv_label_set_text(title, "C6 Sensor Node");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        s_temp_lbl     = make_row(s_main_scr, 40,  HISTORY_METRIC_DHT_TEMP);
        s_humi_lbl     = make_row(s_main_scr, 88,  HISTORY_METRIC_DHT_HUMI);
        s_co2_lbl      = make_row(s_main_scr, 136, HISTORY_METRIC_SCD_CO2);
        s_scd_temp_lbl = make_row(s_main_scr, 184, HISTORY_METRIC_SCD_TEMP);
        s_scd_humi_lbl = make_row(s_main_scr, 232, HISTORY_METRIC_SCD_HUMI);
        s_batt_lbl     = make_row(s_main_scr, 280, HISTORY_METRIC_BATT_PCT);

        lv_obj_set_style_text_color(s_temp_lbl,     lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(s_humi_lbl,     lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(s_co2_lbl,      lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(s_scd_temp_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(s_scd_humi_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(s_batt_lbl,     lv_color_hex(0xFFFFFF), 0);

        if (!scd41_ok) {
            lv_label_set_text(s_co2_lbl, "SCD CO2   no sensor");
        }

        build_detail_screen();

        lv_timer_create(sample_cb, SAMPLE_MS, NULL);
        lv_timer_create(history_tick_cb, HISTORY_TIMER_MS, NULL);

        s_last_input_ms = lv_tick_get();
        bsp_indev_set_hook(screen_touch_hook);
        lv_timer_create(screen_timer_cb, SCREEN_CHECK_PERIOD_MS, NULL);

        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "C6 sensor node ready");
}
