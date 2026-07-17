/**
 * @file    bsp_c3_pico.c
 * @brief   LOLIN C3 Pico 헤드리스 보드 — 공통 초기화 자리
 */

#include "bsp_c3_pico.h"
#include "esp_log.h"

static const char *TAG = "bsp_c3_pico";

esp_err_t bsp_c3_pico_init(void)
{
    ESP_LOGI(TAG, "board init (headless — no LCD/touch)");
    return ESP_OK;
}
