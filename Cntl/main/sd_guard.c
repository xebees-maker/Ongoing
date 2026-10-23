/**
 * @file    sd_guard.c
 * @brief   sd_guard.h 구현 — SD 쓰기 보호(쓰기 전 내용 → 쓰기 → 다시 읽어 비교 → 다른 블록 다시 쓰기)
 *          + 2026-10-22 듀얼 파티션 이중화(A 영역에 쓰는 섹터를 B 영역 같은 자리에도 씀, 망가진 쪽을 다른 쪽으로 메꿈)
 */
#include "sd_guard.h"
#include "sd_health.h"
#include "ui_log.h"
#include "fs.h"
#include "ff.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "SD";

#define SDG_BLK      16      /* 카드가 한꺼번에 다시 쓰는 단위(섹터) — 10-21·10-22 손상이 모두 16섹터(8KB) 정렬 */
#define SDG_NEIGH    2       /* 앞뒤로 함께 확인할 블록 수 — 10-21에 32섹터(2블록) 떨어진 블록까지 바뀜 */
#define SDG_ROUNDS   3       /* 다시 쓰고 확인하는 최대 횟수 */
#define SDG_SLOW_US  20000   /* 이보다 오래 붙잡은 쓰기를 "오래 붙잡음"으로 셈(평소 약 1ms, 손상 때 47~76ms) */
#define SDG_EVENTS   32
#define SDG_FILE     FS_MOUNT_POINT "/sd_guard.bin"
#define SDG_VERSION  2       /* 2026-10-22 — 2: 볼륨별(이중화) 집계 추가. 1은 읽어서 옮김 */
#define SDG_PRE_MAX  ((SDG_NEIGH + 1) * SDG_BLK * 2)   /* 쓰기 전 내용 최대 섹터 수(앞쪽 + 뒤쪽) */
#define SDG_DUAL_START  8192u        /* 듀얼 포맷: 첫 파티션 시작(4MB 정렬) */
#define SDG_DUAL_ALIGN  8192u        /* 파티션 크기 정렬(4MB) */
#define SDG_DUAL_MIN    (2097152u)   /* 한 쪽 최소 1GB(FAT32 16KB 클러스터 65525개 이상) */

typedef struct __attribute__((packed)) {
    uint32_t writes;          /* 보호한 쓰기 수 */
    uint32_t slow;            /* 오래 붙잡은 쓰기 */
    uint32_t damaged;         /* 쓴 뒤 비교에서 틀린 쓰기(사건) */
    uint32_t sec_written_blk; /* 틀린 섹터 — 쓴 섹터가 든 블록 안 */
    uint32_t sec_neighbor;    /* 틀린 섹터 — 앞뒤 블록 */
    uint32_t repaired;        /* 다시 써서 고친 사건 */
    uint32_t failed;          /* 끝내 못 고친 사건 */
    uint32_t outside;         /* 보호 범위 밖 손상(10분 FAT 감시가 찾은 섹터 수) */
    uint32_t unguarded;       /* 쓰기 전 내용을 못 읽어 보호 없이 쓴 쓰기 */
    uint32_t max_us;          /* 가장 오래 붙잡은 쓰기(us) */
} sdg_cnt_t;

typedef struct __attribute__((packed)) {
    uint32_t t;               /* 콘 시각(유닉스) */
    uint32_t lba;             /* 콘이 쓴 시작 섹터 */
    uint16_t count;           /* 콘이 쓴 섹터 수 */
    uint16_t bad_written_blk; /* 처음 비교에서 틀린 섹터 — 쓴 블록 안 */
    uint16_t bad_neighbor;    /* 처음 비교에서 틀린 섹터 — 앞뒤 블록 */
    uint8_t  rounds;          /* 다시 쓴 횟수 */
    uint8_t  ok;              /* 1 = 고침 */
    uint32_t write_us;        /* 그 쓰기에서 카드가 붙잡은 시간 */
    uint32_t first_bad;       /* 처음 틀린 섹터 번호 */
} sdg_ev_t;

/* 2026-10-22 — 볼륨(A/B)별 이중화 집계 */
typedef struct __attribute__((packed)) {
    uint32_t fixed_sectors;   /* 다른 쪽 내용으로 메꾼 섹터 */
    uint32_t fix_events;      /* 메꾼 사건 */
    uint32_t fix_failed;      /* 메꾸지 못한 사건(다른 쪽도 틀림·다시 써도 틀림) */
    uint32_t read_fallback;   /* 이 쪽 읽기가 실패해 다른 쪽에서 읽음 */
    uint32_t write_failed;    /* 쓰기 보호로도 못 고쳐 이 쪽을 쓰지 않게 됨 */
    uint32_t last_t;          /* 마지막 사건 시각 */
    uint32_t last_lba;        /* 마지막 사건 섹터(A 기준) */
} sdg_vol_t;

typedef struct __attribute__((packed)) {
    uint32_t  version;
    uint32_t  day;
    sdg_cnt_t today, yesterday, total;
    uint32_t  ev_n;
    sdg_ev_t  ev[SDG_EVENTS];
    sdg_vol_t vol[2];         /* 2026-10-22(v2) */
} sdg_state_t;

