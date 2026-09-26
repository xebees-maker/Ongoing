#include "photo_storage.h"
#include "sd_storage.h"
#include "storage_mgr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "photo_storage";

/* 파일명: <kind 1글자><8자리 십진수 순번>.jpg — 카메라 폴더 하나당 최대 99,999,999장
 * (콘은 CAM의 예전 500장 링버퍼와 달리 영구 저장소라 base36 4자리보다 훨씬 넉넉하게 잡음,
 * 십진수라 별도 인코더 없이 snprintf/strtoul로 충분) */
#define PHOTO_SEQ_DIGITS  8
#define PHOTO_FNAME_LEN   (1 + PHOTO_SEQ_DIGITS + 4)  /* kind + 8자리 + ".jpg" */

/* 2026-09-26(SD 안전정책 — 쓰다가 리셋돼도 반쯤 쓴 파일이 사진으로 안 보이게) — 먼저 이 확장자로
 * 다 쓰고 fsync+close한 뒤 최종 이름(.jpg)으로 rename. 재스캔 때 남아있는 .tmp는 지움 */
#define PHOTO_TMP_SUFFIX  ".tmp"

/* 2026-09-26(재스캔 방어) — 사진 한 장이 이보다 크면 손상 의심(OV5640 최대 약 1MB — 여유 있게) */
#define PHOTO_MAX_SANE_BYTES (16u * 1024u * 1024u)

/* ---- RAM 상태(2026-09-26, storage_mgr.h 참고) — 사용량 합계와 카메라별 다음 순번.
 * 예전엔 사용량을 매번 폴더 전체 스캔으로 구했고(LVGL 태스크에서 약 765ms), 저장할 때마다
 * next_seq_in_dir()로 카메라 폴더를 스캔했음. 이제 재스캔 때 한 번만 훑고 이후엔 여기서 관리 */
#define PHOTO_SEQ_CACHE_CAP 8  /* NODE_HUB_MAX_NODES와 같은 값(이 파일은 저수준이라 헤더 의존 안 함) */
typedef struct {
    uint8_t  mac[6];
    uint32_t next_seq;
    bool     used;
} seq_cache_t;

static SemaphoreHandle_t s_mutex = NULL;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_used_bytes = 0;
static seq_cache_t s_seq_cache[PHOTO_SEQ_CACHE_CAP];

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

static void used_add(int64_t delta)
{
    lock();
    if (delta < 0 && (uint64_t)(-delta) > s_used_bytes) s_used_bytes = 0;
    else s_used_bytes = (uint64_t)((int64_t)s_used_bytes + delta);
    unlock();
}

static void mac_to_hex(const uint8_t mac[6], char out[13])
{
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* mac_to_hex()의 역변환 — 폴더명(12자리 소문자 hex)이 아니면 false */
static bool hex_to_mac(const char *hex, uint8_t out[6])
{
    if (strlen(hex) != 12) return false;
    for (int i = 0; i < 6; i++) {
        unsigned byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return false;
        out[i] = (uint8_t)byte;
    }
    return true;
}

static void camera_dir_path(const uint8_t mac[6], char *out, size_t out_len)
{
    char mac_hex[13];
    mac_to_hex(mac, mac_hex);
    snprintf(out, out_len, "%s/photos/%s", SD_STORAGE_MOUNT_POINT, mac_hex);
}

static bool is_valid_kind(uint8_t kind)
{
    return kind == 'M' || kind == 'T';
}

static void file_path_for(const uint8_t mac[6], uint8_t kind, uint32_t seq, char *out, size_t out_len)
{
    char dir_path[64];
    camera_dir_path(mac, dir_path, sizeof(dir_path));
    snprintf(out, out_len, "%s/%c%0*u.jpg", dir_path, (char)kind, PHOTO_SEQ_DIGITS, (unsigned)seq);
}

/* 파일명("<kind><8자리>.jpg")을 파싱 — 형식이 아니면 false */
static bool parse_fname(const char *d_name, uint8_t *out_kind, uint32_t *out_seq)
{
    size_t len = strlen(d_name);
    if (len != PHOTO_FNAME_LEN) return false;
    if (!is_valid_kind((uint8_t)d_name[0])) return false;
    if (strcmp(d_name + 1 + PHOTO_SEQ_DIGITS, ".jpg") != 0) return false;

    char seq_str[PHOTO_SEQ_DIGITS + 1];
    memcpy(seq_str, d_name + 1, PHOTO_SEQ_DIGITS);
    seq_str[PHOTO_SEQ_DIGITS] = '\0';
    char *end = NULL;
    uint32_t seq = (uint32_t)strtoul(seq_str, &end, 10);
    if (end != seq_str + PHOTO_SEQ_DIGITS) return false;

    *out_kind = (uint8_t)d_name[0];
    *out_seq = seq;
    return true;
}

/* 이 카메라 폴더 안에서 가장 큰 순번+1(비어있으면 0) — 폴더가 아예 없어도 0(첫 저장 때
 * mkdir로 새로 만듦) */
static uint32_t next_seq_in_dir(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    uint32_t max_seq_plus_one = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len != PHOTO_FNAME_LEN) continue;
        if (!is_valid_kind((uint8_t)ent->d_name[0])) continue;
        if (strcmp(ent->d_name + 1 + PHOTO_SEQ_DIGITS, ".jpg") != 0) continue;

        char seq_str[PHOTO_SEQ_DIGITS + 1];
        memcpy(seq_str, ent->d_name + 1, PHOTO_SEQ_DIGITS);
        seq_str[PHOTO_SEQ_DIGITS] = '\0';
        char *end = NULL;
        uint32_t seq = (uint32_t)strtoul(seq_str, &end, 10);
        if (end != seq_str + PHOTO_SEQ_DIGITS) continue;  /* 자릿수 전부 숫자가 아니었음 */

        if (seq + 1 > max_seq_plus_one) max_seq_plus_one = seq + 1;
    }
    closedir(dir);
    return max_seq_plus_one;
}

