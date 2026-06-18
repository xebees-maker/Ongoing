/**
 * @file    history_log.c
 * @brief   측정값 1개월치 시간 단위 히스토리 로그 — NVS 영구 저장
 */

#include "history_log.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "history_log";

#define NVS_NAMESPACE  "hist"

typedef struct {
    uint32_t last_write_epoch;
    uint32_t head;            /* 최신 샘플의 인덱스 */
    uint32_t total_ticks;     /* 누적 커밋 횟수 (랩 없음) */
    uint32_t magic_tick_sec;  /* 기록 당시 HISTORY_TICK_SEC — 빌드 간 변경 감지용 */
} history_meta_t;

/* metric별 고정소수점 배율 — temp/humi는 0.1 단위 해상도, CO2/배터리는 정수 그대로 */
static const float s_scale[HISTORY_METRIC_COUNT] = {
    10.0f,  /* HISTORY_METRIC_DHT_TEMP */
    10.0f,  /* HISTORY_METRIC_DHT_HUMI */
    1.0f,   /* HISTORY_METRIC_SCD_CO2  */
    10.0f,  /* HISTORY_METRIC_SCD_TEMP */
    10.0f,  /* HISTORY_METRIC_SCD_HUMI */
    1.0f,   /* HISTORY_METRIC_BATT_PCT */
};

static const uint32_t s_window_ticks[HISTORY_WINDOW_COUNT] = {
    HISTORY_WINDOW_8H_TICKS,
    HISTORY_WINDOW_DAY_TICKS,
    HISTORY_WINDOW_WEEK_TICKS,
    HISTORY_WINDOW_MONTH_TICKS,
};

static nvs_handle_t s_nvs = 0;
static bool         s_loaded = false;

static int16_t  s_ring[HISTORY_METRIC_COUNT][HISTORY_TICK_CAPACITY];
static int16_t  s_staged[HISTORY_METRIC_COUNT];
static history_meta_t s_meta;

static const char *ring_key(history_metric_t m)
{
    static const char *keys[HISTORY_METRIC_COUNT] = { "m0", "m1", "m2", "m3", "m4", "m5" };
    return keys[m];
}

static void stage_reset(void)
{
    for (int m = 0; m < HISTORY_METRIC_COUNT; m++) s_staged[m] = INT16_MIN;
}

static void rings_reset(void)
{
    for (int m = 0; m < HISTORY_METRIC_COUNT; m++) {
        for (int i = 0; i < HISTORY_TICK_CAPACITY; i++) s_ring[m][i] = INT16_MIN;
    }
}

static esp_err_t persist_all(void)
{
    for (int m = 0; m < HISTORY_METRIC_COUNT; m++) {
        esp_err_t err = nvs_set_blob(s_nvs, ring_key((history_metric_t)m), s_ring[m], sizeof(s_ring[m]));
        if (err != ESP_OK) return err;
    }
    /* meta는 마지막에 기록 — 전원 손실 시 최악의 경우 이번 틱만 누락되고 기존 데이터는 보존됨 */
    esp_err_t err = nvs_set_blob(s_nvs, "meta", &s_meta, sizeof(s_meta));
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs);
}

bool history_log_init(void)
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 실패: %s", esp_err_to_name(err));
        return false;
    }

    size_t sz = sizeof(s_meta);
    err = nvs_get_blob(s_nvs, "meta", &s_meta, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "기존 데이터 없음 — 새 링버퍼로 초기화");
        rings_reset();
        s_meta.last_write_epoch = 0;
        s_meta.head             = HISTORY_TICK_CAPACITY - 1;  /* 첫 커밋이 인덱스 0에 기록되도록 */
        s_meta.total_ticks      = 0;
        s_meta.magic_tick_sec   = HISTORY_TICK_SEC;
        stage_reset();
        esp_err_t werr = persist_all();
        if (werr != ESP_OK) {
            ESP_LOGE(TAG, "초기 데이터 기록 실패: %s", esp_err_to_name(werr));
        }
    } else if (err == ESP_OK) {
        for (int m = 0; m < HISTORY_METRIC_COUNT; m++) {
            size_t rsz = sizeof(s_ring[m]);
            esp_err_t rerr = nvs_get_blob(s_nvs, ring_key((history_metric_t)m), s_ring[m], &rsz);
            if (rerr != ESP_OK) {
                ESP_LOGW(TAG, "%s 로드 실패(%s) — 해당 지표 초기화", ring_key((history_metric_t)m), esp_err_to_name(rerr));
                for (int i = 0; i < HISTORY_TICK_CAPACITY; i++) s_ring[m][i] = INT16_MIN;
            }
        }
        if (s_meta.magic_tick_sec != HISTORY_TICK_SEC) {
            ESP_LOGW(TAG, "HISTORY_TICK_SEC 변경 감지(%u -> %u) — 기존 샘플의 시각은 부정확할 수 있음",
                     (unsigned)s_meta.magic_tick_sec, (unsigned)HISTORY_TICK_SEC);
            s_meta.magic_tick_sec = HISTORY_TICK_SEC;
        }
        stage_reset();
        ESP_LOGI(TAG, "기존 데이터 로드 완료 — total_ticks=%u head=%u last_write_epoch=%u",
                 (unsigned)s_meta.total_ticks, (unsigned)s_meta.head, (unsigned)s_meta.last_write_epoch);
    } else {
        ESP_LOGE(TAG, "meta 로드 실패(%s) — 새 링버퍼로 초기화", esp_err_to_name(err));
        rings_reset();
        s_meta.last_write_epoch = 0;
        s_meta.head             = HISTORY_TICK_CAPACITY - 1;
        s_meta.total_ticks      = 0;
        s_meta.magic_tick_sec   = HISTORY_TICK_SEC;
        stage_reset();
    }

    s_loaded = true;
    return true;
}