static sdg_state_t *s_st = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_dirty = false;
static sdmmc_card_t *s_card = NULL;
static volatile bool s_enabled = true;
static uint8_t *s_pre = NULL;    /* 쓰기 전 내용(SDG_PRE_MAX 섹터, PSRAM) */
static uint8_t *s_chk = NULL;    /* 다시 읽기·블록 다시 쓰기용 한 블록(PSRAM) */
static uint8_t *s_one = NULL;    /* 2026-10-22 — 다른 쪽 한 섹터(PSRAM) */
static SemaphoreHandle_t s_wmtx = NULL;   /* s_pre/s_chk/s_one 보호(FatFs 볼륨 잠금 밖에서도 메꾸기가 씀) */

/* 2026-10-22 — 이중화 배치: A 영역 [s_a0, s_a1), B = A + s_delta. 0번 섹터(MBR)와 그 뒤 빈 곳은 한 벌 */
static volatile bool    s_dual = false;
static volatile uint32_t s_a0 = 0, s_a1 = 0, s_delta = 0;
static volatile uint8_t s_vol_bad[2] = { 0, 0 };   /* 1 = 이 쪽은 쓰지도 읽지도 않음(메꾸지 못함 — 카드 교체 준비) */


static void save_now(void)
{
    if (!s_st) return;
    FILE *f = fopen(SDG_FILE, "wb");
    if (!f) return;
    fwrite(s_st, sizeof(*s_st), 1, f);
    fclose(f);
    s_dirty = false;
}

/* 2026-10-22 — now는 잠금(critical) 밖에서 구해 넘김: time()은 libc 잠금을 잡는데 critical 안에서 다른 태스크가 그 잠금을 쥐고
 * 있으면 IDF가 abort() — 10-22 23:25 복원 중 콘 재부팅(lock_acquire_generic → abort) */
static void roll_day_locked(uint32_t now)
{
    uint32_t d = now / 86400;
    if (now < 1704067200u || d == s_st->day) return;
    if (s_st->day != 0) s_st->yesterday = s_st->today;
    memset(&s_st->today, 0, sizeof(s_st->today));
    s_st->day = d;
}

/* 집계 더하기(두 칸 같이) */
#define SDG_ADD(field, n) do { s_st->today.field += (n); s_st->total.field += (n); } while (0)

void sd_guard_init(void)
{
    if (s_st) return;
    s_st = heap_caps_calloc(1, sizeof(*s_st), MALLOC_CAP_SPIRAM);
    s_pre = heap_caps_malloc(SDG_PRE_MAX * 512, MALLOC_CAP_SPIRAM);
    s_chk = heap_caps_malloc(SDG_BLK * 512, MALLOC_CAP_SPIRAM);
    s_one = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    s_wmtx = xSemaphoreCreateRecursiveMutex();
    if (!s_st || !s_pre || !s_chk || !s_one || !s_wmtx) { ESP_LOGE(TAG, "SD guard alloc failed - writes not guarded"); s_st = NULL; return; }
    FILE *f = fopen(SDG_FILE, "rb");
    if (f) {
        size_t v1 = offsetof(sdg_state_t, vol);   /* 1판 크기(볼륨 집계 없음) */
        size_t got = fread(s_st, 1, sizeof(*s_st), f);
        if (s_st->version == SDG_VERSION && got == sizeof(*s_st)) {
            /* 그대로 */
        } else if (s_st->version == 1 && got == v1) {
            memset(s_st->vol, 0, sizeof(s_st->vol));   /* 1판 → 2판: 쓰기 보호 집계는 그대로 */
        } else {
            memset(s_st, 0, sizeof(*s_st));
        }
        fclose(f);
    }
    s_st->version = SDG_VERSION;
}

void sd_guard_set_enabled(bool on) { s_enabled = on; }

void sd_guard_note_outside(uint32_t sectors)
{
    if (!s_st || !sectors) return;
    uint32_t now = (uint32_t)time(NULL);
    taskENTER_CRITICAL(&s_mux);
    roll_day_locked(now);
    SDG_ADD(outside, sectors);
    taskEXIT_CRITICAL(&s_mux);
    s_dirty = true;
}

/* ── 이중화 도우미 ── */
static inline bool in_a(uint32_t s, uint32_t n) { return s_dual && s >= s_a0 && s + n <= s_a1; }
static inline uint32_t vol_off(int v) { return v ? s_delta : 0; }
static inline int primary_vol(void) { return s_vol_bad[0] ? 1 : 0; }
static inline char vol_ch(int v) { return v ? 'B' : 'A'; }

static void vol_count(int v, uint32_t sectors, bool fixed, uint32_t lba, bool fallback)
{
    if (!s_st) return;
    uint32_t now = (uint32_t)time(NULL);
    taskENTER_CRITICAL(&s_mux);
    sdg_vol_t *c = &s_st->vol[v];
    if (fallback) c->read_fallback++;
    else if (fixed) { c->fixed_sectors += sectors; c->fix_events++; }
    else c->fix_failed++;
    c->last_t = now;
    c->last_lba = lba;
    taskEXIT_CRITICAL(&s_mux);
    s_dirty = true;
}

