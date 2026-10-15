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
#include <dirent.h>
#include "esp_rom_crc.h"
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
#define SDH_GUARD_SEC   600    /* 2026-10-15 — 부트 영역 감시 주기(10분, 읽기만이라 카드 수명과 무관) */
#define SDH_GUARD_MAX   16384  /* 감시할 부트 영역 섹터 상한(8 MB) */
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
    "boot sector unreadable", "boot area changed", "bad directory entry", "bad stats record",
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

/* 2026-10-15 — 바이트 n개를 16진 글자로(사건 기록 path 칸에 넣음) */
static void hexstr(char *out, size_t cap, const char *prefix, uint32_t num, const uint8_t *b, size_t n)
{
    int k = snprintf(out, cap, "%s%u:", prefix, (unsigned)num);
    for (size_t i = 0; i < n && k + 2 < (int)cap; i++) k += snprintf(out + k, cap - k, "%02X", b[i]);
}

/* 0번 섹터(MBR 또는 바로 부트 섹터)와 부트 섹터를 읽어 파티션 시작·예약 섹터·FAT 크기·FSInfo 위치를 얻음(buf = 512바이트).
 * 실패하면 false와 이유(사건 기록용 글) — 2026-10-15 예전엔 FAT 검사가 이 실패를 말없이 건너뛰었음(MPIO 0번 섹터 손상을
 * 10:04부터 못 잡음) */
static bool read_layout(uint8_t *buf, uint32_t *part, uint32_t *rsv, uint32_t *fatsz, uint32_t *fsinfo, char *why, size_t why_cap)
{
    *part = 0;
    if (sd_storage_read_sectors(buf, 0, 1) != ESP_OK) { snprintf(why, why_cap, "S0 read failed"); return false; }
    if (buf[510] != 0x55 || buf[511] != 0xAA) { hexstr(why, why_cap, "S", 0, buf, 16); return false; }
    if (!(buf[0] == 0xEB || buf[0] == 0xE9)) {   /* 0번 섹터가 MBR이면 첫 파티션의 부트 섹터로 */
        *part = le32(buf + 454);
        if (sd_storage_read_sectors(buf, *part, 1) != ESP_OK) { snprintf(why, why_cap, "VBR %u read failed", (unsigned)*part); return false; }
        if (buf[510] != 0x55 || buf[511] != 0xAA) { hexstr(why, why_cap, "S", *part, buf, 16); return false; }
    }
    uint16_t bps = le16(buf + 11);
    uint8_t nfat = buf[16];
    *rsv = le16(buf + 14);
    *fatsz = le16(buf + 22) ? le16(buf + 22) : le32(buf + 36);
    *fsinfo = le16(buf + 22) ? UINT32_MAX : *part + le16(buf + 48);   /* FAT32만 FSInfo가 있음 */
    if (bps != 512 || nfat < 2 || *fatsz == 0 || *rsv == 0) {
        snprintf(why, why_cap, "VBR %u bps%u nfat%u", (unsigned)*part, (unsigned)bps, (unsigned)nfat);
        return false;
    }
    return true;
}

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
    uint32_t nbad = 0, fat1 = 0, fatsz = 0, part = 0, rsv = 0, fsinfo = 0;
    char why[40] = "";
    bool ok = sd_storage_is_mounted();
    bool layout_ok = ok && read_layout(a, &part, &rsv, &fatsz, &fsinfo, why, sizeof(why));
    if (ok && !layout_ok) {
        ok = false;
        ESP_LOGE(TAG, "FAT check: boot sector problem (%s)", why);   /* 2026-10-15 — 예전엔 말없이 건너뜀 */
    }
    fat1 = part + rsv;
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
    if (why[0] && !layout_ok && sd_storage_is_mounted()) sd_health_report(SDH_EV_LAYOUT, why, 0);
    if (!ok) return;
    sd_health_count(SDH_C_FAT_CHECK, 1);
    ESP_LOGI(TAG, "FAT check: %u sectors x2, first pass %u differ, confirmed %u, %ums", (unsigned)fatsz, (unsigned)nbad,
             (unsigned)still, (unsigned)((esp_timer_get_time() - t0) / 1000));
    if (still) sd_health_report(SDH_EV_FAT_MISMATCH, "FAT sector", first_bad);
}

/* ════════════════════════════════════════════════════════════
 * 2026-10-15(MPIO 카드 손상 대응, 사용자 지시 — "유사한 에러가 발생할 때 로그로 잡을 수 있도록") — 부트 영역 감시.
 * 0번 섹터 ~ FAT 앞(예약 영역)은 FatFs가 FSInfo 한 섹터 말고는 쓰지 않음. 마운트·포맷 직후 섹터마다 CRC를 잡아 두고
 * 10분마다 다시 읽어 비교 — 다르면 바뀐 섹터 번호와 앞 16바이트를 기록하고 SD 이상(쓰기 중지·5009·푸시).
 * MPIO에선 사진 폴더가 새 클러스터를 받은 09:16 뒤 한 시간 안에 0번 섹터가 무작위로 바뀌었음(Docs/한일 153) */
