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
 */
#include "stats_store.h"
#include "sd_storage.h"
#include "storage_mgr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "stats_store";

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

/* 호출부가 lock() 잡은 상태 — 버퍼가 없으면 한 번만 할당. 실패하면 false */
static bool ensure_alloc_locked(void)
{
    if (!s_weeks) s_weeks = heap_caps_calloc(STATS_MAX_WEEKS, sizeof(week_info_t), MALLOC_CAP_SPIRAM);
    if (!s_scratch) s_scratch = heap_caps_malloc(STATS_CHUNK * sizeof(stats_record_t), MALLOC_CAP_SPIRAM);
    if (!s_bscratch) s_bscratch = heap_caps_malloc(STATS_CHUNK * sizeof(stats_bucket_t), MALLOC_CAP_SPIRAM);
    if (!s_weeks || !s_scratch || !s_bscratch) {
        ESP_LOGE(TAG, "색인/읽기 버퍼 할당 실패");
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
        ESP_LOGE(TAG, "주 색인 가득참(%u) — week=%u 기록 불가", (unsigned)STATS_MAX_WEEKS, (unsigned)week);
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

static uint8_t s_agg_known_macs[STATS_AGG_MAX_MACS][6];
static int     s_agg_known_mac_count = 0;

/* mac -> 누적 슬롯 인덱스. 처음 보는 mac이면 새로 등록, 꽉 찼으면 -1(그 장치의 사전집계는
 * 포기 — 원시 기록엔 여전히 남으므로 데이터 유실은 아님, 표시만 못 함) */
static int agg_mac_slot(const uint8_t mac[6])
{
    for (int i = 0; i < s_agg_known_mac_count; i++) {
        if (memcmp(s_agg_known_macs[i], mac, 6) == 0) return i;
    }
    if (s_agg_known_mac_count >= STATS_AGG_MAX_MACS) return -1;
    memcpy(s_agg_known_macs[s_agg_known_mac_count], mac, 6);
    return s_agg_known_mac_count++;
}

/* 각 (스케일, chan_type, mac)마다 "지금 채워지는 중인" 버킷 하나만 RAM에 유지 — 재부팅하면
 * 그냥 리셋(진행 중이던 버킷은 유실, 사용자 설계: "다 채워진 후"에만 보이므로 문제 없음) */
typedef struct {
    uint32_t bucket_start;   /* 0 = 아직 시작 안 함 */
    bool     have_value;     /* 이 버킷에 후보값이 하나라도 들어왔는지 */
    uint32_t best_dist_abs;  /* 지금까지의 최적 후보가 버킷 중앙에서 떨어진 거리(초, 절대값) */
    float    best_value;     /* 버킷 중앙시각에 가장 가까운 실측값(합성/평균 없음) */
    uint16_t seen_count;     /* 이 버킷에 실제로 들어온 실측값 개수(진단용, sample_count로 기록) */
} agg_accum_t;

static agg_accum_t s_agg_accum[STATS_SCALE_COUNT][STATS_AGG_MAX_CHAN_TYPES][STATS_AGG_MAX_MACS];

/* 호출부가 lock() 잡은 상태 — 방금 닫힌 버킷 하나를 그 버킷이 속한 주의 집계 파일에 씀. 실패해도
 * 로그만(치명적 아님, 원시 기록 자체는 이미 성공한 뒤라 유실은 이 사전집계 한 포인트뿐) */
static void agg_flush_bucket_locked(uint8_t scale_idx, uint8_t kind, uint8_t chan_type, const uint8_t mac[6],
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

    uint32_t week = acc->bucket_start / STATS_WEEK_SEC;
    int widx = week_get_or_add_locked(week);
    if (widx < 0) return;

    char path[48];
    agg_path(scale_idx, week, path, sizeof(path));
    FILE *f = fopen(path, "ab");
    if (!f) {
        ESP_LOGW(TAG, "사전집계 버킷 열기 실패(scale=%u) — 이 포인트만 유실", (unsigned)scale_idx);
        return;
    }
    size_t n = fwrite(&rec, sizeof(rec), 1, f);
    close_synced(f);
    if (n == 1) s_weeks[widx].agg_bytes += sizeof(rec);
}

/* 2026-09-15(사용자 설계 — 그래프 결측/보간 대화, "평균 내지 않는다") — 버킷 구간에 들어온
 * 실측값들 중 "버킷 중앙시각에 가장 가까운 값" 하나를 그대로(합성 없이) 저장. 좁은 스케일의
 * 값이 넓은 스케일보다 더 극단적일 수 없다는 부등식이 자동으로 지켜지고, "마지막 값"처럼
 * 버킷 후반부로 편향되지도 않음. stats_agg_update()가 원시 기록 성공 직후 레코드마다 호출 */
static void stats_agg_update_scale_locked(uint8_t scale, int mac_slot, const uint8_t mac[6], uint8_t kind,
                                           uint8_t chan_type, uint32_t unix_time, float value)
{
    uint32_t bucket_width = STATS_SCALE_SECONDS[scale] / STATS_AGG_POINTS_PER_SCALE;
    if (bucket_width == 0) bucket_width = 1;
    uint32_t bucket_start = (unix_time / bucket_width) * bucket_width;  /* 벽시계 정렬 */

    agg_accum_t *acc = &s_agg_accum[scale][chan_type][mac_slot];
    if (acc->have_value && acc->bucket_start != bucket_start) {
        agg_flush_bucket_locked(scale, kind, chan_type, mac, acc);
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

static void stats_agg_update_locked(const uint8_t mac[6], uint8_t kind, uint8_t chan_type,
                                     uint32_t unix_time, float value)
{
    if (chan_type >= STATS_AGG_MAX_CHAN_TYPES) return;
    int mac_slot = agg_mac_slot(mac);
    if (mac_slot < 0) return;  /* STATS_AGG_MAX_MACS 초과 — 이 장치는 사전집계만 스킵(원본은 남음) */
    for (int scale = 0; scale < STATS_SCALE_COUNT; scale++) {
        stats_agg_update_scale_locked((uint8_t)scale, mac_slot, mac, kind, chan_type, unix_time, value);
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

uint32_t stats_agg_read_window(uint8_t scale_idx, uint8_t chan_type, const uint8_t mac[6],
                                uint32_t window_start_unix, uint32_t window_end_unix,
                                stats_bucket_t *out, uint32_t out_cap)
{
    if (scale_idx >= STATS_SCALE_COUNT || out_cap == 0 || window_end_unix <= window_start_unix) return 0;
    if (!sd_storage_is_mounted()) return 0;  /* 미마운트면 시도 자체 스킵 */

    lock();
    if (!ensure_alloc_locked()) { unlock(); return 0; }
    uint32_t w0 = window_start_unix / STATS_WEEK_SEC;
    uint32_t w1 = (window_end_unix - 1) / STATS_WEEK_SEC;
    uint32_t picked = 0;
    for (uint32_t i = 0; i < s_week_count && picked < out_cap; i++) {
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
                if (s_bscratch[k].bucket_start_unix >= window_end_unix) { done = true; break; }
                if (s_bscratch[k].chan_type == chan_type && memcmp(s_bscratch[k].mac, mac, 6) == 0) {
                    out[picked++] = s_bscratch[k];
                }
            }
        }
        fclose(f);
        if (done) break;
    }
    unlock();
    return picked;
}

uint32_t stats_agg_collect_macs(uint8_t chan_type, uint8_t out_macs[][6], uint8_t out_kinds[],
                                 uint32_t out_cap)
{
    if (out_cap == 0) return 0;
    if (!sd_storage_is_mounted()) return 0;

    /* 가장 넓은 스케일(1주)의 주별 집계 파일 전부를 훑음 — 시간창 제한 없이, 이 chan_type을
     * 보고한 적 있는 서로 다른 (mac,kind)를 전부 찾음(모든 스케일에 같은 mac 집합이 쓰이므로
     * 1개 스케일만 봐도 충분, 1주 스케일은 주당 버킷 수가 가장 적음) */
    lock();
    if (!ensure_alloc_locked()) { unlock(); return 0; }
    uint32_t count = 0;
    for (uint32_t i = 0; i < s_week_count; i++) {
        if (s_weeks[i].agg_bytes == 0) continue;
        char path[48];
        agg_path(STATS_SCALE_COUNT - 1, s_weeks[i].week, path, sizeof(path));
        FILE *f = open_read(path);
        if (!f) { if (s_last_io_error) break; continue; }
        size_t got;
        while ((got = fread(s_bscratch, sizeof(stats_bucket_t), STATS_CHUNK, f)) > 0) {
            for (size_t k = 0; k < got; k++) {
                const stats_bucket_t *rec = &s_bscratch[k];
                if (rec->chan_type != chan_type) continue;
                bool dup = false;
                for (uint32_t j = 0; j < count; j++) {
                    if (memcmp(out_macs[j], rec->mac, 6) == 0) { dup = true; break; }
                }
                if (dup || count >= out_cap) continue;
                memcpy(out_macs[count], rec->mac, 6);
                out_kinds[count] = rec->kind;
                count++;
            }
        }
        fclose(f);
    }
    unlock();
    return count;
}

bool stats_store_had_io_error(void)
{
    return s_last_io_error;
}

/* 2026-09-07(임시 진단 — 사용자 지시: "메모리 누수... 이유를 찾아야겠어") — 내부RAM이
 * 측정 성공 사이클마다 서서히 준다는 관찰을 이 함수(SD fopen/fwrite/fclose 반복)로 좁혀서
 * 실측 검증. 원인 확정되면 이 로그는 제거 예정 */
static size_t s_stats_append_call_count = 0;

/* 2026-09-11(SD 신뢰성 재설계 항목4) — WAKE_HELLO_SENS 처리(LVGL 태스크가 아님)에서 쓰기실패가
 * 나면 여기만 세팅. ui_main.c의 1초 주기 LVGL 타이머가 매 틱 stats_store_take_write_io_error()로
 * test-and-clear해서 안전하게 가져감(LVGL API는 항상 LVGL 태스크에서만 호출) */
static volatile bool s_write_io_error_pending = false;

bool stats_store_take_write_io_error(void)
{
    bool v = s_write_io_error_pending;
    s_write_io_error_pending = false;
    return v;
}

bool stats_store_append_batch(const stats_record_t *records, uint32_t count)
{
    s_last_io_error = false;
    if (count == 0 || !records) return true;
    /* 2026-09-11(사용자 지적 — "마운트가 안됬는데 왜 리드라이트를 시도했지?") — 미마운트
     * 상태면 fopen 시도 자체를 안 함("I/O 실패"가 아니라 "애초에 시도 안 함") */
    if (!sd_storage_is_mounted()) return false;

    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    bool ok = true;

    lock();
    if (!ensure_alloc_locked()) { unlock(); return false; }

    /* 한 번의 호출(WAKE_HELLO_SENS 1건)은 보통 같은 시각이라 같은 주 — 그래도 주가 바뀌는 경계에
     * 걸치면 주별로 나눠서 각자 파일에 씀(주마다 open+연속쓰기+fsync+close 1번) */
    uint32_t i = 0;
    while (i < count) {
        uint32_t week = records[i].unix_time / STATS_WEEK_SEC;
        uint32_t j = i + 1;
        while (j < count && records[j].unix_time / STATS_WEEK_SEC == week) j++;

        int widx = week_get_or_add_locked(week);
        if (widx < 0) { ok = false; i = j; continue; }

        char path[48];
        raw_path(week, path, sizeof(path));
        FILE *f = fopen(path, "ab");
        if (!f) {
            s_last_io_error = true;
            s_write_io_error_pending = true;
            ESP_LOGW(TAG, "값 파일 열기 실패(append, errno=%d) — 이번 값 %u개 유실", errno, (unsigned)(j - i));
            ok = false;
            i = j;
            continue;
        }
        size_t written = fwrite(&records[i], sizeof(stats_record_t), j - i, f);
        bool synced = close_synced(f);
        if (written != j - i || !synced) {
            s_last_io_error = true;
            s_write_io_error_pending = true;
            ESP_LOGW(TAG, "값 파일 쓰기 불완전(%u/%u, sync=%d)", (unsigned)written, (unsigned)(j - i), synced ? 1 : 0);
            ok = false;
        }
        s_weeks[widx].raw_count += (uint32_t)written;
        s_total_raw += written;

        /* 원시 기록이 된 레코드만 사전집계에 반영 */
        for (uint32_t k = i; k < i + (uint32_t)written; k++) {
            stats_agg_update_locked(records[k].mac, records[k].kind, records[k].chan_type,
                                    records[k].unix_time, records[k].value);
        }
        i = j;
    }
    unlock();

    storage_mgr_notify_changed();

    s_stats_append_call_count++;
    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "MEMDIAG stats_store_append #%u(레코드 %u개): internal free %u -> %u (delta=%d)",
             (unsigned)s_stats_append_call_count, (unsigned)count, (unsigned)before, (unsigned)after,
             (int)before - (int)after);
    return ok;
}

uint32_t stats_store_get_count(void)
{
    s_last_io_error = false;
    lock();
    uint64_t total = s_total_raw;
    unlock();
    return (total > UINT32_MAX) ? UINT32_MAX : (uint32_t)total;
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

uint32_t stats_store_read_page(uint32_t page_index, uint32_t page_size,
                                stats_record_t *out, uint32_t out_cap)
{
    s_last_io_error = false;
    if (page_size == 0 || out_cap == 0 || !out) return 0;
    if (!sd_storage_is_mounted()) return 0;

    lock();
    if (!ensure_alloc_locked()) { unlock(); return 0; }
    uint64_t total = s_total_raw;
    uint64_t newest_shown_upto = (uint64_t)(page_index + 1) * page_size;  /* 이 페이지가 덮는 끝 */
    if (newest_shown_upto > total) newest_shown_upto = total;
    uint64_t oldest_in_page = (uint64_t)page_index * page_size;
    if (oldest_in_page >= total) { unlock(); return 0; }  /* 존재하지 않는 페이지 */

    uint32_t count_in_page = (uint32_t)(newest_shown_upto - oldest_in_page);
    if (count_in_page > out_cap) count_in_page = out_cap;

    /* page_index=0이 "가장 최근" 페이지 — 전체(가장 오래된 주부터 이어 붙인 순서) 기준 시작 위치를
     * 역산한 뒤, 주 파일들을 따라가며 그 구간을 순서대로 읽음(오래된 것부터 채움) */
    uint64_t start_record = total - newest_shown_upto;
    uint64_t end_record = start_record + count_in_page;
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
        got += (uint32_t)n;
        if (n != (size_t)(to - from)) { s_last_io_error = true; break; }
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
    for (uint32_t i = 0; i < s_week_count; i++) {
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
    if (!sd_storage_is_mounted()) return 0;

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
    if (!sd_storage_is_mounted()) return false;
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

/* 호출부가 lock() 잡은 상태 — idx 주의 원시+집계 파일을 지우고 색인에서 뺌. 지운 원시 레코드 수 반환 */
static uint32_t delete_week_locked(uint32_t idx)
{
    uint32_t week = s_weeks[idx].week;
    uint32_t removed = s_weeks[idx].raw_count;
    char path[48];
    raw_path(week, path, sizeof(path));
    if (unlink(path) != 0 && errno != ENOENT) ESP_LOGW(TAG, "주 파일 삭제 실패(errno=%d): %s", errno, path);
    for (uint8_t s = 0; s < STATS_SCALE_COUNT; s++) {
        agg_path(s, week, path, sizeof(path));
        if (unlink(path) != 0 && errno != ENOENT) ESP_LOGW(TAG, "집계 파일 삭제 실패(errno=%d): %s", errno, path);
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
    /* 채워지던 버킷도 버림 — 지운 뒤에 옛 구간 버킷이 새로 써지지 않게 */
    memset(s_agg_accum, 0, sizeof(s_agg_accum));
    unlock();
    storage_mgr_notify_changed();
    ESP_LOGI(TAG, "측정값 파일 전체 삭제 완료");
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
    lock();
    if (s_weeks) {
        /* 가장 오래된 주부터 통째로 지움(다시 쓰기 없음). 지금 기록 중인 가장 최근 주는 남김 */
        while (s_week_count > 1 && used_bytes_locked() > target_bytes) {
            deleted += delete_week_locked(0);
            weeks++;
        }
    }
    unlock();
    if (weeks > 0) ESP_LOGI(TAG, "정리: 가장 오래된 %u주 삭제(레코드 %u개)", (unsigned)weeks, (unsigned)deleted);
    return deleted;
}

/* 파일 끝에 반쯤 쓰인 레코드가 있으면(쓰다 리셋) 레코드 경계까지 잘라냄 — 안 그러면 이후 이어
 * 붙이는 레코드가 전부 한 칸씩 어긋나서 읽힘. 남은 온전한 레코드 수(또는 집계 바이트) 반환 */
static uint64_t align_file_size(const char *path, uint64_t size, size_t rec_size)
{
    uint64_t aligned = size - (size % rec_size);
    if (aligned != size) {
        if (truncate(path, (off_t)aligned) == 0) {
            ESP_LOGW(TAG, "재스캔: 끝이 반쯤 쓰인 파일을 레코드 경계로 자름(%llu -> %llu): %s",
                     (unsigned long long)size, (unsigned long long)aligned, path);
        } else {
            ESP_LOGW(TAG, "재스캔: 반쯤 쓰인 파일 자르기 실패(errno=%d): %s", errno, path);
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
                ESP_LOGW(TAG, "재스캔: 이름이 비정상인 항목 제외(손상 의심)");
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
                    ESP_LOGW(TAG, "재스캔: 옛 형식 파일은 읽지 않음: %s", name);
                } else {
                    ESP_LOGW(TAG, "재스캔: 알 수 없는 항목 제외(손상 의심): %s", name);
                    bad++;
                }
                continue;
            }

            struct stat st;
            if (stat(path, &st) != 0 || st.st_size < 0 || (sd_total > 0 && (uint64_t)st.st_size > sd_total)) {
                ESP_LOGW(TAG, "재스캔: 크기가 비정상인 항목 제외(손상 의심): %s", name);
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
    unlock();
    if (out_bad_entries) *out_bad_entries = bad;
}