/* 호출부가 lock() 잡은 상태 — 이 카메라의 다음 순번을 캐시에서 꺼내고 1 올림. 캐시에 없으면
 * (재스캔 이후 처음 보는 카메라) 그 폴더를 한 번만 스캔해서 채움 */
static uint32_t take_next_seq_locked(const uint8_t mac[6], const char *dir_path)
{
    int free_slot = -1;
    for (int i = 0; i < PHOTO_SEQ_CACHE_CAP; i++) {
        if (s_seq_cache[i].used && memcmp(s_seq_cache[i].mac, mac, 6) == 0) {
            return s_seq_cache[i].next_seq++;
        }
        if (!s_seq_cache[i].used && free_slot < 0) free_slot = i;
    }
    uint32_t seq = next_seq_in_dir(dir_path);
    if (free_slot >= 0) {
        memcpy(s_seq_cache[free_slot].mac, mac, 6);
        s_seq_cache[free_slot].next_seq = seq + 1;
        s_seq_cache[free_slot].used = true;
    }
    /* 캐시가 꽉 찼으면(카메라 9대 이상 — 실제로는 안 옴) 캐시 없이 매번 스캔으로 동작 */
    return seq;
}

/* 2026-09-26(설계 §4 SR 수신 — 콘은 받는 대로 이어 씀) — 한 장을 여러 번에 나눠 쓰는 저장기.
 * begin에서 순번을 잡고 <최종이름>.tmp를 열고, append로 이어 쓰고, finish에서 fsync+close 후 최종 이름으로
 * rename(SD 안전정책 그대로 — 쓰다 리셋돼도 반쪽 사진이 .jpg로 안 보임). 실패/중단은 abort(임시파일 삭제).
 * 잡은 순번은 중단돼도 되돌리지 않음(순번에 빈 칸이 생길 뿐) */
struct photo_storage_writer {
    FILE    *fp;
    size_t   written;
    uint32_t seq;
    bool     failed;
    char     file_path[96];
    char     tmp_path[104];
};

photo_storage_writer_t *photo_storage_begin(const uint8_t mac[6], uint8_t kind)
{
    if (!sd_storage_is_mounted()) {
        ESP_LOGW(TAG, "저장 시작 실패 — SD 미마운트");
        return NULL;
    }
    if (!is_valid_kind(kind)) {
        ESP_LOGW(TAG, "저장 시작 실패 — 잘못된 kind=%c", (char)kind);
        return NULL;
    }
    char dir_path[64];
    camera_dir_path(mac, dir_path, sizeof(dir_path));
    if (mkdir(dir_path, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "카메라 폴더 생성 실패(%s, errno=%d)", dir_path, errno);
        return NULL;
    }
    photo_storage_writer_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w) w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    lock();
    w->seq = take_next_seq_locked(mac, dir_path);
    unlock();
    snprintf(w->file_path, sizeof(w->file_path), "%s/%c%0*u.jpg", dir_path, (char)kind, PHOTO_SEQ_DIGITS, (unsigned)w->seq);
    snprintf(w->tmp_path, sizeof(w->tmp_path), "%s" PHOTO_TMP_SUFFIX, w->file_path);
    w->fp = fopen(w->tmp_path, "wb");
    if (!w->fp) {
        ESP_LOGW(TAG, "파일 열기 실패: %s (errno=%d)", w->tmp_path, errno);
        free(w);
        return NULL;
    }
    return w;
}