static uint32_t *s_guard_crc = NULL;       /* PSRAM, SDH_GUARD_MAX */
static uint32_t  s_guard_n = 0;            /* 기준값을 잡은 섹터 수(0 = 아직) */
static volatile bool s_guard_rebase = true;

static void guard_check(void)
{
    if (!sd_storage_is_mounted()) return;   /* 읽기만 하므로 고장 상태에서도 돎(재연결 직후 기준값도 이때 잡힘) */
    uint8_t *buf = heap_caps_malloc(SDH_FAT_CHUNK * 512, MALLOC_CAP_SPIRAM);
    if (!s_guard_crc) s_guard_crc = heap_caps_malloc(SDH_GUARD_MAX * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!buf || !s_guard_crc) { heap_caps_free(buf); return; }
    int64_t t0 = esp_timer_get_time();
    uint32_t part, rsv, fatsz, fsinfo;
    char why[40] = "";
    if (!read_layout(buf, &part, &rsv, &fatsz, &fsinfo, why, sizeof(why))) {
        heap_caps_free(buf);
        if (sd_storage_is_mounted()) {
            ESP_LOGE(TAG, "Boot-area guard: boot sector problem (%s)", why);
            sd_health_report(SDH_EV_LAYOUT, why, 0);
        }
        return;
    }
    uint32_t n = part + rsv;
    if (n > SDH_GUARD_MAX) n = SDH_GUARD_MAX;
    bool rebase = s_guard_rebase || s_guard_n != n;
    uint32_t changed = 0, first = 0;
    char detail[40] = "";
    for (uint32_t s = 0; s < n; s += SDH_FAT_CHUNK) {
        uint32_t k = (n - s < SDH_FAT_CHUNK) ? n - s : SDH_FAT_CHUNK;
        if (sd_storage_read_sectors(buf, s, k) != ESP_OK) {
            ESP_LOGW(TAG, "Boot-area guard: read failed at sector %u", (unsigned)s);
            heap_caps_free(buf);
            return;   /* 읽기 실패는 다른 경로(재연결·읽기 회로차단기)가 다룸 — 기준값은 그대로 */
        }
        for (uint32_t i = 0; i < k; i++) {
            uint32_t sec = s + i;
            uint32_t c = (sec == fsinfo) ? 0 : esp_rom_crc32_le(0, buf + i * 512, 512);
            if (rebase) { s_guard_crc[sec] = c; continue; }
            if (c != s_guard_crc[sec]) {
                if (changed++ == 0) { first = sec; hexstr(detail, sizeof(detail), "S", sec, buf + i * 512, 16); }
                ESP_LOGE(TAG, "Boot-area guard: sector %u changed (crc %08X -> %08X), first bytes %02X%02X%02X%02X%02X%02X%02X%02X",
                         (unsigned)sec, (unsigned)s_guard_crc[sec], (unsigned)c, buf[i * 512], buf[i * 512 + 1], buf[i * 512 + 2],
                         buf[i * 512 + 3], buf[i * 512 + 4], buf[i * 512 + 5], buf[i * 512 + 6], buf[i * 512 + 7]);
                s_guard_crc[sec] = c;   /* 같은 변화를 매번 다시 보고하지 않게 */
            }
        }
    }
    heap_caps_free(buf);
    if (rebase) {
        s_guard_n = n;
        s_guard_rebase = false;
        ESP_LOGI(TAG, "Boot-area guard: baseline %u sectors (partition at %u, FSInfo %d), %ums", (unsigned)n, (unsigned)part,
                 (fsinfo == UINT32_MAX) ? -1 : (int)fsinfo, (unsigned)((esp_timer_get_time() - t0) / 1000));
        return;
    }
    ESP_LOGI(TAG, "Boot-area guard: %u sectors, %u changed, %ums", (unsigned)n, (unsigned)changed,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    if (changed) sd_health_report(SDH_EV_BOOT_AREA_CHANGED, detail, changed);
    (void)first;
}

/* 2026-10-15 — 폴더 항목 이름 검사(루트, 통계 폴더, 사진 폴더와 그 아래 기기 폴더). MPIO에선 사진 폴더 디렉터리 클러스터
 * 앞 8KB가 무작위로 바뀌어 항목 255개가 깨졌는데 사진 목록이 짧아졌을 뿐 아무 보고가 없었음. 글자가 0x20~0x7E 밖이면 깨진 것 */
static bool name_ok(const char *nm)
{
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)nm; *p; p++, len++) {
        if (*p < 0x20 || *p > 0x7E || len > 64) return false;
    }
    return len > 0;
}