void history_log_set_time(time_t now)
{
    struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "벽시계 시각 주입: epoch=%lld", (long long)now);
}

time_t history_log_now(void)
{
    return time(NULL);
}

void history_log_record(history_metric_t metric, float value)
{
    if (metric >= HISTORY_METRIC_COUNT) return;
    s_staged[metric] = (int16_t)lroundf(value * s_scale[metric]);
}

void history_log_tick_commit(void)
{
    if (!s_loaded) return;

    uint32_t new_head = (s_meta.head + 1) % HISTORY_TICK_CAPACITY;
    for (int m = 0; m < HISTORY_METRIC_COUNT; m++) {
        s_ring[m][new_head] = s_staged[m];
    }
    s_meta.head             = new_head;
    s_meta.total_ticks++;
    s_meta.last_write_epoch = (uint32_t)history_log_now();

    esp_err_t err = persist_all();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "틱 커밋 영속화 실패: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "틱 커밋: head=%u total_ticks=%u", (unsigned)s_meta.head, (unsigned)s_meta.total_ticks);
    }

    stage_reset();
}

/* k=0(최신)부터 과거로 거슬러 올라간 위치의 원시값 — 미기록 슬롯이면 INT16_MIN */
static int16_t ring_get(history_metric_t metric, uint32_t k, uint32_t valid_count)
{
    if (k >= valid_count) return INT16_MIN;
    uint32_t idx = (s_meta.head + HISTORY_TICK_CAPACITY - k) % HISTORY_TICK_CAPACITY;
    return s_ring[metric][idx];
}

static time_t ring_time(uint32_t k)
{
    return (time_t)((int64_t)s_meta.last_write_epoch - (int64_t)k * HISTORY_TICK_SEC);
}

bool history_log_get_stats(history_metric_t metric, history_window_t window, history_stats_t *out)
{
    if (!s_loaded || metric >= HISTORY_METRIC_COUNT || window >= HISTORY_WINDOW_COUNT || !out) return false;

    memset(out, 0, sizeof(*out));

    uint32_t valid_count = (s_meta.total_ticks < HISTORY_TICK_CAPACITY) ? s_meta.total_ticks : HISTORY_TICK_CAPACITY;
    uint32_t n = s_window_ticks[window];
    if (n > valid_count) n = valid_count;

    if (n == 0) {
        out->valid = false;
        return true;
    }

    float scale = s_scale[metric];
    float sum = 0.0f;
    uint32_t cnt = 0;
    float min_v = 0.0f, max_v = 0.0f;
    time_t min_t = 0, max_t = 0;

    for (uint32_t k = 0; k < n; k++) {
        int16_t raw = ring_get(metric, k, valid_count);
        if (raw == INT16_MIN) continue;  /* 해당 틱에 값이 기록되지 않음(센서 오류 등) */

        float v = (float)raw / scale;
        time_t t = ring_time(k);

        if (cnt == 0) {
            min_v = max_v = v;
            min_t = max_t = t;
        } else {
            if (v < min_v) { min_v = v; min_t = t; }
            if (v > max_v) { max_v = v; max_t = t; }
        }
        sum += v;
        cnt++;
    }

    if (cnt == 0) {
        out->valid = false;
        return true;
    }

    out->valid        = true;
    out->min_val       = min_v;
    out->min_time       = min_t;
    out->max_val       = max_v;
    out->max_time       = max_t;
    out->avg_val       = sum / (float)cnt;
    out->sample_count  = cnt;
    return true;
}
