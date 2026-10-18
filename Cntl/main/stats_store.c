/**
 * @file    stats_store.c
 * @brief   stats_store.h 구현.
 *
 *          2026-09-26(사용자 설계 — 측정값 저장을 1주 단위 파일로) — 예전엔 원시 기록 전체를 파일
 *          하나(values_v2.bin)에, 스케일별 집계를 파일 5개(agg_*_v3.bin)에 계속 이어 붙였고,
 *          오래된 걸 지울 땐 남길 부분을 임시파일에 통째로 다시 써서 교체했음(파일이 커질수록
 *          느려지고, 다시 쓰는 도중 리셋되면 FAT이 깨질 위험 구간도 커짐). 이제:
 *            - 원시:  /sdcard/stats/rWWWWWWWW.bin  (WWWWWWWW = unix_time / 604800, 8자리)
 *            - 집계:  /sdcard/stats/aS_WWWWWWWW.bin (S = 스케일 0..4)
 *          주 경계가 604800초 배수라, 스케일별 버킷폭(60/720/1440/4320/10080초)이 전부 1주를
 *          나누어떨어져서 집계 버킷 하나가 두 주 파일에 걸칠 일이 없음. 정리는 가장 오래된 주의
 *          파일들을 지우는 것뿐(다시 쓰기 없음). 옛 파일(values*.bin, agg_*.bin)은 읽지 않음.
 *
 *          주별 레코드 수/집계 바이트는 RAM 색인(s_weeks)으로 들고 있어서 개수/사용량 조회에
 *          SD I/O가 없음(storage_mgr.h 참고). 색인은 stats_store_rescan()이 만들고, 기록/삭제/
 *          정리 때 직접 갱신함.
 *
 *          레코드는 항상 node_hub.c의 WAKE_HELLO_SENS 처리(단일 지점)에서만 추가되므로 각 주
 *          파일 안의 순서 = 도착순 = Cntl 벽시계 기준 시간순(단조증가) — "N시간 전부터" 조회는
 *          첫 주 파일에서만 이진탐색으로 시작점을 찾고 이후 주 파일은 처음부터 읽음.
 *
 *          여러 태스크가 부름(기록: CAN 소비 태스크, 읽기: LVGL, 정리/재스캔: 파일처리 태스크) —
 *          색인과 파일 조작은 전부 s_mutex 아래에서.
 *
 *          2026-10-15(SD 개선 2단계, 사용자 설계 — 블랙박스처럼 SD에 자주 쓰지 않기) — 기록 경로가 바뀜:
 *            1) 도착한 값은 RAM(PSRAM) 버퍼에만 넣음(SD·플래시 I/O 없음)
 *            2) 1분마다 버퍼를 내부 플래시(LittleFS) 일별 파일 /assets/stats/dDDDDD.bin(DDDDD = unix/86400)에 이어 씀,
 *               7일 보존(새 날 파일을 만들 때 가장 오래된 날 파일을 지움)
 *            3) 1시간마다 내부 플래시에서 아직 SD로 안 옮긴 기록을 SD 주 파일로 옮기고(옮긴 위치 = /assets/stats/sync.bin),
 *               그 사이에 닫힌 집계 칸을 내부 플래시의 기록으로 다시 계산해서 SD 집계 파일에 씀
 *          읽기(개수·페이지·기간·집계 창)는 SD + 내부 플래시(아직 안 옮긴 것) + RAM 버퍼를 이어 붙인 것으로 보여서
 *          함수 이름·결과 형식은 그대로. 집계 칸은 화면용 실시간 누적기가 닫은 칸 중 SD에 아직 없는 것을 RAM 목록에 둠.
 *          부팅하면 내부 플래시의 기록으로 실시간 누적기를 다시 채우고, 빈 SD(새 카드·포맷)를 만나면 내부 플래시의
 *          7일치를 처음부터 다시 옮김.
 */
#include "stats_store.h"
#include "memdiag.h"
#include "sd_storage.h"
#include "storage_mgr.h"
#include "sd_health.h"
#include "fs.h"
#include "rtc_sync.h"
#include "ui_log.h"
#include "esp_rom_crc.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "SENS";

#define STATS_DIR       SD_STORAGE_MOUNT_POINT "/stats"
#define STATS_WEEK_SEC  604800u
#define STATS_MAX_WEEKS 512       /* 약 10년치 — 실제 예산(카드의 10%)으로는 훨씬 전에 정리됨 */
#define STATS_CHUNK     64        /* 순차 읽기 단위(레코드 수) */

/* 2026-09-10(재설계 — fail/행/죽음 3분류 중 "fail" 처리, [[feedback_design_for_exceptions_not_just_fails]]) —
 * fopen() 실패(SD 자체 문제로 추정)와 "그냥 이 조건에 맞는 레코드가 없음"을 호출부가 구분할
 * 수 있어야, 통계탭이 "SD 이상이면 그 틱 전체를 즉시 중단"하는 회로차단기를 만들 수 있음
 * (사용자 지시: "SD 조회 fail이면, 다른 값도 믿을 수 없어. 즉시 중단이지"). 읽기 함수들
 * 각자 자기 시작 지점에서 false로 리셋하고, fopen()이 ENOENT 외의 이유로 실패하면 true */
static bool s_last_io_error = false;
static volatile bool s_write_io_error_pending = false;   /* 아래 stats_store_take_write_io_error 설명 참고 */

/* ════════════════════════════════════════════════════════════
 * 주별 색인(RAM)
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t week;       /* unix_time / STATS_WEEK_SEC */
    uint32_t raw_count;  /* 원시 레코드 수 */
    uint64_t agg_bytes;  /* 이 주 집계 파일 5개 크기 합 */
} week_info_t;

static SemaphoreHandle_t s_mutex = NULL;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static week_info_t *s_weeks = NULL;          /* PSRAM, week 오름차순 */
static uint32_t s_week_count = 0;
static uint64_t s_total_raw = 0;
static stats_record_t *s_scratch = NULL;     /* PSRAM, 순차 읽기 버퍼(스택에 큰 배열 안 둠) */
static stats_bucket_t *s_bscratch = NULL;    /* PSRAM, 집계 순차 읽기 버퍼 */

static void lock(void)
{
    if (!s_mutex) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_mutex_init_lock);
        if (!s_mutex) { s_mutex = m; m = NULL; }
        taskEXIT_CRITICAL(&s_mutex_init_lock);
        if (m) vSemaphoreDelete(m);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

/* 2026-09-28 — SD 재마운트/포맷 동안 측정값 파일 조작을 막음(기록은 열기~닫기가 전부 s_mutex 안이라,
 * 잡으면 진행 중이던 기록이 끝날 때까지 기다린 뒤 막힘). 2026-10-15 — SD 파일을 열어 둔 채 두는 일이 없어짐 */
void stats_store_io_suspend(void) { lock(); }
void stats_store_io_resume(void) { unlock(); }

/* 호출부가 lock() 잡은 상태 — 버퍼가 없으면 한 번만 할당. 실패하면 false */
static bool ensure_alloc_locked(void)
{
    if (!s_weeks) s_weeks = heap_caps_calloc(STATS_MAX_WEEKS, sizeof(week_info_t), MALLOC_CAP_SPIRAM);
    if (!s_scratch) s_scratch = heap_caps_malloc(STATS_CHUNK * sizeof(stats_record_t), MALLOC_CAP_SPIRAM);
    if (!s_bscratch) s_bscratch = heap_caps_malloc(STATS_CHUNK * sizeof(stats_bucket_t), MALLOC_CAP_SPIRAM);
    if (!s_weeks || !s_scratch || !s_bscratch) {
        ESP_LOGE(TAG, "Index/read buffer alloc failed");
        return false;
    }
    return true;
}

static void raw_path(uint32_t week, char *out, size_t out_len)
{
    snprintf(out, out_len, STATS_DIR "/r%08u.bin", (unsigned)week);
}

static void agg_path(uint8_t scale, uint32_t week, char *out, size_t out_len)
{
    snprintf(out, out_len, STATS_DIR "/a%u_%08u.bin", (unsigned)scale, (unsigned)week);
}

static int week_find_locked(uint32_t week)
{
    uint32_t lo = 0, hi = s_week_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s_weeks[mid].week < week) lo = mid + 1;
        else hi = mid;
    }
    return (lo < s_week_count && s_weeks[lo].week == week) ? (int)lo : -1;
}

/* 없으면 정렬 위치에 새로 끼움(보통은 맨 끝 = 새 주) — 꽉 찼으면 -1 */
static int week_get_or_add_locked(uint32_t week)
{
    int idx = week_find_locked(week);
    if (idx >= 0) return idx;
    if (s_week_count >= STATS_MAX_WEEKS) {
        ESP_LOGE(TAG, "Week index full (%u) - week=%u not recorded", (unsigned)STATS_MAX_WEEKS, (unsigned)week);
        return -1;
    }
    uint32_t pos = s_week_count;
    while (pos > 0 && s_weeks[pos - 1].week > week) {
        s_weeks[pos] = s_weeks[pos - 1];
        pos--;
    }
    s_weeks[pos].week = week;
    s_weeks[pos].raw_count = 0;
    s_weeks[pos].agg_bytes = 0;
    s_week_count++;
    return (int)pos;
}

static uint64_t used_bytes_locked(void)
{
    uint64_t sum = s_total_raw * sizeof(stats_record_t);
    for (uint32_t i = 0; i < s_week_count; i++) sum += s_weeks[i].agg_bytes;
    return sum;
}

/* fflush+fsync 후 닫음(SD 안전정책 — 캐시에만 있던 내용이 리셋으로 사라지는 구간을 줄임) */
static bool close_synced(FILE *f)
{
    bool ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    fclose(f);
    return ok;
}

/* 2026-10-15(SD 개선 2단계) — 예전(10-08)엔 이번 주 원시 기록 파일을 열어 둔 채 기록마다 fsync했음. 이제 SD 쓰기는
 * 1시간에 한 번 옮길 때뿐이라 그때 열고 닫음 */

/* ════════════════════════════════════════════════════════════
 * 2026-10-15(SD 개선 2단계) — 내부 플래시(LittleFS) 일별 파일 + 옮긴 위치 표시 + RAM 버퍼
 * ════════════════════════════════════════════════════════════ */
#define LFS_DIR        FS_MOUNT_POINT "/stats"
#define LFS_MARK_PATH  LFS_DIR "/sync.bin"
#define LFS_KEEP_DAYS  7          /* 사용자 결정 — 7일 보존(가장 새 날 + 앞 7일 = 날 파일 최대 8개) */
#define LFS_MAX_DAYS   16
#define DAY_SEC        86400u
#define RBUF_MAX       512        /* 1분 버퍼 — 평소 1분에 20개 안팎, 넘치면 그 자리에서 바로 내부 플래시에 씀 */
#define SYNC_CHUNK     1024       /* SD로 옮기는 한 번(잠금 한 번)의 레코드 수 — 17KB */
#define AGG_MAX_WIDTH  (604800u / STATS_AGG_POINTS_PER_SCALE)   /* 가장 넓은 칸(1주 눈금) 10080초 */
#define MARK_MAGIC     0x534D4B31u   /* "SMK1" */

typedef struct { uint32_t day; uint32_t count; } lfs_day_t;
static lfs_day_t s_lfs_days[LFS_MAX_DAYS];   /* day 오름차순 */
static int       s_lfs_nd = 0;

/* 옮긴 위치 — (day, off) = SD로 아직 안 옮긴 첫 레코드(그 날 파일의 off번째, day=0이면 처음부터).
 * agg_t = SD 집계 파일에 칸 끝 <= agg_t인 칸까지 다 씀(그보다 늦게 끝나는 칸은 아직 안 씀) */
typedef struct { uint32_t magic, day, off, agg_t, crc; } sync_mark_t;
static sync_mark_t s_mark = { MARK_MAGIC, 0, 0, 0, 0 };

static stats_record_t *s_rbuf = NULL;   /* PSRAM — 아직 내부 플래시에 안 쓴 기록 */
static uint32_t        s_rbuf_n = 0;
static stats_record_t *s_sync_buf = NULL;   /* PSRAM, SYNC_CHUNK — 내부 플래시 → SD 옮기기·다시 계산용 */

/* 시험·확인용 수치(/api/stats/sync) */
static struct {
    uint32_t lfs_writes, lfs_write_fail, lfs_pruned_unsynced;
    uint32_t syncs, sync_fail, synced_recs, synced_buckets, last_sync_unix, last_sync_ms;
    uint32_t disp_dropped, restores;
} s_sy;
static volatile bool s_sync_requested = false;
static uint32_t s_last_sync_hour = 0;
static SemaphoreHandle_t s_sync_mtx = NULL;   /* 2026-10-15(5단계) — 매시 옮기기 ↔ 복원(stats_store_init에서 만듦) */

static uint32_t mark_crc(const sync_mark_t *m)
{
    return (m->magic * 31u + m->day) * 31u + m->off * 7u + m->agg_t * 13u + 0x5A5Au;
}

static void lfs_day_path(uint32_t day, char *out, size_t n)
{
    snprintf(out, n, LFS_DIR "/d%05u.bin", (unsigned)day);
}

/* lock 잡은 상태 — 위치 표시를 내부 플래시에 저장(LittleFS는 닫을 때 통째로 바뀌어 반쯤 쓰인 상태가 없음) */
static void mark_save_locked(void)
{
    s_mark.magic = MARK_MAGIC;
    s_mark.crc = mark_crc(&s_mark);
    FILE *f = fopen(LFS_MARK_PATH, "wb");
    bool ok = f && fwrite(&s_mark, sizeof(s_mark), 1, f) == 1;
    if (f && fclose(f) != 0) ok = false;
    if (!ok) ESP_LOGE(TAG, "Sync mark save failed (errno=%d)", errno);
}

/* SD로 아직 안 옮긴 내부 플래시 레코드 수 */
static uint32_t lfs_unsynced_locked(void)
{
    uint32_t n = 0;
    for (int i = 0; i < s_lfs_nd; i++) {
        if (s_lfs_days[i].day < s_mark.day) continue;
        if (s_lfs_days[i].day == s_mark.day) n += (s_lfs_days[i].count > s_mark.off) ? s_lfs_days[i].count - s_mark.off : 0;
        else n += s_lfs_days[i].count;
    }
    return n;
}

