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
#include "freertos/semphr.h"

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

/* 2026-10-22 — 감시·검사가 기록 태스크(10분·매시)와 재연결 검증(화면·웹 태스크)에서 겹치지 않게 */
static SemaphoreHandle_t s_gmtx = NULL;
static void gm_take(void) { if (s_gmtx) xSemaphoreTakeRecursive(s_gmtx, portMAX_DELAY); }
static void gm_give(void) { if (s_gmtx) xSemaphoreGiveRecursive(s_gmtx); }


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

/* ════════════════════════════════════════════════════════════
 * 2026-10-22(듀얼 파티션 — 사용자 결정 "정상인 곳에서 비정상인 곳을 메꾸고, 알리고") — FAT 감시·검사를 벌(copy) 단위로:
 * 한 벌 카드는 A의 FAT1·FAT2 두 벌, 듀얼은 A·B 각각의 FAT1·FAT2 네 벌. 섹터마다 콘이 쓴 내용(CRC, sd_health_on_cmd)과 비교해
 * 틀린 벌은 맞는 벌로 메꿈(sd_guard_copy_sector — 다시 읽어 확인) → 경고 6005. 맞는 벌이 없으면 예전처럼 SD 이상(5009).
 * 기준을 새로 잡을 때(마운트·포맷·재연결 검증)는 벌들 가운데 과반인 내용을 기준으로(과반이 없으면 SD 이상) — 사진·통계 쓰기를 멈추고
 * ════════════════════════════════════════════════════════════ */
#define FATG_MAXCAND  1024   /* 한 번에 다시 볼 틀린 벌 수 상한 */

typedef struct { uint32_t idx; uint8_t copy; } fatg_cand_t;   /* idx = FAT1 기준 섹터 순번, copy = vol*2 + (0 FAT1 / 1 FAT2) */

/* A 기준 절대 섹터에서 FAT 섹터 기대값과 짝(FAT1 ↔ FAT2) */
static bool fat_exp(uint32_t lba, uint32_t *crc, uint32_t *peer)
{
    uint32_t lba0 = s_fx_lba0, n = s_fx_n;
    if (!s_fx_crc || !n || lba < lba0 || lba >= lba0 + 2 * n) return false;
    uint32_t idx = lba - lba0;
    taskENTER_CRITICAL(&s_j_mux);
    bool ok = !s_fx_unk[idx];
    *crc = s_fx_crc[idx];
    taskEXIT_CRITICAL(&s_j_mux);
    *peer = idx < n ? lba + n : lba - n;
    return ok;
}

static uint8_t *s_rc_buf = NULL;   /* FAT 읽기 확인용 한 섹터(PSRAM) — FatFs 볼륨 잠금 아래에서만 */

bool sd_health_fat_read_check(uint32_t lba, uint8_t *sp, int vol)
{
    uint32_t exp, peer;
    if (!fat_exp(lba, &exp, &peer)) return true;
    if (esp_rom_crc32_le(0, sp, 512) == exp) return true;
    if (!s_rc_buf) s_rc_buf = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    if (!s_rc_buf) return true;
    bool dual = sd_guard_dual_info(NULL);
    const struct { int v; uint32_t lba; } src[3] = { { !vol, lba }, { vol, peer }, { !vol, peer } };
    for (int k = 0; k < 3; k++) {
        if (src[k].v != vol && !(dual && sd_guard_vol_usable(src[k].v))) continue;
        if (sd_guard_read_vol(src[k].v, s_rc_buf, src[k].lba, 1) != ESP_OK) continue;
        if (esp_rom_crc32_le(0, s_rc_buf, 512) != exp) continue;
        memcpy(sp, s_rc_buf, 512);
        bool ok = sd_guard_copy_sector(src[k].v, src[k].lba, vol, lba, &exp);
        sd_guard_note_fix(vol, 1, ok, lba, "FAT (read)");
        return true;
    }
    ESP_LOGE(TAG, "FAT read check: sector %u differs from what Cntl wrote on every copy", (unsigned)lba);
    sd_health_report(SDH_EV_FAT_CHANGED_NO_WRITE, "FAT read: all copies", lba);
    return false;
}

