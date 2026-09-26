/**
 * @file    status_led.h
 * @brief   GPIO 상태 LED 패턴 드라이버 (계속 켜짐 / 계속 꺼짐 / 느린 깜박임 / 빠른 깜박임)
 *
 * 여러 LED를 동시에 다룰 수 있도록 GPIO 핀 번호를 키로 내부 상태를 관리한다.
 * 헤드리스 Sens 노드의 Green(ESP-NOW 상태)/Blue(배터리 잔량, 센서 이상 시 BURST_TRIPLE로 임시
 * 대체) LED가 이 드라이버를 공유한다.
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

typedef enum {
    LED_PATTERN_OFF = 0,
    LED_PATTERN_ON,
    LED_PATTERN_BLINK_SLOW,     /* 1s on / 1s off */
    LED_PATTERN_BLINK_FAST,     /* 0.1s on / 0.1s off */
    LED_PATTERN_BURST_TRIPLE,   /* 0.1s on/off x3, 0.5s pause, repeat — "주의" 신호용 */
    LED_PATTERN_HEARTBEAT,      /* 50ms on / 2.95s off (3s 주기) — "정상 동작 중" 저전력 신호용.
                                  * 계속 켜짐 대비 듀티사이클 ~1.7%라 배터리 부담이 훨씬 적으면서도
                                  * 사람 눈엔 "죽은 채 고정"이 아니라 "살아서 갱신 중"으로 더 명확히 읽힘 */
    LED_PATTERN_HEARTBEAT_FAST,   /* 50ms on / 1s off — HEARTBEAT보다 빠른 주기로 "주의(경고 전 단계)" */
    LED_PATTERN_HEARTBEAT_URGENT, /* 50ms on / 300ms off — 배터리 0~20%처럼 더 급한 경고용 */
} led_pattern_t;

/**
 * @brief LED용 GPIO 초기화 (출력, 초기 상태 OFF)
 * @param pin 최대 4개까지 동시 등록 가능
 * @return 성공 시 true
 */
bool status_led_init(gpio_num_t pin);

/**
 * @brief GPIO가 아닌 LED(예: I2C IO 익스팬더 핀)용 초기화 — 켜기/끄기를 write_fn으로 위임.
 *        2026-09-26 추가(CAM 보드의 PWR_LED가 CH32V003 EXIO6에 달려 있음).
 * @param id       status_led_set_pattern()에서 쓸 키. 실제 GPIO와 겹치면 안 되므로
 *                 GPIO_NUM_MAX 이상이어야 함(아니면 false)
 * @param write_fn LED 켜기(true)/끄기(false). esp_timer 태스크와 set_pattern 호출자(WiFi 콜백
 *                 포함) 문맥에서 불리므로 블로킹하지 말 것 — 느린 버스면 호출측이 비동기로 처리
 * @return 성공 시 true
 */
typedef void (*status_led_write_fn_t)(bool on);
bool status_led_init_custom(gpio_num_t id, status_led_write_fn_t write_fn);

/** @brief 이미 같은 패턴이면 아무 일도 하지 않음(타이머 재시작으로 인한 깜박임 튐 방지) */
void status_led_set_pattern(gpio_num_t pin, led_pattern_t pattern);