/* 아직 안 옮긴 것 중 u번째 → (날 파일 인덱스, 파일 안 위치). 없으면 false */
static bool lfs_locate_unsynced(uint32_t u, int *out_i, uint32_t *out_off)
{
    for (int i = 0; i < s_lfs_nd; i++) {
        if (s_lfs_days[i].day < s_mark.day) continue;
        uint32_t from = (s_lfs_days[i].day == s_mark.day) ? s_mark.off : 0;
        uint32_t avail = (s_lfs_days[i].count > from) ? s_lfs_days[i].count - from : 0;
        if (u < avail) { *out_i = i; *out_off = from + u; return true; }
        u -= avail;
    }
    return false;
}

/* 날 파일 i의 off번째부터 n개를 out에 읽음. 읽은 개수 반환 */
static uint32_t lfs_read_at(int i, uint32_t off, stats_record_t *out, uint32_t n)
{
    char path[40];
    lfs_day_path(s_lfs_days[i].day, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "Flash stats open failed (errno=%d): %s", errno, path); return 0; }
    uint32_t got = 0;
    if (fseek(f, (long)off * (long)sizeof(stats_record_t), SEEK_SET) == 0) got = (uint32_t)fread(out, sizeof(stats_record_t), n, f);
    fclose(f);
    return got;
}

/* 아직 안 옮긴 것 중 [u, u+n)을 out에 — 여러 날 파일에 걸치면 이어서. 읽은 개수 반환 */
static uint32_t lfs_read_unsynced_locked(uint32_t u, uint32_t n, stats_record_t *out)
{
    uint32_t got = 0;
    int i; uint32_t off;
    if (!lfs_locate_unsynced(u, &i, &off)) return 0;
    while (got < n && i < s_lfs_nd) {
        uint32_t avail = (s_lfs_days[i].count > off) ? s_lfs_days[i].count - off : 0;
        uint32_t k = (n - got < avail) ? n - got : avail;
        if (k > 0) {
            uint32_t r = lfs_read_at(i, off, out + got, k);
            got += r;
            if (r != k) break;
        }
        i++;
        off = 0;
    }
    return got;
}

/* lock 잡은 상태 — 가장 새 날보다 LFS_KEEP_DAYS일 넘게 오래된 날 파일을 지움. SD로 못 옮긴 채 지워지면 수를 남김 */
static void lfs_prune_locked(void)
{
    if (s_lfs_nd == 0) return;
    uint32_t newest = s_lfs_days[s_lfs_nd - 1].day;
    while (s_lfs_nd > 0 && s_lfs_days[0].day + LFS_KEEP_DAYS < newest) {
        char path[40];
        lfs_day_path(s_lfs_days[0].day, path, sizeof(path));
        if (s_lfs_days[0].day >= s_mark.day) {
            uint32_t lost = (s_lfs_days[0].day == s_mark.day && s_lfs_days[0].count > s_mark.off) ? s_lfs_days[0].count - s_mark.off
                          : (s_lfs_days[0].day == s_mark.day) ? 0 : s_lfs_days[0].count;
            if (lost > 0) {
                s_sy.lfs_pruned_unsynced += lost;
                ESP_LOGW(TAG, "Flash stats day %u removed before reaching SD - %u values only there were lost",
                         (unsigned)s_lfs_days[0].day, (unsigned)lost);
            }
        }
        if (unlink(path) != 0 && errno != ENOENT) ESP_LOGW(TAG, "Flash stats delete failed (errno=%d): %s", errno, path);
        for (int k = 1; k < s_lfs_nd; k++) s_lfs_days[k - 1] = s_lfs_days[k];
        s_lfs_nd--;
        if (s_lfs_nd > 0 && s_mark.day < s_lfs_days[0].day) { s_mark.day = s_lfs_days[0].day; s_mark.off = 0; mark_save_locked(); }
    }
}

/* lock 잡은 상태 — RAM 버퍼를 내부 플래시 날 파일에 이어 씀. 날은 기록 시각 기준이되 뒤로 가지 않음(시계가 뒤로
 * 가도 파일 순서 = 도착 순서). 실패하면 버퍼를 그대로 두고 다음 분에 다시 */
static void lfs_flush_locked(void)
{
    uint32_t i = 0;
    while (i < s_rbuf_n) {
        uint32_t newest = s_lfs_nd ? s_lfs_days[s_lfs_nd - 1].day : 0;
        uint32_t day = s_rbuf[i].unix_time / DAY_SEC;
        if (day < newest) day = newest;
        uint32_t j = i + 1;
        while (j < s_rbuf_n) {
            uint32_t d = s_rbuf[j].unix_time / DAY_SEC;
            if (d > day) break;
            j++;
        }
        if (s_lfs_nd == 0 || day != newest) {
            if (s_lfs_nd >= LFS_MAX_DAYS) lfs_prune_locked();
            if (s_lfs_nd >= LFS_MAX_DAYS) {   /* 날이 크게 건너뛰어도 지울 게 없을 수는 없지만 안전하게 */
                for (int k = 1; k < s_lfs_nd; k++) s_lfs_days[k - 1] = s_lfs_days[k];
                s_lfs_nd--;
            }
            s_lfs_days[s_lfs_nd].day = day;
            s_lfs_days[s_lfs_nd].count = 0;
            s_lfs_nd++;
        }
        char path[40];
        lfs_day_path(day, path, sizeof(path));
        FILE *f = fopen(path, "ab");
        size_t w = f ? fwrite(&s_rbuf[i], sizeof(stats_record_t), j - i, f) : 0;
        bool ok = f && w == j - i;
        if (f && fclose(f) != 0) ok = false;
        if (!ok) {
            /* 실제로 몇 개가 들어갔는지는 파일 크기로 다시 셈(색인 수와 파일이 어긋나면 읽는 위치가 틀어짐) */
            s_sy.lfs_write_fail++;
            int saved_errno = errno;
            uint32_t c0 = s_lfs_days[s_lfs_nd - 1].count, c1 = c0;
            struct stat st;
            if (stat(path, &st) == 0 && st.st_size >= 0) {
                c1 = (uint32_t)((uint64_t)st.st_size / sizeof(stats_record_t));
                if ((uint64_t)c1 * sizeof(stats_record_t) != (uint64_t)st.st_size) truncate(path, (off_t)c1 * (off_t)sizeof(stats_record_t));
            }
            uint32_t in = (c1 > c0) ? c1 - c0 : 0;
            if (in > j - i) in = j - i;
            s_lfs_days[s_lfs_nd - 1].count = c0 + in;
            ESP_LOGE(TAG, "Flash stats write failed (errno=%d, %u/%u kept): %s", saved_errno, (unsigned)in, (unsigned)(j - i), path);
            if (s_lfs_days[s_lfs_nd - 1].count == 0) s_lfs_nd--;   /* 새로 만들려던 날 — 다음에 다시 */
            i += in;
            break;   /* 남은 것은 버퍼에 두고 다음 분에 다시 */
        }
        s_lfs_days[s_lfs_nd - 1].count += (uint32_t)w;
        s_sy.lfs_writes++;
        i = j;
    }
    if (i > 0) {
        memmove(s_rbuf, s_rbuf + i, (s_rbuf_n - i) * sizeof(stats_record_t));
        s_rbuf_n -= i;
        lfs_prune_locked();
    }
}

/* ════════════════════════════════════════════════════════════
 * 그래프용 스케일별 사전집계 — [[project_cntl_stats_graph_redesign_2026_09_10]]
 * ════════════════════════════════════════════════════════════ */
const uint32_t STATS_SCALE_SECONDS[STATS_SCALE_COUNT] = { 3600, 43200, 86400, 259200, 604800 };

/* chan_type은 sensor_channel_type_t(esp_now_link.h)인데 이 파일은 저수준이라 그 헤더에
 * 의존 안 함 — 그냥 작은 고정크기 배열로 충분(현재 SENSOR_CHAN_TYPE_COUNT=5, 여유있게 8) */
#define STATS_AGG_MAX_CHAN_TYPES 8

/* NODE_HUB_MAX_NODES(node_hub.h)와 같은 값이지만 이 파일은 저수준이라 그 헤더에
 * 의존 안 함(위 chan_type과 동일 원칙) — 실제 페어링 가능한 노드 수 이상은 어차피 안 옴 */
#define STATS_AGG_MAX_MACS 8

/* 각 (스케일, chan_type, mac)마다 "지금 채워지는 중인" 버킷 하나만 RAM에 유지 */
typedef struct {
    uint32_t bucket_start;   /* 0 = 아직 시작 안 함 */
    bool     have_value;     /* 이 버킷에 후보값이 하나라도 들어왔는지 */
    uint32_t best_dist_abs;  /* 지금까지의 최적 후보가 버킷 중앙에서 떨어진 거리(초, 절대값) */
    float    best_value;     /* 버킷 중앙시각에 가장 가까운 실측값(합성/평균 없음) */
    uint16_t seen_count;     /* 이 버킷에 실제로 들어온 실측값 개수(진단용, sample_count로 기록) */
} agg_accum_t;

/* 2026-09-30(사용자 지시 — PSRAM 원칙) — [스케일][chan_type][기기 슬롯], 약 6.4KB, PSRAM */
#define AGG_ACCUM_BYTES (sizeof(agg_accum_t) * STATS_SCALE_COUNT * STATS_AGG_MAX_CHAN_TYPES * STATS_AGG_MAX_MACS)

typedef struct { uint8_t scale; uint32_t week; stats_bucket_t rec; } agg_pend_t;

/* 2026-10-15(SD 개선 2단계) — 누적기를 둘로: 화면용 실시간(s_live — 닫힌 칸을 RAM 목록에, SD 안 씀)과 옮기기용
 * (s_regen — 1시간마다 내부 플래시 기록으로 처음부터 다시 계산해서 SD에 씀). 같은 기록·같은 계산이라 결과가 같음 */
typedef struct agg_state agg_state_t;
struct agg_state {
    agg_accum_t (*acc)[STATS_AGG_MAX_CHAN_TYPES][STATS_AGG_MAX_MACS];   /* PSRAM, 처음 쓸 때 */
    uint8_t  macs[STATS_AGG_MAX_MACS][6];
    uint8_t  slot_kind[STATS_AGG_MAX_MACS];      /* 슬롯을 넘겨줄 때 남은 버킷을 기록하는 데 씀 */
    uint32_t slot_last[STATS_AGG_MAX_MACS];      /* 이 슬롯에 마지막으로 값이 들어온 시각 */
    int      n;
    void   (*emit)(uint8_t scale, const stats_bucket_t *rec);   /* 닫힌 칸 하나 */
};
static void live_emit(uint8_t scale, const stats_bucket_t *rec);
static void regen_emit(uint8_t scale, const stats_bucket_t *rec);
static agg_state_t s_live  = { .emit = live_emit };
static agg_state_t s_regen = { .emit = regen_emit };

static uint32_t scale_width(uint8_t sc)
{
    uint32_t w = STATS_SCALE_SECONDS[sc] / STATS_AGG_POINTS_PER_SCALE;
    return w ? w : 1;
}

static void agg_state_reset(agg_state_t *st)
{
    if (st->acc) memset(st->acc, 0, AGG_ACCUM_BYTES);
    memset(st->macs, 0, sizeof(st->macs));
    memset(st->slot_kind, 0, sizeof(st->slot_kind));
    memset(st->slot_last, 0, sizeof(st->slot_last));
    st->n = 0;
}

/* 화면용 닫힌 칸 목록 — SD 집계 파일에 아직 없는 칸(칸 끝 > s_mark.agg_t). 1시간마다 옮긴 뒤 SD에 들어간 칸은 뺌.
 * SD가 오래 고장이면 늘어남 — 꽉 차면 가장 오래 전에 닫힌 것부터 버림(화면만 영향, SD에는 내부 플래시로 다시 계산해 씀) */
#define DISP_MAX 8192
static agg_pend_t *s_disp = NULL;   /* PSRAM, 처음 쓸 때(약 230KB) */
static uint32_t    s_disp_n = 0;

/* 옮기기용 다시 계산 결과를 SD에 쓰기 전에 모으는 곳 — 차면 그때그때 씀 */
#define REGEN_MAX 2048
static agg_pend_t *s_regen_buf = NULL;
static uint32_t    s_regen_n = 0;
static uint32_t    s_regen_lo = 0, s_regen_hi = 0;   /* 이번에 쓸 칸: lo < 칸 끝 <= hi */
static uint32_t    s_regen_written = 0;
static uint32_t    s_restore_gen = 0, s_regen_gen = 0;   /* 지우기·새 카드 복원마다 +1 — 진행 중인 다시 계산을 버리게 */

/* 2026-10-14(SD 개선 1단계) — 방금 쓴 끝부분(end-len ~ end)을 새 핸들로 다시 읽어 비교. 1 같음, 0 다름, -1 확인 못 함 */
static int verify_tail(const char *path, long end, const void *data, size_t len)
{
    if (len == 0 || end < (long)len) return -1;
    FILE *v = fopen(path, "rb");
    if (!v) return (errno == EMFILE || errno == ENFILE) ? -1 : 0;
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(v); return -1; }
    bool ok = fseek(v, end - (long)len, SEEK_SET) == 0 && fread(buf, 1, len, v) == len && memcmp(buf, data, len) == 0;
    fclose(v);
    heap_caps_free(buf);
    if (!ok) ESP_LOGE(TAG, "Verify failed: %s (%u bytes ending at %ld)", path, (unsigned)len, end);
    return ok ? 1 : 0;
}

static stats_bucket_t *s_agg_vbuf = NULL;   /* 2026-10-14 — 집계 쓰기 확인용(PSRAM, REGEN_MAX칸) */

