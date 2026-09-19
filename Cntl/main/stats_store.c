/**
 * @file    stats_store.c
 * @brief   stats_store.h 구현 — /sdcard/stats/values_v2.bin, 17바이트 고정 레코드
 *          (2026-09-19, kind 필드 추가로 옛 values.bin에서 포맷 변경 — 옛 파일은 고아로
 *          남김, 마이그레이션은 PC ETL로 별도 처리).
 *
 *          레코드는 항상 esp_now_hub.c의 WAKE_HELLO_SENS 처리(s_nodes_mutex 아래, 단일
 *          지점)에서만 추가되므로 파일 내 순서 = 도착순 = Cntl 자기 벽시계 기준 시간순이
 *          항상 보장됨(단조증가). 이 성질을 이용해 "N시간 전부터" 조회는 순차 스캔 대신
 *          이진탐색으로 시작 오프셋만 찾고, 그 뒤부터 끝까지만 순차로 읽음.
 */
#include "stats_store.h"
#include "sd_storage.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

static const char *TAG = "stats_store";
#define STATS_FILE_PATH SD_STORAGE_MOUNT_POINT "/stats/values_v2.bin"

/* 2026-09-10(재설계 — fail/행/죽음 3분류 중 "fail" 처리, [[feedback_design_for_exceptions_not_just_fails]]) —
 * fopen() 실패(SD 자체 문제로 추정)와 "그냥 이 조건에 맞는 레코드가 없음"을 호출부가 구분할
 * 수 있어야, 통계탭이 "SD 이상이면 그 틱 전체를 즉시 중단"하는 회로차단기를 만들 수 있음
 * (사용자 지시: "SD 조회 fail이면, 다른 값도 믿을 수 없어. 즉시 중단이지"). 읽기 함수들
 * 각자 자기 시작 지점에서 false로 리셋하고, 자기(또는 내부에서 부르는 다른 stats_store
 * 함수)의 fopen()이 실패하면 true로 세팅 — 호출부는 함수가 리턴한 직후 이 값을 확인 */
static bool s_last_io_error = false;

/* ════════════════════════════════════════════════════════════
 * 그래프용 스케일별 사전집계 — [[project_cntl_stats_graph_redesign_2026_09_10]]
 * ════════════════════════════════════════════════════════════ */
const uint32_t STATS_SCALE_SECONDS[STATS_SCALE_COUNT] = { 3600, 43200, 86400, 259200, 604800 };

/* 2026-09-15 — mac을 저장 키에 추가하면서 레코드 크기가 바뀜(10->16바이트, _v2).
 * 2026-09-19(계열 자기서술 재설계) — kind 필드 추가로 다시 바뀜(16->17바이트, _v3) — 예전
 * 포맷 파일을 잘못 읽지 않도록 파일명 자체를 바꿔서 예전 파일은 그냥 고아로 남김(신뢰성
 * 항목들과 동일하게 "조용히 잘못 읽는 것"보다 "그냥 새로 시작"이 안전) */
static const char *s_agg_file_path[STATS_SCALE_COUNT] = {
    SD_STORAGE_MOUNT_POINT "/stats/agg_1h_v3.bin",
    SD_STORAGE_MOUNT_POINT "/stats/agg_12h_v3.bin",
    SD_STORAGE_MOUNT_POINT "/stats/agg_1d_v3.bin",
    SD_STORAGE_MOUNT_POINT "/stats/agg_3d_v3.bin",
    SD_STORAGE_MOUNT_POINT "/stats/agg_1w_v3.bin",
};

/* chan_type은 sensor_channel_type_t(esp_now_link.h)인데 이 파일은 저수준이라 그 헤더에
 * 의존 안 함 — 그냥 작은 고정크기 배열로 충분(현재 SENSOR_CHAN_TYPE_COUNT=5, 여유있게 8) */
#define STATS_AGG_MAX_CHAN_TYPES 8

