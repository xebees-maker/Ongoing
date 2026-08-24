#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Waveshare ESP32-S3-CAM-OVxxxx 보드의 온보드 I2C GPIO 익스팬더(CH32V003, 작은 RISC-V
 * MCU를 IO 익스팬더 펌웨어로 씀) 제어용. 원본: Waveshare 공식 Arduino 예제
 * (examples/Arduino-v3.2.0/examples/04_SDMMC_Test/io_extension.c/.h)의 I2C 레지스터
 * 프로토콜을 그대로 ESP-IDF C로 이식 — Common/components/io_expander_tca9554와 같은
 * 카테고리(보드 내장 I2C GPIO 익스팬더)지만 레지스터 프로토콜이 달라서 별개 컴포넌트.
 *
 * IO2/IO6을 HIGH로 출력해야 이 보드의 SD카드가 마운트된다는 것까지는 Waveshare 예제로
 * 확인했지만, 정확히 왜(전원 게이팅? 먹스 선택?)인지는 원본 헤더 주석이 이 보드와 무관해
 * 보이는 다른 Waveshare LCD 제품에서 복사된 것으로 보여 불확실 — bsp_esp32s3_cam.h의
 * 관련 주석 참고.
 */

#define CH32V003_I2C_ADDR         0x24

#define CH32V003_REG_MODE         0x02  /* 비트마스크, 핀당 1비트: 0=input, 1=output */
#define CH32V003_REG_OUTPUT       0x03
#define CH32V003_REG_INPUT        0x04
#define CH32V003_REG_PWM          0x05
#define CH32V003_REG_ADC          0x06

/* 핀 번호(0~7) — Waveshare 원본 예제의 IO_EXTENSION_IO_* 라벨과 동일한 인덱싱 */
#define CH32V003_IO_0  0
#define CH32V003_IO_1  1
#define CH32V003_IO_2  2
#define CH32V003_IO_3  3
#define CH32V003_IO_4  4
#define CH32V003_IO_5  5
#define CH32V003_IO_6  6
#define CH32V003_IO_7  7

/**
 * @brief 초기화 — 공유 I2C 버스에 디바이스 등록 + 모든 핀을 출력 모드로 설정
 * @param i2c_bus 이미 만들어진 I2C 마스터 버스 핸들
 */
esp_err_t ch32v003_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief 출력 핀 하나의 레벨 설정(핀은 미리 출력 모드여야 함 — ch32v003_init()이 전부
 *        출력으로 설정해둠)
 * @param pin CH32V003_IO_0..7
 * @param level 0 또는 1
 */
void ch32v003_set_output(uint8_t pin, bool level);

/**
 * @brief 입력 핀 하나의 레벨 읽기
 */
bool ch32v003_get_input(uint8_t pin);

/**
 * @brief ADC 레지스터(0x06) 원시값 읽기(16bit, 리틀엔디안 — Waveshare 공식 Arduino 예제
 *        DEV_I2C_Read_Word()로 확인) — 이 보드에서 배터리 전압 감지(BAT_ADC)에 쓰임.
 *        CH32V003 자체 ADC는 통상 10bit/자체 VDD(3.3V) 기준으로 추정(WCH 데이터시트 기준 —
 *        Waveshare 쪽 문서/예제 어디에도 비트폭·기준전압이 명시돼있지 않아 확정은 아님,
 *        2026-08-22). 호출측에서 실측 대조 권장.
 * @param out_raw 읽은 원시값(0 실패 시 미정의)
 * @return ESP_OK 성공
 */
esp_err_t ch32v003_get_adc(uint16_t *out_raw);

#ifdef __cplusplus
}
#endif
