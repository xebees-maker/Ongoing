#pragma once

#include <stdint.h>

/**
 * 2026-09-22 — 콘<->브릿지 CAN 프로토콜(can_bridge_link) 검증 테스트. 두 콘 보드에 동일
 * 펌웨어를 플래시하면 MAC 기반으로 역할(CNTL/BRIDGE)을 나눠 갖고, CONTROL(PING/PONG)과
 * DATA(가짜 CASK 모양 페이로드)를 실제 설계한 ISO-TP 프로토콜로 주고받음.
 */

void can_test_init(void);

/* 원시 TWAI 레벨 진단(종단저항 문제 등 배선 레벨 확인용, 기존 그대로 유지) */
uint32_t can_test_get_tx_count(void);
uint32_t can_test_get_rx_count(void);
uint32_t can_test_get_tx_done_ok(void);
uint32_t can_test_get_tx_done_fail(void);

/* 프로토콜 레벨 — 큐 깊이/최대값 노출(사용자 지시: 비정상 적체를 화면에서 바로 확인 가능해야 함) */
uint32_t can_test_get_ctrl_queue_depth(void);
uint32_t can_test_get_data_queue_depth(void);
uint32_t can_test_get_ctrl_queue_hwm(void);
uint32_t can_test_get_data_queue_hwm(void);
uint32_t can_test_get_ctrl_msgs_completed(void);
uint32_t can_test_get_data_msgs_completed(void);