/* ESP_NOW_HUB_MAX_NODES(esp_now_hub.h)와 같은 값이지만 이 파일은 저수준이라 그 헤더에
 * 의존 안 함(위 chan_type과 동일 원칙) — 실제 페어링 가능한 노드 수 이상은 어차피 안 옴 */
#define STATS_AGG_MAX_MACS 8

static uint8_t s_agg_known_macs[STATS_AGG_MAX_MACS][6];
static int     s_agg_known_mac_count = 0;

/* mac -> 누적 슬롯 인덱스. 처음 보는 mac이면 새로 등록, 꽉 찼으면 -1(그 장치의 사전집계는
 * 포기 — 원본 로그(values.bin)엔 여전히 남으므로 데이터 유실은 아님, 표시만 못 함) */
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

/* 방금 닫힌 버킷 하나를 그 스케일의 파일에 씀 — 실패해도 로그만(치명적 아님, 원본 기록
 * 자체는 이미 성공한 뒤라 데이터 유실은 이 사전집계 한 포인트뿐) */
static void agg_flush_bucket(uint8_t scale_idx, uint8_t kind, uint8_t chan_type, const uint8_t mac[6],
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

    FILE *f = fopen(s_agg_file_path[scale_idx], "ab");
    if (!f) {
        ESP_LOGW(TAG, "사전집계 버킷 열기 실패(scale=%u) — 이 포인트만 유실", (unsigned)scale_idx);
        return;
    }
    fwrite(&rec, sizeof(rec), 1, f);
    fclose(f);
}

/* 2026-09-15(사용자 설계 — 그래프 결측/보간 대화, "평균 내지 않는다") — 예전엔 버킷당
 * 항상 고정 2표본을 평균했지만(2026-09-12), 렌더링 시점에 국소 회귀로 추세선을 그리는
 * 쪽으로 설계가 바뀌면서 평균 자체가 필요 없어짐 — 대신 그 버킷 구간에 들어온 실측값들
 * 중 "버킷 중앙시각에 가장 가까운 값" 하나를 그대로(합성 없이) 저장한다. 이러면:
 *   - 저장값이 항상 실제 있었던 실측값 그 자체라서, 좁은 스케일의 값이 넓은 스케일보다
 *     더 극단적일 수 없다는 부등식이 자동으로 지켜짐(평균/합성이 없으니 다시 깨질 수가
 *     없음 — 2026-09-12에 고생해서 고친 문제의 재발 없이 해결).
 *   - "마지막 값" 대신 "중앙에 가장 가까운 값"을 쓰는 이유: 마지막 값은 버킷 후반부에
 *     우연히 들어온 값이 항상 이겨서 구조적으로 편향됨(사용자 질문 "지난 값을 하는 경우
 *     놓치는 경우가 있을까봐" — 그 우려대로).
 * stats_agg_update()가 raw 기록 성공 직후 레코드마다 호출 */
