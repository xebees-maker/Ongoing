#pragma once
/**
 * @file    flash_usage.h
 * @brief   내부 플래시(16 MB) 영역별 사용량 — 2026-10-15(사용자 지시, 개발·시험용 성격) 콘 Summary·웹 주화면에 표시.
 *          60초마다 기록 태스크에서 계산해 두고(LittleFS 폴더 훑기 포함), 화면은 복사만 함.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     valid;
    uint32_t seq;          /* 계산할 때마다 1씩 — 화면이 바뀐 때만 다시 그리게 */
    uint32_t app_used;     /* 지금 펌웨어 이미지 크기 */
    uint32_t app_size;     /* app 파티션 크기 */
    uint32_t fs_used;      /* LittleFS 사용량 */
    uint32_t fs_size;      /* LittleFS 파티션 크기 */
    uint32_t fs_stats;     /* 그중 통계 보관(/assets/stats) */
    uint32_t nvs_used;     /* 항목 수 */
    uint32_t nvs_total;
    uint32_t phy_size;     /* phy_init 파티션 크기(고정) */
} flash_usage_t;

/* main에서 LittleFS 마운트 뒤 한 번 — 바로 한 번 계산하고 60초 타이머 시작 */
void flash_usage_init(void);
void flash_usage_get(flash_usage_t *out);
