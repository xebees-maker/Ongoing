/**
 * @file    sd_health.c
 * @brief   sd_health.h 구현 — SD 개선 1단계(2026-10-14, 사용자 설계)
 */
#include "sd_health.h"
#include "sd_storage.h"
#include "stats_store.h"
#include "photo_storage.h"
#include "fs.h"
#include "sd_guard.h"

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
    "boot sector unreadable", "boot area changed", "bad directory entry", "bad stats record", "cluster chain broken",
    "FAT changed without write", "guard repair failed",
};

static void journal_save(const char *why);   /* 아래(2026-10-21 명령 기록) */

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
    if (first) journal_save(s_ev_name[ev]);   /* 2026-10-21 — 사건 직전 명령 기록 */
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

/* ════════════════════════════════════════════════════════════
 * 2026-10-21(사용자 지시 — "처음 손상 원인을 밝히는 로그") — 10-21 손상은 10-18에 쓰고 확인까지 끝난 FAT·데이터가 02:40~03:02 사이
 * 동작 중에 카드 절대 섹터 16개(8KB) 단위로 바뀐 것(비트 약 27% 뒤집힘 또는 FF), 카드는 에러 없음. 매시 비교로는 "그 한 시간 안"까지만
 * 알 수 있어서, 아래 두 가지를 남김:
 *  1) 명령 기록(RAM 링) — 모든 쓰기(주소·블록 수·가장 긴 명령 시간, 이어지는 쓰기는 한 줄로), 50ms 넘은 명령, 실패한 명령, 지우기,
 *     리셋(CMD0), 다른 장치 표시(릴레이·화면). sd_storage의 명령 감싸기(sd_do_transaction_cs)가 명령마다 부름.
 *  2) FAT 섹터 기대값 — 콘이 FAT 섹터에 쓸 때마다 쓴 내용의 CRC를 기억. 10분마다 FAT 두 벌을 읽어 비교해서 "콘이 쓰지 않았는데 바뀜"
 *     (카드 안에서 바뀜)과 "콘이 그렇게 씀"(매시 FAT1/FAT2 비교에서 갈라짐)을 가름. 바뀐 섹터·마지막으로 쓴 시각·앞 16바이트를 남기고
 *     명령 기록을 LittleFS(sd_journal.bin)에 저장 + 로그에 최근 줄을 찍음 */
#define SDJ_N          4096          /* 명령 기록 줄 수(16바이트씩 64KB, PSRAM) */
#define SDJ_SLOW_US    50000         /* 이보다 오래 걸린 명령은 종류와 상관없이 남김 */
#define SDJ_JOIN_MS    100           /* 이어지는 쓰기를 한 줄로 묶는 간격 */
#define SDJ_FILE       FS_MOUNT_POINT "/sd_journal.bin"
#define SDJ_F_ERR      0x01
#define SDJ_F_MARK     0x02
#define SDJ_F_FAT      0x04          /* 이 쓰기가 FAT 영역을 건드림 */
#define SDH_FX_MAX     8192          /* FAT 한 벌 섹터 수 상한(32GB 카드 FAT32 약 7.6K) */

typedef struct __attribute__((packed)) {
    uint32_t t_ms;        /* 부팅 뒤 ms(첫 명령) */
    uint32_t arg;         /* 섹터 번호(SDHC) 또는 표시 값 */
    uint32_t dur_max_us;  /* 이 줄에 묶인 명령 중 가장 긴 것 */
    uint16_t blocks;      /* 묶인 블록 수(65535에서 멈춤), 표시는 종류 */
    uint8_t  op;          /* SD 명령 번호, 표시는 0xFF */
    uint8_t  flags;
} sdj_t;

