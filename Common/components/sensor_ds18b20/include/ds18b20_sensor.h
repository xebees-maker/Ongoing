/**
 * @file    ds18b20_sensor.h
 * @brief   DS18B20(1-Wire) — Agar 접촉 온도용 드라이버, ESP-IDF 공식 컴포넌트(espressif/ds18b20, onewire_bus/RMT) 래퍼
 *
 * 2026-09-30 — 버스에 센서 하나(중간 보드에 풀업 있음, 3.3V 전원). 12비트(0.0625°C), 변환 대기 800ms(컴포넌트가 블로킹).
 * PT100(MAX31865)과 번갈아 써 보고 하나로 정하는 비교용(사용자 결정).
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

/* ds18b20_sensor_read()의 결과 */
typedef enum {
    DS18B20_SENSOR_OK = 0,
    DS18B20_SENSOR_ERR_NO_DEVICE,   /* 리셋에 응답(presence) 없음 — 센서/배선 없음 */
    DS18B20_SENSOR_ERR_CRC,         /* 스크래치패드 CRC 불일치 */
    DS18B20_SENSOR_ERR_READ,        /* 그 밖의 버스 오류, 또는 변환이 안 된 전원 투입 값(85.0°C) */
} ds18b20_sensor_result_t;

/**
 * @brief 1-Wire 버스(RMT) 생성과 센서 찾기
 * @return 센서를 찾으면 true(못 찾아도 버스는 남겨 두고 읽을 때 다시 찾음)
 */
bool ds18b20_sensor_init(gpio_num_t data_gpio);

/** @brief 변환 시작 → 약 800ms 대기 → 판독(블로킹) */
ds18b20_sensor_result_t ds18b20_sensor_read(float *temp_c);