/* lock 잡은 상태 — 칸들을 같은 (눈금, 주) 파일끼리 한 번만 열어 SD에 씀(2026-10-08 방식) + 쓴 끝부분 확인. 쓴 칸 수 반환 */
static uint32_t agg_write_sd_locked(agg_pend_t *pend, uint32_t n)
{
    if (!sd_storage_is_mounted() || !sd_health_writes_allowed()) return 0;   /* 2026-10-14 — SD 고장 상태면 쓰지 않음 */
    if (!s_agg_vbuf) s_agg_vbuf = heap_caps_malloc(sizeof(stats_bucket_t) * REGEN_MAX, MALLOC_CAP_SPIRAM);
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (pend[i].scale == 0xFF) continue;   /* 이미 같은 파일 묶음으로 씀 */
        uint8_t sc = pend[i].scale;
        uint32_t week = pend[i].week;
        int widx = week_get_or_add_locked(week);
        char path[48];
        agg_path(sc, week, path, sizeof(path));
        FILE *f = (widx >= 0) ? fopen(path, "ab") : NULL;
        if (!f) {
            ESP_LOGW(TAG, "Aggregate file open failed (scale=%u, errno=%d)", (unsigned)sc, errno);
            s_write_io_error_pending = true;
        }
        long start = -1;
        if (f && fseek(f, 0, SEEK_END) == 0) start = ftell(f);
        uint32_t nv = 0;
        for (uint32_t j = i; j < n; j++) {
            if (pend[j].scale != sc || pend[j].week != week) continue;
            if (f && fwrite(&pend[j].rec, sizeof(stats_bucket_t), 1, f) == 1) {
                s_weeks[widx].agg_bytes += sizeof(stats_bucket_t);
                if (s_agg_vbuf && nv < REGEN_MAX) s_agg_vbuf[nv] = pend[j].rec;
                nv++;
            }
            pend[j].scale = 0xFF;
        }
        total += nv;
        if (f && close_synced(f) && nv > 0) {
            size_t nb = nv * sizeof(stats_bucket_t);
            sd_health_count(SDH_C_FSYNC, 1);
            sd_health_count(SDH_C_BYTES, (uint32_t)nb);
            if (s_agg_vbuf && start >= 0 && nv <= REGEN_MAX) {
                int v = verify_tail(path, start + (long)nb, s_agg_vbuf, nb);
                if (v == 0) sd_health_report(SDH_EV_VERIFY_AGG, path, (uint32_t)start);
                else if (v > 0) sd_health_count(SDH_C_VERIFY_OK, 1);
            }
        }
    }
    return total;
}

/* 닫힌 칸 하나를 그 누적기의 받는 곳으로 */
static void agg_flush_bucket(agg_state_t *st, uint8_t scale_idx, uint8_t kind, uint8_t chan_type, const uint8_t mac[6],
                             const agg_accum_t *acc)
{
    if (!acc->have_value) return;
    stats_bucket_t rec = {
        .bucket_start_unix = acc->bucket_start,
        .kind               = kind,
        .chan_type          = chan_type,
        .sample_count       = (acc->seen_count > 255) ? 255 : (uint8_t)acc->seen_count,
        .avg_value          = acc->best_value,
    };
    memcpy(rec.mac, mac, 6);
    st->emit(scale_idx, &rec);
}

/* lock 잡은 상태 — 화면용: SD에 아직 없는 칸만 목록에 */
static void live_emit(uint8_t scale, const stats_bucket_t *rec)
{
    if (rec->bucket_start_unix + scale_width(scale) <= s_mark.agg_t) return;   /* 이미 SD 집계 파일에 있음 */
    if (!s_disp) s_disp = heap_caps_malloc(sizeof(agg_pend_t) * DISP_MAX, MALLOC_CAP_SPIRAM);
    if (!s_disp) return;
    if (s_disp_n >= DISP_MAX) {
        const uint32_t drop = DISP_MAX / 8;
        memmove(s_disp, s_disp + drop, (s_disp_n - drop) * sizeof(agg_pend_t));
        s_disp_n -= drop;
        s_sy.disp_dropped += drop;
        ESP_LOGW(TAG, "Pending aggregate list full - oldest %u dropped from display (SD gets them from flash)", (unsigned)drop);
    }
    s_disp[s_disp_n].scale = scale;
    s_disp[s_disp_n].week  = rec->bucket_start_unix / STATS_WEEK_SEC;
    s_disp[s_disp_n].rec   = *rec;
    s_disp_n++;
}

/* 옮기기용: (lo, hi]에 끝나는 칸만 모았다가 차면 SD에 씀(stats_wr 태스크 안, 잠금은 쓸 때만) */
static void regen_flush(void)
{
    if (s_regen_n == 0) return;
    lock();
    if (s_regen_gen == s_restore_gen) s_regen_written += agg_write_sd_locked(s_regen_buf, s_regen_n);
    unlock();
    s_regen_n = 0;
}

static void regen_emit(uint8_t scale, const stats_bucket_t *rec)
{
    uint32_t end = rec->bucket_start_unix + scale_width(scale);
    if (end <= s_regen_lo || end > s_regen_hi || !s_regen_buf) return;
    if (s_regen_n >= REGEN_MAX) regen_flush();
    s_regen_buf[s_regen_n].scale = scale;
    s_regen_buf[s_regen_n].week  = rec->bucket_start_unix / STATS_WEEK_SEC;
    s_regen_buf[s_regen_n].rec   = *rec;
    s_regen_n++;
}

/* mac -> 누적 슬롯 인덱스. 처음 보는 mac이면 새로 등록.
 * 2026-09-28(사용자 설계 — 기기 교체) — 꽉 차면 가장 오래 값이 안 들어온 슬롯을 넘겨받음 — 동시에
 * 붙는 기기는 페어링 상한으로 8대를 넘지 않으므로 그 슬롯의 기기는 이미 떠난 기기. 넘기기 전에 그 기기의 진행
 * 중이던 칸을 내보냄(떠난 기기의 마지막 칸은 다음 값이 영영 안 와서 원래도 닫힐 일이 없었음) */
static int agg_mac_slot(agg_state_t *st, const uint8_t mac[6], uint32_t unix_time)
{
    for (int i = 0; i < st->n; i++) {
        if (memcmp(st->macs[i], mac, 6) == 0) return i;
    }
    int slot;
    if (st->n < STATS_AGG_MAX_MACS) {
        slot = st->n++;
    } else {
        slot = 0;
        for (int i = 1; i < STATS_AGG_MAX_MACS; i++) {
            if (st->slot_last[i] < st->slot_last[slot]) slot = i;
        }
        for (int sc = 0; sc < STATS_SCALE_COUNT; sc++) {
            for (int ch = 0; ch < STATS_AGG_MAX_CHAN_TYPES; ch++) {
                agg_flush_bucket(st, (uint8_t)sc, st->slot_kind[slot], (uint8_t)ch, st->macs[slot], &st->acc[sc][ch][slot]);
                memset(&st->acc[sc][ch][slot], 0, sizeof(agg_accum_t));
            }
        }
        if (st == &s_live) ESP_LOGI(TAG, "Aggregate slot of %02X%02X handed to %02X%02X", st->macs[slot][4],
                                    st->macs[slot][5], mac[4], mac[5]);
    }
    memcpy(st->macs[slot], mac, 6);
    st->slot_last[slot] = unix_time;
    return slot;
}

/* 2026-09-15(사용자 설계 — 그래프 결측/보간 대화, "평균 내지 않는다") — 버킷 구간에 들어온
 * 실측값들 중 "버킷 중앙시각에 가장 가까운 값" 하나를 그대로(합성 없이) 저장. 좁은 스케일의
 * 값이 넓은 스케일보다 더 극단적일 수 없다는 부등식이 자동으로 지켜지고, "마지막 값"처럼
 * 버킷 후반부로 편향되지도 않음 */
static void agg_update_scale(agg_state_t *st, uint8_t scale, int mac_slot, const uint8_t mac[6], uint8_t kind,
                             uint8_t chan_type, uint32_t unix_time, float value)
{
    uint32_t bucket_width = scale_width(scale);
    uint32_t bucket_start = (unix_time / bucket_width) * bucket_width;  /* 벽시계 정렬 */

    agg_accum_t *acc = &st->acc[scale][chan_type][mac_slot];
    if (acc->have_value && acc->bucket_start != bucket_start) {
        agg_flush_bucket(st, scale, kind, chan_type, mac, acc);
        acc->have_value  = false;
        acc->seen_count  = 0;
    }
    acc->bucket_start = bucket_start;

    uint32_t center = bucket_start + bucket_width / 2;
    uint32_t dist = (unix_time > center) ? (unix_time - center) : (center - unix_time);
    acc->seen_count++;
    if (!acc->have_value || dist < acc->best_dist_abs) {
        acc->have_value    = true;
        acc->best_dist_abs = dist;
        acc->best_value    = value;
    }
}

/* 칸이 끝났으면(now 기준) 다음 측정을 기다리지 않고 내보냄(2026-09-30 사용자 결정). 값이 없는 칸은 안 내보냄 */
static void agg_flush_ended(agg_state_t *st, uint32_t now_unix)
{
    if (!st->acc) return;
    for (int sc = 0; sc < STATS_SCALE_COUNT; sc++) {
        uint32_t width = scale_width((uint8_t)sc);
        for (int ch = 0; ch < STATS_AGG_MAX_CHAN_TYPES; ch++) {
            for (int slot = 0; slot < st->n; slot++) {
                agg_accum_t *acc = &st->acc[sc][ch][slot];
                if (!acc->have_value || acc->bucket_start + width > now_unix) continue;
                agg_flush_bucket(st, (uint8_t)sc, st->slot_kind[slot], (uint8_t)ch, st->macs[slot], acc);
                acc->have_value = false;
                acc->seen_count = 0;
            }
        }
    }
}

/* 2026-09-30 — 통계 화면이 그리기 직전에 부름. 2026-10-15(SD 개선 2단계) — 이제 RAM 목록에만 넣음(SD I/O 없음) */
void stats_store_flush_ended_buckets(uint32_t now_unix)
{
    if (now_unix == 0) return;
    lock();
    agg_flush_ended(&s_live, now_unix);
    unlock();
}

static void agg_update(agg_state_t *st, const stats_record_t *r)
{
    if (r->chan_type >= STATS_AGG_MAX_CHAN_TYPES) return;
    if (!st->acc) {
        st->acc = heap_caps_calloc(1, AGG_ACCUM_BYTES, MALLOC_CAP_SPIRAM);
        if (!st->acc) {
            ESP_LOGE(TAG, "Aggregate accumulator alloc failed (%u bytes)", (unsigned)AGG_ACCUM_BYTES);
            return;
        }
    }
    int mac_slot = agg_mac_slot(st, r->mac, r->unix_time);
    st->slot_kind[mac_slot] = r->kind;
    if (r->unix_time > st->slot_last[mac_slot]) st->slot_last[mac_slot] = r->unix_time;
    for (int scale = 0; scale < STATS_SCALE_COUNT; scale++) {
        agg_update_scale(st, (uint8_t)scale, mac_slot, r->mac, r->kind, r->chan_type, r->unix_time, r->value);
    }
}

/* fopen 실패 처리 공통 — ENOENT(아직 없음)는 "데이터 없음"이지 SD 고장이 아님 */
static FILE *open_read(const char *path)
{
    errno = 0;
    FILE *f = fopen(path, "rb");
    if (!f && errno != ENOENT) s_last_io_error = true;
    return f;
}

static bool agg_read_bucket_at(FILE *f, uint32_t idx, stats_bucket_t *out)
{
    if (fseek(f, (long)idx * (long)sizeof(stats_bucket_t), SEEK_SET) != 0) return false;
    return fread(out, sizeof(*out), 1, f) == 1;
}

uint32_t stats_agg_read_window(uint8_t scale_idx, uint32_t window_start_unix, uint32_t window_end_unix,
                                stats_bucket_t *out, uint32_t out_cap)
{
    s_last_io_error = false;
    if (scale_idx >= STATS_SCALE_COUNT || out_cap == 0 || window_end_unix <= window_start_unix) return 0;
    bool sd = sd_storage_is_mounted();  /* 미마운트면 SD 쪽은 시도 자체 스킵(2026-10-15 — 아직 안 옮긴 칸은 그래도 보임) */

    lock();
    if (!ensure_alloc_locked()) { unlock(); return 0; }
    uint32_t w0 = window_start_unix / STATS_WEEK_SEC;
    uint32_t w1 = (window_end_unix - 1) / STATS_WEEK_SEC;
    uint32_t picked = 0;
    bool bad_reported = false;   /* 2026-10-14 */
    for (uint32_t i = 0; sd && i < s_week_count && picked < out_cap; i++) {
        uint32_t week = s_weeks[i].week;
        if (week < w0) continue;
        if (week > w1) break;
        if (s_weeks[i].agg_bytes == 0) continue;

        char path[48];
        agg_path(scale_idx, week, path, sizeof(path));
        FILE *f = open_read(path);
        if (!f) { if (s_last_io_error) break; continue; }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        uint32_t total = (fsize > 0) ? (uint32_t)(fsize / (long)sizeof(stats_bucket_t)) : 0;

        /* 이진탐색 — bucket_start_unix >= window_start_unix인 첫 레코드(한 주 파일 안은 그 스케일의
         * 버킷이 닫힌 순서 = 시간순). 첫 주가 아니면 결과가 0이라 처음부터 읽음 */
        uint32_t lo = 0, hi = total;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            stats_bucket_t rec;
            if (!agg_read_bucket_at(f, mid, &rec)) { s_last_io_error = true; break; }
            if (rec.bucket_start_unix < window_start_unix) lo = mid + 1;
            else hi = mid;
        }
        if (s_last_io_error) { fclose(f); break; }

        fseek(f, (long)lo * (long)sizeof(stats_bucket_t), SEEK_SET);
        bool done = false;
        size_t got;
        while (!done && picked < out_cap && (got = fread(s_bscratch, sizeof(stats_bucket_t), STATS_CHUNK, f)) > 0) {
            for (size_t k = 0; k < got && picked < out_cap; k++) {
                if (!bad_reported && s_bscratch[k].bucket_start_unix / STATS_WEEK_SEC != week) {
                    bad_reported = true;   /* 한 번 읽을 때 한 번만 */
                    /* 2026-10-14(SD 개선 1단계) — 이 주 파일에 다른 주(0xFF·쓰레기 포함) 칸 = 파일 손상. 읽기 방식은 그대로 둠 */
                    sd_health_report(SDH_EV_BAD_AGG_RECORD, path, (uint32_t)(ftell(f) / (long)sizeof(stats_bucket_t)));
                }
                if (s_bscratch[k].bucket_start_unix >= window_end_unix) { done = true; break; }
                out[picked++] = s_bscratch[k];
            }
        }
        fclose(f);
        if (done) break;
    }
    /* 2026-10-15(SD 개선 2단계) — SD 집계 파일에 아직 없는 닫힌 칸(RAM 목록, 닫힌 순서 = 계열마다 시간순).
     * SD 읽기가 실패했으면 이번 결과는 버려지므로(회로차단기) 붙이지 않음 */
    for (uint32_t k = 0; !s_last_io_error && k < s_disp_n && picked < out_cap; k++) {
        const agg_pend_t *e = &s_disp[k];
        if (e->scale != scale_idx || e->rec.bucket_start_unix < window_start_unix || e->rec.bucket_start_unix >= window_end_unix) continue;
        out[picked++] = e->rec;
    }
    unlock();
    return picked;
}

