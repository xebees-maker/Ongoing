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
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "SREG";

#define SREG_FILE     FS_MOUNT_POINT "/sensors.bin"
#define SREG_VERSION  2   /* 2026-10-28 — 고정 자리: 센서 12·센싱 16, 센싱에 key_mac(캠 저장 키) */

typedef struct {
    uint16_t id;            /* 센서 ID(0 = 빈 칸) */
    uint8_t  kind;          /* sensor_kind_t(캠은 SREG_KIND_CAM) */
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

/* 1판(10-23~10-27): 센서 8·센싱 12, 센싱에 key_mac 없음 — 읽어서 2판으로 옮김 */
typedef struct {
    uint16_t id;
    uint8_t  what, where, precise, ordinal;
    uint16_t sensor_id;
    uint8_t  sensor_chan, reserved;
} sreg_sensing_v1_t;
typedef struct {
    uint32_t          version;
    uint16_t          next_sensor_id;
    uint16_t          next_sensing_id;
    sreg_sensor_t     sensors[8];
    sreg_sensing_v1_t sensings[12];
} sreg_file_v1_t;

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
        default:                  *where = SENSING_WHERE_AIR;       *precise = 0; break;   /* SCD41, DHT22, 암모니아, 캠 */
    }
}

/* 2026-10-28(사용자 결정) — 종류별 자리 수: 배지 온도 3, 캠 2, 나머지 1 */
static int slot_limit(uint8_t what, uint8_t where)
{
    if (what == SREG_WHAT_CAMERA) return 2;
    if (where == SENSING_WHERE_SUBSTRATE) return SREG_MAX_SLOTS;
    return 1;
}

