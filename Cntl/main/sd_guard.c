/**
 * @file    sd_guard.c
 * @brief   sd_guard.h 구현 — SD 쓰기 보호(쓰기 전 내용 → 쓰기 → 다시 읽어 비교 → 다른 블록 다시 쓰기)
 */
#include "sd_guard.h"
#include "sd_health.h"
#include "fs.h"
#include "ff.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "SD";

#define SDG_BLK      16      /* 카드가 한꺼번에 다시 쓰는 단위(섹터) — 10-21·10-22 손상이 모두 16섹터(8KB) 정렬 */
#define SDG_NEIGH    2       /* 앞뒤로 함께 확인할 블록 수 — 10-21에 32섹터(2블록) 떨어진 블록까지 바뀜 */
#define SDG_ROUNDS   3       /* 다시 쓰고 확인하는 최대 횟수 */
#define SDG_SLOW_US  20000   /* 이보다 오래 붙잡은 쓰기를 "오래 붙잡음"으로 셈(평소 약 1ms, 손상 때 47~76ms) */
#define SDG_EVENTS   32
#define SDG_FILE     FS_MOUNT_POINT "/sd_guard.bin"
#define SDG_VERSION  1
#define SDG_PRE_MAX  ((SDG_NEIGH + 1) * SDG_BLK * 2)   /* 쓰기 전 내용 최대 섹터 수(앞쪽 + 뒤쪽) */

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

typedef struct __attribute__((packed)) {
    uint32_t  version;
    uint32_t  day;
    sdg_cnt_t today, yesterday, total;
    uint32_t  ev_n;
    sdg_ev_t  ev[SDG_EVENTS];
} sdg_state_t;

static sdg_state_t *s_st = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_dirty = false;
static sdmmc_card_t *s_card = NULL;
static volatile bool s_enabled = true;
static uint8_t *s_pre = NULL;    /* 쓰기 전 내용(SDG_PRE_MAX 섹터, PSRAM) */
static uint8_t *s_chk = NULL;    /* 다시 읽기·블록 다시 쓰기용 한 블록(PSRAM) */


static void save_now(void)
{
    if (!s_st) return;
    FILE *f = fopen(SDG_FILE, "wb");
    if (!f) return;
    fwrite(s_st, sizeof(*s_st), 1, f);
    fclose(f);
    s_dirty = false;
}

static void roll_day_locked(void)
{
    uint32_t now = (uint32_t)time(NULL);
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
    if (!s_st || !s_pre || !s_chk) { ESP_LOGE(TAG, "SD guard alloc failed - writes not guarded"); return; }
    FILE *f = fopen(SDG_FILE, "rb");
    if (f) {
        if (fread(s_st, sizeof(*s_st), 1, f) != 1 || s_st->version != SDG_VERSION) memset(s_st, 0, sizeof(*s_st));
        fclose(f);
    }
    s_st->version = SDG_VERSION;
}

void sd_guard_set_enabled(bool on) { s_enabled = on; }

void sd_guard_note_outside(uint32_t sectors)
{
    if (!s_st || !sectors) return;
    taskENTER_CRITICAL(&s_mux);
    roll_day_locked();
    SDG_ADD(outside, sectors);
    taskEXIT_CRITICAL(&s_mux);
    s_dirty = true;
}

/* ── FatFs 디스크 계층(원래 diskio_sdmmc와 같고 쓰기만 보호) ── */
static DSTATUS g_init(BYTE pdrv)   { (void)pdrv; return (s_card && sdmmc_get_status(s_card) == ESP_OK) ? 0 : STA_NOINIT; }
static DSTATUS g_status(BYTE pdrv) { (void)pdrv; return 0; }

