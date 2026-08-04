/**
 * @file    fs.h
 * @brief   LittleFS 파일시스템 초기화
 *          파티션 레이블 "fonts" 마운트 → /fonts
 */
#pragma once

#include "esp_err.h"

#define FS_MOUNT_POINT      "/fonts"
#define FS_PARTITION_LABEL  "fonts"

esp_err_t fs_init(void);
