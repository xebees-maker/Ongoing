#include "photo_storage.h"
#include "sd_storage.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "photo_storage";

/* 파일명: <kind 1글자><8자리 십진수 순번>.jpg — 카메라 폴더 하나당 최대 99,999,999장
 * (콘은 CAM의 예전 500장 링버퍼와 달리 영구 저장소라 base36 4자리보다 훨씬 넉넉하게 잡음,
 * 십진수라 별도 인코더 없이 snprintf/strtoul로 충분) */
#define PHOTO_SEQ_DIGITS  8
#define PHOTO_FNAME_LEN   (1 + PHOTO_SEQ_DIGITS + 4)  /* kind + 8자리 + ".jpg" */

static void mac_to_hex(const uint8_t mac[6], char out[13])
{
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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

bool photo_storage_save(const uint8_t mac[6], uint8_t kind, const uint8_t *jpeg, size_t len,
                         uint32_t *out_seq)
{
    if (!sd_storage_is_mounted()) {
        ESP_LOGW(TAG, "저장 실패 — SD 미마운트");
        return false;
    }
    if (!is_valid_kind(kind)) {
        ESP_LOGW(TAG, "저장 실패 — 잘못된 kind=%c", (char)kind);
        return false;
    }

    char dir_path[64];
    camera_dir_path(mac, dir_path, sizeof(dir_path));
    if (mkdir(dir_path, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "카메라 폴더 생성 실패(%s, errno=%d)", dir_path, errno);
        return false;
    }

    uint32_t seq = next_seq_in_dir(dir_path);

    char file_path[96];
    snprintf(file_path, sizeof(file_path), "%s/%c%0*u.jpg", dir_path, (char)kind, PHOTO_SEQ_DIGITS, (unsigned)seq);

    FILE *fp = fopen(file_path, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "파일 열기 실패: %s", file_path);
        return false;
    }
    size_t written = fwrite(jpeg, 1, len, fp);
    fclose(fp);
    if (written != len) {
        ESP_LOGW(TAG, "쓰기 불완전(%u/%u bytes) — 파일 삭제: %s", (unsigned)written, (unsigned)len, file_path);
        unlink(file_path);
        return false;
    }

    if (out_seq) *out_seq = seq;
    ESP_LOGI(TAG, "사진 저장 완료: %s (%u bytes)", file_path, (unsigned)len);
    return true;
}

uint64_t photo_storage_get_used_bytes(void)
{
    if (!sd_storage_is_mounted()) return 0;

    char photos_root[32];
    snprintf(photos_root, sizeof(photos_root), "%s/photos", SD_STORAGE_MOUNT_POINT);
    DIR *root = opendir(photos_root);
    if (!root) return 0;

    uint64_t total = 0;
    struct dirent *cam_ent;
    while ((cam_ent = readdir(root)) != NULL) {
        if (cam_ent->d_name[0] == '.') continue;  /* "."/".." 건너뜀 */
        char cam_dir[320];
        snprintf(cam_dir, sizeof(cam_dir), "%s/%s", photos_root, cam_ent->d_name);
        DIR *dir = opendir(cam_dir);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char path[640];
            snprintf(path, sizeof(path), "%s/%s", cam_dir, ent->d_name);
            struct stat st;
            if (stat(path, &st) == 0) total += (uint64_t)st.st_size;
        }
        closedir(dir);
    }
    closedir(root);
    return total;
}

/* 전체 카메라 폴더를 훑어 mtime이 가장 오래된 파일 하나의 경로를 찾음 — 못 찾으면 false */
static bool find_oldest_file(char *out_path, size_t out_path_len)
{
    char photos_root[32];
    snprintf(photos_root, sizeof(photos_root), "%s/photos", SD_STORAGE_MOUNT_POINT);
    DIR *root = opendir(photos_root);
    if (!root) return false;

    bool found = false;
    time_t oldest_mtime = 0;
    struct dirent *cam_ent;
    while ((cam_ent = readdir(root)) != NULL) {
        if (cam_ent->d_name[0] == '.') continue;
        char cam_dir[320];
        snprintf(cam_dir, sizeof(cam_dir), "%s/%s", photos_root, cam_ent->d_name);
        DIR *dir = opendir(cam_dir);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char path[640];
            snprintf(path, sizeof(path), "%s/%s", cam_dir, ent->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            if (!found || st.st_mtime < oldest_mtime) {
                oldest_mtime = st.st_mtime;
                strncpy(out_path, path, out_path_len - 1);
                out_path[out_path_len - 1] = '\0';
                found = true;
            }
        }
        closedir(dir);
    }
    closedir(root);
    return found;
}

uint32_t photo_storage_trim_to(uint64_t target_bytes)
{
    if (!sd_storage_is_mounted()) return 0;

    uint32_t deleted = 0;
    while (photo_storage_get_used_bytes() > target_bytes) {
        char oldest[96];
        if (!find_oldest_file(oldest, sizeof(oldest))) break;  /* 더 지울 게 없음 */
        if (unlink(oldest) != 0) {
            ESP_LOGW(TAG, "정리 중 삭제 실패: %s", oldest);
            break;  /* 무한루프 방지 — 못 지우는 파일이면 중단 */
        }
        ESP_LOGI(TAG, "정리로 삭제: %s", oldest);
        deleted++;
    }
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
    if (unlink(file_path) != 0) {
        ESP_LOGW(TAG, "delete: 실패(errno=%d): %s", errno, file_path);
        return false;
    }
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
    struct dirent *ent;
    uint8_t kind; uint32_t seq;
    while ((ent = readdir(dir)) != NULL) {
        if (!parse_fname(ent->d_name, &kind, &seq)) continue;
        /* 320 — get_used_bytes()/find_oldest_file()의 cam_dir[320]과 동일 여유(d_name은
         * NAME_MAX=255까지 이론상 가능, GCC의 -Wformat-truncation을 만족시키려면 96으로는
         * 정적으로 증명 불가 — 실제로는 parse_fname()이 이미 PHOTO_FNAME_LEN 고정 길이만
         * 통과시킴) */
        char file_path[320];
        snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, ent->d_name);
        if (unlink(file_path) == 0) deleted++;
        else ESP_LOGW(TAG, "delete_all: 실패(errno=%d): %s", errno, file_path);
    }
    closedir(dir);
    ESP_LOGI(TAG, "delete_all: %u개 삭제 (%s)", (unsigned)deleted, dir_path);
    return deleted;
}
