/**
 * @file    sht4x.h
 * @brief   SHT45/SHT40 온습도 센서 — I2C 드라이버 (Sensirion 프로토콜)
 *
 * SHT45와 SHT40은 정확도 등급만 다르고 I2C 주소·명령·응답 포맷이 동일해서
 * 드라이버를 하나만 둔다. ESP-NOW로 보낼 때 어느 모델인지는 esp_now_link.h의
 * sensor_kind_t로만 구분한다.
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

#define SHT4X_I2C_ADDR   0x44

/**
 * @brief SHT4x 초기화: 전용 I2C 버스 생성 + 디바이스 등록
 * @param i2c_port 전용으로 쓸 I2C 포트 번호
 * @param sda_gpio SDA 핀
 * @param scl_gpio SCL 핀
 * @return 성공 시 true
 */
bool sht4x_init(int i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio);

/**
 * @brief 측정 1회 수행 (고정밀 모드, 블로킹 ~10ms)
 * @param temperature 섭씨 온도 출력 (NULL 가능)
 * @param humidity    상대습도(%) 출력 (NULL 가능)
 * @return 성공 시 true, CRC 오류/통신 실패 시 false
 */
bool sht4x_read(float *temperature, float *humidity);
