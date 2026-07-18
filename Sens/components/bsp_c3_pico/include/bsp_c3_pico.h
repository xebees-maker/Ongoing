/**
 * @file    bsp_c3_pico.h
 * @brief   LOLIN C3 Pico(ESP32-C3) 헤드리스 센서 노드 보드 정의
 *
 * bsp_c6.h와 달리 LCD/터치가 없다 — I2C 센서(SCD41/SHT4x)는 각자 전용 버스를
 * 스스로 만들어 쓰므로(scd41_init/sht4x_init) 여기서 공유 버스를 만들지 않는다.
 * 이 헤더는 핀 상수만 모아둔다.
 *
 * 아래 GPIO는 WeMos 공식 스키매틱(sch_c3_pico_v1.0.0.pdf, 2026-07-18 확인)으로
 * 검증된 값이다. GPIO7=온보드 WS2812 RGB LED(J3 헤더에도 노출, 다른 용도로 쓰면
 * 충돌), GPIO9=BOOT 버튼 전용(헤더에 노출 안 됨), GPIO2=스트래핑 — 전부 회피.
 * USB 전원 감지는 J3 헤더의 "V" 핀(스키매틱상 net명 VIN)을 분압해서 쓴다 — 이건
 * raw VBUS가 아니라 Q1(P-FET)+D1(1N5819W)로 배터리/VBUS를 먼저 섞은 시스템 전원
 * 노드다. USB 미연결 시 Q1이 배터리 전압을 거의 그대로 통과(실측: VBAT과 동일),
 * USB 연결 시 D1을 거친 VBUS(실측 ~4.6~4.7V)로 바뀐다 — 배터리 완충 전압(~4.2V)
 * 보다 확실히 높아서 고정 임계값으로 구분 가능. 원래 계획했던 충전 LED(U4 CHRG
 * 핀 직결, LED1 자리)는 실물 보드에 해당 풋프린트가 없어서 포기함.
 */
#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════
 * I2C (SCD41 / SHT45 / SHT40 공용 — 빌드 시 하나만 실제로 씀)
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_I2C_PORT      0   /* esp32c3는 I2C 컨트롤러가 1개뿐(포트0) — LP_I2C 없음 */
#define BSP_C3_I2C_SDA       GPIO_NUM_8   /* 보드 내장 I2C 커넥터(P1)와 공유 */
#define BSP_C3_I2C_SCL       GPIO_NUM_10  /* 보드 내장 I2C 커넥터(P1)와 공유 */

/* ════════════════════════════════════════════════════════════
 * DHT22 (단일 GPIO bit-bang)
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_DHT22_PIN     GPIO_NUM_6

/* ════════════════════════════════════════════════════════════
 * 배터리 ADC — 보드 내장 분압(R7/R10 100k+100k, JP1 "BAT_AD" 점퍼로 GPIO3에 연결)
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_BATTERY_ADC_UNIT     ADC_UNIT_1
#define BSP_C3_BATTERY_ADC_CHANNEL  ADC_CHANNEL_3   /* GPIO3 */
#define BSP_C3_BATTERY_ADC_ATTEN    ADC_ATTEN_DB_12
#define BSP_C3_BATTERY_DIV          (2.0f)          /* 스키매틱 R7=R10=100k로 확인됨 */

/* ════════════════════════════════════════════════════════════
 * VIN(전원 소스) 감지 — J3 헤더 "V" 핀(net VIN)을 외부 100k+100k 분압해서 GPIO1로.
 * ADC 핀 기준 임계값(분압 후, 실제 VIN 아님) — 배선 후 실측 재보정 필요.
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_VIN_ADC_CHANNEL   ADC_CHANNEL_1   /* GPIO1 */
#define BSP_C3_VIN_ADC_ATTEN     ADC_ATTEN_DB_12
#define BSP_C3_VIN_PRESENT_MV    2200            /* 배터리단독 ADC핀 ~2100mV / USB연결 ADC핀 ~2300~2350mV 사이 — 재보정 필요 */

/* ════════════════════════════════════════════════════════════
 * 상태 LED — Green: ESP-NOW 링크 상태 / Blue: 배터리 잔량
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_LED_GREEN     GPIO_NUM_0
#define BSP_C3_LED_BLUE      GPIO_NUM_4

/**
 * @brief 보드 레벨 초기화 — 현재는 로그만 남김(센서/LED/ADC는 각자 컴포넌트가 스스로 초기화).
 *        향후 공통 보드 초기화가 필요해지면 여기에 추가.
 */
esp_err_t bsp_c3_pico_init(void);

#ifdef __cplusplus
}
#endif