static sdj_t   *s_j = NULL;
static uint32_t s_j_n = 0;              /* 지금까지 넣은 줄 수(링 위치 = s_j_n % SDJ_N) */
static int64_t  s_j_last_end_us = 0;     /* 마지막 쓰기 줄의 끝 시각 — 묶기 판단 */
static int64_t  s_j_boot_unix_us = 0;    /* 표시용: 유닉스 시각 - 부팅 뒤 시각 */
static portMUX_TYPE s_j_mux = portMUX_INITIALIZER_UNLOCKED;

/* FAT 기대값: [0, fatsz) = FAT1, [fatsz, 2·fatsz) = FAT2 */
static uint32_t *s_fx_crc = NULL;        /* 콘이 마지막으로 쓴(또는 기준 때 읽은) 내용의 CRC */
static uint32_t *s_fx_wt = NULL;         /* 마지막으로 콘이 쓴 시각(부팅 뒤 초, 0 = 기준 뒤 안 씀) */
static uint8_t  *s_fx_unk = NULL;        /* 1 = 기대값 모름(쓰기 실패 등) — 다음 감시에서 새로 잡음 */
static volatile uint32_t s_fx_lba0 = 0, s_fx_n = 0;   /* FAT1 시작 절대 섹터, FAT 한 벌 섹터 수(0 = 아직) */
static volatile uint32_t s_fx_wseq = 0;  /* FAT 쓰기 횟수 — 감시 중 쓰기가 끼었는지 */
static volatile bool s_fx_rebase = true;

static void j_put_locked(const sdj_t *e)
{
    s_j[s_j_n % SDJ_N] = *e;
    s_j_n++;
}

void sd_health_on_cmd(uint8_t op, uint32_t arg, const void *data, uint32_t blocks, uint32_t dur_us, int err)
{
    if (!s_j) return;
    int64_t now = esp_timer_get_time();
    bool is_write = (op == 24 || op == 25);
    uint8_t fl = err ? SDJ_F_ERR : 0;
    /* FAT 기대값 갱신(쓰기 성공이면 쓴 내용의 CRC, 실패면 모름) */
    uint32_t lba0 = s_fx_lba0, n = s_fx_n;
    if (is_write && n && arg < lba0 + 2 * n && arg + blocks > lba0) {
        fl |= SDJ_F_FAT;
        uint32_t ts = (uint32_t)(now / 1000000);
        for (uint32_t i = 0; i < blocks; i++) {
            uint32_t lba = arg + i;
            if (lba < lba0 || lba >= lba0 + 2 * n) continue;
            uint32_t idx = lba - lba0;
            uint32_t c = (!err && data) ? esp_rom_crc32_le(0, (const uint8_t *)data + i * 512, 512) : 0;
            taskENTER_CRITICAL(&s_j_mux);
            if (!err && data) { s_fx_crc[idx] = c; s_fx_unk[idx] = 0; } else { s_fx_unk[idx] = 1; }
            s_fx_wt[idx] = ts ? ts : 1;
            s_fx_wseq++;
            taskEXIT_CRITICAL(&s_j_mux);
        }
    }
    bool keep = is_write || err || dur_us > SDJ_SLOW_US || op == 0 || op == 32 || op == 33 || op == 38;
    if (!keep) return;
    uint32_t t_ms = (uint32_t)(now / 1000);
    taskENTER_CRITICAL(&s_j_mux);
    sdj_t *last = s_j_n ? &s_j[(s_j_n - 1) % SDJ_N] : NULL;
    if (is_write && !fl && last && (last->op == 24 || last->op == 25) && !(last->flags & (SDJ_F_ERR | SDJ_F_FAT | SDJ_F_MARK)) &&
        last->arg + last->blocks == arg && (now - s_j_last_end_us) < SDJ_JOIN_MS * 1000 && (uint32_t)last->blocks + blocks < 65535) {
        last->blocks += (uint16_t)blocks;
        if (dur_us > last->dur_max_us) last->dur_max_us = dur_us;
    } else {
        sdj_t e = { .t_ms = t_ms, .arg = arg, .dur_max_us = dur_us, .blocks = (uint16_t)(blocks > 65535 ? 65535 : blocks), .op = op, .flags = fl };
        j_put_locked(&e);
    }
    if (is_write) s_j_last_end_us = now;
    taskEXIT_CRITICAL(&s_j_mux);
}