static void stats_agg_update_scale(uint8_t scale, int mac_slot, const uint8_t mac[6], uint8_t kind,
                                    uint8_t chan_type, uint32_t unix_time, float value)
{
    uint32_t bucket_width = STATS_SCALE_SECONDS[scale] / STATS_AGG_POINTS_PER_SCALE;
    if (bucket_width == 0) bucket_width = 1;
    uint32_t bucket_start = (unix_time / bucket_width) * bucket_width;  /* 벽시계 정렬 */

    agg_accum_t *acc = &s_agg_accum[scale][chan_type][mac_slot];
    if (acc->have_value && acc->bucket_start != bucket_start) {
        agg_flush_bucket(scale, kind, chan_type, mac, acc);
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

static void stats_agg_update(const uint8_t mac[6], uint8_t kind, uint8_t chan_type,
                              uint32_t unix_time, float value)
{
    if (chan_type >= STATS_AGG_MAX_CHAN_TYPES) return;
    int mac_slot = agg_mac_slot(mac);
    if (mac_slot < 0) return;  /* STATS_AGG_MAX_MACS 초과 — 이 장치는 사전집계만 스킵(원본은 남음) */
    for (int scale = 0; scale < STATS_SCALE_COUNT; scale++) {
        stats_agg_update_scale((uint8_t)scale, mac_slot, mac, kind, chan_type, unix_time, value);
    }
}

/* idx번째 버킷 레코드만 읽음(이진탐색용) — stats_store.c의 read_time_at()과 동일 패턴 */
static bool agg_read_bucket_at(FILE *f, uint32_t idx, stats_bucket_t *out)
{
    if (fseek(f, (long)idx * (long)sizeof(stats_bucket_t), SEEK_SET) != 0) return false;
    return fread(out, sizeof(*out), 1, f) == 1;
}

uint32_t stats_agg_read_window(uint8_t scale_idx, uint8_t chan_type, const uint8_t mac[6],
                                uint32_t window_start_unix, uint32_t window_end_unix,
                                stats_bucket_t *out, uint32_t out_cap)
{
    if (scale_idx >= STATS_SCALE_COUNT || out_cap == 0) return 0;
    if (!sd_storage_is_mounted()) return 0;  /* 항목4와 동일 원칙 — 미마운트면 시도 자체 스킵 */

    /* 2026-09-11(사용자 지적 — "파일이 없거나 값이 없다고 문제가 아니잖아, 통상적인 경우일
     * 수 있지") — 이 스케일의 집계 파일이 아직 한 번도 안 만들어졌을 수 있음(예: 1주 스케일은
     * 실제로 1주일치 버킷이 한 번도 안 닫혔으면 파일 자체가 없음) — ENOENT는 "데이터 없음"이지
     * SD 고장(5009)이 아님. 그 외 errno(권한/마운트 해제 등 실제 I/O 이상)만 진짜 실패로 침 */
    errno = 0;
    FILE *f = fopen(s_agg_file_path[scale_idx], "rb");
    if (!f) {
        if (errno != ENOENT) s_last_io_error = true;
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) { fclose(f); return 0; }
    uint32_t total = (uint32_t)(fsize / (long)sizeof(stats_bucket_t));

    /* 이진탐색 — bucket_start_unix >= window_start_unix인 첫 레코드(파일 전체는 시간순
     * 단조증가 — 여러 chan_type이 섞여 있어도 "그 스케일의 버킷이 닫힌 순서"라 시간순 보장) */
    uint32_t lo = 0, hi = total;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        stats_bucket_t rec;
        if (!agg_read_bucket_at(f, mid, &rec)) { s_last_io_error = true; fclose(f); return 0; }
        if (rec.bucket_start_unix < window_start_unix) lo = mid + 1;
        else hi = mid;
    }

    fseek(f, (long)lo * (long)sizeof(stats_bucket_t), SEEK_SET);
    uint32_t picked = 0;
    stats_bucket_t rec;
    while (picked < out_cap && fread(&rec, sizeof(rec), 1, f) == 1) {
        if (rec.bucket_start_unix >= window_end_unix) break;
        if (rec.chan_type == chan_type && memcmp(rec.mac, mac, 6) == 0) out[picked++] = rec;
    }
    fclose(f);
    return picked;
}

uint32_t stats_agg_collect_macs(uint8_t chan_type, uint8_t out_macs[][6], uint8_t out_kinds[],
                                 uint32_t out_cap)
{
    if (out_cap == 0) return 0;
    if (!sd_storage_is_mounted()) return 0;

    /* 가장 넓은 스케일(1주) 파일 전체를 훑음 — 시간창 제한 없이, 이 chan_type을 보고한 적
     * 있는 서로 다른 (mac,kind)를 전부 찾음(agg_flush_bucket이 모든 스케일에 같은 mac
     * 집합을 쓰므로 1개 스케일만 봐도 충분) */
    errno = 0;
    FILE *f = fopen(s_agg_file_path[STATS_SCALE_COUNT - 1], "rb");
    if (!f) {
        if (errno != ENOENT) s_last_io_error = true;
        return 0;
    }

    uint32_t count = 0;
    stats_bucket_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        if (rec.chan_type != chan_type) continue;
        bool dup = false;
        for (uint32_t i = 0; i < count; i++) {
            if (memcmp(out_macs[i], rec.mac, 6) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (count >= out_cap) break;
        memcpy(out_macs[count], rec.mac, 6);
        out_kinds[count] = rec.kind;
        count++;
    }
    fclose(f);
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

/* 2026-09-11(SD 신뢰성 재설계 항목4) — WAKE_HELLO_SENS 처리(esp_now_hub.c의 recv_cb 컨텍스트,
 * LVGL 태스크가 아님)에서 쓰기실패가 나면 여기만 세팅. ui_main.c의 1초 주기 LVGL 타이머가
 * 매 틱 stats_store_take_write_io_error()로 test-and-clear해서 안전하게 가져감(LVGL API는
 * 항상 LVGL 태스크에서만 호출) */
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
    if (count == 0) return true;
    /* 2026-09-11(사용자 지적 — "마운트가 안됬는데 왜 리드라이트를 시도했지?") — 미마운트
     * 상태면 fopen 시도 자체를 안 함. 이건 "I/O 실패"(s_last_io_error)가 아니라 "애초에
     * 시도 안 함" — 미마운트는 이미 sd_storage_is_mounted()로 별도 판별되므로 여기서까지
     * I/O 실패로 잡으면 5008(마운트실패)/5009(I/O실패)가 같이 뜨는 모순이 생김 */
    if (!sd_storage_is_mounted()) return false;

    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    FILE *f = fopen(STATS_FILE_PATH, "ab");
    if (!f) {
        s_last_io_error = true;
        s_write_io_error_pending = true;
        ESP_LOGW(TAG, "값 파일 열기 실패(append) — SD 미마운트 등으로 추정, 이번 값 %u개 유실",
                 (unsigned)count);
        return false;
    }
    fwrite(records, sizeof(stats_record_t), count, f);
    fclose(f);

    /* 2026-09-11(그래프 재설계 항목2) — 원본 기록 성공 직후, 같은 값들로 스케일별 사전집계도
     * 갱신(항목12: 이건 각 스케일이 독립 파일이라 "여러 파일에 걸친 모아쓰기"는 의미가 없고
     * — 한 스케일이 한 번에 닫는 버킷은 사실상 항상 최대 1개뿐이라 애초에 모아쓸 게 없음) */
    for (uint32_t i = 0; i < count; i++) {
        stats_agg_update(records[i].mac, records[i].kind, records[i].chan_type,
                          records[i].unix_time, records[i].value);
    }

    s_stats_append_call_count++;
    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "MEMDIAG stats_store_append #%u(레코드 %u개): internal free %u -> %u (delta=%d)",
             (unsigned)s_stats_append_call_count, (unsigned)count, (unsigned)before, (unsigned)after,
             (int)before - (int)after);
    return true;
}

uint32_t stats_store_get_count(void)
{
    s_last_io_error = false;
    /* 2026-09-11 — 위 stats_store_append_batch()와 동일 이유. 이 함수는 read_page/
     * read_since/get_min_max_avg_since/trim_to/get_used_bytes가 전부 내부에서 먼저
     * 부르므로, 여기서 한 번만 막아도 그 아래 함수들의 fopen 시도까지 자연히 다 같이 막힘 */
    if (!sd_storage_is_mounted()) return 0;
    /* 2026-09-19(실기 확인 — 파일명 바꾼 직후 5009 오탐) — 이 함수만 다른 읽기 함수들
     * (stats_agg_read_window 등)과 달리 ENOENT(파일이 아직 없음, 예: 방금 포맷을 바꿔서
     * 아직 한 번도 안 만들어진 경우)를 구분 안 하고 fopen 실패면 무조건 진짜 I/O 오류로
     * 잡았음 — "아직 데이터 없음"과 "SD 자체 문제"를 혼동해 5009를 오보고 */
    errno = 0;
    FILE *f = fopen(STATS_FILE_PATH, "rb");
    if (!f) {
        if (errno != ENOENT) s_last_io_error = true;
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);
    if (fsize < 0) return 0;
    return (uint32_t)(fsize / (long)sizeof(stats_record_t));
}

uint32_t stats_store_read_page(uint32_t page_index, uint32_t page_size,
                                stats_record_t *out, uint32_t out_cap)
{
    if (page_size == 0 || out_cap == 0) return 0;
    uint32_t total = stats_store_get_count();
    if (total == 0) return 0;

    uint64_t newest_shown_upto = (uint64_t)(page_index + 1) * page_size;  /* 이 페이지가 덮는 끝 */
    if (newest_shown_upto > total) newest_shown_upto = total;
    uint64_t oldest_in_page = (uint64_t)page_index * page_size;
    if (oldest_in_page >= total) return 0;  /* 존재하지 않는 페이지 */

    uint32_t count_in_page = (uint32_t)(newest_shown_upto - oldest_in_page);
    if (count_in_page > out_cap) count_in_page = out_cap;

    /* page_index=0이 "가장 최근" 페이지가 되도록, 파일 뒤에서부터 페이지 크기만큼 역산 */
    uint64_t start_record = total - newest_shown_upto;

    FILE *f = fopen(STATS_FILE_PATH, "rb");
    if (!f) { s_last_io_error = true; return 0; }
    fseek(f, (long)(start_record * sizeof(stats_record_t)), SEEK_SET);
    uint32_t got = (uint32_t)fread(out, sizeof(stats_record_t), count_in_page, f);
    fclose(f);
    return got;
}

bool stats_store_get_min_max(uint8_t chan_type, float *out_min, float *out_max)
{
    FILE *f = fopen(STATS_FILE_PATH, "rb");
    if (!f) return false;

    bool found = false;
    float mn = 0.0f, mx = 0.0f;
    stats_record_t buf[64];
    size_t got;
    while ((got = fread(buf, sizeof(stats_record_t), 64, f)) > 0) {
        for (size_t i = 0; i < got; i++) {
            if (buf[i].chan_type != chan_type) continue;
            if (!found) { mn = mx = buf[i].value; found = true; }
            else {
                if (buf[i].value < mn) mn = buf[i].value;
                if (buf[i].value > mx) mx = buf[i].value;
            }
        }
    }
    fclose(f);
    if (found) { *out_min = mn; *out_max = mx; }
    return found;
}

void stats_store_delete_all(void)
{
    if (remove(STATS_FILE_PATH) != 0) {
        ESP_LOGI(TAG, "값 파일 삭제 — 이미 없음(정상, 미마운트/최초상태 등)");
    } else {
        ESP_LOGI(TAG, "값 파일 삭제 완료");
    }
}

/* idx번째 레코드의 unix_time만 읽어옴(이진탐색용) */
static bool read_time_at(FILE *f, uint32_t idx, uint32_t *out_time)
{
    if (fseek(f, (long)idx * (long)sizeof(stats_record_t), SEEK_SET) != 0) return false;
    stats_record_t rec;
    if (fread(&rec, sizeof(rec), 1, f) != 1) return false;
    *out_time = rec.unix_time;
    return true;
}

uint32_t stats_store_read_since(uint32_t cutoff_unix_time, uint8_t chan_type,
                                 stats_record_t *out, uint32_t out_cap)
{
    if (out_cap == 0) return 0;
    uint32_t total = stats_store_get_count();
    if (total == 0) return 0;

    FILE *f = fopen(STATS_FILE_PATH, "rb");
    if (!f) { s_last_io_error = true; return 0; }

    /* 이진탐색 — unix_time >= cutoff인 첫 레코드 인덱스(레코드가 도착순=시간순 단조증가라
     * 가능, 위 파일 헤더 주석 참고). 순차 스캔 없이 log2(total)번의 직접 오프셋 접근만 함 */
    uint32_t lo = 0, hi = total;  /* [lo, hi) 반개구간, hi=total이면 "cutoff 이후 레코드 없음" */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t t;
        if (!read_time_at(f, mid, &t)) { s_last_io_error = true; fclose(f); return 0; }
        if (t < cutoff_unix_time) lo = mid + 1;
        else hi = mid;
    }
    uint32_t start_idx = lo;

    if (start_idx >= total) { fclose(f); return 0; }

    /* 1차 순차패스(이진탐색으로 찾은 지점부터 끝까지, 청크단위) — 이 chan_type 매치 개수만 셈 */
    fseek(f, (long)start_idx * (long)sizeof(stats_record_t), SEEK_SET);
    uint32_t match_count = 0;
    stats_record_t buf[64];
    size_t got;
    while ((got = fread(buf, sizeof(stats_record_t), 64, f)) > 0) {
        for (size_t i = 0; i < got; i++) if (buf[i].chan_type == chan_type) match_count++;
    }
    if (match_count == 0) { fclose(f); return 0; }

    uint32_t stride = (match_count + out_cap - 1) / out_cap;
    if (stride < 1) stride = 1;

    /* 2차 순차패스 — 같은 범위를 다시 읽으며 stride번째 매치마다 하나씩 out에 채움
     * (그래프는 다운샘플이 전제라 균등 간격 선택이면 충분, 사용자 지시: "부담되면 그냥
     * 그 순서의 값을 표시해도 되") */
    fseek(f, (long)start_idx * (long)sizeof(stats_record_t), SEEK_SET);
    uint32_t seen = 0, picked = 0;
    while (picked < out_cap && (got = fread(buf, sizeof(stats_record_t), 64, f)) > 0) {
        for (size_t i = 0; i < got && picked < out_cap; i++) {
            if (buf[i].chan_type != chan_type) continue;
            if ((seen % stride) == 0) out[picked++] = buf[i];
            seen++;
        }
    }
    fclose(f);
    return picked;
}