static uint32_t dir_check_one(const char *dir, char *first, size_t cap, uint32_t *entries)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    uint32_t bad = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        (*entries)++;
        if (name_ok(e->d_name)) continue;
        if (bad++ == 0) {
            if (!first[0]) hexstr(first, cap, "D", (uint32_t)*entries, (const uint8_t *)e->d_name, 12);
            ESP_LOGE(TAG, "Directory check: bad entry in %s, name bytes %02X%02X%02X%02X%02X%02X%02X%02X", dir,
                     (unsigned char)e->d_name[0], (unsigned char)e->d_name[1], (unsigned char)e->d_name[2], (unsigned char)e->d_name[3],
                     (unsigned char)e->d_name[4], (unsigned char)e->d_name[5], (unsigned char)e->d_name[6], (unsigned char)e->d_name[7]);
        }
    }
    closedir(d);
    if (bad) ESP_LOGE(TAG, "Directory check: %s has %u bad entries", dir, (unsigned)bad);
    return bad;
}

static void dir_check(void)
{
    if (!sd_storage_is_mounted() || !sd_health_writes_allowed()) return;
    const size_t cap = 300;   /* 폴더 + 긴 이름 — 스택에 두지 않음 */
    char *sub = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    char *first = heap_caps_calloc(1, 40, MALLOC_CAP_SPIRAM);
    if (!sub || !first) { heap_caps_free(sub); heap_caps_free(first); return; }
    int64_t t0 = esp_timer_get_time();
    uint32_t bad = 0, entries = 0;
    bad += dir_check_one(SD_STORAGE_MOUNT_POINT, first, 40, &entries);
    bad += dir_check_one(SD_STORAGE_MOUNT_POINT "/stats", first, 40, &entries);
    bad += dir_check_one(SD_STORAGE_MOUNT_POINT "/photos", first, 40, &entries);
    DIR *d = opendir(SD_STORAGE_MOUNT_POINT "/photos");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!name_ok(e->d_name) || e->d_type != DT_DIR) continue;
            snprintf(sub, cap, SD_STORAGE_MOUNT_POINT "/photos/%s", e->d_name);
            bad += dir_check_one(sub, first, 40, &entries);
        }
        closedir(d);
    }
    ESP_LOGI(TAG, "Directory check: %u entries, %u bad, %ums", (unsigned)entries, (unsigned)bad,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    if (bad) sd_health_report(SDH_EV_BAD_DIR_ENTRY, first, bad);
    heap_caps_free(sub);
    heap_caps_free(first);
}

static void hourly_job(const uint8_t *arg);   /* 아래 */

static void guard_job(const uint8_t *arg)
{
    (void)arg;
    guard_check();
}

/* 2026-10-15(시험, 임시) — sd_health_check_now 참고 */
static void flip_job(const uint8_t *arg)
{
    (void)arg;
    uint8_t *buf = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    uint32_t part, rsv, fatsz, fsinfo;
    char why[40] = "";
    if (buf && sd_storage_is_mounted() && read_layout(buf, &part, &rsv, &fatsz, &fsinfo, why, sizeof(why)) && rsv > 8 &&
        part + 2 != fsinfo && sd_storage_read_sectors(buf, part + 2, 1) == ESP_OK) {
        uint8_t before = buf[0];
        buf[0] ^= 0xA5;
        esp_err_t e = sd_storage_write_sectors(buf, part + 2, 1);
        ESP_LOGW(TAG, "SELFTEST: sector %u byte0 %02X -> %02X (%s)", (unsigned)(part + 2), before, buf[0], esp_err_to_name(e));
    } else {
        ESP_LOGW(TAG, "SELFTEST: not done (%s)", why);
    }
    heap_caps_free(buf);
}

void sd_health_check_now(bool flip)
{
    if (flip) stats_store_post_job(flip_job, NULL, 0);
    else stats_store_post_job(hourly_job, NULL, 0);
}

/* 마운트·포맷 직후(그 태스크에서) — 기준값은 기록 태스크에서 새로 잡음 */
void sd_health_on_mount(void)
{
    s_guard_rebase = true;
    stats_store_post_job(guard_job, NULL, 0);
}

static void hourly_job(const uint8_t *arg)
{
    (void)arg;
    fat_check();
    guard_check();
    dir_check();
    stats_store_sd_check();   /* 2026-10-15 — 최근 두 주 통계 원시·집계 파일의 레코드가 그 주에 속하는지 */
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

static esp_timer_handle_t s_guard_timer = NULL;
static void guard_timer_cb(void *arg)
{
    (void)arg;
    stats_store_post_job(guard_job, NULL, 0);
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
    const esp_timer_create_args_t tg = { .callback = guard_timer_cb, .name = "sd_guard" };
    if (esp_timer_create(&tg, &s_guard_timer) == ESP_OK) esp_timer_start_periodic(s_guard_timer, (uint64_t)SDH_GUARD_SEC * 1000000ULL);
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
