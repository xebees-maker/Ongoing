/**
 * @file    stats_store.c
 * @brief   stats_store.h 구현 — /sdcard/stats/values.bin, 16바이트 고정 레코드.
 *
 *          레코드는 항상 esp_now_hub.c의 WAKE_HELLO_SENS 처리(s_nodes_mutex 아래, 단일
 *          지점)에서만 추가되므로 파일 내 순서 = 도착순 = Cntl 자기 벽시계 기준 시간순이
 *          항상 보장됨(단조증가). 이 성질을 이용해 "N시간 전부터" 조회는 순차 스캔 대신
 *          이진탐색으로 시작 오프셋만 찾고, 그 뒤부터 끝까지만 순차로 읽음.
 */
#include "stats_store.h"
#include "sd_storage.h"

#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "stats_store";
#define STATS_FILE_PATH SD_STORAGE_MOUNT_POINT "/stats/values.bin"

void stats_store_append(const uint8_t mac[6], uint8_t chan_type, uint8_t chan_index,
                         uint32_t unix_time, float value)
{
    FILE *f = fopen(STATS_FILE_PATH, "ab");
    if (!f) {
        ESP_LOGW(TAG, "값 파일 열기 실패(append) — SD 미마운트 등으로 추정, 이번 값은 유실");
        return;
    }
    stats_record_t rec = {
        .unix_time  = unix_time,
        .chan_type  = chan_type,
        .chan_index = chan_index,
        .value      = value,
    };
    memcpy(rec.mac, mac, 6);
    fwrite(&rec, sizeof(rec), 1, f);
    fclose(f);
}

uint32_t stats_store_get_count(void)
{
    FILE *f = fopen(STATS_FILE_PATH, "rb");
    if (!f) return 0;
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
    if (!f) return 0;
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
    if (!f) return 0;

    /* 이진탐색 — unix_time >= cutoff인 첫 레코드 인덱스(레코드가 도착순=시간순 단조증가라
     * 가능, 위 파일 헤더 주석 참고). 순차 스캔 없이 log2(total)번의 직접 오프셋 접근만 함 */
    uint32_t lo = 0, hi = total;  /* [lo, hi) 반개구간, hi=total이면 "cutoff 이후 레코드 없음" */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t t;
        if (!read_time_at(f, mid, &t)) { fclose(f); return 0; }
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

bool stats_store_get_min_max_avg_since(uint32_t cutoff_unix_time, uint8_t chan_type,
                                        float *out_min, float *out_max, float *out_avg)
{
    uint32_t total = stats_store_get_count();
    if (total == 0) return false;

    FILE *f = fopen(STATS_FILE_PATH, "rb");
    if (!f) return false;

    /* stats_store_read_since()와 동일한 이진탐색으로 시작 오프셋만 찾음 */
    uint32_t lo = 0, hi = total;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t t;
        if (!read_time_at(f, mid, &t)) { fclose(f); return false; }
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