uint64_t stats_store_get_used_bytes(void)
{
    return (uint64_t)stats_store_get_count() * sizeof(stats_record_t);
}

/* 2026-09-12(사용자 지시 — "집계 잘릴 때 같이 잘라야 단순하잖아", 원 설계 항목9: "원본과
 * 같은 생명주기") — raw 트림 직후 남은 가장 오래된 raw 레코드 시각(cutoff_unix) 기준으로,
 * 5개 스케일 집계 파일에서도 그보다 오래된 버킷을 같이 지움. agg_read_bucket_at()/이진탐색은
 * stats_agg_read_window()와 동일 패턴 재사용 */
static void agg_trim_before(uint32_t cutoff_unix)
{
    for (int scale = 0; scale < STATS_SCALE_COUNT; scale++) {
        FILE *f = fopen(s_agg_file_path[scale], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        if (fsize <= 0) { fclose(f); continue; }
        uint32_t total = (uint32_t)(fsize / (long)sizeof(stats_bucket_t));

        uint32_t lo = 0, hi = total;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            stats_bucket_t rec;
            if (!agg_read_bucket_at(f, mid, &rec)) { lo = hi; break; }
            if (rec.bucket_start_unix < cutoff_unix) lo = mid + 1;
            else hi = mid;
        }
        if (lo == 0) { fclose(f); continue; }  /* 지울 게 없음 */

        char tmp_path[80];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", s_agg_file_path[scale]);
        FILE *dst = fopen(tmp_path, "wb");
        if (!dst) { fclose(f); continue; }
        fseek(f, (long)lo * (long)sizeof(stats_bucket_t), SEEK_SET);
        stats_bucket_t buf[64];
        size_t got;
        while ((got = fread(buf, sizeof(stats_bucket_t), 64, f)) > 0) {
            fwrite(buf, sizeof(stats_bucket_t), got, dst);
        }
        fclose(f);
        fclose(dst);
        if (remove(s_agg_file_path[scale]) == 0) {
            rename(tmp_path, s_agg_file_path[scale]);
            ESP_LOGI(TAG, "agg_trim_before: 스케일idx=%u %u개 버킷 삭제", (unsigned)scale, (unsigned)lo);
        } else {
            remove(tmp_path);
        }
    }
}