static bool in_group(const sreg_sensing_t *g, uint8_t what, uint8_t where, uint8_t precise)
{
    return g->id && g->what == what && g->where == where && g->precise == precise;
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

static void convert_v1(const sreg_file_v1_t *o)
{
    s_r->next_sensor_id = o->next_sensor_id;
    s_r->next_sensing_id = o->next_sensing_id;
    memcpy(s_r->sensors, o->sensors, sizeof(o->sensors));
    for (int i = 0; i < 12; i++) {
        sreg_sensing_t *g = &s_r->sensings[i];
        g->id = o->sensings[i].id;
        g->what = o->sensings[i].what;
        g->where = o->sensings[i].where;
        g->precise = o->sensings[i].precise;
        g->ordinal = o->sensings[i].ordinal;
        g->sensor_id = o->sensings[i].sensor_id;
        g->sensor_chan = o->sensings[i].sensor_chan;
    }
    ESP_LOGW(TAG, "Sensor registry file v1 converted to v%d", SREG_VERSION);
}

void sreg_init(void)
{
    if (s_r) return;
    s_mtx = xSemaphoreCreateRecursiveMutex();
    s_r = heap_caps_calloc(1, sizeof(*s_r), MALLOC_CAP_SPIRAM);
    if (!s_r || !s_mtx) { ESP_LOGE(TAG, "alloc failed"); s_r = NULL; return; }
    FILE *f = fopen(SREG_FILE, "rb");
    if (f) {
        size_t cap = sizeof(sreg_file_t) > sizeof(sreg_file_v1_t) ? sizeof(sreg_file_t) : sizeof(sreg_file_v1_t);
        uint8_t *raw = heap_caps_calloc(1, cap, MALLOC_CAP_SPIRAM);
        size_t n = raw ? fread(raw, 1, cap, f) : 0;
        fclose(f);
        uint32_t ver = 0;
        if (n >= 4) memcpy(&ver, raw, 4);
        if (ver == SREG_VERSION && n == sizeof(sreg_file_t)) memcpy(s_r, raw, sizeof(*s_r));
        else if (ver == 1 && n == sizeof(sreg_file_v1_t)) { convert_v1((const sreg_file_v1_t *)raw); save_later(); }
        else if (n) ESP_LOGE(TAG, "Sensor registry file unreadable (v%u, %u bytes) - starting empty", (unsigned)ver, (unsigned)n);
        heap_caps_free(raw);
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

/* 이 센서가 가진 자리 수 */
static int sensor_owns(uint16_t id)
{
    int n = 0;
    for (int i = 0; i < SREG_MAX_SENSINGS; i++) n += id && s_r->sensings[i].id && s_r->sensings[i].sensor_id == id;
    return n;
}

static uint16_t take_sensor_id(void)
{
    uint16_t id;
    do { id = s_r->next_sensor_id++; if (s_r->next_sensor_id == 0) s_r->next_sensor_id = 1; } while (id == 0 || sensor_by_id(id));
    return id;
}

/* 빈 칸에 센서 등록. 칸이 없으면 자리가 없는 센서 칸을 다시 씀(자리를 가진 센서는 지우지 않음) */
static sreg_sensor_t *sensor_new(uint16_t want_id)
{
    if (want_id && sensor_by_id(want_id)) want_id = 0;
    sreg_sensor_t *e = NULL;
    for (int i = 0; i < SREG_MAX_SENSORS && !e; i++) if (!s_r->sensors[i].id) e = &s_r->sensors[i];
    for (int i = 0; i < SREG_MAX_SENSORS && !e; i++) {
        if (!s_r->sensors[i].attached && !sensor_owns(s_r->sensors[i].id)) {
            ESP_LOGW(TAG, "Sensor table full - reusing entry of sensor %u (no slot)", (unsigned)s_r->sensors[i].id);
            e = &s_r->sensors[i];
        }
    }
    if (!e) { ESP_LOGE(TAG, "Sensor table full (%d) - sensor not registered", SREG_MAX_SENSORS); return NULL; }
    memset(e, 0, sizeof(*e));
    if (want_id) {
        e->id = want_id;
        if (want_id >= s_r->next_sensor_id) s_r->next_sensor_id = want_id + 1;
    } else {
        e->id = take_sensor_id();
    }
    return e;
}

static bool board_live(const sreg_live_t *live, const uint8_t mac[6])
{
    for (int i = 0; live && i < live->n; i++) if (memcmp(live->mac[i], mac, 6) == 0) return true;
    return false;
}

/* 보드의 센서 설명 하나(옛 형식이면 보드 전체를 센서 하나로) */
static bool board_desc(const sreg_board_t *b, int k, esp_now_sensor_desc_t *d)
{
    if (b->sensor_count == 0) {
        if (k != 0) return false;
        *d = (esp_now_sensor_desc_t){ .kind = b->node_kind, .port = 0, .chan_first = 0, .chan_n = b->chan_count, .sensor_id = 0 };
        return true;
    }
    if (k >= b->sensor_count || k >= ESP_NOW_MAX_SENSORS) return false;
    *d = b->sensors[k];
    return d->chan_first + d->chan_n <= b->chan_count;
}

static int board_desc_count(const sreg_board_t *b)
{
    if (b->sensor_count == 0) return 1;
    return b->sensor_count < ESP_NOW_MAX_SENSORS ? b->sensor_count : ESP_NOW_MAX_SENSORS;
}

/* 이 보드·포트의 센서 칸(있으면). 보드가 ID를 보냈으면 ID로, 아니면 같은 자리·같은 종류 */
static sreg_sensor_t *find_sensor_for(const uint8_t mac[6], const esp_now_sensor_desc_t *d)
{
    if (d->sensor_id) return sensor_by_id(d->sensor_id);
    sreg_sensor_t *s = sensor_at(mac, d->port);
    return (s && s->kind == d->kind) ? s : NULL;
}

/* 자리 판정(잠금 잡은 상태) */
static void plan_item(const uint8_t mac[6], const sreg_board_t *b, const esp_now_sensor_desc_t *d, const sreg_decision_t *dec,
                      const sreg_live_t *live, sreg_plan_item_t *it)
{
    memset(it, 0, sizeof(*it));
    it->port = d->port;
    it->kind = d->kind;
    sreg_sensor_t *sen = find_sensor_for(mac, d);
    if (sen && sensor_owns(sen->id)) { it->state = SREG_ITEM_BOUND; return; }

    uint8_t where, precise;
    kind_attrs(d->kind, &where, &precise);
    bool any = false, can_new = true, single = true;
    for (uint8_t c = 0; c < d->chan_n; c++) {
        uint8_t what = b->chan_type[d->chan_first + c];
        int lim = slot_limit(what, where), cnt = 0;
        if (lim > 1) single = false;
        for (int i = 0; i < SREG_MAX_SENSINGS; i++) cnt += in_group(&s_r->sensings[i], what, where, precise);
        if (cnt) any = true;
        if (cnt >= lim) can_new = false;
    }
    it->can_new = can_new;

    /* 대치 후보 = 같은 종류 자리(센서마다 하나, 자리 번호순) */
    for (int i = 0; i < SREG_MAX_SENSINGS; i++) {
        const sreg_sensing_t *g = &s_r->sensings[i];
        bool mine = false;
        for (uint8_t c = 0; c < d->chan_n && !mine; c++) mine = in_group(g, b->chan_type[d->chan_first + c], where, precise);
        if (!mine) continue;
        const sreg_sensor_t *o = sensor_by_id(g->sensor_id);
        uint16_t osid = o ? o->id : 0;
        bool dup = false;
        for (int k = 0; k < it->ncand && osid; k++) dup |= it->cand[k].sensor == osid;
        if (dup || it->ncand >= SREG_MAX_SLOTS) continue;
        sreg_cand_t *cd = &it->cand[it->ncand++];
        cd->sensor = osid;
        cd->sensing = g->id;
        cd->ordinal = g->ordinal;
        if (o) memcpy(cd->board, o->board_mac, 6);
        cd->live = o && o->attached && memcmp(o->board_mac, mac, 6) != 0 && board_live(live, o->board_mac);
    }
    for (int a = 1; a < it->ncand; a++) {   /* 자리 번호순 */
        sreg_cand_t t = it->cand[a];
        int k = a - 1;
        while (k >= 0 && it->cand[k].ordinal > t.ordinal) { it->cand[k + 1] = it->cand[k]; k--; }
        it->cand[k + 1] = t;
    }

    for (int k = 0; dec && k < dec->n; k++) {   /* 사용자가 고른 것 */
        if (dec->c[k].port != d->port) continue;
        if (dec->c[k].target == 0) { it->state = can_new ? SREG_ITEM_NEW : SREG_ITEM_ASK; return; }
        for (int q = 0; q < it->ncand; q++) {
            if (it->cand[q].sensor != dec->c[k].target) continue;
            it->target = it->cand[q].sensor;
            it->state = it->cand[q].live ? SREG_ITEM_BLOCKED : SREG_ITEM_REPLACE;
            return;
        }
    }
    if (!any) { it->state = SREG_ITEM_NEW; return; }
    if (single && it->ncand > 0) {
        it->target = it->cand[0].sensor;
        it->state = it->cand[0].live ? SREG_ITEM_BLOCKED : SREG_ITEM_REPLACE;
        return;
    }
    it->state = SREG_ITEM_ASK;
}

bool sreg_plan(const uint8_t mac[6], const sreg_board_t *b, const sreg_decision_t *dec, const sreg_live_t *live, sreg_plan_t *out)
{
    if (out) out->n = 0;
    if (!s_r) return true;
    bool ok = true;
    lock();
    for (int k = 0; k < board_desc_count(b); k++) {
        esp_now_sensor_desc_t d;
        if (!board_desc(b, k, &d)) continue;
        sreg_plan_item_t it;
        plan_item(mac, b, &d, dec, live, &it);
        if (it.state == SREG_ITEM_ASK || it.state == SREG_ITEM_BLOCKED) ok = false;
        if (out && out->n < ESP_NOW_MAX_SENSORS) out->it[out->n++] = it;
    }
    unlock();
    return ok;
}

/* 그 종류의 빈 자리 번호(1..상한), 없으면 0 */
static uint8_t free_ordinal(uint8_t what, uint8_t where, uint8_t precise)
{
    int lim = slot_limit(what, where);
    for (int o = 1; o <= lim; o++) {
        bool used = false;
        for (int i = 0; i < SREG_MAX_SENSINGS && !used; i++) used = in_group(&s_r->sensings[i], what, where, precise) && s_r->sensings[i].ordinal == o;
        if (!used) return (uint8_t)o;
    }
    return 0;
}

static sreg_sensing_t *new_sensing(const sreg_sensor_t *sen, uint8_t chan, uint8_t what)
{
    uint8_t where, precise;
    kind_attrs(sen->kind, &where, &precise);
    uint8_t ord = free_ordinal(what, where, precise);
    if (!ord) { ESP_LOGE(TAG, "No free slot (what %u) - sensor %u ch %u not recorded", (unsigned)what, (unsigned)sen->id, (unsigned)chan); return NULL; }
    for (int i = 0; i < SREG_MAX_SENSINGS; i++) {
        sreg_sensing_t *g = &s_r->sensings[i];
        if (g->id) continue;
        memset(g, 0, sizeof(*g));
        g->id = s_r->next_sensing_id++;
        g->what = what;
        g->where = where;
        g->precise = precise;
        g->ordinal = ord;
        g->sensor_id = sen->id;
        g->sensor_chan = chan;
        if (what == SREG_WHAT_CAMERA) {
            /* 저장 키 = 이 캠 주소. 그 주소가 이미 다른 캠 자리의 키면(대치로 떠났던 캠이 신규로 돌아옴) 겹치지 않는 키를 만듦 */
            memcpy(g->key_mac, sen->board_mac, 6);
            for (int tries = 0; tries < 256; tries++) {
                bool used = false;
                for (int j = 0; j < SREG_MAX_SENSINGS && !used; j++) used = &s_r->sensings[j] != g && s_r->sensings[j].id && memcmp(s_r->sensings[j].key_mac, g->key_mac, 6) == 0;
                if (!used) break;
                g->key_mac[0] |= 0x02;   /* 로컬 관리 주소 — 실제 보드 주소와 겹치지 않음 */
                g->key_mac[5]++;
            }
        }
        ESP_LOGI(TAG, "New sensing %u: what %u where %u precise %u #%u (sensor %u ch %u)", (unsigned)g->id, (unsigned)what,
                 (unsigned)where, (unsigned)precise, (unsigned)ord, (unsigned)sen->id, (unsigned)chan);
        return g;
    }
    ESP_LOGE(TAG, "Sensing table full (%d) - sensor %u ch %u not recorded", SREG_MAX_SENSINGS, (unsigned)sen->id, (unsigned)chan);
    return NULL;
}

static void set_sensor_fields(sreg_sensor_t *sen, const uint8_t mac[6], const esp_now_sensor_desc_t *d, uint32_t now, bool *changed)
{
    if (sen->kind != d->kind || memcmp(sen->board_mac, mac, 6) != 0 || sen->port != d->port || sen->chan_first != d->chan_first ||
        sen->chan_n != d->chan_n || !sen->attached) {
        sen->kind = d->kind;
        memcpy(sen->board_mac, mac, 6);
        sen->port = d->port;
        sen->chan_first = d->chan_first;
        sen->chan_n = d->chan_n;
        sen->attached = 1;
        *changed = true;
    }
    sen->last_seen = now;
}

bool sreg_on_pair(const uint8_t mac[6], const sreg_board_t *b, const sreg_decision_t *dec, const sreg_live_t *live,
                  esp_now_sensor_id_assign_t *out_assign, uint8_t out_replaced_board[6])
{
    if (out_replaced_board) memset(out_replaced_board, 0, 6);
    if (!s_r) return false;
    bool is_legacy = (b->sensor_count == 0);
    uint32_t now = (uint32_t)time(NULL);
    bool changed = false;
    int nassign = 0;
    lock();
    for (int k = 0; k < board_desc_count(b); k++) {
        esp_now_sensor_desc_t d;
        if (!board_desc(b, k, &d)) continue;
        sreg_plan_item_t it;
        plan_item(mac, b, &d, dec, live, &it);
        uint8_t where, precise;
        kind_attrs(d.kind, &where, &precise);
        sreg_sensor_t *sen = find_sensor_for(mac, &d);

        if (it.state == SREG_ITEM_BOUND) {
            set_sensor_fields(sen, mac, &d, now, &changed);
            for (uint8_t c = 0; c < d.chan_n; c++) {   /* 채널이 늘었으면 새 자리 */
                bool has = false;
                for (int i = 0; i < SREG_MAX_SENSINGS && !has; i++) has = s_r->sensings[i].id && s_r->sensings[i].sensor_id == sen->id && s_r->sensings[i].sensor_chan == c;
                if (!has && new_sensing(sen, c, b->chan_type[d.chan_first + c])) changed = true;
            }
        } else if (it.state == SREG_ITEM_NEW) {
            if (!sen) sen = sensor_new(d.sensor_id);
            if (!sen) continue;
            set_sensor_fields(sen, mac, &d, now, &changed);
            for (uint8_t c = 0; c < d.chan_n; c++) new_sensing(sen, c, b->chan_type[d.chan_first + c]);
            changed = true;
        } else if (it.state == SREG_ITEM_REPLACE) {
            /* 대치 — 옛 센서 칸을 새 센서가 덮어씀(내부 번호만 새로). 이 보드에 이미 있던 자리 없는 칸은 지움 */
            sreg_sensor_t *old = sensor_by_id(it.target);
            if (sen && sen != old) memset(sen, 0, sizeof(*sen));
            uint16_t old_id = old ? old->id : 0;
            if (old) {
                if (out_replaced_board && memcmp(old->board_mac, mac, 6) != 0) memcpy(out_replaced_board, old->board_mac, 6);
                sen = old;
                sen->id = take_sensor_id();
            } else {
                sen = sensor_new(0);
                if (!sen) continue;
            }
            sen->attached = 0;   /* set_sensor_fields가 모든 칸을 다시 씀 */
            set_sensor_fields(sen, mac, &d, now, &changed);
            for (uint8_t c = 0; c < d.chan_n; c++) {
                uint8_t what = b->chan_type[d.chan_first + c];
                sreg_sensing_t *take = NULL;
                for (int i = 0; i < SREG_MAX_SENSINGS && !take; i++) {
                    sreg_sensing_t *g = &s_r->sensings[i];
                    if (in_group(g, what, where, precise) && old_id && g->sensor_id == old_id) take = g;
                }
                for (int i = 0; i < SREG_MAX_SENSINGS && !take; i++) {   /* 자리가 1개인 종류 — 그 자리 */
                    sreg_sensing_t *g = &s_r->sensings[i];
                    if (in_group(g, what, where, precise) && slot_limit(what, where) == 1) take = g;
                }
                if (take) {
                    take->sensor_id = sen->id;
                    take->sensor_chan = c;
                } else {
                    new_sensing(sen, c, what);
                }
            }
            for (int i = 0; i < SREG_MAX_SENSINGS && old_id; i++) {   /* 새 센서에 없는 값의 자리는 센서 없음으로 */
                if (s_r->sensings[i].id && s_r->sensings[i].sensor_id == old_id) s_r->sensings[i].sensor_id = 0;
            }
            ESP_LOGW(TAG, "Sensor %u (kind %u) replaced by sensor %u on " MACSTR " port %u", (unsigned)old_id, (unsigned)d.kind,
                     (unsigned)sen->id, MAC2STR(mac), (unsigned)d.port);
            changed = true;
        } else {
            ESP_LOGW(TAG, "Sensor kind %u on " MACSTR " port %u not placed (%s)", (unsigned)d.kind, MAC2STR(mac), (unsigned)d.port,
                     it.state == SREG_ITEM_ASK ? "replace or new not chosen" : "slot taken by a connected sensor");
            continue;
        }
        /* 같은 보드·포트에 붙어 있던 다른 센서는 떨어진 것으로 */
        for (int i = 0; i < SREG_MAX_SENSORS; i++) {
            sreg_sensor_t *o = &s_r->sensors[i];
            if (o != sen && o->id && o->attached && o->port == d.port && memcmp(o->board_mac, mac, 6) == 0) { o->attached = 0; changed = true; }
        }
        if (!is_legacy && sen->id != d.sensor_id && out_assign && nassign < ESP_NOW_MAX_SENSORS) {
            out_assign->ids[nassign].port = d.port;
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

void sreg_cam_storage_mac(const uint8_t mac[6], uint8_t out[6])
{
    memcpy(out, mac, 6);
    if (!s_r) return;
    lock();
    for (int i = 0; i < SREG_MAX_SENSORS; i++) {
        const sreg_sensor_t *s = &s_r->sensors[i];
        if (!s->id || s->kind != SREG_KIND_CAM || !s->attached || memcmp(s->board_mac, mac, 6) != 0) continue;
        for (int g = 0; g < SREG_MAX_SENSINGS; g++) {
            const sreg_sensing_t *sg = &s_r->sensings[g];
            static const uint8_t zero[6] = { 0 };
            if (sg->id && sg->sensor_id == s->id && memcmp(sg->key_mac, zero, 6) != 0) { memcpy(out, sg->key_mac, 6); break; }
        }
        break;
    }
    unlock();
}

int sreg_list_slots(sreg_slot_row_t *out, int max)
{
    if (!s_r) return 0;
    int n = 0;
    lock();
    for (int i = 0; i < SREG_MAX_SENSORS && n < max; i++) {
        const sreg_sensor_t *s = &s_r->sensors[i];
        if (!s->id || !sensor_owns(s->id)) continue;
        sreg_slot_row_t *r = &out[n++];
        memset(r, 0, sizeof(*r));
        r->sensor = s->id;
        r->kind = s->kind;
        memcpy(r->board, s->board_mac, 6);
        r->attached = s->attached;
        for (int c = 0; c < ESP_NOW_MAX_CHANNELS; c++) {   /* 센서 채널 순서 */
            for (int g = 0; g < SREG_MAX_SENSINGS; g++) {
                const sreg_sensing_t *sg = &s_r->sensings[g];
                if (sg->id && sg->sensor_id == s->id && sg->sensor_chan == c && r->nsensing < ESP_NOW_MAX_CHANNELS) r->sensing[r->nsensing++] = sg->id;
            }
        }
    }
    for (int g = 0; g < SREG_MAX_SENSINGS && n < max; g++) {   /* 센서 없는 자리 */
        const sreg_sensing_t *sg = &s_r->sensings[g];
        if (!sg->id || sensor_by_id(sg->sensor_id)) continue;
        sreg_slot_row_t *r = &out[n++];
        memset(r, 0, sizeof(*r));
        r->nsensing = 1;
        r->sensing[0] = sg->id;
    }
    unlock();
    return n;
}

int sreg_forget_row(const sreg_slot_row_t *row, sreg_sensing_t *out, int max)
{
    if (!s_r) return 0;
    int n = 0;
    lock();
    for (int g = 0; g < SREG_MAX_SENSINGS; g++) {
        sreg_sensing_t *sg = &s_r->sensings[g];
        if (!sg->id) continue;
        bool hit = row->sensor && sg->sensor_id == row->sensor;
        for (int k = 0; k < row->nsensing && !hit; k++) hit = sg->id == row->sensing[k];
        if (!hit) continue;
        if (n < max) out[n] = *sg;
        n++;
        ESP_LOGW(TAG, "Sensing %u (what %u #%u) removed", (unsigned)sg->id, (unsigned)sg->what, (unsigned)sg->ordinal);
        memset(sg, 0, sizeof(*sg));
    }
    sreg_sensor_t *s = sensor_by_id(row->sensor);
    if (s) {
        ESP_LOGW(TAG, "Sensor %u removed", (unsigned)s->id);
        memset(s, 0, sizeof(*s));
    }
    unlock();
    save_later();
    return n < max ? n : max;
}

int sreg_forget_board(const uint8_t mac[6], sreg_sensing_t *out, int max)
{
    if (!s_r) return 0;
    int n = 0;
    lock();
    for (int i = 0; i < SREG_MAX_SENSORS; i++) {
        const sreg_sensor_t *s = &s_r->sensors[i];
        if (!s->id || memcmp(s->board_mac, mac, 6) != 0) continue;
        sreg_slot_row_t row = { .sensor = s->id };
        n += sreg_forget_row(&row, out + n, max - n);
        if (n >= max) break;
    }
    unlock();
    return n;
}

bool sreg_board_has_sensors(const uint8_t mac[6])
{
    if (!s_r) return false;
    bool any = false;
    lock();
    for (int i = 0; i < SREG_MAX_SENSORS && !any; i++) any = s_r->sensors[i].id && memcmp(s_r->sensors[i].board_mac, mac, 6) == 0;
    unlock();
    return any;
}

int sreg_list_owner_boards(uint8_t (*out)[6], bool *is_cam, int max)
{
    if (!s_r || !out || max <= 0) return 0;
    int n = 0;
    lock();
    for (int g = 0; g < SREG_MAX_SENSINGS && n < max; g++) {
        const sreg_sensing_t *sg = &s_r->sensings[g];
        if (!sg->id || !sg->sensor_id) continue;
        const uint8_t *mac = NULL;
        for (int i = 0; i < SREG_MAX_SENSORS; i++) if (s_r->sensors[i].id == sg->sensor_id) { mac = s_r->sensors[i].board_mac; break; }
        if (!mac) continue;
        bool dup = false;
        for (int k = 0; k < n && !dup; k++) dup = memcmp(out[k], mac, 6) == 0;
        if (dup) continue;
        memcpy(out[n], mac, 6);
        if (is_cam) is_cam[n] = sg->what == SREG_WHAT_CAMERA;
        n++;
    }
    unlock();
    return n;
}

bool sreg_merge_sensing(uint16_t from, uint16_t to)
{
    if (!s_r || !from || !to || from == to) return false;
    sreg_sensing_t *f = NULL, *t = NULL;
    lock();
    for (int g = 0; g < SREG_MAX_SENSINGS; g++) {
        if (s_r->sensings[g].id == from) f = &s_r->sensings[g];
        if (s_r->sensings[g].id == to) t = &s_r->sensings[g];
    }
    bool ok = f && t && f->what == t->what && f->where == t->where && f->precise == t->precise;
    if (ok) {
        t->sensor_id = f->sensor_id;
        t->sensor_chan = f->sensor_chan;
        memset(f, 0, sizeof(*f));
        ESP_LOGW(TAG, "Sensing %u merged into %u (sensor %u)", (unsigned)from, (unsigned)to, (unsigned)t->sensor_id);
    }
    unlock();
    if (ok) save_later();
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
    for (int i = 0; i < SREG_MAX_SENSINGS && n < (int)cap - 160; i++) {
        const sreg_sensing_t *g = &s_r->sensings[i];
        if (!g->id) continue;
        n += snprintf(buf + n, cap - n, "%s{\"id\":%u,\"what\":%u,\"where\":%u,\"precise\":%u,\"ord\":%u,\"sensor\":%u,\"ch\":%u,\"key\":\"%02x%02x%02x%02x%02x%02x\"}",
                      first ? "" : ",", (unsigned)g->id, (unsigned)g->what, (unsigned)g->where, (unsigned)g->precise,
                      (unsigned)g->ordinal, (unsigned)g->sensor_id, (unsigned)g->sensor_chan,
                      g->key_mac[0], g->key_mac[1], g->key_mac[2], g->key_mac[3], g->key_mac[4], g->key_mac[5]);
        first = false;
    }
    n += snprintf(buf + n, cap - n, "]}");
    unlock();
    return n < (int)cap ? n : (int)cap - 1;
}