/* 이 쪽을 더는 쓰지 않음 — 다른 쪽도 이미 그렇다면 SD 이상(쓰기 중지) */
static void vol_fail(int v, const char *why, uint32_t lba)
{
    if (s_vol_bad[v]) return;
    s_vol_bad[v] = 1;
    if (s_st) {
        uint32_t now = (uint32_t)time(NULL);
        taskENTER_CRITICAL(&s_mux);
        s_st->vol[v].write_failed++;
        s_st->vol[v].last_t = now;
        s_st->vol[v].last_lba = lba;
        taskEXIT_CRITICAL(&s_mux);
        save_now();
    }
    ESP_LOGE(TAG, "SD dual: volume %c taken out of use (%s at %u) - running on %c only, prepare a card replacement", vol_ch(v), why,
             (unsigned)lba, vol_ch(!v));
    if (s_vol_bad[!v]) {
        sd_health_report(SDH_EV_GUARD_REPAIR_FAILED, "both volumes failed", lba);
        return;
    }
    ui_log_add_warn(UI_WARN_SD_VOL_FAILED, "SD volume %c failed (%s) - running on %c, replace the card", vol_ch(v), why, vol_ch(!v));
}

void sd_guard_vol_fail(int v, const char *why, uint32_t lba_a) { vol_fail(v ? 1 : 0, why, lba_a); }

bool sd_guard_dual_info(uint32_t *delta)
{
    if (delta) *delta = s_delta;
    return s_dual;
}

bool sd_guard_vol_usable(int v) { return s_dual && !s_vol_bad[v ? 1 : 0]; }

/* 2026-10-23(10-23 11:57 크래시 — 꺼내기가 카드를 해제한 뒤 다른 태스크의 FatFs 읽기가 해제된 카드로 명령을 보냄) — 카드를 쓰는
 * 모든 진입점은 s_wmtx 아래에서 s_card를 확인. 언마운트 전에 sd_guard_detach가 진행 중인 명령이 끝나길 기다려 카드를 놓음 */
void sd_guard_detach(void)
{
    if (!s_wmtx) { s_card = NULL; s_dual = false; return; }
    xSemaphoreTakeRecursive(s_wmtx, portMAX_DELAY);
    s_card = NULL;
    s_dual = false;
    xSemaphoreGiveRecursive(s_wmtx);
}

esp_err_t sd_guard_read_vol(int v, void *dst, uint32_t lba_a, uint32_t n)
{
    xSemaphoreTakeRecursive(s_wmtx, portMAX_DELAY);
    esp_err_t e = s_card ? sdmmc_read_sectors(s_card, dst, lba_a + vol_off(v), n) : ESP_ERR_INVALID_STATE;
    xSemaphoreGiveRecursive(s_wmtx);
    return e;
}

/* src_v의 src_lba_a 한 섹터를 dst_v의 dst_lba_a에 복사하고 다시 읽어 확인(lba는 A 기준). want_crc가 있으면 원본이 그 CRC일 때만 */
bool sd_guard_copy_sector(int src_v, uint32_t src_lba_a, int dst_v, uint32_t dst_lba_a, const uint32_t *want_crc)
{
    if (!s_one) return false;
    xSemaphoreTakeRecursive(s_wmtx, portMAX_DELAY);
    if (!s_card || ((src_v || dst_v) && !s_dual)) { xSemaphoreGiveRecursive(s_wmtx); return false; }
    bool ok = sdmmc_read_sectors(s_card, s_one, src_lba_a + vol_off(src_v), 1) == ESP_OK;
    uint32_t c = ok ? esp_rom_crc32_le(0, s_one, 512) : 0;
    if (ok && want_crc && c != *want_crc) ok = false;   /* 원본도 틀림 */
    if (ok) ok = sdmmc_write_sectors(s_card, s_one, dst_lba_a + vol_off(dst_v), 1) == ESP_OK;
    if (ok) ok = sdmmc_read_sectors(s_card, s_one, dst_lba_a + vol_off(dst_v), 1) == ESP_OK && esp_rom_crc32_le(0, s_one, 512) == c;
    xSemaphoreGiveRecursive(s_wmtx);
    return ok;
}

/* 메꾼 결과 집계·로그·경고(성공 = 경고 6005, 실패 = 로그만 — 실패의 다음 조치는 부른 쪽이 정함) */
void sd_guard_note_fix(int v, uint32_t sectors, bool ok, uint32_t lba_a, const char *what)
{
    vol_count(v, sectors, ok, lba_a, false);
    if (ok) {
        ESP_LOGW(TAG, "SD dual: volume %c %s - %u sector(s) at %u repaired from the other copy", vol_ch(v), what, (unsigned)sectors,
                 (unsigned)lba_a);
        ui_log_add_warn(UI_WARN_SD_VOL_REPAIRED, "SD volume %c: %s, %u sector(s) repaired from the other copy", vol_ch(v), what,
                        (unsigned)sectors);
    } else {
        ESP_LOGE(TAG, "SD dual: volume %c %s - sector %u NOT repaired", vol_ch(v), what, (unsigned)lba_a);
    }
}

/* bad_v 쪽 섹터 [lba_a, lba_a+n)를 다른 쪽 같은 자리 내용으로 메꿈(want_crc가 있으면 섹터마다 그 CRC일 때만) */
bool sd_guard_fix_from_other(int bad_v, uint32_t lba_a, uint32_t n, const uint32_t *want_crc, const char *what)
{
    if (!s_dual || s_vol_bad[!bad_v]) return false;
    bool ok = true;
    for (uint32_t i = 0; i < n && ok; i++) ok = sd_guard_copy_sector(!bad_v, lba_a + i, bad_v, lba_a + i, want_crc ? &want_crc[i] : NULL);
    sd_guard_note_fix(bad_v, n, ok, lba_a, what);
    return ok;
}