uint32_t stats_store_trim_to(uint64_t target_bytes)
{
    uint32_t total = stats_store_get_count();
    uint64_t target_records64 = target_bytes / sizeof(stats_record_t);
    uint32_t target_records = (target_records64 > total) ? total : (uint32_t)target_records64;
    if (total <= target_records) return 0;  /* 이미 목표 이하 */

    uint32_t drop_count = total - target_records;

    FILE *src = fopen(STATS_FILE_PATH, "rb");
    if (!src) return 0;
    if (fseek(src, (long)drop_count * (long)sizeof(stats_record_t), SEEK_SET) != 0) {
        fclose(src);
        return 0;
    }

    static const char *tmp_path = SD_STORAGE_MOUNT_POINT "/stats/values.bin.tmp";
    FILE *dst = fopen(tmp_path, "wb");
    if (!dst) {
        fclose(src);
        ESP_LOGW(TAG, "trim_to: 임시파일 열기 실패");
        return 0;
    }

    stats_record_t buf[64];
    size_t got;
    uint32_t new_oldest_unix = 0;
    bool have_new_oldest = false;
    while ((got = fread(buf, sizeof(stats_record_t), 64, src)) > 0) {
        if (!have_new_oldest) { new_oldest_unix = buf[0].unix_time; have_new_oldest = true; }
        fwrite(buf, sizeof(stats_record_t), got, dst);
    }
    fclose(src);
    fclose(dst);

    if (remove(STATS_FILE_PATH) != 0) {
        ESP_LOGW(TAG, "trim_to: 원본 삭제 실패");
        remove(tmp_path);
        return 0;
    }
    if (rename(tmp_path, STATS_FILE_PATH) != 0) {
        ESP_LOGW(TAG, "trim_to: 임시파일 교체(rename) 실패");
        return 0;
    }

    if (have_new_oldest) agg_trim_before(new_oldest_unix);

    ESP_LOGI(TAG, "trim_to: %u개 레코드 삭제(오래된 것부터), %u -> %u개 남음",
             (unsigned)drop_count, (unsigned)total, (unsigned)target_records);
    return drop_count;
}