void sd_health_mark(uint8_t code, uint32_t val)
{
    if (!s_j) return;
    sdj_t e = { .t_ms = (uint32_t)(esp_timer_get_time() / 1000), .arg = val, .blocks = code, .op = 0xFF, .flags = SDJ_F_MARK };
    taskENTER_CRITICAL(&s_j_mux);
    j_put_locked(&e);
    taskEXIT_CRITICAL(&s_j_mux);
}

static const char *mark_name(uint16_t code)
{
    switch (code) {
    case SDH_MARK_RELAY:     return "relay";
    case SDH_MARK_SCREEN:    return "screen";
    case SDH_MARK_MOUNT:     return "mount";
    case SDH_MARK_FATGUARD:  return "fat-guard";
    default:                 return "mark";
    }
}

/* 한 줄 글(시각은 콘 시계 기준 HH:MM:SS.mmm) */
static int j_line(char *out, size_t cap, const sdj_t *e)
{
    int64_t unix_ms = (s_j_boot_unix_us + (int64_t)e->t_ms * 1000) / 1000;
    time_t ts = (time_t)(unix_ms / 1000);
    struct tm tmv;
    localtime_r(&ts, &tmv);
    if (e->flags & SDJ_F_MARK) {
        return snprintf(out, cap, "%02d:%02d:%02d.%03d MARK %s %u\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(unix_ms % 1000),
                        mark_name(e->blocks), (unsigned)e->arg);
    }
    return snprintf(out, cap, "%02d:%02d:%02d.%03d CMD%u lba %u n %u max %uus%s%s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                    (int)(unix_ms % 1000), (unsigned)e->op, (unsigned)e->arg, (unsigned)e->blocks, (unsigned)e->dur_max_us,
                    (e->flags & SDJ_F_FAT) ? " FAT" : "", (e->flags & SDJ_F_ERR) ? " ERR" : "");
}

static void j_update_clock(void)
{
    time_t now = time(NULL);
    if (now > 1704067200) s_j_boot_unix_us = (int64_t)now * 1000000 - esp_timer_get_time();
}

int sd_health_journal_text(char *buf, size_t cap, uint32_t max_lines)
{
    if (!s_j || cap == 0) return 0;
    j_update_clock();
    uint32_t total = s_j_n, have = total < SDJ_N ? total : SDJ_N;
    if (max_lines && have > max_lines) have = max_lines;
    int n = snprintf(buf, cap, "# %u lines shown of %u (newest last)\n", (unsigned)have, (unsigned)total);
    sdj_t e;
    for (uint32_t i = total - have; i < total && n < (int)cap - 96; i++) {
        taskENTER_CRITICAL(&s_j_mux);
        e = s_j[i % SDJ_N];
        taskEXIT_CRITICAL(&s_j_mux);
        n += j_line(buf + n, cap - n, &e);
    }
    return n;
}

