#include "cam_storage.h"
#include "bsp_esp32s3_cam.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "cam_storage";

static sdmmc_card_t *s_card = NULL;

static void file_path(uint32_t file_id, char kind, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%c%010u.jpg", BSP_CAM_SD_MOUNT_POINT, kind, (unsigned)file_id);
}

/* file_id만으로는 M(수동)/T(자동) 중 어느 쪽 파일인지 몰라서 둘 다 순서대로 존재 확인 —
 * 수동 촬영이 우선(같은 초에 자동 촬영과 겹치는 드문 경우 수동을 우선 취급) */
static bool resolve_path(uint32_t file_id, char *out, size_t out_len, char *out_kind)
{
    const char kinds[2] = { CAM_CAPTURE_KIND_MANUAL, CAM_CAPTURE_KIND_AUTO };
    for (int i = 0; i < 2; i++) {
        file_path(file_id, kinds[i], out, out_len);
        struct stat st;
        if (stat(out, &st) == 0) {
            if (out_kind) *out_kind = kinds[i];
            return true;
        }
    }
    return false;
}

/* /sdcard 안의 <M|T><10자리 숫자>.jpg 패턴 파일들을 전부 훑어서 file_id 배열에 채움
 * (정렬 안 된 상태로) — cam_storage_list()와 용량 관리(오래된 것부터 삭제)가 공유 */
static int scan_all_files(uint32_t *ids, int max)
{
    DIR *dir = opendir(BSP_CAM_SD_MOUNT_POINT);
    if (!dir) return -1;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        size_t len = strlen(ent->d_name);
        if (len != 15 /* "M0123456789.jpg" */) continue;
        if (ent->d_name[0] != CAM_CAPTURE_KIND_MANUAL && ent->d_name[0] != CAM_CAPTURE_KIND_AUTO) continue;
        if (strcmp(ent->d_name + 11, ".jpg") != 0) continue;

        bool all_digits = true;
        for (int i = 1; i <= 10; i++) {
            if (ent->d_name[i] < '0' || ent->d_name[i] > '9') { all_digits = false; break; }
        }
        if (!all_digits) continue;

        ids[count++] = (uint32_t)strtoul(ent->d_name + 1, NULL, 10);
    }
    closedir(dir);
    return count;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t va = *(const uint32_t *)a, vb = *(const uint32_t *)b;
    return (va > vb) - (va < vb);
}

static void enforce_capacity(void)
{
    uint32_t ids[CAM_STORAGE_MAX_FILES + 1];
    int count = scan_all_files(ids, CAM_STORAGE_MAX_FILES + 1);
    if (count <= CAM_STORAGE_MAX_FILES) return;

    qsort(ids, count, sizeof(ids[0]), cmp_u32);
    int to_delete = count - CAM_STORAGE_MAX_FILES;
    for (int i = 0; i < to_delete; i++) {
        char path[64];
        char kind;
        if (!resolve_path(ids[i], path, sizeof(path), &kind)) continue;
        if (remove(path) == 0) {
            ESP_LOGI(TAG, "순환 삭제: %s", path);
        } else {
            ESP_LOGW(TAG, "순환 삭제 실패: %s", path);
        }
    }
}

esp_err_t cam_storage_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,  /* 마운트 실패해도 카드 내용을 함부로 안 지움 */
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;  /* 1-bit 모드 — Waveshare 예제(SD_MMC.begin(mode1bit=true))와 동일 */
    slot_cfg.clk   = BSP_CAM_SD_CLK;
    slot_cfg.cmd   = BSP_CAM_SD_CMD;
    slot_cfg.d0    = BSP_CAM_SD_D0;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_CAM_SD_MOUNT_POINT, &host, &slot_cfg,
                                             &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD 마운트 실패: %s (SD카드 있는지, IO 익스팬더 인에이블 핀 확인)",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SD 마운트 완료 (%s, %lluMB)", BSP_CAM_SD_MOUNT_POINT,
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

esp_err_t cam_storage_save_capture(const uint8_t *jpeg_data, size_t len, cam_capture_kind_t kind, uint32_t *out_file_id)
{
    enforce_capacity();

    uint32_t file_id = (uint32_t)time(NULL);
    char path[64];
    file_path(file_id, (char)kind, path, sizeof(path));

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "캡처 저장 실패(열기): %s", path);
        return ESP_FAIL;
    }
    size_t written = fwrite(jpeg_data, 1, len, fp);
    fclose(fp);

    if (written != len) {
        ESP_LOGE(TAG, "캡처 저장 실패(쓰기 %u/%u): %s", (unsigned)written, (unsigned)len, path);
        remove(path);
        return ESP_FAIL;
    }

    if (out_file_id) *out_file_id = file_id;
    ESP_LOGI(TAG, "캡처 저장: %s (%u bytes)", path, (unsigned)len);
    return ESP_OK;
}

int cam_storage_list(photo_request_mode_t mode, uint32_t param, uint32_t *out_file_ids, int max)
{
    uint32_t all_ids[CAM_STORAGE_MAX_FILES];
    int total = scan_all_files(all_ids, CAM_STORAGE_MAX_FILES);
    if (total < 0) return -1;
    qsort(all_ids, total, sizeof(all_ids[0]), cmp_u32);

    if (mode == PHOTO_REQUEST_MODE_LATEST) {
        if (total == 0 || max < 1) return 0;
        out_file_ids[0] = all_ids[total - 1];
        return 1;
    }

    uint32_t cutoff = 0;
    if (mode == PHOTO_REQUEST_MODE_RECENT_HOURS) {
        uint32_t now = (uint32_t)time(NULL);
        uint32_t window = param * 3600U;
        cutoff = (window < now) ? (now - window) : 0;
    }

    int count = 0;
    for (int i = 0; i < total && count < max; i++) {
        if (mode == PHOTO_REQUEST_MODE_RECENT_HOURS && all_ids[i] < cutoff) continue;
        out_file_ids[count++] = all_ids[i];
    }
    return count;
}

esp_err_t cam_storage_stat(uint32_t file_id, uint32_t *out_size, char *out_kind)
{
    char path[64];
    char kind;
    if (!resolve_path(file_id, path, sizeof(path), &kind)) return ESP_ERR_NOT_FOUND;

    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;

    if (out_size) *out_size = (uint32_t)st.st_size;
    if (out_kind) *out_kind = kind;
    return ESP_OK;
}

esp_err_t cam_storage_open_read(uint32_t file_id, FILE **out_fp, uint32_t *out_size)
{
    char path[64];
    if (!resolve_path(file_id, path, sizeof(path), NULL)) return ESP_ERR_NOT_FOUND;

    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_ERR_NOT_FOUND;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);
    if (size < 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    *out_fp   = fp;
    *out_size = (uint32_t)size;
    return ESP_OK;
}

int cam_storage_delete_all(void)
{
    DIR *dir = opendir(BSP_CAM_SD_MOUNT_POINT);
    if (!dir) return -1;

    int deleted = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len != 15) continue;
        if (ent->d_name[0] != CAM_CAPTURE_KIND_MANUAL && ent->d_name[0] != CAM_CAPTURE_KIND_AUTO) continue;
        if (strcmp(ent->d_name + 11, ".jpg") != 0) continue;

        char path[64];
        snprintf(path, sizeof(path), "%s/%.15s", BSP_CAM_SD_MOUNT_POINT, ent->d_name);
        if (remove(path) == 0) deleted++;
    }
    closedir(dir);
    return deleted;
}
