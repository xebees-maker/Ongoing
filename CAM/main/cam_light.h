/**
 * @file    cam_light.h
 * @brief   촬영용 LED(2026-09-29, 사용자 설계) — 캠 I2C 단자(GPIO7/8, 카메라 SCCB·CH32V003·ES8311과 같은 버스)에
 *          단 PCF8574(주소 0x20, A2~A0 = GND)의 P0으로 LED 모듈 신호(S)를 켜고 끔. 촬영 직전에 켜고 프레임을
 *          받자마자 끔. PCF8574가 없는 보드(LED를 안 단 캠)는 부팅 때 한 번 로그만 남기고 아무것도 안 함.
 */
#pragma once

#include <stdbool.h>

/* 부팅 직후(bsp_esp32s3_cam_init() 다음) 한 번 — PCF8574를 찾고 LED를 끔(전원이 들어오면 출력이 HIGH로 시작해서
 * 펌웨어가 쓰기 전까지는 켜져 있음). 없으면 false */
bool cam_light_init(void);

/* 켜기/끄기 — PCF8574가 없으면 아무것도 안 함 */
void cam_light_set(bool on);

bool cam_light_present(void);