bool stats_store_had_io_error(void)
{
    return s_last_io_error;
}

/* 2026-09-07(임시 진단 — 사용자 지시: "메모리 누수... 이유를 찾아야겠어") — 내부RAM이
 * 측정 성공 사이클마다 서서히 준다는 관찰을 이 함수(SD fopen/fwrite/fclose 반복)로 좁혀서
 * 실측 검증. 원인 확정되면 이 로그는 제거 예정 */
static size_t s_stats_append_call_count = 0;
static stats_store_appended_cb_t s_appended_cb = NULL;
static volatile uint32_t s_append_seq;  /* 아래 정의(2026-10-09) */

void stats_store_set_appended_cb(stats_store_appended_cb_t cb) { s_appended_cb = cb; }

/* 2026-09-11(SD 신뢰성 재설계 항목4) — WAKE_HELLO_SENS 처리(LVGL 태스크가 아님)에서 쓰기실패가
 * 나면 여기만 세팅. ui_main.c의 1초 주기 LVGL 타이머가 매 틱 stats_store_take_write_io_error()로
 * test-and-clear해서 안전하게 가져감(LVGL API는 항상 LVGL 태스크에서만 호출) */
/* s_write_io_error_pending — 위(파일 앞쪽)로 옮김 */

bool stats_store_take_write_io_error(void)
{
    bool v = s_write_io_error_pending;
    s_write_io_error_pending = false;
    return v;
}

/* 2026-10-15(SD 개선 2단계) — 기록은 RAM 버퍼에 넣고 화면용 집계 누적기만 갱신(SD·플래시 I/O 없음).
 * 내부 플래시엔 1분마다(stats_store_init이 건 타이머 → 같은 기록 태스크), SD엔 1시간마다 옮김 */
bool stats_store_append_batch(const stats_record_t *records, uint32_t count)
{
    s_last_io_error = false;
    if (count == 0 || !records) return true;
    lock();
    if (!s_rbuf) s_rbuf = heap_caps_malloc(sizeof(stats_record_t) * RBUF_MAX, MALLOC_CAP_SPIRAM);
    if (!s_rbuf) { unlock(); ESP_LOGE(TAG, "Value buffer alloc failed - %u values lost", (unsigned)count); return false; }
    for (uint32_t i = 0; i < count; i++) {
        if (s_rbuf_n >= RBUF_MAX) lfs_flush_locked();   /* 1분 안에 넘치면 바로 */
        if (s_rbuf_n >= RBUF_MAX) {
            ESP_LOGE(TAG, "Value buffer full and flash write failing - %u values lost", (unsigned)(count - i));
            break;
        }
        s_rbuf[s_rbuf_n++] = records[i];
        agg_update(&s_live, &records[i]);
    }
    unlock();

    s_append_seq++;   /* 2026-10-09 — 웹 기록 표 자동 갱신용 */
    if (s_appended_cb) s_appended_cb();  /* 잠금 밖에서 — 받는 쪽이 표를 다시 읽음 */
    s_stats_append_call_count++;
    return true;
}

/* 2026-10-09 — 기록이 늘 때마다 바뀌는 번호(잠금 없음 — 웹 주화면이 1초마다 읽음, 기록 태스크의 SD 쓰기를 기다리지 않게) */
static volatile uint32_t s_append_seq = 0;
uint32_t stats_store_get_append_seq(void) { return s_append_seq; }

/* lock 잡은 상태 — SD에 있는 레코드 수(미마운트면 0 — 읽기 위치 계산을 SD 쪽 없이) */
static uint64_t sd_count_locked(void)
{
    return sd_storage_is_mounted() ? s_total_raw : 0;
}

/* 2026-10-15 — SD + 내부 플래시(아직 안 옮긴 것) + RAM 버퍼 */
static uint64_t total_count_locked(void)
{
    return sd_count_locked() + lfs_unsynced_locked() + s_rbuf_n;
}

uint32_t stats_store_get_count(void)
{
    s_last_io_error = false;
    lock();
    uint64_t total = total_count_locked();
    unlock();
    return (total > UINT32_MAX) ? UINT32_MAX : (uint32_t)total;
}

/* 2026-10-15 — 레코드가 그 주 파일에 들어갈 수 있는 값인지(시각이 그 주, 채널 종류가 범위 안) */
static bool record_ok(const stats_record_t *r, uint32_t week)
{
    return r->unix_time / STATS_WEEK_SEC == week && r->chan_type < STATS_AGG_MAX_CHAN_TYPES;
}

void stats_store_sd_check(void)
{
    if (!sd_storage_is_mounted()) return;
    int64_t t0 = esp_timer_get_time();
    uint32_t files = 0, recs = 0, bad = 0;
    lock();
    if (!ensure_alloc_locked()) { unlock(); return; }
    uint32_t from = (s_week_count > 2) ? s_week_count - 2 : 0;
    for (uint32_t i = from; i < s_week_count && sd_storage_is_mounted(); i++) {
        uint32_t week = s_weeks[i].week;
        char path[48];
        /* 원시 */
        raw_path(week, path, sizeof(path));
        FILE *f = (s_weeks[i].raw_count > 0) ? fopen(path, "rb") : NULL;
        if (f) {
            files++;
            uint32_t idx = 0, fb = 0;
            size_t got;
            while ((got = fread(s_scratch, sizeof(stats_record_t), STATS_CHUNK, f)) > 0) {
                for (size_t k = 0; k < got; k++, idx++) {
                    if (record_ok(&s_scratch[k], week)) continue;
                    if (fb++ == 0) {
                        ESP_LOGE(TAG, "SD check: bad record %u in %s (t=%u chan=%u)", (unsigned)idx, path,
                                 (unsigned)s_scratch[k].unix_time, (unsigned)s_scratch[k].chan_type);
                        sd_health_report(SDH_EV_BAD_STATS_RECORD, path, idx);
                    }
                }
            }
            fclose(f);
            recs += idx;
            if (fb) ESP_LOGE(TAG, "SD check: %s has %u bad records of %u", path, (unsigned)fb, (unsigned)idx);
            bad += fb;
        }
        /* 집계(눈금 5개) — 칸 시작이 그 주가 아니면 손상 */
        for (uint8_t sc = 0; sc < STATS_SCALE_COUNT; sc++) {
            agg_path(sc, week, path, sizeof(path));
            f = fopen(path, "rb");
            if (!f) continue;
            files++;
            uint32_t idx = 0, fb = 0;
            size_t got;
            while ((got = fread(s_bscratch, sizeof(stats_bucket_t), STATS_CHUNK, f)) > 0) {
                for (size_t k = 0; k < got; k++, idx++) {
                    if (s_bscratch[k].bucket_start_unix / STATS_WEEK_SEC == week && s_bscratch[k].chan_type < STATS_AGG_MAX_CHAN_TYPES) continue;
                    if (fb++ == 0) {
                        ESP_LOGE(TAG, "SD check: bad bucket %u in %s (t=%u)", (unsigned)idx, path, (unsigned)s_bscratch[k].bucket_start_unix);
                        sd_health_report(SDH_EV_BAD_AGG_RECORD, path, idx);
                    }
                }
            }
            fclose(f);
            recs += idx;
            bad += fb;
        }
    }
    unlock();
    ESP_LOGI(TAG, "SD check: %u files, %u records, %u bad, %ums", (unsigned)files, (unsigned)recs, (unsigned)bad,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
}

void stats_store_probe_io(void)
{
    s_last_io_error = false;
    if (!sd_storage_is_mounted()) return;
    errno = 0;
    DIR *d = opendir(STATS_DIR);
    if (!d) {
        if (errno != ENOENT) s_last_io_error = true;
        return;
    }
    closedir(d);
}

/* lock 잡은 상태 — SD 주 파일들을 이어 붙인 순서로 [start_record, end_record)를 out에. 읽은 개수 반환 */
static uint32_t sd_read_range_locked(uint64_t start_record, uint64_t end_record, stats_record_t *out)
{
    uint64_t cum = 0;
    uint32_t got = 0;
    for (uint32_t i = 0; i < s_week_count && cum < end_record; i++) {
        uint64_t w_start = cum, w_end = cum + s_weeks[i].raw_count;
        cum = w_end;
        if (w_end <= start_record || s_weeks[i].raw_count == 0) continue;
        uint64_t from = (start_record > w_start) ? start_record : w_start;
        uint64_t to = (end_record < w_end) ? end_record : w_end;

        char path[48];
        raw_path(s_weeks[i].week, path, sizeof(path));
        FILE *f = open_read(path);
        if (!f) { s_last_io_error = true; break; }
        fseek(f, (long)((from - w_start) * sizeof(stats_record_t)), SEEK_SET);
        size_t n = fread(&out[got], sizeof(stats_record_t), (size_t)(to - from), f);
        fclose(f);
        /* 2026-10-15(MPIO 손상 — 표에 시각 1495542756·채널 249 같은 쓰레기가 그대로 보였는데 보고 없었음) — 그 주에 속하지
         * 않는 레코드면 SD 이상(읽기 한 번에 한 번만). 보여 주는 방식은 그대로 */
        for (size_t k = 0; k < n; k++) {
            if (record_ok(&out[got + k], s_weeks[i].week)) continue;
            sd_health_report(SDH_EV_BAD_STATS_RECORD, path, (uint32_t)(from - w_start + k));
            break;
        }
        got += (uint32_t)n;
        if (n != (size_t)(to - from)) { s_last_io_error = true; break; }
    }
    return got;
}

uint32_t stats_store_read_page(uint32_t page_index, uint32_t page_size,
                                stats_record_t *out, uint32_t out_cap)
{
    s_last_io_error = false;
    if (page_size == 0 || out_cap == 0 || !out) return 0;

    lock();
    if (!ensure_alloc_locked()) { unlock(); return 0; }
    uint64_t sdn = sd_count_locked();
    uint64_t lfn = lfs_unsynced_locked();
    uint64_t total = sdn + lfn + s_rbuf_n;
    uint64_t newest_shown_upto = (uint64_t)(page_index + 1) * page_size;  /* 이 페이지가 덮는 끝 */
    if (newest_shown_upto > total) newest_shown_upto = total;
    uint64_t oldest_in_page = (uint64_t)page_index * page_size;
    if (oldest_in_page >= total) { unlock(); return 0; }  /* 존재하지 않는 페이지 */

    uint32_t count_in_page = (uint32_t)(newest_shown_upto - oldest_in_page);
    if (count_in_page > out_cap) count_in_page = out_cap;

    /* page_index=0이 "가장 최근" 페이지 — 전체(SD 주 파일들 → 내부 플래시 → RAM 버퍼 순으로 이어 붙인 순서) 기준
     * 시작 위치를 역산한 뒤 그 구간을 순서대로 읽음(오래된 것부터 채움) */
    uint64_t start = total - newest_shown_upto;
    uint64_t end = start + count_in_page;
    uint32_t got = 0;
    if (start < sdn) {
        uint64_t e = (end < sdn) ? end : sdn;
        got = sd_read_range_locked(start, e, out);
        if (got != (uint32_t)(e - start)) { unlock(); return got; }
    }
    uint64_t pos = start + got;
    if (pos < end && pos < sdn + lfn) {
        uint64_t e = (end < sdn + lfn) ? end : sdn + lfn;
        uint32_t want = (uint32_t)(e - pos);
        uint32_t r = lfs_read_unsynced_locked((uint32_t)(pos - sdn), want, out + got);
        got += r;
        if (r != want) { unlock(); return got; }
        pos += r;
    }
    while (pos < end) {
        out[got++] = s_rbuf[pos - sdn - lfn];
        pos++;
    }
    unlock();
    return got;
}

/* ---- "cutoff 이후" 순차 순회(read_since / min_max_avg_since / min_max 공용) ----
 * 호출부가 lock() 잡은 상태. visit이 false를 돌려주면 순회 중단. I/O 실패면 false */
typedef bool (*rec_visit_fn)(const stats_record_t *r, void *ctx);

static bool read_time_at(FILE *f, uint32_t idx, uint32_t *out_time)
{
    if (fseek(f, (long)idx * (long)sizeof(stats_record_t), SEEK_SET) != 0) return false;
    stats_record_t rec;
    if (fread(&rec, sizeof(rec), 1, f) != 1) return false;
    *out_time = rec.unix_time;
    return true;
}

static bool scan_since_locked(uint32_t cutoff_unix_time, rec_visit_fn visit, void *ctx)
{
    uint32_t w0 = cutoff_unix_time / STATS_WEEK_SEC;
    for (uint32_t i = 0; sd_storage_is_mounted() && i < s_week_count; i++) {
        if (s_weeks[i].week < w0 || s_weeks[i].raw_count == 0) continue;
        char path[48];
        raw_path(s_weeks[i].week, path, sizeof(path));
        FILE *f = open_read(path);
        if (!f) { if (s_last_io_error) return false; continue; }

        uint32_t start = 0;
        if (s_weeks[i].week == w0) {
            /* 첫 주 파일에서만 이진탐색 — unix_time >= cutoff인 첫 레코드 */
            uint32_t lo = 0, hi = s_weeks[i].raw_count;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                uint32_t t;
                if (!read_time_at(f, mid, &t)) { s_last_io_error = true; fclose(f); return false; }
                if (t < cutoff_unix_time) lo = mid + 1;
                else hi = mid;
            }
            start = lo;
        }
        fseek(f, (long)start * (long)sizeof(stats_record_t), SEEK_SET);
        size_t got;
        while ((got = fread(s_scratch, sizeof(stats_record_t), STATS_CHUNK, f)) > 0) {
            for (size_t k = 0; k < got; k++) {
                if (s_scratch[k].unix_time < cutoff_unix_time) continue;
                if (!visit(&s_scratch[k], ctx)) { fclose(f); return true; }
            }
        }
        fclose(f);
    }
    /* 2026-10-15(SD 개선 2단계) — 아직 SD로 안 옮긴 것(내부 플래시 → RAM 버퍼) */
    uint32_t lfn = lfs_unsynced_locked();
    for (uint32_t u = 0; u < lfn; ) {
        uint32_t n = lfs_read_unsynced_locked(u, STATS_CHUNK, s_scratch);
        if (n == 0) break;
        for (uint32_t k = 0; k < n; k++) {
            if (s_scratch[k].unix_time < cutoff_unix_time) continue;
            if (!visit(&s_scratch[k], ctx)) return true;
        }
        u += n;
    }
    for (uint32_t k = 0; k < s_rbuf_n; k++) {
        if (s_rbuf[k].unix_time < cutoff_unix_time) continue;
        if (!visit(&s_rbuf[k], ctx)) return true;
    }
    return true;
}

