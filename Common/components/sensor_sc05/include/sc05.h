/**
 * @file    sc05.h
 * @brief   SC05-NH3(YYS) 전기화학식 암모니아 센서 — UART(9600 8N1) 드라이버
 *
 * 2026-09-15(사용자 제공 데이터시트 이미지 3장 — 기술지표/핀배정/통신명령) 기준 구현.
 * 프레임(9바이트, 데이터시트 Table4/7/8): FF [가스명] [단위] [소수점자리NC]
 * [농도High] [농도Low] [풀레인지High] [풀레인지Low] [체크섬]
 *   - PPM = (농도High*256 + 농도Low) / 100
 *   - 체크섬 = (Byte1~Byte7 합)의 2의보수(예시값으로 직접 검산 완료: 일치)
 *
 * Auto 모드(공장 기본값, 전원 인가 후 별도 명령 없이 1초마다 이 프레임을 그냥 쏨)만
 * 사용 — Non-Auto(질의응답) 전환 명령 바이트가 데이터시트 내에서도 서로 안 맞아서
 * (Table5는 Byte3=0x40, Table6/설명문은 Byte3=0x41 및 "0x03/0x04"라고 서술, 상호
 * 불일치) 신뢰하지 않기로 함(사용자 확인). 그래서 sc05_read()는 깨어난 뒤 일정
 * 시간 동안 그냥 수신 대기하다가 유효한(시작바이트+체크섬 일치) 첫 프레임을 잡는다.
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/uart.h"

/**
 * @brief SC05 UART 초기화
 * @param uart_port UART 포트 번호(bsp_c3_pico.h의 BSP_C3_SC05_UART_PORT)
 * @param rx_gpio   SC05 TXD가 물린 GPIO(이 쪽에서 수신)
 * @param tx_gpio   SC05 RXD가 물린 GPIO(Auto 모드라 실제로 안 씀, 배선만 해둠)
 * @param baud      보드레이트(9600)
 * @return 성공 시 true
 */
bool sc05_init(uart_port_t uart_port, gpio_num_t rx_gpio, gpio_num_t tx_gpio, int baud);

/**
 * @brief 유효한 프레임 하나를 잡을 때까지 최대 timeout_ms만큼 수신 대기 후 판독
 * @param ppm        암모니아 농도(ppm) 출력, NULL 가능
 * @param timeout_ms 최대 대기 시간(ms) — Auto 모드가 1초마다 쏘므로 2000ms 이상 권장
 * @return 유효한 프레임을 잡아 판독에 성공하면 true
 */
bool sc05_read(float *ppm, uint32_t timeout_ms);
