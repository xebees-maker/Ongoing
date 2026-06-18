/**
 * @file    main.c
 * @brief   startup 프로젝트 — 진입점 및 시스템 초기화
 */

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp_ws_1_47.h"
#include "draw/lv_draw_buf_private.h"
#include "fs.h"
#include "ui_font.h"
#include "ui_main.h"

static const char *TAG = "main";

static void *font_buf_malloc(size_t size, lv_color_format_t cf)
{
    (void)cf;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static void font_buf_free(void *buf)
{
    heap_caps_free(buf);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== startup project boot ===");

    /* NVS 초기화 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* BSP 초기화 (LCD + 터치 + LVGL + 백라이트) */
    ESP_ERROR_CHECK(bsp_board_init());

    /* LVGL PSRAM 풀 추가 + POSIX 드라이버 등록 */
    if (bsp_lvgl_lock(BSP_MUTEX_WAIT_DEFAULT)) {
        void *psram_pool = heap_caps_malloc(3 * 1024 * 1024, MALLOC_CAP_SPIRAM);
        if (psram_pool) {
            lv_mem_add_pool(psram_pool, 3 * 1024 * 1024);
            ESP_LOGI(TAG, "LVGL PSRAM pool: 3MB");
        }
        lv_fs_posix_init();
        ESP_LOGI(TAG, "POSIX driver registered");

        /* 폰트 글리프 비트맵을 PSRAM에서 할당 */
        lv_draw_buf_handlers_t *font_handlers = lv_draw_buf_get_font_handlers();
        font_handlers->buf_malloc_cb = font_buf_malloc;
        font_handlers->buf_free_cb   = font_buf_free;
        ESP_LOGI(TAG, "Font draw buf: PSRAM");
        bsp_lvgl_unlock();
    }

    /* LittleFS 마운트 (폰트 파티션) */
    ESP_ERROR_CHECK(fs_init());

    /* 폰트 로드 + UI 생성 — LVGL 뮤텍스 보호 구간 */
    if (bsp_lvgl_lock(BSP_MUTEX_WAIT_DEFAULT)) {
        ESP_ERROR_CHECK(ui_font_init());
        ui_init();
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire LVGL mutex");
    }

    ESP_LOGI(TAG, "UI ready");
}