bool photo_storage_append(photo_storage_writer_t *w, const uint8_t *data, size_t len)
{
    if (!w || w->failed) return false;
    if (len == 0) return true;
    size_t n = fwrite(data, 1, len, w->fp);
    w->written += n;
    if (n != len) {
        ESP_LOGW(TAG, "쓰기 실패(%u/%u bytes): %s", (unsigned)n, (unsigned)len, w->tmp_path);
        w->failed = true;
    }
    return !w->failed;
}

bool photo_storage_finish(photo_storage_writer_t *w, uint32_t *out_seq)
{
    if (!w) return false;
    bool synced = (fflush(w->fp) == 0) && (fsync(fileno(w->fp)) == 0);
    fclose(w->fp);
    w->fp = NULL;
    if (w->failed || !synced || w->written == 0) {
        ESP_LOGW(TAG, "쓰기 불완전(%u bytes, sync=%d) — 임시파일 삭제: %s",
                 (unsigned)w->written, synced ? 1 : 0, w->tmp_path);
        unlink(w->tmp_path);
        free(w);
        return false;
    }
    if (rename(w->tmp_path, w->file_path) != 0) {
        ESP_LOGW(TAG, "임시파일 이름 바꾸기 실패(errno=%d): %s", errno, w->tmp_path);
        unlink(w->tmp_path);
        free(w);
        return false;
    }
    used_add((int64_t)w->written);
    storage_mgr_notify_changed();
    if (out_seq) *out_seq = w->seq;
    ESP_LOGI(TAG, "사진 저장 완료: %s (%u bytes)", w->file_path, (unsigned)w->written);
    free(w);
    return true;
}

void photo_storage_abort(photo_storage_writer_t *w)
{
    if (!w) return;
    if (w->fp) fclose(w->fp);
    unlink(w->tmp_path);
    ESP_LOGW(TAG, "저장 중단 — 임시파일 삭제: %s", w->tmp_path);
    free(w);
}

bool photo_storage_save(const uint8_t mac[6], uint8_t kind, const uint8_t *jpeg, size_t len,
                         uint32_t *out_seq)
{
    if (!jpeg || len == 0) {
        ESP_LOGW(TAG, "저장 실패 — 빈 데이터(len=%u)", (unsigned)len);
        return false;
    }
    photo_storage_writer_t *w = photo_storage_begin(mac, kind);
    if (!w) return false;
    if (!photo_storage_append(w, jpeg, len)) {
        photo_storage_abort(w);
        return false;
    }
    return photo_storage_finish(w, out_seq);
}