/* 쓰기 보호 없이 한 쪽에 바로 씀(MBR 다시 쓰기 등, lba는 절대 섹터) */
esp_err_t sd_guard_write_raw(const void *src, uint32_t lba, uint32_t n)
{
    xSemaphoreTakeRecursive(s_wmtx, portMAX_DELAY);
    esp_err_t e = s_card ? sdmmc_write_sectors(s_card, src, lba, n) : ESP_ERR_INVALID_STATE;
    xSemaphoreGiveRecursive(s_wmtx);
    return e;
}

/* ── FatFs 디스크 계층(원래 diskio_sdmmc와 같고 쓰기만 보호) ── */
static DSTATUS g_init(BYTE pdrv)
{
    (void)pdrv;
    xSemaphoreTakeRecursive(s_wmtx, portMAX_DELAY);
    DSTATUS st = (s_card && sdmmc_get_status(s_card) == ESP_OK) ? 0 : STA_NOINIT;
    xSemaphoreGiveRecursive(s_wmtx);
    return st;
}
static DSTATUS g_status(BYTE pdrv) { (void)pdrv; return 0; }

/* 2026-10-22 — 듀얼: A 영역 읽기는 쓰는 쪽(A, A를 못 쓰면 B)에서. 읽기가 실패하면 다른 쪽에서. FAT 섹터는 콘이 쓴 내용(CRC)과
 * 다르면 다른 쪽을 보고, 그쪽이 맞으면 그 내용을 돌려주고 틀린 쪽을 메꿈 — FatFs가 망가진 FAT를 읽어 파일을 늘려 버리는 일
 * (10-21·10-22 s0009)을 막음 */
static DRESULT g_read_locked(BYTE *buf, uint32_t sector, unsigned count)
{
    if (!s_card) return RES_NOTRDY;
    if (!in_a(sector, count)) {
        if (sdmmc_read_sectors(s_card, buf, sector, count) != ESP_OK) return RES_ERROR;
        if (s_dual) return RES_OK;   /* 듀얼의 한 벌 영역(MBR 쪽) */
        for (unsigned i = 0; i < count; i++) {   /* 한 벌 카드 — FAT1이 틀리면 FAT2로(sd_health_fat_read_check) */
            if (!sd_health_fat_read_check(sector + i, buf + (size_t)i * 512, 0)) return RES_ERROR;
        }
        return RES_OK;
    }
    int p = primary_vol();
    if (sdmmc_read_sectors(s_card, buf, sector + vol_off(p), count) != ESP_OK) {
        if (s_vol_bad[!p] || sdmmc_read_sectors(s_card, buf, sector + vol_off(!p), count) != ESP_OK) return RES_ERROR;
        vol_count(p, count, false, sector, true);
        ESP_LOGW(TAG, "SD dual: read %u+%u failed on %c - read from %c", (unsigned)sector, count, vol_ch(p), vol_ch(!p));
        return RES_OK;
    }
    for (unsigned i = 0; i < count; i++) {
        if (!sd_health_fat_read_check(sector + i, buf + (size_t)i * 512, p)) return RES_ERROR;   /* 모든 벌이 틀림 — FatFs가 쓰지 않게 */
    }
    return RES_OK;
}

static DRESULT g_read(BYTE pdrv, BYTE *buf, uint32_t sector, unsigned count)
{
    (void)pdrv;
    xSemaphoreTakeRecursive(s_wmtx, portMAX_DELAY);
    DRESULT r = g_read_locked(buf, sector, count);
    xSemaphoreGiveRecursive(s_wmtx);
    return r;
}

static DRESULT g_ioctl(BYTE pdrv, BYTE cmd, void *buf)
{
    (void)pdrv;
    if (!s_card) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_COUNT: *((DWORD *)buf) = s_card->csd.capacity; return RES_OK;
    case GET_SECTOR_SIZE:  *((WORD *)buf) = s_card->csd.sector_size; return RES_OK;
    case GET_BLOCK_SIZE:   return RES_ERROR;
    case CTRL_TRIM:        return RES_PARERR;   /* SD는 원래도 지원 안 함(sdmmc_can_trim) */
    default:               return RES_ERROR;
    }
}

/* 섹터 s의 맞는 내용: 콘이 쓴 범위면 새 내용, 아니면 쓰기 전 내용 */
static const uint8_t *expected(uint32_t s, const uint8_t *buf, uint32_t sector, uint32_t count, uint32_t r0)
{
    if (s >= sector && s < sector + count) return buf + (size_t)(s - sector) * 512;
    uint32_t head = sector - r0;
    if (s < sector) return s_pre + (size_t)(s - r0) * 512;
    return s_pre + (size_t)(head + (s - sector - count)) * 512;
}

/* 한 영역에 보호 쓰기. [lo, hi) = 앞뒤 블록을 볼 수 있는 범위(그 영역). 고쳤거나 손상 없으면 true.
 * dual = 듀얼 쓰기의 한 쪽(못 고치면 호출부가 그 쪽을 빼고, 둘 다 못 쓰면 SD 이상) */
