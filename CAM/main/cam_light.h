/**
 * @file    cam_light.h
 * @brief   촬영용 LED(2026-09-29, 사용자 설계) — 두 번째 SH1.0 단자(J11)의 ESP_RXD = GPIO44로 LED용 MOSFET(IRLZ44N)
 *          게이트를 직접 구동. 촬영 직전(센서 초기화 전)에 켜고 프레임을 받자마자 끔.
 */
#pragma once

#include <stdbool.h>

/* app_main 맨 앞에서 한 번 — GPIO44를 출력 LOW(끔)로. 실패하면 false */
bool cam_light_init(void);

/* 켜기/끄기 */
void cam_light_set(bool on);

bool cam_light_present(void);
