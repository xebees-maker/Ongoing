/**
 * @file    fs.c
 * @brief   LittleFS 파일시스템 초기화 구현
 */

#include "fs.h"
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "fs";

esp_err_t fs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = FS_MOUNT_POINT,
        .partition_label        = FS_PARTITION_LABEL,
        .format_if_mount_failed = false,
        .dont_mount             = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(FS_PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted: total=%uKB, used=%uKB",
             (unsigned)(total / 1024), (unsigned)(used / 1024));

    return ESP_OK;
}