static DRESULT g_read(BYTE pdrv, BYTE *buf, uint32_t sector, unsigned count)
{
    (void)pdrv;
    if (!s_card) return RES_NOTRDY;
    return sdmmc_read_sectors(s_card, buf, sector, count) == ESP_OK ? RES_OK : RES_ERROR;
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

static DRESULT g_write(BYTE pdrv, const BYTE *buf, uint32_t sector, unsigned count)
{
    (void)pdrv;
    if (!s_card) return RES_NOTRDY;
    if (!s_enabled || !s_st || !s_pre || !s_chk || count == 0) {
        return sdmmc_write_sectors(s_card, buf, sector, count) == ESP_OK ? RES_OK : RES_ERROR;
    }
    uint32_t cap = s_card->csd.capacity;
    uint32_t a0 = sector / SDG_BLK * SDG_BLK, a1 = (sector + count + SDG_BLK - 1) / SDG_BLK * SDG_BLK;
    uint32_t r0 = (a0 >= SDG_NEIGH * SDG_BLK) ? a0 - SDG_NEIGH * SDG_BLK : 0;
    uint32_t r1 = a1 + SDG_NEIGH * SDG_BLK;
    if (r1 > cap) r1 = cap;
    uint32_t head = sector - r0, tail = r1 - (sector + count);

    /* 1) 쓰기 전 내용 */
    bool pre_ok = (head + tail <= SDG_PRE_MAX) &&
                  (head == 0 || sdmmc_read_sectors(s_card, s_pre, r0, head) == ESP_OK) &&
                  (tail == 0 || sdmmc_read_sectors(s_card, s_pre + (size_t)head * 512, sector + count, tail) == ESP_OK);

    /* 2) 쓰기 */
    int64_t t0 = esp_timer_get_time();
    esp_err_t we = sdmmc_write_sectors(s_card, buf, sector, count);
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    taskENTER_CRITICAL(&s_mux);
    roll_day_locked();
    SDG_ADD(writes, 1);
    if (dt > SDG_SLOW_US) SDG_ADD(slow, 1);
    if (dt > s_st->today.max_us) s_st->today.max_us = dt;
    if (dt > s_st->total.max_us) s_st->total.max_us = dt;
    if (!pre_ok) SDG_ADD(unguarded, 1);
    taskEXIT_CRITICAL(&s_mux);
    if (we != ESP_OK) return RES_ERROR;
    if (!pre_ok) return RES_OK;


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
    if (bad_w + bad_n == 0) return RES_OK;

    /* 사건 기록 */
    taskENTER_CRITICAL(&s_mux);
    SDG_ADD(damaged, 1);
    SDG_ADD(sec_written_blk, bad_w);
    SDG_ADD(sec_neighbor, bad_n);
    if (clean) SDG_ADD(repaired, 1); else SDG_ADD(failed, 1);
    sdg_ev_t *e = &s_st->ev[s_st->ev_n % SDG_EVENTS];
    e->t = (uint32_t)time(NULL);
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
        return RES_OK;
    }
    ESP_LOGE(TAG, "SD guard: NOT repaired after %d rewrites - write %u+%u", rounds, (unsigned)sector, (unsigned)count);
    char d[40];
    snprintf(d, sizeof(d), "guard write %u+%u", (unsigned)sector, (unsigned)count);
    sd_health_report(SDH_EV_GUARD_REPAIR_FAILED, d, first_bad);
    return RES_ERROR;
}

void sd_guard_attach(sdmmc_card_t *card)
{
    static const ff_diskio_impl_t impl = { .init = g_init, .status = g_status, .read = g_read, .write = g_write, .ioctl = g_ioctl };
    BYTE pdrv = ff_diskio_get_pdrv_card(card);
    if (pdrv == 0xff) { ESP_LOGE(TAG, "SD guard: drive for card not found - writes not guarded"); return; }
    s_card = card;
    ff_diskio_register(pdrv, &impl);
    ESP_LOGI(TAG, "SD guard on (drive %u, block %u sectors, +-%u neighbor blocks)", (unsigned)pdrv, SDG_BLK, SDG_NEIGH);
}

void sd_guard_hourly(void)
{
    if (!s_st) return;
    taskENTER_CRITICAL(&s_mux);
    roll_day_locked();
    sdg_cnt_t c = s_st->today;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "SDGUARD today: writes %u slow %u (max %uus) damaged %u (sectors %u in block, %u neighbor) repaired %u failed %u | outside %u, unguarded %u",
             (unsigned)c.writes, (unsigned)c.slow, (unsigned)c.max_us, (unsigned)c.damaged, (unsigned)c.sec_written_blk,
             (unsigned)c.sec_neighbor, (unsigned)c.repaired, (unsigned)c.failed, (unsigned)c.outside, (unsigned)c.unguarded);
    if (s_dirty) save_now();
}

static int cnt_json(char *b, size_t cap, const char *name, const sdg_cnt_t *c)
{
    return snprintf(b, cap, "\"%s\":{\"writes\":%u,\"slow\":%u,\"max_us\":%u,\"damaged\":%u,\"sec_block\":%u,\"sec_neighbor\":%u,"
                    "\"repaired\":%u,\"failed\":%u,\"outside\":%u,\"unguarded\":%u}", name, (unsigned)c->writes, (unsigned)c->slow,
                    (unsigned)c->max_us, (unsigned)c->damaged, (unsigned)c->sec_written_blk, (unsigned)c->sec_neighbor,
                    (unsigned)c->repaired, (unsigned)c->failed, (unsigned)c->outside, (unsigned)c->unguarded);
}

int sd_guard_json(char *buf, size_t cap)
{
    if (!s_st || cap < 64) return 0;
    sdg_state_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_SPIRAM);
    if (!st) return 0;
    taskENTER_CRITICAL(&s_mux);
    roll_day_locked();
    memcpy(st, s_st, sizeof(*st));
    taskEXIT_CRITICAL(&s_mux);
    int n = snprintf(buf, cap, "\"guard\":{\"block\":%u,\"neighbor\":%u,", SDG_BLK, SDG_NEIGH);
    n += cnt_json(buf + n, cap - n, "today", &st->today);
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
