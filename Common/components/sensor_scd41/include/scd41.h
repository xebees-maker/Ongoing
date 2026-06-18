/**
 * @file    scd41.h
 * @brief   SCD41 CO2/온습도 센서 — I2C 드라이버 (Sensirion 프로토콜)
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

#define SCD41_I2C_ADDR   0x62

/**
 * @brief SCD41 초기화: 전용 I2C 버스 생성 + 디바이스 등록 + 주기 측정 시작
 * @param i2c_port 전용으로 쓸 I2C 포트 번호 (예: 1 — 다른 버스와 분리 권장)
 * @param sda_gpio SDA 핀
 * @param scl_gpio SCL 핀
 * @return 성공 시 true
 */
bool scd41_init(int i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio);

/**
 * @brief 새 측정값이 준비됐으면 읽기 (5초 주기로 갱신됨, 그 전엔 false)
 * @param co2_ppm     CO2 농도 ppm 출력 (NULL 가능)
 * @param temperature 섭씨 온도 출력 (NULL 가능)
 * @param humidity    상대습도(%) 출력 (NULL 가능)
 * @return 새 데이터를 읽었으면 true, 준비 안 됐거나 오류면 false
 */
bool scd41_read(int *co2_ppm, float *temperature, float *humidity);
