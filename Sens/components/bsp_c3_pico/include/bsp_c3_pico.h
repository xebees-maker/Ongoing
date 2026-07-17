/**
 * @file    bsp_c3_pico.h
 * @brief   LOLIN C3 Pico(ESP32-C3) 헤드리스 센서 노드 보드 정의
 *
 * bsp_c6.h와 달리 LCD/터치가 없다 — I2C 센서(SCD41/SHT4x)는 각자 전용 버스를
 * 스스로 만들어 쓰므로(scd41_init/sht4x_init) 여기서 공유 버스를 만들지 않는다.
 * 이 헤더는 핀 상수만 모아둔다.
 *
 * ⚠ 아래 GPIO 번호는 아직 실물 LOLIN C3 Pico 핀아웃과 대조해서 확정한 값이 아니다.
 *   배선하기 전에 반드시 보드 실크스크린/핀아웃 문서를 보고:
 *   - 스트래핑 핀(GPIO2, GPIO8, GPIO9)과 겹치지 않는지
 *   - USB D+/D-(보드에 따라 GPIO18/19)와 겹치지 않는지
 *   확인한 뒤 이 파일의 값을 실측값으로 교체할 것.
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
#define BSP_C3_I2C_PORT      1
#define BSP_C3_I2C_SDA       GPIO_NUM_4   /* TODO: 실물 핀아웃 대조 후 확정 */
#define BSP_C3_I2C_SCL       GPIO_NUM_5   /* TODO: 실물 핀아웃 대조 후 확정 */

/* ════════════════════════════════════════════════════════════
 * DHT22 (단일 GPIO bit-bang)
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_DHT22_PIN     GPIO_NUM_6   /* TODO: 실물 핀아웃 대조 후 확정 */

/* ════════════════════════════════════════════════════════════
 * 배터리 ADC
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_BATTERY_ADC_UNIT     ADC_UNIT_1
#define BSP_C3_BATTERY_ADC_CHANNEL  ADC_CHANNEL_0   /* TODO: 실물 핀아웃 대조 후 확정 */
#define BSP_C3_BATTERY_ADC_ATTEN    ADC_ATTEN_DB_12
#define BSP_C3_BATTERY_DIV          (2.0f)          /* TODO: 실제 분압 저항비로 교체 */

/* ════════════════════════════════════════════════════════════
 * VBUS 감지 (USB 전원 유무 판별용 분압)
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_VBUS_ADC_CHANNEL  ADC_CHANNEL_1   /* TODO: 실물 핀아웃 대조 후 확정 */
#define BSP_C3_VBUS_ADC_ATTEN    ADC_ATTEN_DB_12
#define BSP_C3_VBUS_PRESENT_MV   1200            /* Sens/main/sensor-c6.c 실측값 참고 — 재보정 필요 */

/* ════════════════════════════════════════════════════════════
 * 상태 LED — Green: ESP-NOW 링크 상태 / Blue: 배터리 잔량
 * ════════════════════════════════════════════════════════════ */
#define BSP_C3_LED_GREEN     GPIO_NUM_7   /* TODO: 실물 핀아웃 대조 후 확정 */
#define BSP_C3_LED_BLUE      GPIO_NUM_10  /* TODO: 실물 핀아웃 대조 후 확정 */

/**
 * @brief 보드 레벨 초기화 — 현재는 로그만 남김(센서/LED/ADC는 각자 컴포넌트가 스스로 초기화).
 *        향후 공통 보드 초기화가 필요해지면 여기에 추가.
 */
esp_err_t bsp_c3_pico_init(void);

#ifdef __cplusplus
}
#endif
