/**
 * @file    sensor_registry.c
 * @brief   sensor_registry.h 구현
 */
#include "sensor_registry.h"
#include "node_hub.h"
#include "stats_store.h"
#include "fs.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "SREG";

#define SREG_FILE     FS_MOUNT_POINT "/sensors.bin"
#define SREG_VERSION  1
#define SREG_DETACHED_SEC  600   /* 이만큼 소식이 없으면 "떨어진 센서" — 같은 센싱의 새 센서가 이어받을 수 있음 */

typedef struct {
    uint16_t id;            /* 센서 ID(0 = 빈 칸) */
    uint8_t  kind;          /* sensor_kind_t */
    uint8_t  board_mac[6];  /* 지금 붙어 있는 보드 */
    uint8_t  port;
    uint8_t  chan_first;    /* 보드 채널 배열에서 시작 */
    uint8_t  chan_n;
    uint8_t  attached;      /* 1 = 이 보드·포트에 붙어 있음(같은 자리에 다른 센서가 오면 0) */
    uint32_t last_seen;     /* 유닉스 시각(페어링 응답 때) */
} sreg_sensor_t;

typedef struct {
    uint32_t       version;
    uint16_t       next_sensor_id;
    uint16_t       next_sensing_id;
    sreg_sensor_t  sensors[SREG_MAX_SENSORS];
    sreg_sensing_t sensings[SREG_MAX_SENSINGS];
} sreg_file_t;

static sreg_file_t *s_r = NULL;
/* 2026-10-23 — 센서별 측정 상태(s_r->sensors와 같은 칸, 파일에 안 씀) */
static struct { uint8_t fault; uint16_t streak; } s_st[SREG_MAX_SENSORS];
static SemaphoreHandle_t s_mtx = NULL;

static void lock(void)   { xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_mtx); }

/* 센서 모델 → 센싱의 어디·정밀도(콘이 아는 센서 지식) */
static void kind_attrs(uint8_t kind, uint8_t *where, uint8_t *precise)
{
    switch (kind) {
        case SENSOR_KIND_SHT45:
        case SENSOR_KIND_SHT40:   *where = SENSING_WHERE_AIR;       *precise = 1; break;
        case SENSOR_KIND_PT100:
        case SENSOR_KIND_DS18B20: *where = SENSING_WHERE_SUBSTRATE; *precise = 1; break;
        default:                  *where = SENSING_WHERE_AIR;       *precise = 0; break;   /* SCD41, DHT22, 암모니아 */
    }
}

static void save_job(const uint8_t *arg)
{
    (void)arg;
    if (!s_r) return;
    sreg_file_t *copy = heap_caps_malloc(sizeof(*copy), MALLOC_CAP_SPIRAM);
    if (!copy) return;
    lock();
    memcpy(copy, s_r, sizeof(*copy));
    unlock();
    FILE *f = fopen(SREG_FILE, "wb");
    if (f) {
        fwrite(copy, sizeof(*copy), 1, f);
        fclose(f);
    }
    heap_caps_free(copy);
}

static void save_later(void)
{
    stats_store_post_job(save_job, NULL, 0);   /* 통신 밖(기록 태스크)에서 LittleFS에 씀 */
}

void sreg_init(void)
{
    if (s_r) return;
    s_mtx = xSemaphoreCreateRecursiveMutex();
    s_r = heap_caps_calloc(1, sizeof(*s_r), MALLOC_CAP_SPIRAM);
    if (!s_r || !s_mtx) { ESP_LOGE(TAG, "alloc failed"); s_r = NULL; return; }
    FILE *f = fopen(SREG_FILE, "rb");
    if (f) {
        if (fread(s_r, sizeof(*s_r), 1, f) != 1 || s_r->version != SREG_VERSION) memset(s_r, 0, sizeof(*s_r));
        fclose(f);
    }
    s_r->version = SREG_VERSION;
    if (s_r->next_sensor_id == 0) s_r->next_sensor_id = 1;
    if (s_r->next_sensing_id == 0) s_r->next_sensing_id = 1;
    int ns = 0, ng = 0;
    for (int i = 0; i < SREG_MAX_SENSORS; i++) ns += s_r->sensors[i].id != 0;
    for (int i = 0; i < SREG_MAX_SENSINGS; i++) ng += s_r->sensings[i].id != 0;
    ESP_LOGI(TAG, "Sensor registry: %d sensors, %d sensings", ns, ng);
}