typedef struct { uint8_t chan_type; uint32_t count; } count_ctx_t;
static bool visit_count(const stats_record_t *r, void *ctx)
{
    count_ctx_t *c = ctx;
    if (r->chan_type == c->chan_type) c->count++;
    return true;
}

typedef struct {
    uint8_t chan_type;
    uint32_t stride, seen, picked, out_cap;
    stats_record_t *out;
} pick_ctx_t;
static bool visit_pick(const stats_record_t *r, void *ctx)
{
    pick_ctx_t *c = ctx;
    if (r->chan_type != c->chan_type) return true;
    if ((c->seen % c->stride) == 0) c->out[c->picked++] = *r;
    c->seen++;
    return c->picked < c->out_cap;
}

typedef struct {
    uint8_t chan_type;
    bool found;
    float mn, mx;
    double sum;
    uint32_t count;
} stat_ctx_t;
static bool visit_stat(const stats_record_t *r, void *ctx)
{
    stat_ctx_t *c = ctx;
    if (r->chan_type != c->chan_type) return true;
    if (!c->found) { c->mn = c->mx = r->value; c->found = true; }
    else {
        if (r->value < c->mn) c->mn = r->value;
        if (r->value > c->mx) c->mx = r->value;
    }
    c->sum += (double)r->value;
    c->count++;
    return true;
}

uint32_t stats_store_read_since(uint32_t cutoff_unix_time, uint8_t chan_type,
                                 stats_record_t *out, uint32_t out_cap)
{
    s_last_io_error = false;
    if (out_cap == 0 || !out) return 0;

    lock();
    if (!ensure_alloc_locked()) { unlock(); return 0; }
    /* 1차 순회 — 이 chan_type 매치 개수만 셈(균등 간격 선택용) */
    count_ctx_t cc = { .chan_type = chan_type, .count = 0 };
    if (!scan_since_locked(cutoff_unix_time, visit_count, &cc) || cc.count == 0) { unlock(); return 0; }

    /* 2차 순회 — stride번째 매치마다 하나씩(그래프는 다운샘플이 전제, 사용자 지시: "부담되면 그냥
     * 그 순서의 값을 표시해도 되") */
    pick_ctx_t pc = { .chan_type = chan_type, .stride = (cc.count + out_cap - 1) / out_cap,
                      .seen = 0, .picked = 0, .out_cap = out_cap, .out = out };
    if (pc.stride < 1) pc.stride = 1;
    scan_since_locked(cutoff_unix_time, visit_pick, &pc);
    unlock();
    return pc.picked;
}

bool stats_store_get_min_max_avg_since(uint32_t cutoff_unix_time, uint8_t chan_type,
                                        float *out_min, float *out_max, float *out_avg)
{
    s_last_io_error = false;
    lock();
    if (!ensure_alloc_locked()) { unlock(); return false; }
    stat_ctx_t sc = { .chan_type = chan_type };
    bool ok = scan_since_locked(cutoff_unix_time, visit_stat, &sc);
    unlock();
    if (!ok || !sc.found) return false;
    *out_min = sc.mn;
    *out_max = sc.mx;
    *out_avg = (float)(sc.sum / (double)sc.count);
    return true;
}

bool stats_store_get_min_max(uint8_t chan_type, float *out_min, float *out_max)
{
    float avg;
    return stats_store_get_min_max_avg_since(0, chan_type, out_min, out_max, &avg);
}

/* ════════════════════════════════════════════════════════════
 * 2026-10-15(SD 개선 2단계) — 1시간마다 내부 플래시 → SD
 * ════════════════════════════════════════════════════════════ */

/* lock 잡은 상태 — 레코드들을 SD 주 파일에 이어 씀(주마다 open+연속쓰기+fsync+close 한 번 + 쓴 끝부분 확인).
 * 앞에서부터 이어서 성공한 개수 반환(실패한 곳에서 멈춤 — 옮긴 위치가 그 앞까지만 나아가게) */
static uint32_t sd_raw_write_locked(const stats_record_t *records, uint32_t count)
{
    uint32_t i = 0;
    while (i < count) {
        uint32_t week = records[i].unix_time / STATS_WEEK_SEC;
        uint32_t j = i + 1;
        while (j < count && records[j].unix_time / STATS_WEEK_SEC == week) j++;

        int widx = week_get_or_add_locked(week);
        if (widx < 0) return i;
        char path[48];
        raw_path(week, path, sizeof(path));
        FILE *f = fopen(path, "ab");
        if (!f) {
            s_last_io_error = true;
            s_write_io_error_pending = true;
            ESP_LOGW(TAG, "Value file open failed (errno=%d) - move to SD stopped, kept in flash", errno);
            return i;
        }
        long start = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
        size_t written = fwrite(&records[i], sizeof(stats_record_t), j - i, f);
        bool synced = close_synced(f);
        long fsize = (start >= 0) ? start + (long)(written * sizeof(stats_record_t)) : -1;
        if (written > 0 && synced) {
            /* 2026-10-14(SD 개선 1단계) — 방금 쓴 끝부분을 다른 핸들로 다시 읽어 비교 */
            sd_health_count(SDH_C_FSYNC, 1);
            sd_health_count(SDH_C_BYTES, (uint32_t)(written * sizeof(stats_record_t)));
            int v = (fsize >= 0) ? verify_tail(path, fsize, &records[i], written * sizeof(stats_record_t)) : -1;
            if (v == 0) sd_health_report(SDH_EV_VERIFY_STATS, path, (uint32_t)fsize);
            else if (v > 0) sd_health_count(SDH_C_VERIFY_OK, 1);
        }
        s_weeks[widx].raw_count += (uint32_t)written;
        s_total_raw += written;
        if (written != j - i || !synced) {
            s_last_io_error = true;
            s_write_io_error_pending = true;
            ESP_LOGW(TAG, "Value file write incomplete (%u/%u, sync=%d) - rest kept in flash", (unsigned)written, (unsigned)(j - i), synced ? 1 : 0);
            return i + (synced ? (uint32_t)written : 0);   /* fsync 실패면 쓴 것도 믿지 않음(다음에 다시 — 겹칠 수 있음) */
        }
        i = j;
    }
    return count;
}

/* lock 잡은 상태 — 옮긴 위치를 n개 앞으로 */
static void mark_advance_locked(uint32_t n)
{
    while (n > 0) {
        int i; uint32_t off;
        if (!lfs_locate_unsynced(0, &i, &off)) break;
        uint32_t avail = s_lfs_days[i].count - off;
        uint32_t k = (n < avail) ? n : avail;
        s_mark.day = s_lfs_days[i].day;
        s_mark.off = off + k;
        n -= k;
    }
}

/* 내부 플래시의 모든 날 파일(+ RAM 버퍼)에서 시각 >= from인 기록을 차례로 fn에. 기록 태스크 안에서만
 * (날 파일을 바꾸는 쪽도 이 태스크뿐이라 잠금 없이 읽음 — RAM 버퍼만 잠금 아래) */
static void lfs_replay(uint32_t from, agg_state_t *st, bool lock_rbuf)
{
    for (int i = 0; i < s_lfs_nd; i++) {
        if ((s_lfs_days[i].day + 1) * DAY_SEC <= from) continue;   /* 날 파일 D의 기록 시각은 D일 끝을 넘지 않음 */
        /* 파일 안은 도착순 = 시간순 — 시각 >= from인 첫 레코드를 이진탐색(매시 하루 파일 전체를 읽지 않게) */
        uint32_t off = 0;
        if (s_lfs_days[i].day * DAY_SEC < from) {
            uint32_t lo = 0, hi = s_lfs_days[i].count;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                stats_record_t r;
                if (lfs_read_at(i, mid, &r, 1) != 1) { lo = 0; break; }
                if (r.unix_time < from) lo = mid + 1;
                else hi = mid;
            }
            off = lo;
        }
        while (off < s_lfs_days[i].count) {
            uint32_t want = s_lfs_days[i].count - off;
            if (want > SYNC_CHUNK) want = SYNC_CHUNK;
            uint32_t n = lfs_read_at(i, off, s_sync_buf, want);
            if (n == 0) break;
            for (uint32_t k = 0; k < n; k++) {
                if (s_sync_buf[k].unix_time >= from) agg_update(st, &s_sync_buf[k]);
            }
            off += n;
        }
    }
    /* RAM 버퍼는 잠금 아래에서 복사만 하고 계산은 밖에서(옮기기용은 계산 중에 SD 쓰기가 잠금을 잡음) */
    if (lock_rbuf) lock();
    uint32_t nb = (s_rbuf_n < SYNC_CHUNK) ? s_rbuf_n : SYNC_CHUNK;
    memcpy(s_sync_buf, s_rbuf, nb * sizeof(stats_record_t));
    if (lock_rbuf) unlock();
    for (uint32_t k = 0; k < nb; k++) {
        if (s_sync_buf[k].unix_time >= from) agg_update(st, &s_sync_buf[k]);
    }
}

/* 1시간마다(또는 새 카드·재연결 뒤) — 기록 태스크에서만. 성공하면 true */
static bool sync_run(void)
{
    uint32_t now = rtc_sync_get_unix_time();
    if (now == 0 || !sd_storage_is_mounted() || !sd_health_writes_allowed()) return false;
    if (!s_sync_buf) s_sync_buf = heap_caps_malloc(sizeof(stats_record_t) * SYNC_CHUNK, MALLOC_CAP_SPIRAM);
    if (!s_regen_buf) s_regen_buf = heap_caps_malloc(sizeof(agg_pend_t) * REGEN_MAX, MALLOC_CAP_SPIRAM);
    if (!s_sync_buf || !s_regen_buf) { ESP_LOGE(TAG, "Sync buffer alloc failed"); return false; }
    int64_t t0 = esp_timer_get_time();
    s_sy.syncs++;

    /* 1) 원시 기록 — 덩어리마다 잠금 한 번(그 사이에 화면 읽기·SD 재연결이 끼어들 수 있게) */
    uint32_t moved = 0;
    bool ok = true;
    for (;;) {
        lock();
        if (!ensure_alloc_locked() || !sd_storage_is_mounted() || !sd_health_writes_allowed()) { unlock(); ok = false; break; }
        uint32_t left = lfs_unsynced_locked();
        if (left == 0) { unlock(); break; }
        uint32_t want = (left < SYNC_CHUNK) ? left : SYNC_CHUNK;
        uint32_t n = lfs_read_unsynced_locked(0, want, s_sync_buf);
        uint32_t w = n ? sd_raw_write_locked(s_sync_buf, n) : 0;
        if (w > 0) { mark_advance_locked(w); mark_save_locked(); }
        moved += w;
        unlock();
        if (n != want || w != n) { ok = false; break; }
    }
    s_sy.synced_recs += moved;

    /* 2) 집계 — 지난번 이후 끝난 칸(agg_t < 칸 끝 <= now)을 내부 플래시 기록으로 다시 계산해서 SD에 씀.
     *    가장 넓은 칸(10080초)이 agg_t에 걸쳐 있을 수 있어 그만큼 앞에서부터 다시 돌림 */
    lock();
    uint32_t lo = s_mark.agg_t;
    s_regen_gen = s_restore_gen;   /* 도중에 지우기·새 카드 복원이 끼면 이번 계산은 버림 */
    unlock();
    uint32_t buckets = 0;
    if (ok && now > lo) {
        agg_state_reset(&s_regen);
        s_regen_lo = lo;
        s_regen_hi = now;
        s_regen_n = 0;
        s_regen_written = 0;
        lfs_replay(lo > AGG_MAX_WIDTH ? lo - AGG_MAX_WIDTH : 0, &s_regen, true);
        agg_flush_ended(&s_regen, now);
        regen_flush();
        buckets = s_regen_written;
        lock();
        if (s_regen_gen != s_restore_gen) {
            unlock();
            s_sync_requested = true;   /* 다음 분에 처음부터 다시 */
            ESP_LOGW(TAG, "Sync: data reset during aggregate rebuild - redo");
            return false;
        }
        s_mark.agg_t = now;
        mark_save_locked();
        /* SD에 들어간 칸(끝 <= now)은 화면용 목록에서 뺌 */
        uint32_t k = 0;
        for (uint32_t i = 0; i < s_disp_n; i++) {
            if (s_disp[i].rec.bucket_start_unix + scale_width(s_disp[i].scale) <= now) continue;
            s_disp[k++] = s_disp[i];
        }
        s_disp_n = k;
        unlock();
        s_sy.synced_buckets += buckets;
    }
    if (!ok) s_sy.sync_fail++;
    s_sy.last_sync_unix = now;
    s_sy.last_sync_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "Moved to SD: %u values, %u buckets in %u ms%s", (unsigned)moved, (unsigned)buckets,
             (unsigned)s_sy.last_sync_ms, ok ? "" : " (stopped - rest kept in flash)");
    if (moved > 0 || buckets > 0) storage_mgr_notify_changed();
    return ok;
}

/* 1분마다(타이머 → 기록 태스크) — 버퍼를 내부 플래시에, 시(時)가 바뀌었거나 요청이 있으면 SD로 옮김.
 * SD가 없거나 고장이면 옮기지 않고 다음 분에 다시 봄 */
static void backup_remind_check(uint32_t now);   /* 아래(5단계) */

