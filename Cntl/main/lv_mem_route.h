#pragma once
/**
 * @file    lv_mem_route.h
 * @brief   2026-10-17(시험) — LVGL 할당 위치를 "그리는 중인가"로 나눔: 화면 갱신(REFR_START~REFR_READY) 동안은 내부 RAM,
 *          그 밖(위젯 생성·이벤트·타이머)은 PSRAM. 크기 기준(SPIRAM_MALLOC_ALWAYSINTERNAL)으로는 매 프레임 그리기용 임시 할당과
 *          위젯 할당을 나눌 수 없었음(project_cntl_stats_tab_memory). 함께 화면 갱신 시간(평균·최대)을 10초마다 Dev Log에 남김
 */
#include "lvgl.h"

/* 디스플레이 등록 직후 한 번 */
void lv_mem_route_attach(lv_display_t *disp);