static sreg_sensor_t *sensor_by_id(uint16_t id)
{
    for (int i = 0; i < SREG_MAX_SENSORS; i++) if (id && s_r->sensors[i].id == id) return &s_r->sensors[i];
    return NULL;
}

static sreg_sensor_t *sensor_at(const uint8_t mac[6], uint8_t port)
{
    for (int i = 0; i < SREG_MAX_SENSORS; i++) {
        sreg_sensor_t *s = &s_r->sensors[i];
        if (s->id && s->attached && s->port == port && memcmp(s->board_mac, mac, 6) == 0) return s;
    }
    return NULL;
}

static sreg_sensor_t *sensor_new(uint16_t want_id)
{
    for (int i = 0; i < SREG_MAX_SENSORS; i++) {
        if (s_r->sensors[i].id) continue;
        memset(&s_r->sensors[i], 0, sizeof(s_r->sensors[i]));
        s_r->sensors[i].id = want_id ? want_id : s_r->next_sensor_id++;
        if (s_r->sensors[i].id >= s_r->next_sensor_id) s_r->next_sensor_id = s_r->sensors[i].id + 1;
        return &s_r->sensors[i];
    }
    /* 가득 참 — 가장 오래 소식 없는 떨어진 센서 칸을 다시 씀(그 센서의 센싱은 남아 있음) */
    int victim = -1;
    for (int i = 0; i < SREG_MAX_SENSORS; i++) {
        if (s_r->sensors[i].attached) continue;
        if (victim < 0 || s_r->sensors[i].last_seen < s_r->sensors[victim].last_seen) victim = i;
    }
    if (victim < 0) { ESP_LOGE(TAG, "Sensor table full (%d) - sensor not registered", SREG_MAX_SENSORS); return NULL; }
    ESP_LOGW(TAG, "Sensor table full - reusing slot of detached sensor %u", (unsigned)s_r->sensors[victim].id);
    memset(&s_r->sensors[victim], 0, sizeof(s_r->sensors[victim]));
    s_r->sensors[victim].id = want_id ? want_id : s_r->next_sensor_id++;
    return &s_r->sensors[victim];
}

/* 센서가 떨어졌는지(자리를 내줬거나 오래 소식 없음) */
static bool sensor_detached(const sreg_sensor_t *s, uint32_t now)
{
    if (!s) return true;
    if (!s->attached) return true;
    return now > s->last_seen + SREG_DETACHED_SEC;
}

/* 새 센서 채널의 센싱 정하기 — 이미 그 센서에 묶인 센싱이 있으면 그대로, 같은 센싱(무엇·어디·정밀도)이 하나고 그 센서가 떨어졌으면
 * 이어받기, 아니면 새 센싱 */
static void bind_sensing(const sreg_sensor_t *sen, uint8_t chan, uint8_t what, uint32_t now, bool *changed)
{
    for (int i = 0; i < SREG_MAX_SENSINGS; i++) {
        sreg_sensing_t *g = &s_r->sensings[i];
        if (g->id && g->sensor_id == sen->id && g->sensor_chan == chan) {
            if (g->what != what) { g->what = what; *changed = true; }
            return;
        }
    }
    uint8_t where, precise;
    kind_attrs(sen->kind, &where, &precise);
    int same = 0, cand = -1, max_ord = 0;
    for (int i = 0; i < SREG_MAX_SENSINGS; i++) {
        sreg_sensing_t *g = &s_r->sensings[i];
        if (!g->id || g->what != what || g->where != where || g->precise != precise) continue;
        same++;
        if (g->ordinal > max_ord) max_ord = g->ordinal;
        if (sensor_detached(sensor_by_id(g->sensor_id), now)) cand = i;
    }
    if (same == 1 && cand >= 0) {   /* 교체 — 자동 이어받기(사용자 결정 10-23) */
        sreg_sensing_t *g = &s_r->sensings[cand];
        ESP_LOGW(TAG, "Sensing %u (what %u) taken over by sensor %u (was sensor %u)", (unsigned)g->id, (unsigned)what,
                 (unsigned)sen->id, (unsigned)g->sensor_id);
        g->sensor_id = sen->id;
        g->sensor_chan = chan;
        *changed = true;
        return;
    }
    for (int i = 0; i < SREG_MAX_SENSINGS; i++) {
        sreg_sensing_t *g = &s_r->sensings[i];
        if (g->id) continue;
        memset(g, 0, sizeof(*g));
        g->id = s_r->next_sensing_id++;
        g->what = what;
        g->where = where;
        g->precise = precise;
        g->ordinal = (uint8_t)(max_ord + 1);
        g->sensor_id = sen->id;
        g->sensor_chan = chan;
        *changed = true;
        ESP_LOGI(TAG, "New sensing %u: what %u where %u precise %u #%u (sensor %u ch %u)%s", (unsigned)g->id, (unsigned)what,
                 (unsigned)where, (unsigned)precise, (unsigned)g->ordinal, (unsigned)sen->id, (unsigned)chan,
                 same ? " - same kind exists, not taken over automatically" : "");
        return;
    }
    ESP_LOGE(TAG, "Sensing table full (%d) - sensor %u ch %u not recorded", SREG_MAX_SENSINGS, (unsigned)sen->id, (unsigned)chan);
}

