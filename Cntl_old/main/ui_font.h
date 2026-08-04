/**
 * @file    ui_font.h
 * @brief   TinyTTF 런타임 폰트 (NanumGothic) — 12 / 18 / 30pt
 */
#pragma once

#include "lvgl.h"
#include "esp_err.h"

#define UI_FONT_SIZE_12     12
#define UI_FONT_SIZE_18     18
#define UI_FONT_SIZE_30     30

#define UI_FONT_TTF_VFS_PATH  "/fonts/NanumGothic-Regular.ttf"
#define UI_FONT_TTF_PATH      "F:/fonts/NanumGothic-Regular.ttf"

esp_err_t        ui_font_init(void);
const lv_font_t *ui_font_get(uint8_t size);
void             ui_font_deinit(void);
