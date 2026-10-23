/**
 * @file    rgb_status.h
 * @brief   보드 내장 RGB LED(WS2812, GPIO7) 상태 표시 — 2026-10-23(사용자 설계).
 *
 * 외부 LED 보드(초록 GPIO5 = 연결, 파랑 GPIO2 = 전원·배터리)와 VIN 분압을 모든 센스에서 떼면서, 표시는 내장 RGB 하나로.
 * 합성색은 가독성이 나빠 단색(R·G·B)만, 같은 색 안에서는 깜빡임 횟수로 구분(한 번 = 켜짐 100ms·꺼짐 150ms):
 *   정상 연결 B 1번 / 콘 찾는 중·끊김 B 계속 깜빡임 / 측정 시작 G 1번 / 측정 성공 G 2번 / 측정 실패 R 1번 / 배터리 20% 미만 R 3번.
 *   예외(사용자 결정 10-23): 측정 일부 성공(센서 중 일부만 실패)은 합성색 노랑 2번.
 * 타이머로 돌아 부르는 쪽(ESP-NOW 콜백 포함)을 막지 않음. 요청은 차례로 표시하고, 찾는 중 깜빡임은 그 사이사이에.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RGB_STATUS_RED,
    RGB_STATUS_GREEN,
    RGB_STATUS_BLUE,
    RGB_STATUS_YELLOW,   /* 2026-10-23(사용자 결정) — 합성색(R+G). 측정 일부 성공(센서 중 일부만 실패) */
} rgb_status_color_t;

/* color로 times번 깜빡임(차례를 기다림) */
void rgb_status_blink(rgb_status_color_t color, int times);

/* 콘을 찾는 동안 파랑 계속 깜빡임 켜기/끄기 */
void rgb_status_search(bool on);

/* 딥슬립 직전 — 남은 깜빡임을 max_ms까지 기다린 뒤 끔 */
void rgb_status_flush(uint32_t max_ms);
