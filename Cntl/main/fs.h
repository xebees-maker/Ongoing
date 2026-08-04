/**
 * @file    fs.h
 * @brief   LittleFS 파일시스템 초기화
 *          파티션 레이블 "assets" 마운트 → /assets (폰트 + 설정파일 공용)
 */
#pragma once

#include "esp_err.h"

#define FS_MOUNT_POINT      "/assets"
#define FS_PARTITION_LABEL  "assets"

esp_err_t fs_init(void);