/* 사건 때 — 명령 기록 전체를 LittleFS에(다음 사건이 덮어씀), 최근 줄은 로그에도 */
static void journal_save(const char *why)
{
    if (!s_j) return;
    j_update_clock();
    sdj_t *copy = heap_caps_malloc(SDJ_N * sizeof(sdj_t), MALLOC_CAP_SPIRAM);
    if (!copy) return;
    uint32_t total;
    taskENTER_CRITICAL(&s_j_mux);
    total = s_j_n;
    memcpy(copy, s_j, SDJ_N * sizeof(sdj_t));
    taskEXIT_CRITICAL(&s_j_mux);
    FILE *f = fopen(SDJ_FILE, "wb");
    if (f) {
        int64_t hdr[2] = { s_j_boot_unix_us, (int64_t)total };
        fwrite(hdr, sizeof(hdr), 1, f);
        fwrite(copy, sizeof(sdj_t), SDJ_N, f);
        fclose(f);
    }
    ESP_LOGW(TAG, "SD journal saved (%s): %u commands so far, last lines:", why, (unsigned)total);
    uint32_t have = total < SDJ_N ? total : SDJ_N, show = have < 40 ? have : 40;
    char line[96];
    for (uint32_t i = total - show; i < total; i++) {
        j_line(line, sizeof(line), &copy[i % SDJ_N]);
        size_t L = strlen(line);
        if (L && line[L - 1] == '\n') line[L - 1] = '\0';
        ESP_LOGW(TAG, "  J %s", line);
    }
    heap_caps_free(copy);
}

