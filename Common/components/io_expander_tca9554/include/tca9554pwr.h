#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Waveshare ESP32-S3-Touch-LCD-1.85C 등 보드에서 LCD_RST/TP_RST 같은 신호를
 * 직접 GPIO가 아니라 이 I2C IO 익스팬더(TCA9554PWR) 경유로 제어할 때 사용.
 * 원본: Waveshare 공식 데모(ESP32-S3-Touch-LCD-1.85C) EXIO/TCA9554PWR.c/.h —
 * 전역 esp_ret_i2c_handle() 의존만 제거하고 i2c 핸들을 인자로 받도록 수정.
 */

#define TCA9554_EXIO1 0x01
#define TCA9554_EXIO2 0x02
#define TCA9554_EXIO3 0x03
#define TCA9554_EXIO4 0x04
#define TCA9554_EXIO5 0x05
#define TCA9554_EXIO6 0x06
#define TCA9554_EXIO7 0x07
#define TCA9554_EXIO8 0x08

#define TCA9554_ADDRESS       0x20
#define TCA9554_INPUT_REG     0x00
#define TCA9554_OUTPUT_REG    0x01
#define TCA9554_Polarity_REG  0x02
#define TCA9554_CONFIG_REG    0x03

uint8_t Read_REG(uint8_t REG);
void Write_REG(uint8_t REG, uint8_t Data);

void Mode_EXIO(uint8_t Pin, uint8_t State);
void Mode_EXIOS(uint8_t PinState);

uint8_t Read_EXIO(uint8_t Pin);
uint8_t Read_EXIOS(void);

void Set_EXIO(uint8_t Pin, bool State);
void Set_EXIOS(uint8_t PinState);
void Set_Toggle(uint8_t Pin);

/* PinState: 7개 핀의 입력/출력 모드 비트마스크(0=출력, 1=입력), 기본 전부 출력(0x00) */
esp_err_t tca9554pwr_init(i2c_master_bus_handle_t i2c_bus, uint8_t pin_state);

#ifdef __cplusplus
}
#endif
