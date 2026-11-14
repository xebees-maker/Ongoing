#pragma once
/**
 * @file    power_button.h
 * @brief   전원 푸시 스위치 — 2026-11-13(사용자 지시: 큰 ON/OFF 스위치 대신 접점식 푸시 스위치, 길게 눌러 끄고 짧게 눌러 켬)
 *
 * 배선: 스위치 한쪽 BSP_C3_POWER_BUTTON_PIN(GPIO1, J2 헤더 IO1), 다른 쪽 GND. 풀업은 칩 안의 것(깨어 있을 때 gpio_config,
 * 잘 때는 IDF가 CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS로 자동).
 *  - 켜져 있을 때 3초 누름: 빨강 5번 → 손을 떼면 꺼짐. 끄기는 표시(RTC_NOINIT)를 남기고 다시 시작해서, 전원 켤 때와 같은 센서 초기화
 *    (SCD41 power_down 등)를 마친 뒤 이 스위치로만 깨는 딥슬립에 들어감 — 꺼진 동안의 전류는 측정 사이에 잘 때와 같음.
 *  - 꺼져 있을 때 누름: 초록 3번 → 켜짐(보통 동작을 처음부터).
 *  - 켜져 있을 때 짧게 누름: 초록 3번(켜져 있음) — 자고 있었으면 원래 깰 시각까지 다시 잠.
 * 콘에는 알리지 않음 — 콘은 꺼진 보드를 끊김으로 봄.
 */
#include <stdint.h>

#define POWER_BUTTON_LONG_MS 3000   /* 이만큼 누르고 있으면 끔 */

/* app_main 맨 앞(esp_now_node_capture_wake_info 바로 뒤, rwdt_guard_arm 앞). 스위치로 깨어났으면 여기서 처리:
 * 짧게 → 원래 깰 시각까지 다시 잠(돌아오지 않음), 길게 → 끄기(돌아오지 않음), 꺼져 있었으면 → 켜고 돌아옴.
 * 돌아오면 깨어 있는 동안의 길게 누름 감시가 켜져 있음 */
void power_button_boot(void);

/* 센서 초기화 바로 뒤 — 끄기가 걸려 있으면 꺼짐 딥슬립으로(돌아오지 않음) */
void power_button_off_if_requested(void);

/* esp_deep_sleep_start() 바로 앞 — 이번에 깰 시각을 기억하고 스위치 깨우기를 켬 */
void power_button_prepare_sleep(uint64_t sleep_us);
