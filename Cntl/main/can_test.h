#pragma once

#include <stdint.h>

/**
 * 2026-09-22(임시 진단 — 사용자 지시: 콘-콘 CAN 배선 통신 여부를 요약판넬 "Bridge: Tx/Rx"로
 * 확인) — 두 콘 보드에 동일 펌웨어를 플래시, 서로 주기적으로 송신하고 받은 걸 카운트함.
 * 확인 끝나면 제거할 것.
 */

void can_test_init(void);
uint32_t can_test_get_tx_count(void);
uint32_t can_test_get_rx_count(void);