/* read_layout을 A에서, 안 되면(듀얼) B에서 */
static bool read_layout_any(uint8_t *buf, uint32_t *part, uint32_t *rsv, uint32_t *fatsz, uint32_t *fsinfo, char *why, size_t why_cap)
{
    uint32_t delta, a0 = sd_guard_dual_a0();
    if (!sd_guard_dual_info(&delta)) return read_layout(buf, part, rsv, fatsz, fsinfo, why, why_cap);
    /* 듀얼 — 배치는 마운트 때 확인한 값(MBR이 망가져도 감시가 멈추지 않고 되살릴 수 있게) */
    *part = a0;
    for (int v = 0; v < 2; v++) {
        if (!sd_guard_vol_usable(v)) continue;
        if (sd_guard_read_vol(v, buf, a0, 1) != ESP_OK || buf[510] != 0x55 || buf[511] != 0xAA) continue;
        uint16_t bps = le16(buf + 11);
        *rsv = le16(buf + 14);
        *fatsz = le16(buf + 22) ? le16(buf + 22) : le32(buf + 36);
        *fsinfo = le16(buf + 22) ? UINT32_MAX : a0 + le16(buf + 48);
        if (bps == 512 && buf[16] >= 2 && *fatsz && *rsv) return true;
    }
    snprintf(why, why_cap, "VBR %u bad on A and B", (unsigned)a0);
    return false;
}

/* 벌 목록(v·f). 반환 = 벌 수 */
static int fat_copies(uint8_t copies[4])
{
    int n = 0;
    bool dual = sd_guard_dual_info(NULL);
    for (int v = 0; v < 2; v++) {
        if (dual ? !sd_guard_vol_usable(v) : v != 0) continue;
        copies[n++] = (uint8_t)(v * 2);
        copies[n++] = (uint8_t)(v * 2 + 1);
    }
    return n;
}

/* 틀린 벌 하나를 맞는 벌(기대값과 같은)로 메꿈 — 다른 쪽 같은 자리 → 같은 쪽 짝 → 다른 쪽 짝 */
static bool fat_fix_copy(uint32_t lba0, uint32_t fatsz, uint32_t idx, uint8_t dst, uint32_t exp, const uint8_t *copies, int ncopies)
{
    int dv = dst >> 1, df = dst & 1;
    const uint8_t order[3] = { (uint8_t)((!dv) * 2 + df), (uint8_t)(dv * 2 + !df), (uint8_t)((!dv) * 2 + !df) };
    uint32_t dlba = lba0 + idx + (df ? fatsz : 0);
    for (int k = 0; k < 3; k++) {
        bool have = false;
        for (int j = 0; j < ncopies; j++) if (copies[j] == order[k]) have = true;
        if (!have) continue;
        uint32_t slba = lba0 + idx + ((order[k] & 1) ? fatsz : 0);
        if (sd_guard_copy_sector(order[k] >> 1, slba, dv, dlba, &exp)) return true;
    }
    return false;
}

/* 과반(벌 2개면 둘이 같을 때만) */
static bool fat_majority(const uint32_t *cc, int nc, uint32_t *out)
{
    int best = 0, bestn = 0;
    for (int j = 0; j < nc; j++) {
        int m = 0;
        for (int q = 0; q < nc; q++) m += (cc[q] == cc[j]);
        if (m > bestn) { bestn = m; best = j; }
    }
    *out = cc[best];
    return (bestn * 2 > nc) || (nc == 2 && bestn == 2);
}

/* 기준을 잡는 중 — 아직 콘이 안 쓴 FAT 섹터(FAT1·FAT2 짝)의 기대값을 정함 */
static void fat_set_exp(uint32_t idx, uint32_t fatsz, uint32_t exp)
{
    taskENTER_CRITICAL(&s_j_mux);
    if (s_fx_unk[idx]) { s_fx_crc[idx] = exp; s_fx_unk[idx] = 0; }
    if (s_fx_unk[idx + fatsz]) { s_fx_crc[idx + fatsz] = exp; s_fx_unk[idx + fatsz] = 0; }
    taskEXIT_CRITICAL(&s_j_mux);
}