uint64_t photo_storage_get_used_bytes(void)
{
    lock();
    uint64_t v = s_used_bytes;
    unlock();
    return v;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

void photo_storage_rescan(uint64_t sd_total, uint32_t *out_bad_entries)
{
    uint32_t bad = 0;
    uint64_t total = 0;
    uint64_t sane_limit = PHOTO_MAX_SANE_BYTES;
    if (sd_total > 0 && sd_total < sane_limit) sane_limit = sd_total;

    /* 스캔 동안 저장(카메라 사진 도착)이 끼어들어 이중으로 세지 않게 전체를 잠금 —
     * 마운트/재연결/포맷 직후에만 도는 일이라 저장이 잠깐 기다리는 건 허용 */
    lock();
    memset(s_seq_cache, 0, sizeof(s_seq_cache));

    char photos_root[32];
    snprintf(photos_root, sizeof(photos_root), "%s/photos", SD_STORAGE_MOUNT_POINT);
    DIR *root = opendir(photos_root);
    if (root) {
        struct dirent *cam_ent;
        while ((cam_ent = readdir(root)) != NULL) {
            if (cam_ent->d_name[0] == '.') continue;  /* "."/".." 건너뜀 */
            uint8_t cam_mac[6];
            if (!hex_to_mac(cam_ent->d_name, cam_mac)) {
                ESP_LOGW(TAG, "재스캔: 알 수 없는 항목(카메라 폴더 아님) 제외: %.40s", cam_ent->d_name);
                bad++;
                continue;
            }
            char cam_dir[64];
            camera_dir_path(cam_mac, cam_dir, sizeof(cam_dir));
            DIR *dir = opendir(cam_dir);
            if (!dir) continue;

            uint32_t max_seq_plus_one = 0;
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char path[96 + 16];
                if (ends_with(ent->d_name, PHOTO_TMP_SUFFIX) && strlen(ent->d_name) < 32) {
                    /* 저장 도중 리셋으로 남은 반쯤 쓴 임시파일 — 지움 */
                    snprintf(path, sizeof(path), "%s/%.31s", cam_dir, ent->d_name);
                    if (unlink(path) == 0) ESP_LOGW(TAG, "재스캔: 남은 임시파일 삭제: %s", path);
                    continue;
                }
                uint8_t kind; uint32_t seq;
                if (!parse_fname(ent->d_name, &kind, &seq)) {
                    ESP_LOGW(TAG, "재스캔: 이름이 형식에 안 맞는 항목 제외(손상 의심): %.40s", ent->d_name);
                    bad++;
                    continue;
                }
                file_path_for(cam_mac, kind, seq, path, sizeof(path));  /* parse_fname 통과 = 고정 길이 이름 */
                struct stat st;
                if (stat(path, &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size > sane_limit) {
                    ESP_LOGW(TAG, "재스캔: 크기가 비정상인 항목 제외(손상 의심): %s", path);
                    bad++;
                    continue;
                }
                total += (uint64_t)st.st_size;
                if (seq + 1 > max_seq_plus_one) max_seq_plus_one = seq + 1;
            }
            closedir(dir);

            for (int i = 0; i < PHOTO_SEQ_CACHE_CAP; i++) {
                if (!s_seq_cache[i].used) {
                    memcpy(s_seq_cache[i].mac, cam_mac, 6);
                    s_seq_cache[i].next_seq = max_seq_plus_one;
                    s_seq_cache[i].used = true;
                    break;
                }
            }
        }
        closedir(root);
    }
    s_used_bytes = total;
    unlock();

    if (out_bad_entries) *out_bad_entries = bad;
}

/* ---- 정리 — 가장 오래된 것부터. 한 번 훑을 때 오래된 후보를 TRIM_BATCH개씩 모아서 지움
 * (예전엔 한 장 지울 때마다 전체를 두 번 훑었음 — get_used_bytes()+find_oldest_file()) ---- */
#define TRIM_BATCH 64
typedef struct {
    time_t   mtime;
    uint32_t size;
    uint32_t seq;
    uint8_t  kind;
    uint8_t  mac[6];
} trim_cand_t;

/* cands(오름차순, mtime 오래된 것부터)에 후보 하나를 끼워 넣음 — 꽉 찼으면 가장 새로운 걸 밀어냄 */
static void cand_insert(trim_cand_t *cands, int *count, const trim_cand_t *c)
{
    int n = *count;
    if (n == TRIM_BATCH && c->mtime >= cands[n - 1].mtime) return;
    int pos = (n < TRIM_BATCH) ? n : TRIM_BATCH - 1;
    while (pos > 0 && cands[pos - 1].mtime > c->mtime) {
        cands[pos] = cands[pos - 1];
        pos--;
    }
    cands[pos] = *c;
    if (n < TRIM_BATCH) *count = n + 1;
}

static int collect_oldest(trim_cand_t *cands, uint64_t sane_limit)
{
    int count = 0;
    char photos_root[32];
    snprintf(photos_root, sizeof(photos_root), "%s/photos", SD_STORAGE_MOUNT_POINT);
    DIR *root = opendir(photos_root);
    if (!root) return 0;
    struct dirent *cam_ent;
    while ((cam_ent = readdir(root)) != NULL) {
        uint8_t cam_mac[6];
        if (cam_ent->d_name[0] == '.' || !hex_to_mac(cam_ent->d_name, cam_mac)) continue;
        char cam_dir[64];
        camera_dir_path(cam_mac, cam_dir, sizeof(cam_dir));
        DIR *dir = opendir(cam_dir);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            trim_cand_t c;
            if (!parse_fname(ent->d_name, &c.kind, &c.seq)) continue;  /* 손상 의심 항목은 정리 대상 아님 */
            char path[96];
            file_path_for(cam_mac, c.kind, c.seq, path, sizeof(path));
            struct stat st;
            if (stat(path, &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size > sane_limit) continue;
            c.mtime = st.st_mtime;
            c.size = (uint32_t)st.st_size;
            memcpy(c.mac, cam_mac, 6);
            cand_insert(cands, &count, &c);
        }
        closedir(dir);
    }
    closedir(root);
    return count;
}

uint32_t photo_storage_trim_to(uint64_t target_bytes)
{
    if (!sd_storage_is_mounted()) return 0;

    static trim_cand_t *s_cands = NULL;  /* 파일처리 태스크 전용, PSRAM 한 번만 */
    if (!s_cands) {
        s_cands = heap_caps_malloc(sizeof(trim_cand_t) * TRIM_BATCH, MALLOC_CAP_SPIRAM);
        if (!s_cands) { ESP_LOGE(TAG, "정리 후보 버퍼 할당 실패 — 정리 안 함"); return 0; }
    }

    uint32_t deleted = 0;
    while (photo_storage_get_used_bytes() > target_bytes) {
        int n = collect_oldest(s_cands, PHOTO_MAX_SANE_BYTES);
        if (n == 0) break;  /* 더 지울 게 없음 */
        bool progress = false;
        for (int i = 0; i < n && photo_storage_get_used_bytes() > target_bytes; i++) {
            char path[96];
            file_path_for(s_cands[i].mac, s_cands[i].kind, s_cands[i].seq, path, sizeof(path));
            if (unlink(path) != 0) {
                ESP_LOGW(TAG, "정리 중 삭제 실패(errno=%d): %s", errno, path);
                continue;
            }
            used_add(-(int64_t)s_cands[i].size);
            deleted++;
            progress = true;
        }
        if (!progress) break;  /* 이번 묶음을 하나도 못 지움 — 무한루프 방지 */
    }
    if (deleted > 0) ESP_LOGI(TAG, "정리: %u장 삭제(오래된 것부터)", (unsigned)deleted);
    return deleted;
}

uint32_t photo_storage_get_count(const uint8_t mac[6])
{
    char dir_path[64];
    camera_dir_path(mac, dir_path, sizeof(dir_path));
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    uint32_t count = 0;
    struct dirent *ent;
    uint8_t kind; uint32_t seq;
    while ((ent = readdir(dir)) != NULL) {
        if (parse_fname(ent->d_name, &kind, &seq)) count++;
    }
    closedir(dir);
    return count;
}

/* seq 내림차순 정렬용(qsort) — 최신(seq 큰 것)이 앞 */
static int cmp_seq_desc(const void *a, const void *b)
{
    uint32_t sa = ((const uint32_t *)a)[0];
    uint32_t sb = ((const uint32_t *)b)[0];
    if (sa < sb) return 1;
    if (sa > sb) return -1;
    return 0;
}

uint32_t photo_storage_read_page(const uint8_t mac[6], uint32_t page_index, uint32_t page_size,
                                  photo_storage_item_t *out, uint32_t out_cap)
{
    char dir_path[64];
    camera_dir_path(mac, dir_path, sizeof(dir_path));
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    /* 1차: 전체 개수 세기(스크래치 배열 크기 결정용) */
    uint32_t total = 0;
    struct dirent *ent;
    uint8_t kind; uint32_t seq;
    while ((ent = readdir(dir)) != NULL) {
        if (parse_fname(ent->d_name, &kind, &seq)) total++;
    }
    if (total == 0) { closedir(dir); return 0; }

    /* {seq, kind} 쌍을 uint32_t 2개로 — seq가 정렬 키, kind는 나란히 들고만 감(인코딩 아님,
     * 정렬 후 짝을 잃지 않기 위한 내부 스크래치 구조일 뿐) */
    uint32_t *scratch = heap_caps_malloc((size_t)total * 2 * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!scratch) { closedir(dir); ESP_LOGW(TAG, "read_page: 스크래치 할당 실패(total=%u)", (unsigned)total); return 0; }

    rewinddir(dir);
    uint32_t filled = 0;
    while ((ent = readdir(dir)) != NULL && filled < total) {
        if (!parse_fname(ent->d_name, &kind, &seq)) continue;
        scratch[filled * 2 + 0] = seq;
        scratch[filled * 2 + 1] = (uint32_t)kind;
        filled++;
    }
    closedir(dir);

    qsort(scratch, filled, 2 * sizeof(uint32_t), cmp_seq_desc);

    uint64_t start = (uint64_t)page_index * page_size;
    uint32_t got = 0;
    for (uint64_t i = start; i < filled && got < page_size && got < out_cap; i++) {
        uint32_t item_seq  = scratch[i * 2 + 0];
        uint8_t  item_kind = (uint8_t)scratch[i * 2 + 1];

        char file_path[96];
        file_path_for(mac, item_kind, item_seq, file_path, sizeof(file_path));
        struct stat st;
        if (stat(file_path, &st) != 0) continue;  /* 스캔 이후 삭제됐을 수도 있음(드묾) — 건너뜀 */

        out[got].kind = item_kind;
        out[got].seq = item_seq;
        out[got].mtime = st.st_mtime;
        out[got].file_size = (size_t)st.st_size;
        got++;
    }

    heap_caps_free(scratch);
    return got;
}

bool photo_storage_read_file(const uint8_t mac[6], uint8_t kind, uint32_t seq,
                              uint8_t *out_buf, size_t buf_cap, size_t *out_len)
{
    char file_path[96];
    file_path_for(mac, kind, seq, file_path, sizeof(file_path));

    struct stat st;
    if (stat(file_path, &st) != 0) {
        ESP_LOGW(TAG, "read_file: 파일 없음: %s", file_path);
        return false;
    }
    if ((size_t)st.st_size > buf_cap) {
        ESP_LOGW(TAG, "read_file: 버퍼 부족(파일 %u > 버퍼 %u): %s",
                 (unsigned)st.st_size, (unsigned)buf_cap, file_path);
        return false;
    }

    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "read_file: 열기 실패: %s", file_path);
        return false;
    }
    size_t read_len = fread(out_buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (read_len != (size_t)st.st_size) {
        ESP_LOGW(TAG, "read_file: 읽기 불완전(%u/%u): %s", (unsigned)read_len, (unsigned)st.st_size, file_path);
        return false;
    }

    if (out_len) *out_len = read_len;
    return true;
}

