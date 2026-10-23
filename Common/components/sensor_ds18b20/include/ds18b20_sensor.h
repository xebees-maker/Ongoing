/**
 * @file    ds18b20_sensor.h
 * @brief   DS18B20(1-Wire) — Agar 접촉 온도용 드라이버, ESP-IDF 공식 컴포넌트(espressif/ds18b20, onewire_bus/RMT) 래퍼
 *
 * 12비트(0.0625°C), 변환 대기 800ms(컴포넌트가 블로킹). 2026-10-23 — 핀마다 센서 하나(고온 보드 배지 온도 1·2), 읽을 때만
 * 버스를 만듦. 예전 버스 하나짜리 API(ds18b20_sensor_init/read — Agar 단독 보드)는 그 보드를 안 쓰게 되어 지움(사용자).
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

/* ds18b20_sensor_read_pin()의 결과 */
typedef enum {
    DS18B20_SENSOR_OK = 0,
    DS18B20_SENSOR_ERR_NO_DEVICE,   /* 리셋에 응답(presence) 없음 — 센서/배선 없음 */
    DS18B20_SENSOR_ERR_CRC,         /* 스크래치패드 CRC 불일치 */
    DS18B20_SENSOR_ERR_READ,        /* 그 밖의 버스 오류, 또는 변환이 안 된 전원 투입 값(85.0°C) */
} ds18b20_sensor_result_t;

/** @brief 2026-10-23 — 핀을 받아 그때만 버스를 만들고 읽고 지움(센서 여러 개를 핀 하나씩, RMT 채널을 붙잡지 않음). 블로킹 약 800ms */
ds18b20_sensor_result_t ds18b20_sensor_read_pin(gpio_num_t data_gpio, float *temp_c);