/* FAT 감시 본체. rebase = 기준을 과반으로 새로 잡음(마운트·포맷·재연결 검증). 메꾸지 못한 틀린 벌이 없으면 true */
static bool fat_guard_run(bool rebase)
{
    if (!sd_storage_is_mounted() || !s_fx_crc) return false;
    uint8_t copies[4];
    int nc = fat_copies(copies);
    uint8_t *buf = heap_caps_malloc(4 * SDH_FAT_CHUNK * 512, MALLOC_CAP_SPIRAM);
    fatg_cand_t *cand = heap_caps_malloc(FATG_MAXCAND * sizeof(fatg_cand_t), MALLOC_CAP_SPIRAM);
    if (!buf || !cand || nc == 0) { heap_caps_free(buf); heap_caps_free(cand); return false; }
    int64_t t0 = esp_timer_get_time();
    uint32_t part = 0, rsv, fatsz, fsinfo;
    char why[40] = "";
    if (!read_layout_any(buf, &part, &rsv, &fatsz, &fsinfo, why, sizeof(why)) || fatsz > SDH_FX_MAX) {
        ESP_LOGE(TAG, "FAT guard: boot sector problem (%s)", why);
        heap_caps_free(buf); heap_caps_free(cand);
        if (why[0] && sd_storage_is_mounted()) sd_health_report(SDH_EV_LAYOUT, why, 0);
        return false;
    }
    uint32_t lba0 = part + rsv;
    rebase = rebase || s_fx_rebase || s_fx_lba0 != lba0 || s_fx_n != fatsz;
    if (rebase) {
        /* 기대값을 모두 "모름"으로 두고 쓰기 추적은 켠 채 읽음 — 읽는 사이 콘이 쓴 섹터는 그 쓴 내용이 기대값(10-22 실기: 기준을 잡는
         * 사이 알림 기록 쓰기가 끼어 네 벌이 2:2로 갈림 → 오탐 5009) */
        taskENTER_CRITICAL(&s_j_mux);
        s_fx_lba0 = lba0;
        s_fx_n = fatsz;
        memset(s_fx_unk, 1, 2 * fatsz);
        memset(s_fx_wt, 0, 2 * fatsz * sizeof(uint32_t));
        taskEXIT_CRITICAL(&s_j_mux);
    }
    uint32_t ncand = 0, unresolved = 0, fixed = 0, first_bad = 0;
    bool overflow = false, read_fail = false;
    for (uint32_t s = 0; s < fatsz && !read_fail; s += SDH_FAT_CHUNK) {
        uint32_t k = (fatsz - s < SDH_FAT_CHUNK) ? fatsz - s : SDH_FAT_CHUNK;
        uint32_t seq0 = s_fx_wseq;
        for (int j = 0; j < nc && !read_fail; j++) {
            uint8_t c = copies[j];
            if (sd_guard_read_vol(c >> 1, buf + (size_t)j * SDH_FAT_CHUNK * 512, lba0 + s + ((c & 1) ? fatsz : 0), k) != ESP_OK) {
                ESP_LOGW(TAG, "FAT guard: read failed at sector %u (copy %u)", (unsigned)(lba0 + s), (unsigned)c);
                read_fail = true;
            }
        }
        if (read_fail) break;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t idx = s + i;
            uint32_t cc[4];
            for (int j = 0; j < nc; j++) cc[j] = esp_rom_crc32_le(0, buf + ((size_t)j * SDH_FAT_CHUNK + i) * 512, 512);
            if (rebase) {
                taskENTER_CRITICAL(&s_j_mux);
                bool unk = s_fx_unk[idx] && s_fx_unk[idx + fatsz];   /* 기준을 잡기 시작한 뒤 콘이 안 씀 */
                taskEXIT_CRITICAL(&s_j_mux);
                if (unk) {
                    uint32_t exp;
                    if (!fat_majority(cc, nc, &exp)) {   /* 과반 없음 — 잠깐 뒤 다시 봄 */
                        if (ncand < FATG_MAXCAND) cand[ncand++] = (fatg_cand_t){ idx, 0xFF }; else overflow = true;
                        continue;
                    }
                    fat_set_exp(idx, fatsz, exp);
                }
            }
            /* 기대값과 비교(기준을 잡을 때는 방금 정한 과반 또는 그 사이 콘이 쓴 내용) */
            for (int j = 0; j < nc; j++) {
                uint32_t ei = idx + ((copies[j] & 1) ? fatsz : 0);
                taskENTER_CRITICAL(&s_j_mux);
                uint32_t exp = s_fx_crc[ei];
                bool unk = s_fx_unk[ei];
                if (unk && !rebase && (copies[j] >> 1) == 0) { s_fx_crc[ei] = cc[j]; s_fx_unk[ei] = 0; }   /* 모르던 기대값은 A의 지금 내용으로 */
                taskEXIT_CRITICAL(&s_j_mux);
                if (unk || cc[j] == exp) continue;
                if (ncand < FATG_MAXCAND) cand[ncand++] = (fatg_cand_t){ idx, copies[j] }; else overflow = true;
            }
        }
        (void)seq0;
    }
    if (rebase && read_fail) {
        heap_caps_free(buf); heap_caps_free(cand);
        return false;   /* 기준을 잡던 중 — 다음에 다시(s_fx_rebase 그대로) */
    }
    if (ncand) vTaskDelay(pdMS_TO_TICKS(1000));   /* 읽는 사이 콘의 쓰기(A 다음 B)가 끼었을 수 있음 — 잠깐 뒤 다시 봄 */
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    for (uint32_t c = 0; c < ncand; c++) {
        uint32_t idx = cand[c].idx;
        uint8_t cp = cand[c].copy;
        if (cp == 0xFF) {   /* 기준 때 과반이 없던 섹터 — 다시 읽어 과반(그 사이 콘이 썼으면 그 내용) */
            uint32_t cc[4], exp;
            bool rd = true;
            for (int j = 0; j < nc && rd; j++) {
                rd = sd_guard_read_vol(copies[j] >> 1, buf, lba0 + idx + ((copies[j] & 1) ? fatsz : 0), 1) == ESP_OK;
                cc[j] = esp_rom_crc32_le(0, buf, 512);
            }
            taskENTER_CRITICAL(&s_j_mux);
            bool unk = s_fx_unk[idx] && s_fx_unk[idx + fatsz];
            exp = s_fx_crc[idx];
            taskEXIT_CRITICAL(&s_j_mux);
            if (unk) {
                if (!rd || !fat_majority(cc, nc, &exp)) {
                    ESP_LOGE(TAG, "FAT guard: sector %u has no majority among %d copies", (unsigned)idx, nc);
                    if (unresolved++ == 0) first_bad = lba0 + idx;
                    fat_set_exp(idx, fatsz, cc[0]);   /* 같은 것을 매번 다시 보고하지 않게 */
                    continue;
                }
                fat_set_exp(idx, fatsz, exp);
            }
            for (int j = 0; j < nc; j++) {
                if (cc[j] == exp) continue;
                uint32_t lba = lba0 + idx + ((copies[j] & 1) ? fatsz : 0);
                bool ok = fat_fix_copy(lba0, fatsz, idx, copies[j], exp, copies, nc);
                sd_guard_note_fix(copies[j] >> 1, 1, ok, lba, "FAT (mount check)");
                if (ok) fixed++; else if (unresolved++ == 0) first_bad = lba;
            }
            continue;
        }
        uint32_t ei = idx + ((cp & 1) ? fatsz : 0);
        uint32_t lba = lba0 + ei;
        taskENTER_CRITICAL(&s_j_mux);
        uint32_t exp = s_fx_crc[ei];
        uint32_t wt = s_fx_wt[ei];
        taskEXIT_CRITICAL(&s_j_mux);
        if (sd_guard_read_vol(cp >> 1, buf, lba, 1) != ESP_OK) continue;
        uint32_t cur = esp_rom_crc32_le(0, buf, 512);
        if (cur == exp) continue;   /* 그 사이 맞아짐(콘의 쓰기) */
        ESP_LOGE(TAG, "FAT guard: %c FAT%u sector %u (abs %u) differs from what Cntl wrote (crc %08X -> %08X), last Cntl write: %s %us ago, "
                 "first bytes %02X%02X%02X%02X%02X%02X%02X%02X",
                 (cp >> 1) ? 'B' : 'A', (unsigned)((cp & 1) + 1), (unsigned)idx, (unsigned)lba, (unsigned)exp,
                 (unsigned)cur, wt ? "write" : "none since baseline,", (unsigned)(wt ? now_s - wt : now_s),
                 buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
        if (fat_fix_copy(lba0, fatsz, idx, cp, exp, copies, nc)) {
            fixed++;
            sd_guard_note_fix(cp >> 1, 1, true, lba, rebase ? "FAT (mount check)" : "FAT (10-min check)");
            continue;
        }
        sd_guard_note_fix(cp >> 1, 1, false, lba, "FAT");
        if (unresolved++ == 0) first_bad = lba;
        taskENTER_CRITICAL(&s_j_mux);
        if (!rebase) s_fx_crc[ei] = cur;   /* 같은 변화를 매번 다시 보고하지 않게 */
        taskEXIT_CRITICAL(&s_j_mux);
    }
    if (rebase) s_fx_rebase = false;
    heap_caps_free(buf);
    heap_caps_free(cand);
    if (overflow) {
        ESP_LOGE(TAG, "FAT guard: more than %u differing copies - not all checked", (unsigned)FATG_MAXCAND);
        if (unresolved++ == 0) first_bad = lba0;
    }
    sd_guard_note_outside(fixed + unresolved);   /* 쓰기 보호 범위 밖 손상(효과 보고) */
    ESP_LOGI(TAG, "FAT guard%s: %u sectors x%d copies, %u differing, %u repaired, %u not repaired, %u Cntl FAT sector writes since boot, %ums",
             rebase ? " (baseline)" : "", (unsigned)fatsz, nc, (unsigned)ncand, (unsigned)fixed, (unsigned)unresolved, (unsigned)s_fx_wseq,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    sd_health_mark(SDH_MARK_FATGUARD, fixed + unresolved);
    if (unresolved) {
        char d[40];
        snprintf(d, sizeof(d), "FAT sector %u", (unsigned)(first_bad - lba0));
        sd_health_report(SDH_EV_FAT_CHANGED_NO_WRITE, d, first_bad);
        return false;
    }
    return true;
}

/* 10분마다(기록 태스크) */
static void fat_guard(void)
{
    fat_guard_run(false);
}

/* 매시(force = false) — FAT 감시와 같음(FAT1·FAT2 모두 기대값과 비교하므로 두 벌이 갈라진 것도 여기서 잡음).
 * 재연결·포맷 검증(force) — 고장 상태에서도, 기준을 과반으로 새로 잡으며 */
static bool fat_check(bool force)
{
    if (!sd_storage_is_mounted() || (!force && !sd_health_writes_allowed())) return false;
    bool ok = fat_guard_run(force);
    if (ok) sd_health_count(SDH_C_FAT_CHECK, 1);
    return ok;
}

/* 2026-10-21(C4 재연결 검증 — 10-21 실기: FAT1/FAT2가 30섹터 다른 카드가 읽기 한 번 통과로 재연결 1초 만에 정상으로 돌아가
 * 쓰기가 다시 열렸음) — 재연결·포맷 뒤 검증에서 고장 상태와 상관없이 FAT1/FAT2를 비교 */
bool sd_health_fat_ok(void)
{
    gm_take();
    bool ok = fat_check(true);
    gm_give();
    return ok;
}

/* ════════════════════════════════════════════════════════════
 * 2026-10-15(MPIO 카드 손상 대응, 사용자 지시 — "유사한 에러가 발생할 때 로그로 잡을 수 있도록") — 부트 영역 감시.
 * 0번 섹터 ~ FAT 앞(예약 영역)은 FatFs가 FSInfo 한 섹터 말고는 쓰지 않음. 마운트·포맷 직후 섹터마다 CRC를 잡아 두고
 * 10분마다 다시 읽어 비교 — 다르면 바뀐 섹터 번호와 앞 16바이트를 기록하고 SD 이상(쓰기 중지·5009·푸시).
 * MPIO에선 사진 폴더가 새 클러스터를 받은 09:16 뒤 한 시간 안에 0번 섹터가 무작위로 바뀌었음(Docs/한일 153) */
static uint32_t *s_guard_crc = NULL;       /* PSRAM, SDH_GUARD_MAX */
static uint32_t  s_guard_n = 0;            /* 기준값을 잡은 섹터 수(0 = 아직) */
static volatile bool s_guard_rebase = true;

/* 2026-10-22(듀얼 파티션) — 부트 영역 감시: 0번 섹터(MBR, 한 벌)는 기준 때 내용을 RAM에 두고 바뀌면 그 내용으로 되살림.
 * A·B의 예약 영역(부트 섹터~FAT 앞, FSInfo 제외)은 기준과 다르면 다른 쪽(기준과 같은)으로 메꿈. 양쪽 다 다르면 SD 이상.
 * 그 사이 빈 곳(1 ~ A 시작 전)은 쓰지 않는 곳이라 보지 않음 */
static uint8_t *s_mbr = NULL;   /* 기준 때 0번 섹터(PSRAM 512) */

static void guard_check_dual(void)
{
    uint8_t *buf = heap_caps_malloc(SDH_FAT_CHUNK * 512, MALLOC_CAP_SPIRAM);
    if (!s_guard_crc) s_guard_crc = heap_caps_malloc(SDH_GUARD_MAX * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!s_mbr) s_mbr = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    if (!buf || !s_guard_crc || !s_mbr) { heap_caps_free(buf); return; }
    int64_t t0 = esp_timer_get_time();
    uint32_t part = 0, rsv, fatsz, fsinfo;
    char why[40] = "";
    if (!read_layout_any(buf, &part, &rsv, &fatsz, &fsinfo, why, sizeof(why)) || rsv > SDH_GUARD_MAX || rsv > SDH_FAT_CHUNK * 4) {
        heap_caps_free(buf);
        if (sd_storage_is_mounted()) {
            ESP_LOGE(TAG, "Boot-area guard: boot sector problem (%s)", why);
            sd_health_report(SDH_EV_LAYOUT, why, 0);
        }
        return;
    }
    bool rebase = s_guard_rebase || s_guard_n != rsv;
    uint32_t fs_rel = (fsinfo == UINT32_MAX) ? UINT32_MAX : fsinfo - part;
    uint32_t fixed = 0, unresolved = 0;
    char detail[40] = "";

    /* 0번 섹터 */
    if (sd_storage_read_sectors(buf, 0, 1) == ESP_OK) {
        if (rebase) memcpy(s_mbr, buf, 512);
        else if (memcmp(buf, s_mbr, 512) != 0) {
            ESP_LOGE(TAG, "Boot-area guard: MBR (sector 0) changed, first bytes %02X%02X%02X%02X%02X%02X%02X%02X - restoring",
                     buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
            bool ok = sd_guard_write_raw(s_mbr, 0, 1) == ESP_OK && sd_storage_read_sectors(buf, 0, 1) == ESP_OK && memcmp(buf, s_mbr, 512) == 0;
            sd_guard_note_fix(0, 1, ok, 0, "MBR");
            if (ok) fixed++;
            else if (unresolved++ == 0) hexstr(detail, sizeof(detail), "S", 0, buf, 16);
        }
    }

    /* 예약 영역 — 기준은 A(A를 못 쓰면 B) */
    int base_v = sd_guard_vol_usable(0) ? 0 : 1;
    bool other_ok = sd_guard_vol_usable(!base_v);
    for (uint32_t i = 0; i < rsv; i++) {
        if (i == fs_rel) continue;   /* FSInfo는 FatFs가 씀 */
        uint32_t lba = part + i;
        uint32_t c[2] = { 0, 0 };
        bool have[2] = { false, false };
        for (int v = 0; v < 2; v++) {
            if (!sd_guard_vol_usable(v)) continue;
            have[v] = sd_guard_read_vol(v, buf, lba, 1) == ESP_OK;
            if (have[v]) c[v] = esp_rom_crc32_le(0, buf, 512);
        }
        if (!have[base_v]) continue;
        if (rebase) {
            s_guard_crc[i] = c[base_v];
            if (other_ok && have[!base_v] && c[!base_v] != c[base_v]) {   /* 다른 쪽이 기준과 다름 — 기준 쪽으로 */
                bool ok = sd_guard_copy_sector(base_v, lba, !base_v, lba, &s_guard_crc[i]);
                sd_guard_note_fix(!base_v, 1, ok, lba, "boot area (mount check)");
                if (ok) fixed++; else unresolved++;
            }
            continue;
        }
        for (int v = 0; v < 2; v++) {
            if (!have[v] || c[v] == s_guard_crc[i]) continue;
            ESP_LOGE(TAG, "Boot-area guard: %c sector %u changed (crc %08X -> %08X)", v ? 'B' : 'A', (unsigned)lba, (unsigned)s_guard_crc[i],
                     (unsigned)c[v]);
            bool ok = have[!v] && c[!v] == s_guard_crc[i] && sd_guard_copy_sector(!v, lba, v, lba, &s_guard_crc[i]);
            sd_guard_note_fix(v, 1, ok, lba, "boot area");
            if (ok) { fixed++; continue; }
            if (unresolved++ == 0) snprintf(detail, sizeof(detail), "%c S%u", v ? 'B' : 'A', (unsigned)lba);
        }
    }
    heap_caps_free(buf);
    if (rebase) {
        s_guard_n = rsv;
        s_guard_rebase = false;
        ESP_LOGI(TAG, "Boot-area guard: baseline MBR + %u reserved sectors x%d (partition at %u, FSInfo %d), %u repaired, %ums", (unsigned)rsv,
                 other_ok ? 2 : 1, (unsigned)part, (fsinfo == UINT32_MAX) ? -1 : (int)fsinfo, (unsigned)fixed,
                 (unsigned)((esp_timer_get_time() - t0) / 1000));
        if (unresolved) sd_health_report(SDH_EV_BOOT_AREA_CHANGED, "mount: B differs, not repaired", unresolved);
        return;
    }
    ESP_LOGI(TAG, "Boot-area guard: MBR + %u sectors x%d, %u repaired, %u not repaired, %ums", (unsigned)rsv, other_ok ? 2 : 1,
             (unsigned)fixed, (unsigned)unresolved, (unsigned)((esp_timer_get_time() - t0) / 1000));
    if (unresolved) sd_health_report(SDH_EV_BOOT_AREA_CHANGED, detail, unresolved);
}

static void guard_check(void)
{
    if (!sd_storage_is_mounted()) return;   /* 읽기만 하므로 고장 상태에서도 돎(재연결 직후 기준값도 이때 잡힘) */
    if (sd_guard_dual_info(NULL)) { guard_check_dual(); return; }   /* 2026-10-22 */
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
    gm_take();
    guard_check();
    fat_guard();   /* 2026-10-21 — FAT 섹터가 콘의 쓰기 없이 바뀌었는지(2026-10-22 — 틀린 벌은 맞는 벌로 메꿈) */
    gm_give();
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
    gm_take();
    fat_check(false);
    guard_check();
    gm_give();
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
    s_gmtx = xSemaphoreCreateRecursiveMutex();
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