/* 10분마다(기록 태스크) — FAT 두 벌을 읽어 기대값과 비교. 기준이 없으면(마운트 직후) 지금 내용을 기준으로 */
static void fat_guard(void)
{
    if (!sd_storage_is_mounted() || !s_fx_crc) return;
    uint8_t *buf = heap_caps_malloc(SDH_FAT_CHUNK * 512, MALLOC_CAP_SPIRAM);
    uint8_t *one = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    if (!buf || !one) { heap_caps_free(buf); heap_caps_free(one); return; }
    int64_t t0 = esp_timer_get_time();
    uint32_t part, rsv, fatsz, fsinfo;
    char why[40] = "";
    if (!read_layout(buf, &part, &rsv, &fatsz, &fsinfo, why, sizeof(why)) || fatsz > SDH_FX_MAX) {
        heap_caps_free(buf); heap_caps_free(one);
        return;
    }
    uint32_t lba0 = part + rsv, total = 2 * fatsz;
    bool rebase = s_fx_rebase || s_fx_lba0 != lba0 || s_fx_n != fatsz;
    if (rebase) {
        s_fx_n = 0;   /* 기준을 다 잡을 때까지 쓰기 갱신 끔 */
        s_fx_lba0 = lba0;
    }
    uint32_t changed = 0, rechecked = 0;
    for (uint32_t s = 0; s < total; s += SDH_FAT_CHUNK) {
        uint32_t k = (total - s < SDH_FAT_CHUNK) ? total - s : SDH_FAT_CHUNK;
        uint32_t seq0 = s_fx_wseq;
        if (sd_storage_read_sectors(buf, lba0 + s, k) != ESP_OK) {
            ESP_LOGW(TAG, "FAT guard: read failed at sector %u", (unsigned)(lba0 + s));
            heap_caps_free(buf); heap_caps_free(one);
            return;   /* 기준을 잡던 중이면 다음에 다시(s_fx_rebase 그대로) */
        }
        for (uint32_t i = 0; i < k; i++) {
            uint32_t idx = s + i;
            const uint8_t *p = buf + i * 512;
            uint32_t c = esp_rom_crc32_le(0, p, 512);
            if (rebase) { s_fx_crc[idx] = c; s_fx_wt[idx] = 0; s_fx_unk[idx] = 0; continue; }
            taskENTER_CRITICAL(&s_j_mux);
            uint32_t exp = s_fx_crc[idx];
            bool unk = s_fx_unk[idx];
            if (unk) { s_fx_crc[idx] = c; s_fx_unk[idx] = 0; }
            taskEXIT_CRITICAL(&s_j_mux);
            if (unk || c == exp) continue;
            if (s_fx_wseq != seq0) {   /* 읽는 사이 콘의 FAT 쓰기가 끼었음 — 그 섹터만 다시 읽어 그때의 기대값과 비교 */
                rechecked++;
                if (sd_storage_read_sectors(one, lba0 + idx, 1) != ESP_OK) continue;
                c = esp_rom_crc32_le(0, one, 512);
                taskENTER_CRITICAL(&s_j_mux);
                exp = s_fx_crc[idx];
                taskEXIT_CRITICAL(&s_j_mux);
                if (c == exp) continue;
                p = one;
            }
            uint32_t wt = s_fx_wt[idx];
            uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
            ESP_LOGE(TAG, "FAT guard: FAT%u sector %u (abs %u) changed WITHOUT a Cntl write (crc %08X -> %08X), last Cntl write: %s %us ago, "
                     "first bytes %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                     (unsigned)(idx < fatsz ? 1 : 2), (unsigned)(idx % fatsz), (unsigned)(lba0 + idx), (unsigned)exp, (unsigned)c,
                     wt ? "write" : "none since baseline,", (unsigned)(wt ? now_s - wt : now_s),
                     p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
            taskENTER_CRITICAL(&s_j_mux);
            s_fx_crc[idx] = c;   /* 같은 변화를 매번 다시 보고하지 않게 */
            taskEXIT_CRITICAL(&s_j_mux);
            if (changed++ == 0) {
                char d[40];
                snprintf(d, sizeof(d), "FAT%u sector %u", (unsigned)(idx < fatsz ? 1 : 2), (unsigned)(idx % fatsz));
                sd_health_report(SDH_EV_FAT_CHANGED_NO_WRITE, d, lba0 + idx);
            }
        }
    }
    heap_caps_free(buf);
    heap_caps_free(one);
    if (rebase) {
        s_fx_n = fatsz;
        s_fx_rebase = false;
        ESP_LOGI(TAG, "FAT guard: baseline %u sectors x2 (FAT1 at %u), %ums", (unsigned)fatsz, (unsigned)lba0,
                 (unsigned)((esp_timer_get_time() - t0) / 1000));
        sd_health_mark(SDH_MARK_FATGUARD, 0);
        return;
    }
    sd_guard_note_outside(changed);   /* 2026-10-22 — 쓰기 보호 범위 밖 손상으로 셈(효과 보고) */
    ESP_LOGI(TAG, "FAT guard: %u sectors x2, %u changed without Cntl write, %u rechecked, %u Cntl FAT sector writes since boot, %ums",
             (unsigned)fatsz, (unsigned)changed, (unsigned)rechecked, (unsigned)s_fx_wseq, (unsigned)((esp_timer_get_time() - t0) / 1000));
    sd_health_mark(SDH_MARK_FATGUARD, changed);
}

/* 매시 FAT1/FAT2 비교에서 다른 섹터 — 각 벌이 콘이 쓴 내용인지(기대값과 같은지) 남김 */
static void fat_mismatch_detail(uint32_t fat_idx, const uint8_t *s1, const uint8_t *s2, uint32_t fatsz)
{
    if (s_fx_n != fatsz || fat_idx >= fatsz) return;
    uint32_t c1 = esp_rom_crc32_le(0, s1, 512), c2 = esp_rom_crc32_le(0, s2, 512);
    uint32_t i2 = fat_idx + fatsz;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    ESP_LOGE(TAG, "FAT mismatch sector %u: FAT1 %s (last Cntl write %us ago), FAT2 %s (last Cntl write %us ago)", (unsigned)fat_idx,
             c1 == s_fx_crc[fat_idx] ? "= what Cntl wrote" : "!= what Cntl wrote", (unsigned)(s_fx_wt[fat_idx] ? now - s_fx_wt[fat_idx] : 0),
             c2 == s_fx_crc[i2] ? "= what Cntl wrote" : "!= what Cntl wrote", (unsigned)(s_fx_wt[i2] ? now - s_fx_wt[i2] : 0));
}

/* FAT1/FAT2를 원본 섹터로 비교. 사진 저장·통계 기록을 잠깐 멈추고(재연결과 같은 잠금 순서: 사진 → 통계) 읽음.
 * 다른 섹터가 있으면 3초 뒤 그 섹터만 다시 읽어 여전히 다를 때만 보고. 끝까지 읽었고 다른 섹터가 없으면 true.
 * force = 고장 상태에서도 돎(재연결·포맷 검증 — sd_health_fat_ok) */
static bool fat_check(bool force)
{
    if (!sd_storage_is_mounted() || (!force && !sd_health_writes_allowed())) return false;
    uint8_t *a = heap_caps_malloc(SDH_FAT_CHUNK * 512, MALLOC_CAP_SPIRAM);
    uint8_t *b = heap_caps_malloc(SDH_FAT_CHUNK * 512, MALLOC_CAP_SPIRAM);
    uint32_t *bad = heap_caps_malloc(SDH_FAT_MAXBAD * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!a || !b || !bad) { heap_caps_free(a); heap_caps_free(b); heap_caps_free(bad); return false; }

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
                if (still < 8) fat_mismatch_detail(bad[i], a, b, fatsz);   /* 2026-10-21 — 각 벌이 콘이 쓴 내용인지 */
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
    if (!ok) return false;
    sd_health_count(SDH_C_FAT_CHECK, 1);
    ESP_LOGI(TAG, "FAT check: %u sectors x2, first pass %u differ, confirmed %u, %ums", (unsigned)fatsz, (unsigned)nbad,
             (unsigned)still, (unsigned)((esp_timer_get_time() - t0) / 1000));
    if (still) sd_health_report(SDH_EV_FAT_MISMATCH, "FAT sector", first_bad);
    return still == 0;
}

/* 2026-10-21(C4 재연결 검증 — 10-21 실기: FAT1/FAT2가 30섹터 다른 카드가 읽기 한 번 통과로 재연결 1초 만에 정상으로 돌아가
 * 쓰기가 다시 열렸음) — 재연결·포맷 뒤 검증에서 고장 상태와 상관없이 FAT1/FAT2를 비교 */
bool sd_health_fat_ok(void)
{
    return fat_check(true);
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
    fat_guard();   /* 2026-10-21 — FAT 섹터가 콘의 쓰기 없이 바뀌었는지 */
}

/* 마운트·포맷 직후(그 태스크에서) — 기준값은 기록 태스크에서 새로 잡음 */
void sd_health_on_mount(void)
{
    s_guard_rebase = true;
    s_fx_rebase = true;   /* 2026-10-21 — FAT 기대값도 새로 */
    sd_health_mark(SDH_MARK_MOUNT, 0);
    stats_store_post_job(guard_job, NULL, 0);
}

static void hourly_job(const uint8_t *arg)
{
    (void)arg;
    fat_check(false);
    guard_check();
    dir_check();
    stats_store_sd_check();   /* 2026-10-15 — 최근 두 주 통계 원시·집계 파일의 레코드가 그 주에 속하는지 */
    sd_guard_hourly();        /* 2026-10-22 — 쓰기 보호 집계 한 줄·저장 */
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
    /* 2026-10-21 — 명령 기록·FAT 기대값(PSRAM, 약 64KB + 72KB) */
    s_j = heap_caps_calloc(SDJ_N, sizeof(sdj_t), MALLOC_CAP_SPIRAM);
    s_fx_crc = heap_caps_calloc(2 * SDH_FX_MAX, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_fx_wt = heap_caps_calloc(2 * SDH_FX_MAX, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_fx_unk = heap_caps_calloc(2 * SDH_FX_MAX, 1, MALLOC_CAP_SPIRAM);
    if (!s_j || !s_fx_crc || !s_fx_wt || !s_fx_unk) ESP_LOGE(TAG, "SD journal alloc failed");
    if (!s_fx_crc || !s_fx_wt || !s_fx_unk) { heap_caps_free(s_fx_crc); heap_caps_free(s_fx_wt); heap_caps_free(s_fx_unk); s_fx_crc = NULL; s_fx_wt = NULL; s_fx_unk = NULL; }
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