static bool guarded_write(const BYTE *buf, uint32_t sector, unsigned count, uint32_t lo, uint32_t hi, bool dual)
{
    if (!s_enabled || !s_st || !s_pre || !s_chk || count == 0) {
        return sdmmc_write_sectors(s_card, buf, sector, count) == ESP_OK;
    }
    uint32_t a0 = sector / SDG_BLK * SDG_BLK, a1 = (sector + count + SDG_BLK - 1) / SDG_BLK * SDG_BLK;
    uint32_t r0 = (a0 >= lo + SDG_NEIGH * SDG_BLK) ? a0 - SDG_NEIGH * SDG_BLK : lo;
    uint32_t r1 = a1 + SDG_NEIGH * SDG_BLK;
    if (r0 > sector) r0 = sector;
    if (r1 > hi) r1 = hi;
    if (r1 < sector + count) r1 = sector + count;
    uint32_t head = sector - r0, tail = r1 - (sector + count);

    /* 1) 쓰기 전 내용 */
    bool pre_ok = (head + tail <= SDG_PRE_MAX) &&
                  (head == 0 || sdmmc_read_sectors(s_card, s_pre, r0, head) == ESP_OK) &&
                  (tail == 0 || sdmmc_read_sectors(s_card, s_pre + (size_t)head * 512, sector + count, tail) == ESP_OK);

    /* 2) 쓰기 */
    int64_t t0 = esp_timer_get_time();
    esp_err_t we = sdmmc_write_sectors(s_card, buf, sector, count);
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    uint32_t now = (uint32_t)time(NULL);
    taskENTER_CRITICAL(&s_mux);
    roll_day_locked(now);
    SDG_ADD(writes, 1);
    if (dt > SDG_SLOW_US) SDG_ADD(slow, 1);
    if (dt > s_st->today.max_us) s_st->today.max_us = dt;
    if (dt > s_st->total.max_us) s_st->total.max_us = dt;
    if (!pre_ok) SDG_ADD(unguarded, 1);
    taskEXIT_CRITICAL(&s_mux);
    if (we != ESP_OK) return false;
    if (!pre_ok) return true;


    /* 3) 다시 읽어 비교, 틀린 블록은 다시 쓰고 다시 확인 */
    uint16_t bad_w = 0, bad_n = 0;
    uint32_t first_bad = 0;
    int rounds = 0;
    bool clean = false;
    for (int pass = 0; pass <= SDG_ROUNDS; pass++) {
        bool any = false;
        for (uint32_t b = r0; b < r1; b += SDG_BLK) {
            uint32_t n = (r1 - b < SDG_BLK) ? r1 - b : SDG_BLK;
            if (sdmmc_read_sectors(s_card, s_chk, b, n) != ESP_OK) { any = true; continue; }
            bool blk_bad = false;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t s = b + i;
                if (memcmp(s_chk + (size_t)i * 512, expected(s, buf, sector, count, r0), 512) == 0) continue;
                blk_bad = true;
                if (pass == 0) {
                    if (s >= a0 && s < a1) bad_w++; else bad_n++;
                    if (bad_w + bad_n == 1) first_bad = s;
                    ESP_LOGW(TAG, "SD guard: sector %u differs after writing %u+%u (card busy %uus)", (unsigned)s, (unsigned)sector,
                             (unsigned)count, (unsigned)dt);
                }
            }
            if (!blk_bad) continue;
            any = true;
            if (pass == SDG_ROUNDS) continue;   /* 마지막 확인 — 다시 쓰지 않음 */
            for (uint32_t i = 0; i < n; i++) memcpy(s_chk + (size_t)i * 512, expected(b + i, buf, sector, count, r0), 512);
            sdmmc_write_sectors(s_card, s_chk, b, n);
        }
        if (!any) { clean = true; break; }
        if (pass < SDG_ROUNDS) rounds++;
    }
    if (bad_w + bad_n == 0) return true;

    /* 사건 기록 */
    uint32_t ev_t = (uint32_t)time(NULL);
    taskENTER_CRITICAL(&s_mux);
    SDG_ADD(damaged, 1);
    SDG_ADD(sec_written_blk, bad_w);
    SDG_ADD(sec_neighbor, bad_n);
    if (clean) SDG_ADD(repaired, 1); else SDG_ADD(failed, 1);
    sdg_ev_t *e = &s_st->ev[s_st->ev_n % SDG_EVENTS];
    e->t = ev_t;
    e->lba = sector;
    e->count = (uint16_t)(count > 65535 ? 65535 : count);
    e->bad_written_blk = bad_w;
    e->bad_neighbor = bad_n;
    e->rounds = (uint8_t)rounds;
    e->ok = clean ? 1 : 0;
    e->write_us = dt;
    e->first_bad = first_bad;
    s_st->ev_n++;
    taskEXIT_CRITICAL(&s_mux);
    save_now();
    if (clean) {
        ESP_LOGW(TAG, "SD guard: repaired - write %u+%u (busy %uus) changed %u sectors in its block, %u in neighbor blocks, %d rewrite(s)",
                 (unsigned)sector, (unsigned)count, (unsigned)dt, (unsigned)bad_w, (unsigned)bad_n, rounds);
        return true;
    }
    ESP_LOGE(TAG, "SD guard: NOT repaired after %d rewrites - write %u+%u", rounds, (unsigned)sector, (unsigned)count);
    if (!dual) {
        char d[40];
        snprintf(d, sizeof(d), "guard write %u+%u", (unsigned)sector, (unsigned)count);
        sd_health_report(SDH_EV_GUARD_REPAIR_FAILED, d, first_bad);
    }
    return false;
}

