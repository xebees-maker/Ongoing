/**
 * @file    sd_health.c
 * @brief   sd_health.h 구현 — SD 개선 1단계(2026-10-14, 사용자 설계)
 */
#include "sd_health.h"
#include "sd_storage.h"
#include "stats_store.h"
#include "photo_storage.h"
#include "fs.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SD";

#define SDH_FILE        FS_MOUNT_POINT "/sd_health.bin"
#define SDH_VERSION     1
#define SDH_EVENTS      20
#define SDH_CHECK_SEC   3600   /* FAT 정기 비교·카운터 저장 주기 */
#define SDH_FAT_CHUNK   16     /* 한 번에 읽는 섹터 수(8 KB) */
#define SDH_FAT_MAXBAD  64     /* 다시 확인할 다른 섹터 수 상한 */

typedef struct {
    uint32_t t;        /* 콘 시각(유닉스) */
    uint8_t  ev;       /* sd_health_event_t */
    uint32_t offset;
    char     path[40];
} sdh_event_t;

/* LittleFS에 그대로 저장하는 상태(PSRAM). 버전이 다르면 버림 */
typedef struct {
    uint32_t    version;
    uint8_t     fault;                 /* 1 = 고장(쓰기 중지) */
    char        card[112];             /* 마운트 직전에 읽은 CID 한 줄 */
    uint32_t    day;                   /* today[]의 날짜(유닉스/86400) */
    uint32_t    today[SDH_C_COUNT];
    uint32_t    yesterday[SDH_C_COUNT];
    uint32_t    total[SDH_C_COUNT];    /* 이 파일이 생긴 뒤 합계 */
    uint32_t    ev_count;              /* 지금까지 이상 보고 수 */
    sdh_event_t ev[SDH_EVENTS];        /* 최근 이상(링) */
} sdh_state_t;

static sdh_state_t *s_st = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_pending = false;      /* 화면·푸시로 아직 안 올린 이상 */
static char s_pending_reason[64];
static volatile bool s_dirty = false;         /* 저장할 변경 */
static esp_timer_handle_t s_timer = NULL;

static const char *const s_ev_name[SDH_EV_COUNT] = {
    "?", "stats verify", "aggregate verify", "photo verify", "photo missing", "bad aggregate record", "FAT1/FAT2 mismatch", "test",
};

static void save_now(void)
{
    if (!s_st) return;
    FILE *f = fopen(SDH_FILE, "wb");
    if (!f) { ESP_LOGW(TAG, "SD health save failed"); return; }
    fwrite(s_st, sizeof(*s_st), 1, f);
    fclose(f);
    s_dirty = false;
}

static void roll_day_locked(uint32_t now)
{
    uint32_t d = now / 86400;
    if (now < 1704067200u || d == s_st->day) return;   /* 시계가 안 맞았으면 날짜를 넘기지 않음 */
    if (s_st->day != 0) memcpy(s_st->yesterday, s_st->today, sizeof(s_st->today));
    memset(s_st->today, 0, sizeof(s_st->today));
    s_st->day = d;
}

void sd_health_count(sd_health_counter_t c, uint32_t n)
{
    if (!s_st || c >= SDH_C_COUNT) return;
    taskENTER_CRITICAL(&s_mux);
    s_st->today[c] += n;
    s_st->total[c] += n;
    taskEXIT_CRITICAL(&s_mux);
    s_dirty = true;
}

bool sd_health_writes_allowed(void)
{
    return !(s_st && s_st->fault);
}

