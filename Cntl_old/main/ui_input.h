/**
 * @file    ui_input.h
 * @brief   최상위 입력 분배기
 *
 * 입력 흐름:
 *   BSP touch → ui_input (global handlers → active tab handlers) → LVGL
 *
 * handler_fn 이 true 를 반환하면 해당 이벤트를 소비(LVGL 전달 차단).
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

typedef bool (*ui_input_fn_t)(bool pressed, lv_point_t pt);

void ui_input_init(void);

/* 탭에 무관하게 항상 먼저 호출되는 핸들러 (낮은 priority 숫자 = 높은 우선순위) */
void ui_input_add_global(ui_input_fn_t fn, int priority);

/* 특정 탭이 열려 있을 때만 호출되는 핸들러 */
void ui_input_add_tab(int tab_idx, ui_input_fn_t fn, int priority);

/* tabview value-changed 이벤트에서 호출 */
void ui_input_set_active_tab(int tab_idx);
