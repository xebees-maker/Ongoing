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