static DRESULT g_write(BYTE pdrv, const BYTE *buf, uint32_t sector, unsigned count)
{
    (void)pdrv;
    xSemaphoreTakeRecursive(s_wmtx, portMAX_DELAY);
    if (!s_card) { xSemaphoreGiveRecursive(s_wmtx); return RES_NOTRDY; }
    DRESULT r;
    if (!in_a(sector, count)) {
        /* 한 벌 영역(MBR 쪽) 또는 듀얼이 아닌 카드 — 예전과 같음 */
        uint32_t hi = s_dual ? s_a0 : s_card->csd.capacity;
        if (s_dual && sector + count > s_a0) hi = s_card->csd.capacity;   /* 걸친 쓰기(없어야 함) */
        r = guarded_write(buf, sector, count, 0, hi, false) ? RES_OK : RES_ERROR;
    } else {
        /* 2026-10-22 — 듀얼: A 다음 B(같은 자리). 한 쪽을 못 고치면 그 쪽을 빼고 다른 쪽으로 계속 */
        bool ok_any = false;
        for (int v = 0; v < 2; v++) {
            if (s_vol_bad[v]) continue;
            uint32_t off = vol_off(v);
            if (guarded_write(buf, sector + off, count, s_a0 + off, s_a1 + off, true)) { ok_any = true; continue; }
            vol_fail(v, "write not repaired", sector);
        }
        r = ok_any ? RES_OK : RES_ERROR;
    }
    xSemaphoreGiveRecursive(s_wmtx);
    return r;
}

static const ff_diskio_impl_t s_impl = { .init = g_init, .status = g_status, .read = g_read, .write = g_write, .ioctl = g_ioctl };

static uint32_t le32p(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* 2026-10-22 — 0번 섹터의 파티션 표가 듀얼 배치(같은 크기 FAT32 두 개, 둘째가 첫째 바로 뒤)면 그 배치를 씀.
 * A·B 부트 섹터가 다르면(한쪽만 포맷 등) B는 쓰지 않음 */
static void detect_dual(sdmmc_card_t *card)
{
    s_dual = false;
    s_vol_bad[0] = s_vol_bad[1] = 0;
    uint8_t *b = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!b) return;
    bool dual = false;
    uint32_t p1s = 0, p1n = 0, p2s = 0, p2n = 0;
    if (sdmmc_read_sectors(card, b, 0, 1) == ESP_OK && b[510] == 0x55 && b[511] == 0xAA && !(b[0] == 0xEB || b[0] == 0xE9)) {
        const uint8_t *e1 = b + 446, *e2 = b + 462;
        p1s = le32p(e1 + 8); p1n = le32p(e1 + 12);
        p2s = le32p(e2 + 8); p2n = le32p(e2 + 12);
        bool t1 = (e1[4] == 0x0B || e1[4] == 0x0C), t2 = (e2[4] == 0x0B || e2[4] == 0x0C);
        dual = t1 && t2 && p1n > 0 && p2s == p1s + p1n && p2n == p1n && p2s + p2n <= card->csd.capacity;
    }
    if (dual) {
        s_a0 = p1s; s_a1 = p1s + p1n; s_delta = p1n;
        s_dual = true;
        /* A·B 부트 섹터 비교 */
        if (sdmmc_read_sectors(card, b, p1s, 1) != ESP_OK || sdmmc_read_sectors(card, b + 512, p2s, 1) != ESP_OK ||
            memcmp(b, b + 512, 512) != 0) {
            s_vol_bad[1] = 1;
            ESP_LOGE(TAG, "SD dual: volume B boot sector differs from A - B not used (format to rebuild)");
            ui_log_add_warn(UI_WARN_SD_VOL_FAILED, "SD volume B does not match A - running on A, format to rebuild");
        }
        ESP_LOGI(TAG, "SD dual: A %u+%u, B %u+%u%s", (unsigned)p1s, (unsigned)p1n, (unsigned)p2s, (unsigned)p2n,
                 s_vol_bad[1] ? " (B out of use)" : "");
    } else {
        ESP_LOGW(TAG, "SD dual: card is not dual-partitioned - single copy (format to make it dual)");
        ui_log_add_warn(UI_WARN_SD_NOT_DUAL, "SD card is a single copy - format it to make it dual");
    }
    heap_caps_free(b);
}

void sd_guard_attach(sdmmc_card_t *card)
{
    BYTE pdrv = ff_diskio_get_pdrv_card(card);
    if (pdrv == 0xff) { ESP_LOGE(TAG, "SD guard: drive for card not found - writes not guarded"); return; }
    s_card = card;
    detect_dual(card);
    ff_diskio_register(pdrv, &s_impl);
    ESP_LOGI(TAG, "SD guard on (drive %u, block %u sectors, +-%u neighbor blocks)", (unsigned)pdrv, SDG_BLK, SDG_NEIGH);
}

/* 2026-10-22(사용자 결정 — 듀얼 파티션) — 마운트 안 된 카드(card는 초기화만 된 것)를 듀얼로 포맷:
 * 0번 섹터에 파티션 표(같은 크기 FAT32 둘, 4MB 정렬)를 쓰고, 첫 파티션만 f_mkfs — 이 계층이 같은 내용을 B에도 씀 */
/* 듀얼 배치: 카드 크기로 한 쪽 크기(0 = 너무 작음) */
static uint32_t dual_half(uint32_t cap)
{
    if (cap <= SDG_DUAL_START) return 0;
    uint32_t half = (cap - SDG_DUAL_START) / 2 / SDG_DUAL_ALIGN * SDG_DUAL_ALIGN;
    return half < SDG_DUAL_MIN ? 0 : half;
}

