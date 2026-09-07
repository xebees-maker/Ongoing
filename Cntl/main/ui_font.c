/**
 * @file    ui_font.c
 * @brief   2026-09-07(임시 실험 브랜치 — 사용자 지시: "TTF 로드를 모두 임시로 막아보자") —
 *          TinyTTF 대신 내장 비트맵 폰트(12/18/24/30pt)로 교체. 목적: 팝업 닫을 때 메모리가
 *          완전히 회수 안 되던 문제(통계/설정/로그 각각 -5092/-5364/-772 bytes)가 TTF
 *          글리프/커닝/드로우데이터 캐시(lv_tiny_ttf.c) 때문인지 확인. 인터페이스
 *          (ui_font_init/ui_font_get/ui_font_deinit)는 무변경 — 호출부 113곳 안 건드림.
 *          원복: 이 파일을 이전 버전(TTF 로드)으로 되돌리면 됨, 폰트 파일/파티션은 무변경.
 */

#include "ui_font.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "ui_font";

esp_err_t ui_font_init(void)
{
    ESP_LOGI(TAG, "비트맵 폰트 모드(TTF 임시 비활성화) — 12/18/24/30pt 내장 Montserrat");
    return ESP_OK;
}

const lv_font_t *ui_font_get(uint8_t size)
{
    switch (size) {
        case 12: return &lv_font_montserrat_12;
        case 18: return &lv_font_montserrat_18;
        case 24: return &lv_font_montserrat_24;
        case 30: return &lv_font_montserrat_30;
        default: return &lv_font_montserrat_12;
    }
}

void ui_font_deinit(void)
{
    /* 비트맵 폰트는 플래시 상주 상수라 해제할 게 없음 */
}
