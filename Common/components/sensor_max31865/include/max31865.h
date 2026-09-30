/**
 * @file    max31865.h
 * @brief   MAX31865 RTD-to-Digital(SPI) + PT100 — Agar 접촉 온도용 드라이버
 *
 * 2026-09-30 — 모듈: Adafruit 호환 MAX31865 브레이크아웃(기준저항 430Ω, 3선 점퍼 설정),
 * 센서: PT100 3선. 1회 변환(1-shot)만 씀: 바이어스 켜기 → 안정화 대기 → 1-shot → 변환 대기
 * (60Hz 필터 52ms) → RTD 코드 읽기 → 바이어스 끄기. 딥슬립 노드라 부팅마다 한 번 읽음.
 * 칩 고장 비트(선 끊김/단락/전압 범위)는 max31865_read()가 fault 출력으로 돌려줌.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"

/* max31865_read()의 결과 */
typedef enum {
    MAX31865_OK = 0,
    MAX31865_ERR_NO_CHIP,   /* 설정 레지스터를 써도 되읽히지 않음 — 칩/배선 없음 */
    MAX31865_ERR_SPI,       /* SPI 전송 실패 */
    MAX31865_ERR_FAULT,     /* 칩이 RTD 고장을 보고함(fault_status에 원인 비트) */
} max31865_result_t;

/**
 * @brief SPI 버스와 장치 등록, 설정 레지스터 쓰기/되읽기로 칩 확인
 * @param host      SPI 호스트(ESP32-C3은 SPI2_HOST)
 * @param sclk      CLK
 * @param mosi      SDI(모듈 입력)
 * @param miso      SDO(모듈 출력)
 * @param cs        CS
 * @param three_wire true = 3선 PT100(설정 레지스터 bit4)
 * @return 칩이 응답하면 true
 */
bool max31865_init(spi_host_device_t host, gpio_num_t sclk, gpio_num_t mosi, gpio_num_t miso,
                   gpio_num_t cs, bool three_wire);

/**
 * @brief 1회 변환 후 온도 판독(블로킹 약 80ms)
 * @param temp_c       온도(°C) 출력
 * @param fault_status 칩 고장 상태 레지스터(07h) 값 출력(고장 아니면 0), NULL 가능
 */
max31865_result_t max31865_read(float *temp_c, uint8_t *fault_status);

/** @brief 마지막으로 읽은 15비트 RTD 코드(저항 = 코드 × 430 / 32768) — 로그·진단용 */
uint16_t max31865_last_code(void);