static void build_mbr(uint8_t *mbr, uint32_t half)
{
    memset(mbr, 0, 512);
    for (int k = 0; k < 2; k++) {
        uint8_t *e = mbr + 446 + k * 16;
        uint32_t st = SDG_DUAL_START + (uint32_t)k * half;
        e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;   /* CHS는 LBA 사용 표시 */
        e[4] = 0x0C;                               /* FAT32 LBA */
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        for (int i = 0; i < 4; i++) { e[8 + i] = (uint8_t)(st >> (8 * i)); e[12 + i] = (uint8_t)(half >> (8 * i)); }
    }
    mbr[510] = 0x55; mbr[511] = 0xAA;
}

uint32_t sd_guard_dual_a0(void) { return s_a0; }

/* 2026-10-22 — 마운트가 실패한 카드(초기화만)의 0번 섹터가 망가졌고 듀얼 배치 자리(A·B)의 부트 섹터가 둘 다 멀쩡하고 같으면
 * 파티션 표를 다시 씀. 다시 썼으면 true(부른 쪽이 마운트를 다시 시도) */
bool sd_guard_repair_mbr(sdmmc_card_t *card)
{
    uint32_t half = dual_half(card->csd.capacity);
    uint8_t *b = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!half || !b) { heap_caps_free(b); return false; }
    bool ok = false;
    if (sdmmc_read_sectors(card, b, 0, 1) == ESP_OK && !(b[510] == 0x55 && b[511] == 0xAA) &&
        sdmmc_read_sectors(card, b, SDG_DUAL_START, 1) == ESP_OK && sdmmc_read_sectors(card, b + 512, SDG_DUAL_START + half, 1) == ESP_OK &&
        b[510] == 0x55 && b[511] == 0xAA && (b[0] == 0xEB || b[0] == 0xE9) && memcmp(b, b + 512, 512) == 0 && le32p(b + 32) == half) {
        build_mbr(b, half);
        ok = sdmmc_write_sectors(card, b, 0, 1) == ESP_OK;
        ESP_LOGW(TAG, "SD dual: MBR was damaged - rebuilt from the A/B boot sectors: %s", ok ? "OK" : "write failed");
    }
    heap_caps_free(b);
    return ok;
}

esp_err_t sd_guard_format_dual(sdmmc_card_t *card)
{
    uint32_t cap = card->csd.capacity;
    uint32_t half = dual_half(cap);
    if (!half) { ESP_LOGE(TAG, "SD dual format: card too small (%u sectors)", (unsigned)cap); return ESP_ERR_INVALID_SIZE; }
    const size_t work_sz = 32 * 1024;
    uint8_t *mbr = heap_caps_calloc(1, 512, MALLOC_CAP_SPIRAM);
    uint8_t *work = heap_caps_malloc(work_sz, MALLOC_CAP_SPIRAM);
    if (!mbr || !work) { heap_caps_free(mbr); heap_caps_free(work); return ESP_ERR_NO_MEM; }
    build_mbr(mbr, half);
    esp_err_t err = sdmmc_write_sectors(card, mbr, 0, 1);
    heap_caps_free(mbr);
    if (err != ESP_OK) { heap_caps_free(work); ESP_LOGE(TAG, "SD dual format: MBR write failed"); return err; }

    s_card = card;
    s_a0 = SDG_DUAL_START; s_a1 = SDG_DUAL_START + half; s_delta = half;
    s_vol_bad[0] = s_vol_bad[1] = 0;
    s_dual = true;
    /* f_mkfs는 예약 영역 32섹터 중 부트·FSInfo와 그 백업만 씀 — 나머지에 카드의 옛 내용이 남아 A·B가 달라 보이지 않게 0으로
     * (10-22 첫 포맷: 마운트 검사가 B 예약 영역 28섹터를 "복구"함) */
    memset(work, 0, work_sz);
    for (int v = 0; v < 2 && err == ESP_OK; v++) err = sdmmc_write_sectors(card, work, s_a0 + vol_off(v), work_sz / 512);
    if (err != ESP_OK) { heap_caps_free(work); ESP_LOGE(TAG, "SD dual format: reserved area clear failed"); return err; }
    BYTE pdrv = 0xFF;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) { heap_caps_free(work); return ESP_FAIL; }
    ff_diskio_register(pdrv, &s_impl);
    VolToPart[pdrv].pt = 1;   /* 이 드라이브는 첫 파티션 */
    char drv[3] = { (char)('0' + pdrv), ':', 0 };
    const MKFS_PARM opt = { FM_FAT32, 2, 0, 0, 16 * 1024 };
    int64_t t0 = esp_timer_get_time();
    FRESULT fr = f_mkfs(drv, &opt, work, work_sz);
    VolToPart[pdrv].pt = 0;
    ff_diskio_unregister(pdrv);
    heap_caps_free(work);
    if (fr != FR_OK) { ESP_LOGE(TAG, "SD dual format: f_mkfs failed (%d)", (int)fr); return ESP_FAIL; }
    ESP_LOGW(TAG, "SD dual format: A %u+%u, B %u+%u, %ums", (unsigned)s_a0, (unsigned)half, (unsigned)(s_a0 + half), (unsigned)half,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

void sd_guard_hourly(void)
{
    if (!s_st) return;
    uint32_t now = (uint32_t)time(NULL);
    taskENTER_CRITICAL(&s_mux);
    roll_day_locked(now);
    sdg_cnt_t c = s_st->today;
    sdg_vol_t va = s_st->vol[0], vb = s_st->vol[1];
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "SDGUARD today: writes %u slow %u (max %uus) damaged %u (sectors %u in block, %u neighbor) repaired %u failed %u | outside %u, unguarded %u",
             (unsigned)c.writes, (unsigned)c.slow, (unsigned)c.max_us, (unsigned)c.damaged, (unsigned)c.sec_written_blk,
             (unsigned)c.sec_neighbor, (unsigned)c.repaired, (unsigned)c.failed, (unsigned)c.outside, (unsigned)c.unguarded);
    ESP_LOGI(TAG, "SDDUAL %s: A fixed %u sectors/%u events, failed %u, read fallback %u, out %u | B fixed %u/%u, failed %u, fallback %u, out %u",
             s_dual ? (s_vol_bad[0] ? "B only" : s_vol_bad[1] ? "A only" : "A+B") : "single",
             (unsigned)va.fixed_sectors, (unsigned)va.fix_events, (unsigned)va.fix_failed, (unsigned)va.read_fallback, (unsigned)s_vol_bad[0],
             (unsigned)vb.fixed_sectors, (unsigned)vb.fix_events, (unsigned)vb.fix_failed, (unsigned)vb.read_fallback, (unsigned)s_vol_bad[1]);
    if (s_dirty) save_now();
}