static void minute_job(const uint8_t *arg)
{
    (void)arg;
    lock();
    lfs_flush_locked();
    unlock();
    uint32_t now = rtc_sync_get_unix_time();
    if (now == 0) return;
    uint32_t hour = now / 3600;
    backup_remind_check(now);   /* 2026-10-15(5단계) — 백업할 때가 됐으면 하루에 한 번 경고 */
    if (hour == s_last_sync_hour && !s_sync_requested) return;
    if (!sd_storage_is_mounted() || !sd_health_writes_allowed()) return;
    if (!s_sync_mtx || xSemaphoreTake(s_sync_mtx, 0) != pdTRUE) return;   /* 복원 중 — 끝나면 다음 분에 */
    s_sync_requested = false;
    s_last_sync_hour = hour;
    sync_run();   /* 실패하면 다음 시(時)에 다시(나쁜 카드를 매분 두드리지 않게) — 재연결하면 재검사가 다시 요청함 */
    xSemaphoreGive(s_sync_mtx);
}

static esp_timer_handle_t s_minute_timer = NULL;
static void minute_timer_cb(void *arg)
{
    (void)arg;
    stats_store_post_job(minute_job, NULL, 0);
}

uint32_t stats_store_get_unsynced(void)
{
    lock();
    uint32_t n = lfs_unsynced_locked() + s_rbuf_n;
    unlock();
    return n;
}

void stats_store_request_sync(void)
{
    s_sync_requested = true;
    stats_store_post_job(minute_job, NULL, 0);
}

int stats_store_sync_json(char *buf, size_t cap)
{
    lock();
    uint32_t lfn = lfs_unsynced_locked(), lft = 0;
    for (int i = 0; i < s_lfs_nd; i++) lft += s_lfs_days[i].count;
    int n = snprintf(buf, cap,
        "{\"mark\":{\"day\":%u,\"off\":%u,\"agg_t\":%u},\"flash\":{\"days\":%d,\"first\":%u,\"last\":%u,\"recs\":%u,\"unsynced\":%u,"
        "\"writes\":%u,\"fail\":%u,\"pruned_unsynced\":%u},\"ram\":%u,\"sd_recs\":%llu,\"disp\":%u,\"disp_dropped\":%u,"
        "\"sync\":{\"n\":%u,\"fail\":%u,\"recs\":%u,\"buckets\":%u,\"last\":%u,\"ms\":%u,\"restores\":%u,\"requested\":%d}}",
        (unsigned)s_mark.day, (unsigned)s_mark.off, (unsigned)s_mark.agg_t, s_lfs_nd,
        (unsigned)(s_lfs_nd ? s_lfs_days[0].day : 0), (unsigned)(s_lfs_nd ? s_lfs_days[s_lfs_nd - 1].day : 0),
        (unsigned)lft, (unsigned)lfn, (unsigned)s_sy.lfs_writes, (unsigned)s_sy.lfs_write_fail, (unsigned)s_sy.lfs_pruned_unsynced,
        (unsigned)s_rbuf_n, (unsigned long long)s_total_raw, (unsigned)s_disp_n, (unsigned)s_sy.disp_dropped,
        (unsigned)s_sy.syncs, (unsigned)s_sy.sync_fail, (unsigned)s_sy.synced_recs, (unsigned)s_sy.synced_buckets,
        (unsigned)s_sy.last_sync_unix, (unsigned)s_sy.last_sync_ms, (unsigned)s_sy.restores, s_sync_requested ? 1 : 0);
    unlock();
    return n;
}

/* ════════════════════════════════════════════════════════════
 * 2026-10-15(SD 개선 5단계, 사용자 설계 — 백업·복원은 브라우저에서만, 통계 원시 기록만, 사진은 안 함) — 백업·복원.
 * 백업 파일 = 머리(32바이트) + 원시 레코드(17바이트씩, 기록 표와 같은 순서 — SD → 내부 플래시 → RAM) + CRC32(레코드 부분).
 * 복원 = 브라우저가 올린 파일을 SD 임시 파일에 받고 확인 → 복원 태스크가 주마다 기존 기록과 시간순으로 합침(같은 레코드는
 * 하나만) → 집계를 원시 기록 전체로 다시 계산. 그동안 매시 SD 옮기기·정리는 쉼(내부 플래시엔 계속 쌓임).
 * 알림 = 마지막 백업에서 BACKUP_REMIND_SEC가 지나면 하루에 한 번 경고(W6003, 휴대폰 알림) — 사용자: 3개월, 시험 중엔 매일
 * ════════════════════════════════════════════════════════════ */
#define BACKUP_MAGIC        0x31425343u   /* "CSB1" */
#define BACKUP_HDR_BYTES    32u
#define BACKUP_REMIND_SEC   (90u * 86400u)   /* 3개월(사용자 지시 "3개월마다") — 10-18 시험 끝(시험 중엔 1일) */
#define BACKUP_STATE_PATH   FS_MOUNT_POINT "/backup.bin"
#define RESTORE_TMP_PATH    SD_STORAGE_MOUNT_POINT "/restore.tmp"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t rec_size;
    uint32_t count;
    uint32_t created;
    uint32_t rsv[4];
} backup_hdr_t;   /* 32바이트 */

typedef struct { uint32_t magic, last_backup, last_remind; } backup_state_t;
static backup_state_t s_bk = { 0, 0, 0 };

static void backup_state_save(void)
{
    s_bk.magic = BACKUP_MAGIC;
    FILE *f = fopen(BACKUP_STATE_PATH, "wb");
    if (!f) return;
    fwrite(&s_bk, sizeof(s_bk), 1, f);
    fclose(f);
}

static void backup_state_load(void)
{
    FILE *f = fopen(BACKUP_STATE_PATH, "rb");
    if (f) {
        if (fread(&s_bk, sizeof(s_bk), 1, f) != 1 || s_bk.magic != BACKUP_MAGIC) memset(&s_bk, 0, sizeof(s_bk));
        fclose(f);
    }
}

uint32_t stats_store_last_backup(void) { return s_bk.last_backup; }

/* 백업 — 전체 개수와 머리 */
uint32_t stats_store_backup_begin(uint8_t hdr[32])
{
    uint32_t total = stats_store_get_count();
    backup_hdr_t h = { .magic = BACKUP_MAGIC, .version = 1, .rec_size = sizeof(stats_record_t), .count = total,
                       .created = rtc_sync_get_unix_time() };
    memcpy(hdr, &h, sizeof(h));
    return total;
}

/* 백업 — 전체(기록 표 순서) 중 [start, start+n)을 out에. 읽은 개수 반환(읽기 실패면 적게) */
uint32_t stats_store_backup_read(uint32_t start, uint32_t n, stats_record_t *out)
{
    s_last_io_error = false;
    lock();
    if (!ensure_alloc_locked()) { unlock(); return 0; }
    uint64_t sdn = sd_count_locked(), lfn = lfs_unsynced_locked();
    uint64_t total = sdn + lfn + s_rbuf_n;
    uint64_t end = (uint64_t)start + n;
    if (end > total) end = total;
    uint32_t got = 0;
    uint64_t pos = start;
    if (pos < sdn && pos < end) {
        uint64_t e = (end < sdn) ? end : sdn;
        uint32_t r = sd_read_range_locked(pos, e, out);
        got += r;
        if (r != (uint32_t)(e - pos)) { unlock(); return got; }
        pos = e;
    }
    if (pos < end && pos < sdn + lfn) {
        uint64_t e = (end < sdn + lfn) ? end : sdn + lfn;
        uint32_t want = (uint32_t)(e - pos);
        uint32_t r = lfs_read_unsynced_locked((uint32_t)(pos - sdn), want, out + got);
        got += r;
        if (r != want) { unlock(); return got; }
        pos = e;
    }
    while (pos < end) { out[got++] = s_rbuf[pos - sdn - lfn]; pos++; }
    unlock();
    return got;
}

/* 백업을 끝까지 내려보냈을 때(웹) — 알림 기준 시각 */
void stats_store_backup_done(void)
{
    s_bk.last_backup = rtc_sync_get_unix_time();
    s_bk.last_remind = 0;
    backup_state_save();
    ESP_LOGI(TAG, "Statistics backup downloaded");
}

/* 1분 처리에서 — 백업할 때가 됐으면 하루에 한 번 경고 */
static void backup_remind_check(uint32_t now)
{
    if (now < 1704067200u) return;   /* 시계가 안 맞음 */
    if (s_bk.last_backup == 0) { s_bk.last_backup = now; backup_state_save(); return; }   /* 처음 — 지금부터 셈 */
    if (now - s_bk.last_backup < BACKUP_REMIND_SEC) return;
    if (s_bk.last_remind && now - s_bk.last_remind < 86400u) return;
    s_bk.last_remind = now;
    backup_state_save();
    ui_log_add_warn(UI_WARN_BACKUP_DUE, "Statistics backup due (last %u days ago)", (unsigned)((now - s_bk.last_backup) / 86400u));
}

/* ── 복원 ── */
typedef enum { RS_IDLE = 0, RS_UPLOADING, RS_RUNNING, RS_DONE, RS_FAILED } restore_state_t;
static volatile restore_state_t s_rs = RS_IDLE;
static volatile bool s_restore_active = false;   /* true면 SD 정리(오래된 주 지우기)를 쉼 */
/* s_sync_mtx — 매시 옮기기와 복원이 겹치지 않게(아래 minute_job·restore_task). 1분 처리는 못 잡으면 옮기기만 건너뜀 */
static FILE    *s_rs_f = NULL;
static uint32_t s_rs_bytes = 0, s_rs_crc = 0, s_rs_count = 0;
static uint8_t  s_rs_hdr[BACKUP_HDR_BYTES];
static uint8_t  s_rs_tail[4];
static uint32_t s_rs_added = 0, s_rs_dup = 0, s_rs_bad = 0, s_rs_weeks_done = 0, s_rs_weeks = 0;
static char     s_rs_msg[48] = "";

static void rs_fail(const char *msg)
{
    snprintf(s_rs_msg, sizeof(s_rs_msg), "%s", msg);
    s_rs = RS_FAILED;
    ESP_LOGE(TAG, "Restore failed: %s", msg);
}

bool stats_store_restore_begin(uint32_t total_bytes)
{
    if (s_rs == RS_UPLOADING || s_rs == RS_RUNNING) return false;
    if (!sd_storage_is_mounted() || !sd_health_writes_allowed()) { rs_fail("SD not available"); return false; }
    if (total_bytes < BACKUP_HDR_BYTES + 4 || (total_bytes - BACKUP_HDR_BYTES - 4) % sizeof(stats_record_t) != 0) {
        rs_fail("not a backup file (size)");
        return false;
    }
    s_rs_f = fopen(RESTORE_TMP_PATH, "wb");
    if (!s_rs_f) { rs_fail("temp file open failed"); return false; }
    s_rs_bytes = 0;
    s_rs_crc = 0;
    s_rs_count = (total_bytes - BACKUP_HDR_BYTES - 4) / sizeof(stats_record_t);
    s_rs_added = s_rs_dup = s_rs_bad = s_rs_weeks_done = s_rs_weeks = 0;
    s_rs_msg[0] = '\0';
    s_rs = RS_UPLOADING;
    return true;
}

/* 올라오는 대로 임시 파일에 씀 — 머리·꼬리(CRC)는 따로 떼어 두고 레코드 부분의 CRC를 셈 */
bool stats_store_restore_write(const uint8_t *data, size_t len)
{
    if (s_rs != RS_UPLOADING || !s_rs_f) return false;
    uint32_t rec_end = BACKUP_HDR_BYTES + s_rs_count * sizeof(stats_record_t);
    for (size_t i = 0; i < len; ) {
        uint32_t pos = s_rs_bytes;
        if (pos < BACKUP_HDR_BYTES) {
            size_t k = BACKUP_HDR_BYTES - pos; if (k > len - i) k = len - i;
            memcpy(s_rs_hdr + pos, data + i, k);
            s_rs_bytes += k; i += k;
        } else if (pos < rec_end) {
            size_t k = rec_end - pos; if (k > len - i) k = len - i;
            if (fwrite(data + i, 1, k, s_rs_f) != k) { rs_fail("temp file write failed"); fclose(s_rs_f); s_rs_f = NULL; return false; }
            s_rs_crc = esp_rom_crc32_le(s_rs_crc, data + i, k);
            s_rs_bytes += k; i += k;
        } else {
            size_t k = rec_end + 4 - pos; if (k > len - i) k = len - i;
            if (pos - rec_end + k > 4) { rs_fail("file longer than header says"); fclose(s_rs_f); s_rs_f = NULL; return false; }
            memcpy(s_rs_tail + (pos - rec_end), data + i, k);
            s_rs_bytes += k; i += k;
        }
    }
    return true;
}

static void restore_task(void *arg);

/* 다 받았으면 확인하고 복원 태스크 시작. false면 이유는 상태 JSON의 msg */
bool stats_store_restore_end(bool complete)
{
    if (s_rs != RS_UPLOADING) return false;
    bool ok = s_rs_f && fflush(s_rs_f) == 0 && fsync(fileno(s_rs_f)) == 0;
    if (s_rs_f) fclose(s_rs_f);
    s_rs_f = NULL;
    backup_hdr_t h;
    memcpy(&h, s_rs_hdr, sizeof(h));
    uint32_t tail;
    memcpy(&tail, s_rs_tail, 4);
    if (!complete) { rs_fail("upload incomplete"); ok = false; }
    else if (!ok) rs_fail("temp file sync failed");
    else if (h.magic != BACKUP_MAGIC || h.version != 1 || h.rec_size != sizeof(stats_record_t)) { rs_fail("not a backup file (header)"); ok = false; }
    else if (h.count != s_rs_count) { rs_fail("record count mismatch"); ok = false; }
    else if (tail != s_rs_crc) { rs_fail("CRC mismatch - file damaged"); ok = false; }
    if (!ok) { unlink(RESTORE_TMP_PATH); return false; }
    s_rs = RS_RUNNING;
    static StackType_t *s_rs_stack = NULL;   /* 한 번만 잡고 다시 씀(정적 태스크) */
    static StaticTask_t s_rs_tcb;
    if (!s_rs_stack) s_rs_stack = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!s_rs_stack || !xTaskCreateStaticPinnedToCore(restore_task, "stats_rs", 8192 / sizeof(StackType_t), NULL, 9, s_rs_stack, &s_rs_tcb, 1)) {
        rs_fail("task start failed");
        unlink(RESTORE_TMP_PATH);
        return false;
    }
    ESP_LOGI(TAG, "Restore: %u records received, CRC ok - merging", (unsigned)s_rs_count);
    return true;
}

