/**
 * @file    ui_font.c
 * @brief   TinyTTF 런타임 폰트 로드 — 12 / 18 / 30pt
 */

#include "ui_font.h"
#include "lvgl.h"
#include "libs/tiny_ttf/lv_tiny_ttf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "ui_font";

static void    *s_ttf_buf  = NULL;
static size_t   s_ttf_size = 0;

static lv_font_t *s_font_12 = NULL;
static lv_font_t *s_font_18 = NULL;
static lv_font_t *s_font_30 = NULL;

static esp_err_t load_ttf_to_psram(void)
{
    struct stat st;
    if (stat(UI_FONT_TTF_VFS_PATH, &st) != 0) {
        ESP_LOGE(TAG, "stat failed: %s", UI_FONT_TTF_VFS_PATH);
        return ESP_FAIL;
    }
    s_ttf_size = (size_t)st.st_size;

    s_ttf_buf = heap_caps_malloc(s_ttf_size, MALLOC_CAP_SPIRAM);
    if (!s_ttf_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed: %u KB", (unsigned)(s_ttf_size / 1024));
        return ESP_FAIL;
    }

    FILE *f = fopen(UI_FONT_TTF_VFS_PATH, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen failed");
        heap_caps_free(s_ttf_buf);
        s_ttf_buf = NULL;
        return ESP_FAIL;
    }
    fread(s_ttf_buf, 1, s_ttf_size, f);
    fclose(f);
    ESP_LOGI(TAG, "TTF loaded to PSRAM: %u KB", (unsigned)(s_ttf_size / 1024));
    return ESP_OK;
}

static lv_font_t *create_font(uint8_t size)
{
    lv_font_t *f = lv_tiny_ttf_create_data_ex(s_ttf_buf, s_ttf_size, size,
                                           LV_FONT_KERNING_NORMAL, LV_TINY_TTF_CACHE_GLYPH_CNT);
    if (!f) ESP_LOGE(TAG, "Failed: %dpt", size);
    else    ESP_LOGI(TAG, "OK: %dpt", size);
    return f;
}

esp_err_t ui_font_init(void)
{
    if (load_ttf_to_psram() != ESP_OK) return ESP_FAIL;

    s_font_12 = create_font(12);
    s_font_18 = create_font(18);
    s_font_30 = create_font(30);

    if (!s_font_12 || !s_font_18 || !s_font_30) return ESP_FAIL;
    return ESP_OK;
}

const lv_font_t *ui_font_get(uint8_t size)
{
    switch (size) {
        case 12: return s_font_12;
        case 18: return s_font_18;
        case 30: return s_font_30;
        default: return s_font_12;
    }
}

void ui_font_deinit(void)
{
    if (s_font_12) { lv_tiny_ttf_destroy(s_font_12); s_font_12 = NULL; }
    if (s_font_18) { lv_tiny_ttf_destroy(s_font_18); s_font_18 = NULL; }
    if (s_font_30) { lv_tiny_ttf_destroy(s_font_30); s_font_30 = NULL; }
    if (s_ttf_buf) { heap_caps_free(s_ttf_buf); s_ttf_buf = NULL; }
}