void sd_health_report(sd_health_event_t ev, const char *path, uint32_t offset)
{
    if (!s_st || ev <= 0 || ev >= SDH_EV_COUNT) return;
    uint32_t now = (uint32_t)time(NULL);
    bool first;
    taskENTER_CRITICAL(&s_mux);
    sdh_event_t *e = &s_st->ev[s_st->ev_count % SDH_EVENTS];
    e->t = now;
    e->ev = (uint8_t)ev;
    e->offset = offset;
    e->path[0] = '\0';
    if (path) {
        size_t n = strlen(path);
        const char *p = (n >= sizeof(e->path)) ? path + n - (sizeof(e->path) - 1) : path;   /* 뒤쪽(파일 이름)이 남게 */
        strncpy(e->path, p, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }
    s_st->ev_count++;
    first = !s_st->fault;
    s_st->fault = 1;
    if (first) {
        snprintf(s_pending_reason, sizeof(s_pending_reason), "%s", s_ev_name[ev]);
        s_pending = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGE(TAG, "SD HEALTH: %s at %s off %u%s", s_ev_name[ev], path ? path : "-", (unsigned)offset,
             first ? " - SD writes stopped until reconnect/format passes" : "");
    s_dirty = true;
    if (first) save_now();   /* 재부팅해도 고장 상태가 남게 바로 저장(LittleFS — SD 아님) */
}

void sd_health_clear(void)
{
    if (!s_st) return;
    bool was;
    taskENTER_CRITICAL(&s_mux);
    was = s_st->fault;
    s_st->fault = 0;
    s_pending = false;
    taskEXIT_CRITICAL(&s_mux);
    if (was) {
        ESP_LOGW(TAG, "SD HEALTH: cleared after verified reconnect/format - SD writes resumed");
        save_now();
    }
}

bool sd_health_take_pending(char *reason, size_t reason_len)
{
    if (!s_pending) return false;
    taskENTER_CRITICAL(&s_mux);
    bool p = s_pending;
    s_pending = false;
    if (p && reason && reason_len) {
        strncpy(reason, s_pending_reason, reason_len - 1);
        reason[reason_len - 1] = '\0';
    }
    taskEXIT_CRITICAL(&s_mux);
    return p;
}

void sd_health_set_card(const char *cid_line)
{
    if (!s_st || !cid_line) return;
    if (strncmp(s_st->card, cid_line, sizeof(s_st->card) - 1) == 0) return;
    strncpy(s_st->card, cid_line, sizeof(s_st->card) - 1);
    s_st->card[sizeof(s_st->card) - 1] = '\0';
    s_dirty = true;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* FAT1/FAT2를 원본 섹터로 비교. 사진 저장·통계 기록을 잠깐 멈추고(재연결과 같은 잠금 순서: 사진 → 통계) 읽음.
 * 다른 섹터가 있으면 3초 뒤 그 섹터만 다시 읽어 여전히 다를 때만 보고 */
static void fat_check(void)
{
    if (!sd_storage_is_mounted() || !sd_health_writes_allowed()) return;
    uint8_t *a = heap_caps_malloc(SDH_FAT_CHUNK * 512, MALLOC_CAP_SPIRAM);
    uint8_t *b = heap_caps_malloc(SDH_FAT_CHUNK * 512, MALLOC_CAP_SPIRAM);
    uint32_t *bad = heap_caps_malloc(SDH_FAT_MAXBAD * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!a || !b || !bad) { heap_caps_free(a); heap_caps_free(b); heap_caps_free(bad); return; }

    int64_t t0 = esp_timer_get_time();
    photo_storage_pause();
    stats_store_io_suspend();
    uint32_t nbad = 0, fat1 = 0, fatsz = 0;
    bool ok = sd_storage_is_mounted() && sd_storage_read_sectors(a, 0, 1) == ESP_OK && a[510] == 0x55 && a[511] == 0xAA;
    if (ok) {
        uint32_t part = 0;
        if (!(a[0] == 0xEB || a[0] == 0xE9)) {   /* 0번 섹터가 MBR이면 첫 파티션의 부트 섹터로 */
            part = le32(a + 454);
            ok = sd_storage_read_sectors(a, part, 1) == ESP_OK && a[510] == 0x55 && a[511] == 0xAA;
        }
        if (ok) {
            uint16_t bps = le16(a + 11), rsv = le16(a + 14);
            uint8_t nfat = a[16];
            fatsz = le16(a + 22) ? le16(a + 22) : le32(a + 36);
            fat1 = part + rsv;
            if (bps != 512 || nfat < 2 || fatsz == 0) {
                ESP_LOGW(TAG, "FAT check skipped (bps=%u nfat=%u fatsz=%u)", (unsigned)bps, (unsigned)nfat, (unsigned)fatsz);
                ok = false;
            }
        }
    }
    for (uint32_t s = 0; ok && s < fatsz; s += SDH_FAT_CHUNK) {
        uint32_t n = (fatsz - s < SDH_FAT_CHUNK) ? fatsz - s : SDH_FAT_CHUNK;
        if (sd_storage_read_sectors(a, fat1 + s, n) != ESP_OK || sd_storage_read_sectors(b, fat1 + fatsz + s, n) != ESP_OK) {
            ESP_LOGW(TAG, "FAT check read failed at sector %u", (unsigned)(fat1 + s));
            ok = false;
            break;
        }
        for (uint32_t k = 0; k < n; k++) {
            if (memcmp(a + k * 512, b + k * 512, 512) != 0 && nbad < SDH_FAT_MAXBAD) bad[nbad++] = s + k;
        }
    }
    stats_store_io_resume();
    photo_storage_unpause();

    uint32_t still = 0, first_bad = 0;
    if (ok && nbad > 0) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        photo_storage_pause();
        stats_store_io_suspend();
        for (uint32_t i = 0; i < nbad && sd_storage_is_mounted(); i++) {
            if (sd_storage_read_sectors(a, fat1 + bad[i], 1) == ESP_OK &&
                sd_storage_read_sectors(b, fat1 + fatsz + bad[i], 1) == ESP_OK && memcmp(a, b, 512) != 0) {
                if (still++ == 0) first_bad = bad[i];
            }
        }
        stats_store_io_resume();
        photo_storage_unpause();
    }
    heap_caps_free(a);
    heap_caps_free(b);
    heap_caps_free(bad);
    if (!ok) return;
    sd_health_count(SDH_C_FAT_CHECK, 1);
    ESP_LOGI(TAG, "FAT check: %u sectors x2, first pass %u differ, confirmed %u, %ums", (unsigned)fatsz, (unsigned)nbad,
             (unsigned)still, (unsigned)((esp_timer_get_time() - t0) / 1000));
    if (still) sd_health_report(SDH_EV_FAT_MISMATCH, "FAT sector", first_bad);
}

static void hourly_job(const uint8_t *arg)
{
    (void)arg;
    fat_check();
    if (!s_st) return;
    uint32_t now = (uint32_t)time(NULL);
    taskENTER_CRITICAL(&s_mux);
    roll_day_locked(now);
    taskEXIT_CRITICAL(&s_mux);
    const uint32_t *c = s_st->today;
    ESP_LOGI(TAG, "SDHEALTH today: fsync %u create %u rename %u unlink %u bytes %u verifyOK %u fatcheck %u | fault %u events %u",
             (unsigned)c[SDH_C_FSYNC], (unsigned)c[SDH_C_CREATE], (unsigned)c[SDH_C_RENAME], (unsigned)c[SDH_C_UNLINK],
             (unsigned)c[SDH_C_BYTES], (unsigned)c[SDH_C_VERIFY_OK], (unsigned)c[SDH_C_FAT_CHECK], (unsigned)s_st->fault,
             (unsigned)s_st->ev_count);
    if (s_dirty) save_now();
}

/* 타이머 문맥에서는 SD·LittleFS를 건드리지 않고 기록 태스크로 넘김 */
static void timer_cb(void *arg)
{
    (void)arg;
    stats_store_post_job(hourly_job, NULL, 0);
}

void sd_health_init(void)
{
    if (s_st) return;
    s_st = heap_caps_calloc(1, sizeof(*s_st), MALLOC_CAP_SPIRAM);
    if (!s_st) { ESP_LOGE(TAG, "SD health alloc failed"); return; }
    FILE *f = fopen(SDH_FILE, "rb");
    if (f) {
        if (fread(s_st, sizeof(*s_st), 1, f) != 1 || s_st->version != SDH_VERSION) memset(s_st, 0, sizeof(*s_st));
        fclose(f);
    }
    s_st->version = SDH_VERSION;
    if (s_st->fault) {   /* 재부팅 전에 고장이었으면 다시 알림(쓰기는 계속 중지) */
        snprintf(s_pending_reason, sizeof(s_pending_reason), "still faulted since before restart");
        s_pending = true;
        ESP_LOGE(TAG, "SD HEALTH: fault state kept from before restart - SD writes stopped until reconnect/format passes");
    }
    const esp_timer_create_args_t ta = { .callback = timer_cb, .name = "sd_health" };
    if (esp_timer_create(&ta, &s_timer) == ESP_OK) esp_timer_start_periodic(s_timer, (uint64_t)SDH_CHECK_SEC * 1000000ULL);
}

int sd_health_json(char *buf, size_t cap)
{
    if (!s_st || cap == 0) return 0;
    int n = snprintf(buf, cap, "{\"fault\":%u,\"card\":\"", (unsigned)s_st->fault);
    for (const char *p = s_st->card; *p && n < (int)cap - 8; p++) {
        if (*p == '"' || *p == '\\') buf[n++] = '\\';
        buf[n++] = (*p >= 32 && *p < 127) ? *p : '?';
    }
    static const char *const names[SDH_C_COUNT] = { "fsync", "create", "rename", "unlink", "bytes", "verify_ok", "fat_check" };
    const uint32_t *sets[3] = { s_st->today, s_st->yesterday, s_st->total };
    static const char *const set_names[3] = { "today", "yesterday", "total" };
    n += snprintf(buf + n, cap - n, "\",\"day\":%u", (unsigned)s_st->day);
    for (int s = 0; s < 3 && n < (int)cap; s++) {
        n += snprintf(buf + n, cap - n, ",\"%s\":{", set_names[s]);
        for (int i = 0; i < SDH_C_COUNT && n < (int)cap; i++) {
            n += snprintf(buf + n, cap - n, "%s\"%s\":%u", i ? "," : "", names[i], (unsigned)sets[s][i]);
        }
        if (n < (int)cap) n += snprintf(buf + n, cap - n, "}");
    }
    if (n < (int)cap) n += snprintf(buf + n, cap - n, ",\"events_total\":%u,\"events\":[", (unsigned)s_st->ev_count);
    uint32_t cnt = s_st->ev_count < SDH_EVENTS ? s_st->ev_count : SDH_EVENTS;
    for (uint32_t i = 0; i < cnt && n < (int)cap; i++) {
        const sdh_event_t *e = &s_st->ev[(s_st->ev_count - 1 - i) % SDH_EVENTS];   /* 최신이 앞 */
        n += snprintf(buf + n, cap - n, "%s{\"t\":%u,\"ev\":\"%s\",\"off\":%u,\"path\":\"%s\"}", i ? "," : "", (unsigned)e->t,
                      (e->ev < SDH_EV_COUNT) ? s_ev_name[e->ev] : "?", (unsigned)e->offset, e->path);
    }
    if (n < (int)cap) n += snprintf(buf + n, cap - n, "]}");
    return (n < (int)cap) ? n : (int)cap - 1;
}