int stats_store_restore_status_json(char *buf, size_t cap)
{
    static const char *const st[] = { "idle", "uploading", "running", "done", "failed" };
    return snprintf(buf, cap, "{\"state\":\"%s\",\"records\":%u,\"added\":%u,\"dup\":%u,\"bad\":%u,\"weeks\":%u,\"weeks_done\":%u,"
                    "\"msg\":\"%s\",\"last_backup\":%u}", st[s_rs], (unsigned)s_rs_count, (unsigned)s_rs_added, (unsigned)s_rs_dup,
                    (unsigned)s_rs_bad, (unsigned)s_rs_weeks, (unsigned)s_rs_weeks_done, s_rs_msg, (unsigned)s_bk.last_backup);
}

/* 순차 레코드 읽기(파일 + PSRAM 덩어리) */
typedef struct {
    FILE *f;
    stats_record_t *buf;
    uint32_t n, i;
    bool eof;
} rec_reader_t;

static bool rr_peek(rec_reader_t *r, stats_record_t *out)
{
    if (r->i >= r->n) {
        if (r->eof || !r->f) return false;
        r->n = (uint32_t)fread(r->buf, sizeof(stats_record_t), STATS_CHUNK, r->f);
        r->i = 0;
        if (r->n == 0) { r->eof = true; return false; }
    }
    *out = r->buf[r->i];
    return true;
}

static void rr_next(rec_reader_t *r) { r->i++; }

static bool rec_sane(const stats_record_t *r)
{
    return r->unix_time >= 1704067200u && r->unix_time < 4102444800u && r->chan_type < STATS_AGG_MAX_CHAN_TYPES;
}

/* 주 week의 기존 원시 파일과, 올린 파일에서 지금 위치부터 같은 주인 레코드를 시간순으로 합쳐 새 파일로 쓰고 바꿔 끼움.
 * 같은 시각 묶음 안에서 17바이트가 모두 같은 레코드는 하나만. 새 파일 레코드 수 반환(실패하면 -1) */
#define RS_GROUP_MAX 64
static int64_t restore_merge_week(uint32_t week, rec_reader_t *in, stats_record_t *grp)
{
    char path[48], npath[52];
    raw_path(week, path, sizeof(path));
    snprintf(npath, sizeof(npath), "%s.new", path);
    rec_reader_t ex = { .f = fopen(path, "rb"), .buf = heap_caps_malloc(sizeof(stats_record_t) * STATS_CHUNK, MALLOC_CAP_SPIRAM) };
    FILE *o = fopen(npath, "wb");
    if (!ex.buf || !o) { if (ex.f) fclose(ex.f); if (o) fclose(o); heap_caps_free(ex.buf); return -1; }
    int64_t out_n = 0;
    bool wok = true;
    stats_record_t a, b;
    for (;;) {
        bool ha = rr_peek(&ex, &a);
        bool hb = rr_peek(in, &b);
        while (hb && !rec_sane(&b)) { s_rs_bad++; rr_next(in); hb = rr_peek(in, &b); }   /* 말이 안 되는 레코드는 버림 */
        if (hb && b.unix_time / STATS_WEEK_SEC != week) hb = false;                        /* 다른 주 — 이 주는 끝 */
        if (!ha && !hb) break;
        if (ha && (!hb || a.unix_time < b.unix_time)) { wok &= fwrite(&a, sizeof(a), 1, o) == 1; out_n++; rr_next(&ex); continue; }
        if (hb && (!ha || b.unix_time < a.unix_time)) { wok &= fwrite(&b, sizeof(b), 1, o) == 1; out_n++; s_rs_added++; rr_next(in); continue; }
        /* 같은 시각 — 기존 쪽 묶음을 모아 쓰고, 올린 쪽은 묶음에 없는 것만 */
        uint32_t t = a.unix_time, ng = 0;
        while (rr_peek(&ex, &a) && a.unix_time == t) {
            if (ng < RS_GROUP_MAX) grp[ng++] = a;
            wok &= fwrite(&a, sizeof(a), 1, o) == 1; out_n++; rr_next(&ex);
        }
        while (rr_peek(in, &b) && b.unix_time == t) {
            bool dup = false;
            for (uint32_t k = 0; k < ng && !dup; k++) dup = memcmp(&grp[k], &b, sizeof(b)) == 0;
            if (dup) s_rs_dup++;
            else { wok &= fwrite(&b, sizeof(b), 1, o) == 1; out_n++; s_rs_added++; if (ng < RS_GROUP_MAX) grp[ng++] = b; }
            rr_next(in);
        }
    }
    if (ex.f) fclose(ex.f);
    heap_caps_free(ex.buf);
    wok = wok && fflush(o) == 0 && fsync(fileno(o)) == 0;
    fclose(o);
    if (!wok) { unlink(npath); return -1; }
    /* 바꿔 끼움 — 색인과 함께(잠금 안에서) */
    lock();
    int widx = week_get_or_add_locked(week);
    bool ok = widx >= 0 && (unlink(path) == 0 || errno == ENOENT) && rename(npath, path) == 0;
    if (ok) {
        s_total_raw = s_total_raw - s_weeks[widx].raw_count + (uint64_t)out_n;
        s_weeks[widx].raw_count = (uint32_t)out_n;
    }
    unlock();
    if (!ok) { ESP_LOGE(TAG, "Restore: week %u swap failed (errno=%d)", (unsigned)week, errno); return -1; }
    sd_health_count(SDH_C_FSYNC, 1);
    return out_n;
}

/* 집계를 SD 원시 기록 전체로 다시 계산 — 집계 파일을 다 지우고 옮기기용 누적기(s_regen)로 처음부터 */
static bool restore_rebuild_aggregates(uint32_t now)
{
    lock();
    for (uint32_t i = 0; i < s_week_count; i++) {
        char path[48];
        for (uint8_t sc = 0; sc < STATS_SCALE_COUNT; sc++) {
            agg_path(sc, s_weeks[i].week, path, sizeof(path));
            if (unlink(path) != 0 && errno != ENOENT) ESP_LOGW(TAG, "Restore: aggregate delete failed %s (errno=%d)", path, errno);
        }
        s_weeks[i].agg_bytes = 0;
    }
    uint32_t nweeks = s_week_count;
    s_regen_gen = s_restore_gen;
    unlock();
    agg_state_reset(&s_regen);
    s_regen_lo = 0;
    s_regen_hi = now;
    s_regen_n = 0;
    s_regen_written = 0;
    for (uint32_t i = 0; i < nweeks; i++) {
        lock();
        uint32_t week = (i < s_week_count) ? s_weeks[i].week : 0;
        unlock();
        char path[48];
        raw_path(week, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t got;
        while ((got = fread(s_sync_buf, sizeof(stats_record_t), SYNC_CHUNK, f)) > 0) {
            for (size_t k = 0; k < got; k++) agg_update(&s_regen, &s_sync_buf[k]);
        }
        fclose(f);
    }
    agg_flush_ended(&s_regen, now);
    regen_flush();
    lock();
    bool ok = s_regen_gen == s_restore_gen;
    if (ok) {
        s_mark.agg_t = now;
        mark_save_locked();
        uint32_t k = 0;
        for (uint32_t i = 0; i < s_disp_n; i++) {
            if (s_disp[i].rec.bucket_start_unix + scale_width(s_disp[i].scale) <= now) continue;
            s_disp[k++] = s_disp[i];
        }
        s_disp_n = k;
    }
    unlock();
    ESP_LOGI(TAG, "Restore: aggregates rebuilt for %u weeks (%u buckets)", (unsigned)nweeks, (unsigned)s_regen_written);
    return ok;
}

static void restore_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    s_restore_active = true;
    xSemaphoreTake(s_sync_mtx, portMAX_DELAY);   /* 진행 중인 매시 옮기기가 끝나길 기다리고, 끝날 때까지 옮기기를 막음 */
    if (!s_sync_buf) s_sync_buf = heap_caps_malloc(sizeof(stats_record_t) * SYNC_CHUNK, MALLOC_CAP_SPIRAM);
    if (!s_regen_buf) s_regen_buf = heap_caps_malloc(sizeof(agg_pend_t) * REGEN_MAX, MALLOC_CAP_SPIRAM);
    stats_record_t *grp = heap_caps_malloc(sizeof(stats_record_t) * RS_GROUP_MAX, MALLOC_CAP_SPIRAM);
    rec_reader_t in = { .f = fopen(RESTORE_TMP_PATH, "rb"), .buf = heap_caps_malloc(sizeof(stats_record_t) * STATS_CHUNK, MALLOC_CAP_SPIRAM) };
    bool ok = s_sync_buf && s_regen_buf && grp && in.f && in.buf;
    if (!ok) rs_fail("memory or temp file");
    /* 1) 내부 플래시에 있는 것부터 SD로(합칠 때 중복으로 걸러지게) */
    if (ok) sync_run();
    /* 2) 주마다 합침 — 올린 파일은 기록 순서(시간순)라 주가 바뀔 때마다 그 주를 합침. 같은 주가 뒤에 또 나오면 다시 합침 */
    stats_record_t r;
    while (ok && rr_peek(&in, &r)) {
        if (!rec_sane(&r)) { s_rs_bad++; rr_next(&in); continue; }
        uint32_t week = r.unix_time / STATS_WEEK_SEC;
        s_rs_weeks++;
        if (restore_merge_week(week, &in, grp) < 0) { rs_fail("week merge failed"); ok = false; break; }
        s_rs_weeks_done++;
        if (!sd_storage_is_mounted() || !sd_health_writes_allowed()) { rs_fail("SD stopped"); ok = false; }
    }
    if (in.f) fclose(in.f);
    heap_caps_free(in.buf);
    heap_caps_free(grp);
    unlink(RESTORE_TMP_PATH);
    /* 3) 집계를 다시 계산(합친 결과로) */
    uint32_t now = rtc_sync_get_unix_time();
    if (ok && now && !restore_rebuild_aggregates(now)) { rs_fail("aggregate rebuild interrupted"); ok = false; }
    s_restore_active = false;
    xSemaphoreGive(s_sync_mtx);
    if (ok) {
        snprintf(s_rs_msg, sizeof(s_rs_msg), "ok");
        s_rs = RS_DONE;
    }
    ESP_LOGI(TAG, "Restore %s: %u records in file, %u added, %u duplicates, %u bad, %u weeks, %u s", ok ? "done" : "FAILED",
             (unsigned)s_rs_count, (unsigned)s_rs_added, (unsigned)s_rs_dup, (unsigned)s_rs_bad, (unsigned)s_rs_weeks_done,
             (unsigned)((esp_timer_get_time() - t0) / 1000000));
    storage_mgr_notify_changed();
    s_append_seq++;   /* 웹 기록 표 다시 읽게 */
    vTaskDelete(NULL);
}

/* 부팅 때 한 번(LittleFS 마운트 뒤, SD·통신 시작 전) — 날 파일 색인, 옮긴 위치 읽기, 화면용 누적기 다시 채우기, 1분 타이머 */
void stats_store_init(void)
{
    if (!s_sync_mtx) s_sync_mtx = xSemaphoreCreateMutex();
    backup_state_load();
    lock();
    if (!ensure_alloc_locked()) { unlock(); return; }
    if (!s_rbuf) s_rbuf = heap_caps_malloc(sizeof(stats_record_t) * RBUF_MAX, MALLOC_CAP_SPIRAM);
    if (!s_sync_buf) s_sync_buf = heap_caps_malloc(sizeof(stats_record_t) * SYNC_CHUNK, MALLOC_CAP_SPIRAM);
    if (mkdir(LFS_DIR, 0777) != 0 && errno != EEXIST) ESP_LOGE(TAG, "Flash stats dir create failed (errno=%d)", errno);

    char *path = heap_caps_malloc(300, MALLOC_CAP_SPIRAM);   /* 긴 파일 이름(d_name 최대 255) — 스택에 두지 않음 */
    DIR *d = path ? opendir(LFS_DIR) : NULL;
    s_lfs_nd = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            unsigned day = 0;
            char tail[8];
            if (strlen(e->d_name) != 10 || sscanf(e->d_name, "d%5u.%3s", &day, tail) != 2 || strcmp(tail, "bin") != 0) continue;
            snprintf(path, 300, LFS_DIR "/%s", e->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || st.st_size <= 0) { unlink(path); continue; }
            uint32_t n = (uint32_t)((uint64_t)st.st_size / sizeof(stats_record_t));
            if ((uint64_t)n * sizeof(stats_record_t) != (uint64_t)st.st_size) {
                ESP_LOGW(TAG, "Flash stats: half-written tail cut (%ld B): %s", (long)st.st_size, e->d_name);
                truncate(path, (off_t)n * (off_t)sizeof(stats_record_t));
            }
            if (n == 0) continue;
            if (s_lfs_nd >= LFS_MAX_DAYS) {   /* 넘치면 가장 오래된 것을 밀어냄(아래 정리가 지움) */
                int oldest = 0;
                for (int k = 1; k < s_lfs_nd; k++) if (s_lfs_days[k].day < s_lfs_days[oldest].day) oldest = k;
                if (s_lfs_days[oldest].day > day) continue;
                s_lfs_days[oldest] = s_lfs_days[--s_lfs_nd];
            }
            s_lfs_days[s_lfs_nd].day = day;
            s_lfs_days[s_lfs_nd].count = n;
            s_lfs_nd++;
        }
        closedir(d);
    }
    heap_caps_free(path);
    for (int a = 1; a < s_lfs_nd; a++) {   /* day 오름차순 */
        lfs_day_t t = s_lfs_days[a];
        int b = a - 1;
        while (b >= 0 && s_lfs_days[b].day > t.day) { s_lfs_days[b + 1] = s_lfs_days[b]; b--; }
        s_lfs_days[b + 1] = t;
    }

    sync_mark_t m = { 0 };
    FILE *f = fopen(LFS_MARK_PATH, "rb");
    bool have = f && fread(&m, sizeof(m), 1, f) == 1 && m.magic == MARK_MAGIC && m.crc == mark_crc(&m);
    if (f) fclose(f);
    if (have) {
        s_mark = m;
    } else if (s_lfs_nd > 0) {
        /* 위치 표시가 없는데 날 파일은 있음(비정상) — SD에 겹쳐 쓰지 않게 "다 옮김"으로 봄 */
        s_mark.day = s_lfs_days[s_lfs_nd - 1].day;
        s_mark.off = s_lfs_days[s_lfs_nd - 1].count;
        stats_record_t last;
        s_mark.agg_t = (lfs_read_at(s_lfs_nd - 1, s_mark.off - 1, &last, 1) == 1) ? last.unix_time : 0;
        ESP_LOGW(TAG, "Flash stats: sync mark missing - assuming all moved");
        mark_save_locked();
    }
    lfs_prune_locked();
    unlock();

    /* 화면용 누적기 — SD 집계에 아직 없는 칸(끝 > agg_t)을 다시 만들고, 진행 중인 칸을 이어 채움 */
    if (s_sync_buf) {
        uint32_t from = s_mark.agg_t > AGG_MAX_WIDTH ? s_mark.agg_t - AGG_MAX_WIDTH : 0;
        lock();
        lfs_replay(from, &s_live, false);
        unlock();
    }
    ESP_LOGI(TAG, "Flash stats: %d day files, %u not yet on SD, %u pending buckets (mark day %u off %u agg_t %u)",
             s_lfs_nd, (unsigned)lfs_unsynced_locked(), (unsigned)s_disp_n, (unsigned)s_mark.day, (unsigned)s_mark.off,
             (unsigned)s_mark.agg_t);

    const esp_timer_create_args_t ta = { .callback = minute_timer_cb, .name = "stats_min" };
    if (esp_timer_create(&ta, &s_minute_timer) == ESP_OK) esp_timer_start_periodic(s_minute_timer, 60ULL * 1000000ULL);
}

