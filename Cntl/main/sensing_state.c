#include "sensing_state.h"
#include "sensor_registry.h"
#include "node_hub.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "SENSING_ST";

/* 센싱마다 기억하는 것(PSRAM, 센싱 ID로 찾음). 상태 자체는 저장하지 않음 */
typedef struct {
    uint16_t id;
    bool     has_value;    /* 이번 부팅에 정상 값을 받음 */
    bool     range_bad;    /* 마지막으로 받은 성공 값이 범위 밖(사용자 결정 11-12 — 한 번이면 고장) */
    float    value;        /* 마지막 정상 값 */
} sensing_rec_t;

static sensing_rec_t *s_rec = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* 범위 — 센서가 물리적으로 낼 수 없는 값. 이산화탄소 50 ppm 미만은 node_hub의 CO2_PPM_PLAUSIBLE_MIN(09-07 실기)과 같음 */
static bool value_in_range(uint8_t what, float v)
{
    if (!isfinite(v)) return false;
    switch (what) {
        case SENSOR_CHAN_TEMP_C:   return v >= -40.0f && v <= 125.0f;
        case SENSOR_CHAN_HUMI_PCT: return v >= 0.0f && v <= 100.0f;
        case SENSOR_CHAN_CO2_PPM:  return v >= 50.0f && v <= 40000.0f;
        case SENSOR_CHAN_NH3_PPM:  return v >= 0.0f;
        default:                   return true;
    }
}

/* s_mux 안에서. create면 빈 칸(없으면 reuse 칸)을 줌 */
static sensing_rec_t *rec_find(uint16_t id, bool create, int reuse)
{
    if (!s_rec || !id) return NULL;
    sensing_rec_t *empty = NULL;
    for (int i = 0; i < SREG_MAX_SENSINGS; i++) {
        if (s_rec[i].id == id) return &s_rec[i];
        if (!empty && s_rec[i].id == 0) empty = &s_rec[i];
    }
    if (!create) return NULL;
    if (!empty && reuse >= 0) empty = &s_rec[reuse];
    if (empty) { memset(empty, 0, sizeof(*empty)); empty->id = id; }
    return empty;
}

/* 칸이 가득 찼을 때 다시 쓸 칸 — 지금 목록에 없는 센싱(완전히 지운 자리)의 칸. 잠금 밖에서(sreg는 뮤텍스) */
static int stale_slot(void)
{
    for (int i = 0; i < SREG_MAX_SENSINGS; i++) {
        uint16_t id = s_rec[i].id;
        if (id == 0 || !sreg_get_sensing(id, NULL)) return i;
    }
    return -1;
}

void sensing_state_note(uint16_t sensing, uint8_t what, bool ok, float value)
{
    if (!sensing) return;
    if (!s_rec) {
        s_rec = heap_caps_calloc(SREG_MAX_SENSINGS, sizeof(sensing_rec_t), MALLOC_CAP_SPIRAM);
        if (!s_rec) { ESP_LOGE(TAG, "PSRAM alloc failed"); return; }
    }
    if (!ok) return;   /* 실패는 센스의 연속 실패 수(sreg)로 봄 — 마지막 정상 값은 그대로 */
    bool in = value_in_range(what, value);
    bool changed;
    int reuse = -1;
    taskENTER_CRITICAL(&s_mux);
    bool known = rec_find(sensing, false, -1) != NULL;
    taskEXIT_CRITICAL(&s_mux);
    if (!known) reuse = stale_slot();
    taskENTER_CRITICAL(&s_mux);
    sensing_rec_t *r = rec_find(sensing, true, reuse);
    changed = r && r->range_bad == in;   /* 범위 판정이 바뀜 */
    if (r) {
        r->range_bad = !in;
        if (in) { r->value = value; r->has_value = true; }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (changed) {
        int v10 = (int)lroundf(value * 10.0f);
        if (!in) ESP_LOGW(TAG, "Sensing %u value out of range (%d.%d)", (unsigned)sensing, v10 / 10, abs(v10 % 10));
        else ESP_LOGI(TAG, "Sensing %u value back in range", (unsigned)sensing);
    }
}

sensing_st_t sensing_state_get(uint16_t sensing, float *out_value)
{
    uint8_t mac[6];
    uint16_t streak = 0;
    if (!sreg_sensing_link(sensing, mac, &streak)) return SENSING_ST_UNREG;
    switch (node_hub_board_link(mac)) {
        case NODE_LINK_NONE:    return SENSING_ST_UNREG;
        case NODE_LINK_UNKNOWN: return SENSING_ST_CHECKING;
        case NODE_LINK_LOST:    return SENSING_ST_LOST;
        case NODE_LINK_LIVE:    break;
    }
    bool has = false, bad = false;
    float v = 0.0f;
    taskENTER_CRITICAL(&s_mux);
    sensing_rec_t *r = rec_find(sensing, false, -1);
    if (r) { has = r->has_value; bad = r->range_bad; v = r->value; }
    taskEXIT_CRITICAL(&s_mux);
    if (bad || streak >= SENSING_FAULT_STREAK) return SENSING_ST_FAULT;
    if (!has) return SENSING_ST_CHECKING;
    if (out_value) *out_value = v;
    return SENSING_ST_USABLE;
}

const char *sensing_state_name(sensing_st_t st)
{
    static const char *const names[] = { "unregistered", "checking", "usable", "lost", "fault" };
    return (unsigned)st < sizeof(names) / sizeof(names[0]) ? names[st] : "?";
}