static int cnt_json(char *b, size_t cap, const char *name, const sdg_cnt_t *c)
{
    return snprintf(b, cap, "\"%s\":{\"writes\":%u,\"slow\":%u,\"max_us\":%u,\"damaged\":%u,\"sec_block\":%u,\"sec_neighbor\":%u,"
                    "\"repaired\":%u,\"failed\":%u,\"outside\":%u,\"unguarded\":%u}", name, (unsigned)c->writes, (unsigned)c->slow,
                    (unsigned)c->max_us, (unsigned)c->damaged, (unsigned)c->sec_written_blk, (unsigned)c->sec_neighbor,
                    (unsigned)c->repaired, (unsigned)c->failed, (unsigned)c->outside, (unsigned)c->unguarded);
}

static int vol_json(char *b, size_t cap, int v, const sdg_vol_t *c)
{
    return snprintf(b, cap, "\"%c\":{\"in_use\":%s,\"fixed_sectors\":%u,\"fix_events\":%u,\"fix_failed\":%u,\"read_fallback\":%u,"
                    "\"write_failed\":%u,\"last_t\":%u,\"last_lba\":%u}", vol_ch(v), (s_dual && !s_vol_bad[v]) ? "true" : "false",
                    (unsigned)c->fixed_sectors, (unsigned)c->fix_events, (unsigned)c->fix_failed, (unsigned)c->read_fallback,
                    (unsigned)c->write_failed, (unsigned)c->last_t, (unsigned)c->last_lba);
}

int sd_guard_json(char *buf, size_t cap)
{
    if (!s_st || cap < 64) return 0;
    sdg_state_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_SPIRAM);
    if (!st) return 0;
    uint32_t now = (uint32_t)time(NULL);
    taskENTER_CRITICAL(&s_mux);
    roll_day_locked(now);
    memcpy(st, s_st, sizeof(*st));
    taskEXIT_CRITICAL(&s_mux);
    int n = snprintf(buf, cap, "\"dual\":{\"on\":%s,\"a0\":%u,\"size\":%u,", s_dual ? "true" : "false", (unsigned)s_a0, (unsigned)s_delta);
    if (n < (int)cap) n += vol_json(buf + n, cap - n, 0, &st->vol[0]);
    if (n < (int)cap) n += snprintf(buf + n, cap - n, ",");
    if (n < (int)cap) n += vol_json(buf + n, cap - n, 1, &st->vol[1]);
    if (n < (int)cap) n += snprintf(buf + n, cap - n, "},\"guard\":{\"block\":%u,\"neighbor\":%u,", SDG_BLK, SDG_NEIGH);
    if (n < (int)cap) n += cnt_json(buf + n, cap - n, "today", &st->today);
    if (n < (int)cap) n += snprintf(buf + n, cap - n, ",");
    if (n < (int)cap) n += cnt_json(buf + n, cap - n, "yesterday", &st->yesterday);
    if (n < (int)cap) n += snprintf(buf + n, cap - n, ",");
    if (n < (int)cap) n += cnt_json(buf + n, cap - n, "total", &st->total);
    if (n < (int)cap) n += snprintf(buf + n, cap - n, ",\"events_total\":%u,\"events\":[", (unsigned)st->ev_n);
    uint32_t cnt = st->ev_n < SDG_EVENTS ? st->ev_n : SDG_EVENTS;
    for (uint32_t i = 0; i < cnt && n < (int)cap - 160; i++) {
        const sdg_ev_t *e = &st->ev[(st->ev_n - 1 - i) % SDG_EVENTS];
        n += snprintf(buf + n, cap - n, "%s{\"t\":%u,\"lba\":%u,\"n\":%u,\"busy_us\":%u,\"bad_block\":%u,\"bad_neighbor\":%u,\"first\":%u,\"rewrites\":%u,\"ok\":%s}",
                      i ? "," : "", (unsigned)e->t, (unsigned)e->lba, (unsigned)e->count, (unsigned)e->write_us, (unsigned)e->bad_written_blk,
                      (unsigned)e->bad_neighbor, (unsigned)e->first_bad, (unsigned)e->rounds, e->ok ? "true" : "false");
    }
    if (n < (int)cap) n += snprintf(buf + n, cap - n, "]}");
    heap_caps_free(st);
    return (n < (int)cap) ? n : (int)cap - 1;
}