bool sreg_on_pair(const uint8_t mac[6], uint8_t node_kind, uint8_t chan_count, const uint8_t *chan_type,
                  uint8_t sensor_count, const esp_now_sensor_desc_t *sensors, esp_now_sensor_id_assign_t *out_assign)
{
    if (!s_r) return false;
    esp_now_sensor_desc_t legacy = { .kind = node_kind, .port = 0, .chan_first = 0, .chan_n = chan_count, .sensor_id = 0 };
    bool is_legacy = (sensor_count == 0);
    if (is_legacy) { sensors = &legacy; sensor_count = 1; }
    if (sensor_count > ESP_NOW_MAX_SENSORS) sensor_count = ESP_NOW_MAX_SENSORS;
    uint32_t now = (uint32_t)time(NULL);
    bool changed = false;
    int nassign = 0;
    lock();
    for (int k = 0; k < sensor_count; k++) {
        const esp_now_sensor_desc_t *d = &sensors[k];
        if (d->chan_first + d->chan_n > chan_count) continue;
        sreg_sensor_t *sen = NULL;
        bool tell = false;
        if (d->sensor_id) {
            sen = sensor_by_id(d->sensor_id);
            if (!sen) { sen = sensor_new(d->sensor_id); changed = true; ESP_LOGW(TAG, "Sensor %u re-registered from board", (unsigned)d->sensor_id); }
        } else {
            sen = sensor_at(mac, d->port);
            if (sen && sen->kind != d->kind) sen = NULL;   /* 같은 자리에 다른 종류 — 새 센서 */
            if (!sen) { sen = sensor_new(0); changed = true; }
            tell = !is_legacy;
        }
        if (!sen) continue;
        /* 같은 보드·포트에 붙어 있던 다른 센서는 떨어진 것으로 */
        for (int i = 0; i < SREG_MAX_SENSORS; i++) {
            sreg_sensor_t *o = &s_r->sensors[i];
            if (o != sen && o->id && o->attached && o->port == d->port && memcmp(o->board_mac, mac, 6) == 0) { o->attached = 0; changed = true; }
        }
        if (sen->kind != d->kind || memcmp(sen->board_mac, mac, 6) != 0 || sen->port != d->port || sen->chan_first != d->chan_first ||
            sen->chan_n != d->chan_n || !sen->attached) {
            sen->kind = d->kind;
            memcpy(sen->board_mac, mac, 6);
            sen->port = d->port;
            sen->chan_first = d->chan_first;
            sen->chan_n = d->chan_n;
            sen->attached = 1;
            changed = true;
        }
        sen->last_seen = now;
        for (uint8_t c = 0; c < d->chan_n; c++) bind_sensing(sen, c, chan_type[d->chan_first + c], now, &changed);
        if (tell && out_assign && nassign < ESP_NOW_MAX_SENSORS) {
            out_assign->ids[nassign].port = d->port;
            out_assign->ids[nassign].sensor_id = sen->id;
            nassign++;
        }
    }
    unlock();
    if (changed) save_later();
    if (nassign && out_assign) {
        out_assign->version = ESP_NOW_LINK_VERSION;
        out_assign->msg_type = ESP_NOW_MSG_SENSOR_ID_ASSIGN;
        out_assign->count = (uint8_t)nassign;
    }
    return nassign > 0;
}

bool sreg_sensing_for(const uint8_t mac[6], uint8_t chan_index, sreg_sensing_t *out)
{
    if (!s_r) return false;
    bool found = false;
    lock();
    for (int i = 0; i < SREG_MAX_SENSORS && !found; i++) {
        const sreg_sensor_t *s = &s_r->sensors[i];
        if (!s->id || !s->attached || memcmp(s->board_mac, mac, 6) != 0) continue;
        if (chan_index < s->chan_first || chan_index >= s->chan_first + s->chan_n) continue;
        uint8_t c = chan_index - s->chan_first;
        for (int g = 0; g < SREG_MAX_SENSINGS; g++) {
            if (s_r->sensings[g].id && s_r->sensings[g].sensor_id == s->id && s_r->sensings[g].sensor_chan == c) {
                if (out) *out = s_r->sensings[g];
                found = true;
                break;
            }
        }
    }
    unlock();
    return found;
}

