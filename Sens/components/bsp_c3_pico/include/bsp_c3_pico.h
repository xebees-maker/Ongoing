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
 * MQ137 (암모니아, 아날로그 AO + 디지털 DO) — 2026-09-12 실험용 배선.
 * LED를 GPIO2/5로 옮겨서 비운 자리(둘 다 진짜 ADC 핀)를 씀 — AO는 ADC 필요, DO는
 * 그냥 디지털이라 아무 핀이어도 되지만 같이 비워진 GPIO4를 그대로 씀
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_MQ137_AO_ADC_UNIT     ADC_UNIT_1
#define BSP_C3_MQ137_AO_ADC_CHANNEL  ADC_CHANNEL_0   /* GPIO0 */
#define BSP_C3_MQ137_AO_ADC_ATTEN    ADC_ATTEN_DB_12
#define BSP_C3_MQ137_DO_PIN          GPIO_NUM_4

/* ════════════════════════════════════════════════════════════
 * SC05-NH3 (YYS, 전기화학식 암모니아, UART) — 2026-09-15 실험용 배선.
 * 이 보드는 J3 헤더(GND/VIN/IO6/IO8/IO10 등이 한 줄에 나옴, WeMos 공식
 * sch_c3_pico_v1.0.0.pdf 확인) 하나에서 GND/VIN/RX/TX를 전부 뽑음 — 납땜 편의상
 * GPIO0/4(J2 쪽) 대신 GPIO6/8(J3 쪽)로 선택(사용자 확인). U0TXD/U0RXD(GPIO20/21)는
 * 콘솔 UART0가 이미 쓰고 있어서 제외(sdkconfig CONFIG_ESP_CONSOLE_UART_NUM=0).
 * 전원은 VIN(배터리/USB 합류, 실측 3.7~4.7V) — SC05 동작전압 3.7~5.5V 범위 안.
 * 단, 배터리 단독 방전 시 3.7V 밑으로 내려가면 스펙 밖이라, 이 노드는 MQ137처럼
 * 상시전원(USB) 전제(사용자 확인, "배터리 단독 안 된다"는 결론까지 나눈 대화 참고).
 * 통신은 Auto 모드(공장 기본값, 별도 명령 없이 1초마다 9바이트 프레임을 그냥 쏨)를
 * 그대로 씀 — Non-Auto 전환 명령 바이트가 데이터시트 내에서도 서로 안 맞아
 * (Table5=0x40 vs Table6=0x41/prose="0x03,0x04") 신뢰 안 하기로 함.
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_SC05_UART_PORT   1
#define BSP_C3_SC05_UART_RX     GPIO_NUM_8   /* SC05 Pin6/T(TXD) <- 여기로 들어옴 */
#define BSP_C3_SC05_UART_TX     GPIO_NUM_6   /* SC05 Pin5/R(RXD) <- 여기로 나감(Auto 모드라 실제로 안 씀) */
#define BSP_C3_SC05_UART_BAUD   9600

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
 * 2026-09-12(사용자 지시 — "향후 임의센서 부착에 유연성을 주려고") — 원래 GPIO0/4(둘 다
 * 진짜 ADC 핀)에 있던 LED를, 이 보드에서 유일하게 비어있던 비-ADC 핀 GPIO2/5로 옮김.
 * GPIO2는 스트래핑 핀이지만 LED는 MCU가 능동적으로 켜고 끄는 수동소자라 외부 센서
 * 출력과 달리 리셋 시점 전압을 흔들 위험이 낮음. 이 덕분에 GPIO0/4가 비어서 향후
 * ADC가 필요한 센서(예: MQ137 AO)가 스트래핑 핀을 안 거치고 쓸 수 있게 됨.
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_LED_GREEN     GPIO_NUM_5
#define BSP_C3_LED_BLUE      GPIO_NUM_2

/**
 * @brief 보드 레벨 초기화 — 현재는 로그만 남김(센서/LED/ADC는 각자 컴포넌트가 스스로 초기화).
 *        향후 공통 보드 초기화가 필요해지면 여기에 추가.
 */
esp_err_t bsp_c3_pico_init(void);

#ifdef __cplusplus
}
#endif