bool stats_store_get_min_max_avg_since(uint32_t cutoff_unix_time, uint8_t chan_type,
                                        float *out_min, float *out_max, float *out_avg)
{
    uint32_t total = stats_store_get_count();
    if (total == 0) return false;

    FILE *f = fopen(STATS_FILE_PATH, "rb");
    if (!f) { s_last_io_error = true; return false; }

    /* stats_store_read_since()와 동일한 이진탐색으로 시작 오프셋만 찾음 */
    uint32_t lo = 0, hi = total;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t t;
        if (!read_time_at(f, mid, &t)) { s_last_io_error = true; fclose(f); return false; }
        if (t < cutoff_unix_time) lo = mid + 1;
        else hi = mid;
    }
    uint32_t start_idx = lo;
    if (start_idx >= total) { fclose(f); return false; }

    fseek(f, (long)start_idx * (long)sizeof(stats_record_t), SEEK_SET);
    bool found = false;
    float mn = 0.0f, mx = 0.0f;
    double sum = 0.0;
    uint32_t count = 0;
    stats_record_t buf[64];
    size_t got;
    while ((got = fread(buf, sizeof(stats_record_t), 64, f)) > 0) {
        for (size_t i = 0; i < got; i++) {
            if (buf[i].chan_type != chan_type) continue;
            if (!found) { mn = mx = buf[i].value; found = true; }
            else {
                if (buf[i].value < mn) mn = buf[i].value;
                if (buf[i].value > mx) mx = buf[i].value;
            }
            sum += (double)buf[i].value;
            count++;
        }
    }
    fclose(f);
    if (!found) return false;
    *out_min = mn;
    *out_max = mx;
    *out_avg = (float)(sum / (double)count);
    return true;
}
