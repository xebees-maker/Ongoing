#pragma once

#include "can_bridge_link.h"
#include <stdint.h>

/**
 * 2026-09-22 — 브(브릿지)의 CAN 엔진. 콘의 can_test.c에서 검증된 ISO-TP 엔진을 그대로 쓰되,
 * 이 프로젝트는 항상 BRIDGE 역할 고정이라(콘 보드를 빌려 쓰던 벤치테스트 때와 달리 MAC 기반
 * 역할판정이 필요 없음) 그 부분만 단순화.
 */

void can_link_init(void);

/* bridge_esp_now.c가 캠에서 받은 프레임을 콘에 릴레이할 때 씀 */
can_bridge_ctx_t *can_link_get_data_ctx(void);
