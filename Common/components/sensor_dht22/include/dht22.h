/**
 * @file    dht22.h
 * @brief   DHT22(AM2302) 온습도 센서 — 단일 GPIO bit-bang 드라이버
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

/**
 * @brief DHT22용 GPIO 초기화
 * @param data_gpio OUT 단자 연결 핀
 */
void dht22_init(gpio_num_t data_gpio);

/**
 * @brief DHT22 한 번 읽기 (블로킹, 약 5ms 동안 인터럽트 비활성화)
 * @param temperature 섭씨 온도 출력 (NULL 가능)
 * @param humidity    상대습도(%) 출력 (NULL 가능)
 * @return 성공 시 true, 타임아웃/체크섬 오류 시 false
 *
 * 센서 사양상 연속 호출 간격은 2초 이상이어야 함.
 */
bool dht22_read(float *temperature, float *humidity);