bool photo_storage_delete(const uint8_t mac[6], uint8_t kind, uint32_t seq)
{
    char file_path[96];
    file_path_for(mac, kind, seq, file_path, sizeof(file_path));
    struct stat st;
    int64_t size = (stat(file_path, &st) == 0 && st.st_size > 0) ? (int64_t)st.st_size : 0;
    if (unlink(file_path) != 0) {
        ESP_LOGW(TAG, "delete: 실패(errno=%d): %s", errno, file_path);
        return false;
    }
    used_add(-size);  /* 삭제가 성공했을 때만 뺌 */
    storage_mgr_notify_changed();
    ESP_LOGI(TAG, "delete: %s", file_path);
    return true;
}

uint32_t photo_storage_delete_all(const uint8_t mac[6])
{
    char dir_path[64];
    camera_dir_path(mac, dir_path, sizeof(dir_path));
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    uint32_t deleted = 0;
    int64_t freed = 0;
    struct dirent *ent;
    uint8_t kind; uint32_t seq;
    while ((ent = readdir(dir)) != NULL) {
        if (!parse_fname(ent->d_name, &kind, &seq)) continue;
        /* parse_fname()이 PHOTO_FNAME_LEN 고정 길이만 통과시키므로 file_path_for()로 다시 만듦
         * (큰 스택 버퍼 없이) */
        char file_path[96];
        file_path_for(mac, kind, seq, file_path, sizeof(file_path));
        struct stat st;
        int64_t size = (stat(file_path, &st) == 0 && st.st_size > 0) ? (int64_t)st.st_size : 0;
        if (unlink(file_path) == 0) { deleted++; freed += size; }
        else ESP_LOGW(TAG, "delete_all: 실패(errno=%d): %s", errno, file_path);
    }
    closedir(dir);
    used_add(-freed);
    storage_mgr_notify_changed();
    ESP_LOGI(TAG, "delete_all: %u개 삭제 (%s)", (unsigned)deleted, dir_path);
    return deleted;
}

uint32_t photo_storage_list_camera_macs(uint8_t out_macs[][6], uint32_t out_cap)
{
    if (!sd_storage_is_mounted()) return 0;

    char photos_root[32];
    snprintf(photos_root, sizeof(photos_root), "%s/photos", SD_STORAGE_MOUNT_POINT);
    DIR *root = opendir(photos_root);
    if (!root) return 0;

    uint32_t count = 0;
    struct dirent *ent;
    while (count < out_cap && (ent = readdir(root)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (hex_to_mac(ent->d_name, out_macs[count])) count++;
    }
    closedir(root);
    return count;
}