/* 호출부가 lock() 잡은 상태 — idx 주의 원시+집계 파일을 지우고 색인에서 뺌. 지운 원시 레코드 수 반환 */
static uint32_t delete_week_locked(uint32_t idx)
{
    uint32_t week = s_weeks[idx].week;
    uint32_t removed = s_weeks[idx].raw_count;
    char path[48];
    raw_path(week, path, sizeof(path));
    if (unlink(path) != 0 && errno != ENOENT) ESP_LOGW(TAG, "Week file delete failed (errno=%d): %s", errno, path);
    for (uint8_t s = 0; s < STATS_SCALE_COUNT; s++) {
        agg_path(s, week, path, sizeof(path));
        if (unlink(path) != 0 && errno != ENOENT) ESP_LOGW(TAG, "Aggregate file delete failed (errno=%d): %s", errno, path);
    }
    s_total_raw -= removed;
    for (uint32_t k = idx + 1; k < s_week_count; k++) s_weeks[k - 1] = s_weeks[k];
    s_week_count--;
    return removed;
}

void stats_store_delete_all(void)
{
    lock();
    if (s_weeks) {
        while (s_week_count > 0) delete_week_locked(s_week_count - 1);
    }
    s_total_raw = 0;
    /* 2026-10-15(SD 개선 2단계) — 내부 플래시 날 파일·RAM 버퍼·화면용 칸 목록도 지움(안 그러면 다음 옮기기 때 SD에 되살아남) */
    for (int i = 0; i < s_lfs_nd; i++) {
        char path[40];
        lfs_day_path(s_lfs_days[i].day, path, sizeof(path));
        if (unlink(path) != 0 && errno != ENOENT) ESP_LOGW(TAG, "Flash stats delete failed (errno=%d): %s", errno, path);
    }
    s_lfs_nd = 0;
    s_rbuf_n = 0;
    s_disp_n = 0;
    s_mark.day = 0;
    s_mark.off = 0;
    s_mark.agg_t = 0;
    mark_save_locked();
    s_restore_gen++;
    /* 채워지던 버킷도 버림 — 지운 뒤에 옛 구간 버킷이 새로 써지지 않게 */
    agg_state_reset(&s_live);
    unlock();
    storage_mgr_notify_changed();
    ESP_LOGI(TAG, "All measurement files deleted");
}

uint64_t stats_store_get_used_bytes(void)
{
    lock();
    uint64_t v = s_weeks ? used_bytes_locked() : 0;
    unlock();
    return v;
}

uint32_t stats_store_trim_to(uint64_t target_bytes)
{
    uint32_t deleted = 0, weeks = 0;
    if (s_restore_active) return 0;   /* 2026-10-15 — 복원이 주 파일을 바꿔 끼우는 중 */
    lock();
    if (s_weeks) {
        /* 가장 오래된 주부터 통째로 지움(다시 쓰기 없음). 지금 기록 중인 가장 최근 주는 남김 */
        while (s_week_count > 1 && used_bytes_locked() > target_bytes) {
            deleted += delete_week_locked(0);
            weeks++;
        }
    }
    unlock();
    if (weeks > 0) ESP_LOGI(TAG, "Trim: oldest %u weeks deleted (%u records)", (unsigned)weeks, (unsigned)deleted);
    return deleted;
}

/* 파일 끝에 반쯤 쓰인 레코드가 있으면(쓰다 리셋) 레코드 경계까지 잘라냄 — 안 그러면 이후 이어
 * 붙이는 레코드가 전부 한 칸씩 어긋나서 읽힘. 남은 온전한 레코드 수(또는 집계 바이트) 반환 */
static uint64_t align_file_size(const char *path, uint64_t size, size_t rec_size)
{
    uint64_t aligned = size - (size % rec_size);
    if (aligned != size) {
        if (truncate(path, (off_t)aligned) == 0) {
            ESP_LOGW(TAG, "Rescan: half-written tail cut to record boundary (%llu -> %llu): %s",
                     (unsigned long long)size, (unsigned long long)aligned, path);
        } else {
            ESP_LOGW(TAG, "Rescan: half-written file cut failed (errno=%d): %s", errno, path);
        }
    }
    return aligned;
}

void stats_store_rescan(uint64_t sd_total, uint32_t *out_bad_entries)
{
    uint32_t bad = 0;
    lock();
    if (!ensure_alloc_locked()) { unlock(); if (out_bad_entries) *out_bad_entries = 0; return; }
    s_week_count = 0;
    s_total_raw = 0;

    DIR *d = opendir(STATS_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *name = ent->d_name;
            if (name[0] == '.') continue;
            size_t len = strlen(name);
            char path[48 + 16];
            if (len > 40) {
                ESP_LOGW(TAG, "Rescan: malformed name skipped (possibly corrupt) in " STATS_DIR ": [%02X%02X%02X%02X%02X%02X%02X%02X]",
                         (unsigned char)name[0], (unsigned char)name[1], (unsigned char)name[2], (unsigned char)name[3],
                         (unsigned char)name[4], (unsigned char)name[5], (unsigned char)name[6], (unsigned char)name[7]);
                bad++;
                continue;
            }
            snprintf(path, sizeof(path), STATS_DIR "/%.40s", name);  /* 위에서 40자 초과는 이미 제외 */

            unsigned week = 0, scale = 0;
            char tail[8];
            bool is_raw = (len == 13 && sscanf(name, "r%8u.%3s", &week, tail) == 2 && strcmp(tail, "bin") == 0);
            bool is_agg = (!is_raw && len == 15 && sscanf(name, "a%1u_%8u.%3s", &scale, &week, tail) == 3 &&
                           strcmp(tail, "bin") == 0 && scale < STATS_SCALE_COUNT);
            if (!is_raw && !is_agg) {
                if (len > 4 && strcmp(name + len - 4, ".tmp") == 0) {
                    unlink(path);  /* 예전 방식(다시 쓰기) 정리 도중 리셋으로 남은 임시파일 */
                } else if (strncmp(name, "values", 6) == 0 || strncmp(name, "agg_", 4) == 0) {
                    ESP_LOGW(TAG, "Rescan: old-format file not read: %s", name);
                } else {
                    ESP_LOGW(TAG, "Rescan: unknown entry skipped (possibly corrupt): %s", name);
                    bad++;
                }
                continue;
            }

            struct stat st;
            if (stat(path, &st) != 0 || st.st_size < 0 || (sd_total > 0 && (uint64_t)st.st_size > sd_total)) {
                ESP_LOGW(TAG, "Rescan: bad size skipped (possibly corrupt): %s", name);
                bad++;
                continue;
            }
            int widx = week_get_or_add_locked(week);
            if (widx < 0) continue;
            if (is_raw) {
                uint64_t aligned = align_file_size(path, (uint64_t)st.st_size, sizeof(stats_record_t));
                uint32_t n = (uint32_t)(aligned / sizeof(stats_record_t));
                s_weeks[widx].raw_count = n;
                s_total_raw += n;
            } else {
                s_weeks[widx].agg_bytes += align_file_size(path, (uint64_t)st.st_size, sizeof(stats_bucket_t));
            }
        }
        closedir(d);
    }
    /* 2026-10-15(SD 개선 2단계) — 빈 SD(새 카드·포맷)인데 내부 플래시에 이미 옮긴 기록이 있으면 처음부터 다시 옮김(7일치 복원) */
    if (d && s_week_count == 0 && s_lfs_nd > 0 && (s_mark.day != 0 || s_mark.off != 0 || s_mark.agg_t != 0)) {
        s_mark.day = 0;
        s_mark.off = 0;
        s_mark.agg_t = 0;
        mark_save_locked();
        s_restore_gen++;
        s_sy.restores++;
        ESP_LOGW(TAG, "Empty SD stats - restoring %u values from flash", (unsigned)lfs_unsynced_locked());
    }
    s_sync_requested = true;   /* 마운트·재연결·포맷 뒤 — 다음 분에 옮김 */
    unlock();
    if (out_bad_entries) *out_bad_entries = bad;
}


/* ── 2026-10-08(실측 — 센스 깨움 처리 중 SD 쓰기·집계가 600ms~1.1초 걸려 응답(ACK)이 늦어지고, 센스가 광고 스윕으로
 * 넘어가 12분마다 "Link report"가 남음) — 통신 경로(콘 ESP-NOW 처리, 우선순위 17)는 큐에 넣기만, SD 쓰기는 이 태스크
 * (파일 작업 우선순위 10 — 통신17/SR15/파일10)가 함. 큐 칸·스택은 PSRAM ── */
#define APPEND_Q_DEPTH 32
#define APPEND_MAX_RECS 8
/* 2026-10-08 — fn이 있으면 측정값 대신 "통신 밖에서 할 저장 작업"(stats_store_post_job) */
typedef struct {
    uint8_t count;
    stats_record_t recs[APPEND_MAX_RECS];
    stats_store_job_fn_t fn;
    uint8_t arg[STATS_STORE_JOB_ARG_MAX];
} append_item_t;
static QueueHandle_t  s_append_q = NULL;
static StaticQueue_t  s_append_q_struct;
static StaticTask_t   s_append_tcb;
static append_item_t *s_append_rx = NULL;
static append_item_t *s_append_tx = NULL;
static SemaphoreHandle_t s_append_tx_mutex = NULL;

static void append_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xQueueReceive(s_append_q, s_append_rx, portMAX_DELAY) == pdTRUE) {
            if (s_append_rx->fn) s_append_rx->fn(s_append_rx->arg);
            else stats_store_append_batch(s_append_rx->recs, s_append_rx->count);
        }
    }
}

static bool append_task_start(void)
{
    if (s_append_q) return true;
    uint8_t *q_storage = heap_caps_malloc(sizeof(append_item_t) * APPEND_Q_DEPTH, MALLOC_CAP_SPIRAM);
    StackType_t *stack = heap_caps_malloc(6144, MALLOC_CAP_SPIRAM);
    s_append_rx = heap_caps_malloc(sizeof(append_item_t), MALLOC_CAP_SPIRAM);
    s_append_tx = heap_caps_malloc(sizeof(append_item_t), MALLOC_CAP_SPIRAM);
    s_append_tx_mutex = xSemaphoreCreateMutex();
    if (!q_storage || !stack || !s_append_rx || !s_append_tx || !s_append_tx_mutex) {
        heap_caps_free(q_storage); heap_caps_free(stack); heap_caps_free(s_append_rx); heap_caps_free(s_append_tx);
        if (s_append_tx_mutex) vSemaphoreDelete(s_append_tx_mutex);
        s_append_rx = s_append_tx = NULL; s_append_tx_mutex = NULL;
        return false;
    }
    s_append_q = xQueueCreateStatic(APPEND_Q_DEPTH, sizeof(append_item_t), q_storage, &s_append_q_struct);
    xTaskCreateStaticPinnedToCore(append_task, "stats_wr", 6144 / sizeof(StackType_t), NULL, 10, stack, &s_append_tcb, 1);
    return true;
}

bool stats_store_append_async(const stats_record_t *records, uint32_t count)
{
    if (count == 0 || !records) return true;
    if (!append_task_start()) return stats_store_append_batch(records, count);  /* 메모리 없으면 예전처럼 바로 씀 */
    bool ok = true;
    xSemaphoreTake(s_append_tx_mutex, portMAX_DELAY);   /* 조립 칸은 PSRAM 하나(스택에 큰 데이터 안 둠) — 큐가 복사함 */
    s_append_tx->fn = NULL;
    while (count > 0) {
        s_append_tx->count = (uint8_t)(count > APPEND_MAX_RECS ? APPEND_MAX_RECS : count);
        memcpy(s_append_tx->recs, records, s_append_tx->count * sizeof(stats_record_t));
        if (xQueueSend(s_append_q, s_append_tx, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Append queue full - %u values lost", (unsigned)s_append_tx->count);
            ok = false;
        }
        records += s_append_tx->count;
        count -= s_append_tx->count;
    }
    xSemaphoreGive(s_append_tx_mutex);
    return ok;
}

bool stats_store_post_job(stats_store_job_fn_t fn, const void *arg, size_t arg_len)
{
    if (!fn || arg_len > STATS_STORE_JOB_ARG_MAX) return false;
    if (!append_task_start()) { fn((const uint8_t *)arg); return true; }  /* 메모리 없으면 예전처럼 바로 */
    xSemaphoreTake(s_append_tx_mutex, portMAX_DELAY);
    s_append_tx->fn = fn;
    s_append_tx->count = 0;
    memset(s_append_tx->arg, 0, sizeof(s_append_tx->arg));
    if (arg && arg_len) memcpy(s_append_tx->arg, arg, arg_len);
    bool ok = xQueueSend(s_append_q, s_append_tx, 0) == pdTRUE;
    s_append_tx->fn = NULL;
    xSemaphoreGive(s_append_tx_mutex);
    if (!ok) ESP_LOGW(TAG, "Append queue full - job dropped");
    return ok;
}