bool sreg_get_sensing(uint16_t sensing_id, sreg_sensing_t *out)
{
    if (!s_r || !sensing_id) return false;
    bool found = false;
    lock();
    for (int g = 0; g < SREG_MAX_SENSINGS; g++) {
        if (s_r->sensings[g].id == sensing_id) { if (out) *out = s_r->sensings[g]; found = true; break; }
    }
    unlock();
    return found;
}

int sreg_list_sensings(sreg_sensing_t *out, int max)
{
    if (!s_r) return 0;
    int n = 0;
    lock();
    for (int g = 0; g < SREG_MAX_SENSINGS && n < max; g++) if (s_r->sensings[g].id) out[n++] = s_r->sensings[g];
    unlock();
    return n;
}

bool sreg_sensing_board(uint16_t sensing_id, uint8_t out_mac[6])
{
    if (!s_r) return false;
    bool ok = false;
    lock();
    for (int g = 0; g < SREG_MAX_SENSINGS; g++) {
        if (s_r->sensings[g].id != sensing_id) continue;
        const sreg_sensor_t *s = sensor_by_id(s_r->sensings[g].sensor_id);
        if (s && s->attached) { memcpy(out_mac, s->board_mac, 6); ok = true; }
        break;
    }
    unlock();
    return ok;
}

bool sreg_note_status(const uint8_t mac[6], uint8_t port, uint8_t fault, uint16_t streak, uint16_t *out_sensor_id)
{
    if (!s_r) return false;
    bool log = false;
    lock();
    sreg_sensor_t *s = sensor_at(mac, port);
    if (s) {
        int i = (int)(s - s_r->sensors);
        log = (fault != s_st[i].fault) ||
              (fault != SENSOR_FAULT_NONE && streak % 40 == 0 && streak != s_st[i].streak);
        s_st[i].fault = fault;
        s_st[i].streak = streak;
        *out_sensor_id = s->id;
    }
    unlock();
    return log;
}

int sreg_json(char *buf, size_t cap)
{
    if (!s_r || cap < 32) return 0;
    int n = 0;
    lock();
    n += snprintf(buf + n, cap - n, "{\"sensors\":[");
    bool first = true;
    for (int i = 0; i < SREG_MAX_SENSORS && n < (int)cap - 160; i++) {
        const sreg_sensor_t *s = &s_r->sensors[i];
        if (!s->id) continue;
        n += snprintf(buf + n, cap - n, "%s{\"id\":%u,\"kind\":%u,\"board\":\"%02x%02x%02x%02x%02x%02x\",\"port\":%u,\"ch\":[%u,%u],\"attached\":%s,\"seen\":%u,\"fault\":%u,\"streak\":%u}",
                      first ? "" : ",", (unsigned)s->id, (unsigned)s->kind, s->board_mac[0], s->board_mac[1], s->board_mac[2],
                      s->board_mac[3], s->board_mac[4], s->board_mac[5], (unsigned)s->port, (unsigned)s->chan_first,
                      (unsigned)s->chan_n, s->attached ? "true" : "false", (unsigned)s->last_seen,
                      (unsigned)s_st[i].fault, (unsigned)s_st[i].streak);
        first = false;
    }
    n += snprintf(buf + n, cap - n, "],\"sensings\":[");
    first = true;
    for (int i = 0; i < SREG_MAX_SENSINGS && n < (int)cap - 120; i++) {
        const sreg_sensing_t *g = &s_r->sensings[i];
        if (!g->id) continue;
        n += snprintf(buf + n, cap - n, "%s{\"id\":%u,\"what\":%u,\"where\":%u,\"precise\":%u,\"ord\":%u,\"sensor\":%u,\"ch\":%u}",
                      first ? "" : ",", (unsigned)g->id, (unsigned)g->what, (unsigned)g->where, (unsigned)g->precise,
                      (unsigned)g->ordinal, (unsigned)g->sensor_id, (unsigned)g->sensor_chan);
        first = false;
    }
    n += snprintf(buf + n, cap - n, "]}");
    unlock();
    return n < (int)cap ? n : (int)cap - 1;
}
