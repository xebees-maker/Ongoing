/**
 * @file    battery.h
 * @brief   배터리 전압/잔량(%) 측정 + USB 연결 여부 — Cntl/Sens 공용
 */
#pragma once

#include <stdbool.h>
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    adc_unit_t    adc_unit;     /**< 배터리 전압 측정용 ADC 유닛 */
    adc_channel_t adc_channel;  /**< 배터리 전압 측정용 ADC 채널 */
    adc_atten_t   atten;        /**< ADC 감쇄 설정 */
    float         divider;      /**< 분압비 — Vbat_mV = Vadc_mV * divider */
    float         full_mv;      /**< 100% 기준 전압 (mV) */
    float         empty_mv;     /**< 0% 기준 전압 (mV) */
    gpio_num_t    ctrl_gpio;    /**< 분압기 활성화 게이트 핀 (없으면 GPIO_NUM_NC) */
} battery_config_t;

/**
 * @brief 배터리 ADC 초기화 (+ 분압기 게이트 핀이 있으면 활성화)
 * @return 성공 시 true
 */
bool battery_init(const battery_config_t *cfg);

/** @brief 배터리 전압(mV) 읽기 — 분압비 적용 완료된 실제 VBAT 추정값 */
int battery_read_mv(void);

/** @brief VBAT(mV) → 잔량(%) 변환 — full_mv/empty_mv 기준 선형 매핑, 0~100 클램프
 *  여러 샘플을 평균낸 mV 값에도 그대로 적용 가능 (호출측에서 자체 평균 후 사용) */
int battery_mv_to_pct(int vbat_mv);

/** @brief 배터리 잔량(%) 읽기 — 단일 샘플 기준 (battery_read_mv() + battery_mv_to_pct()) */
int battery_read_pct(void);

/** @brief USB(시리얼/JTAG) 연결 여부 — 충전 중 여부 판단에 사용 */
bool battery_is_usb_connected(void);

/** @brief 배터리별 완충 전압 보정값 갱신 — 학습된 값으로 battery_mv_to_pct() 기준을 바꿈
 *  (배터리마다 충전 IC가 종료시키는 실제 완충 전압이 달라서 호출측에서 추정한 값을 반영) */
void battery_set_full_mv(float full_mv);

/** @brief battery_init()이 만든 ADC 유닛 핸들 — 같은 유닛의 다른 채널(예: VBUS 분압)을
 *  추가로 설정해 쓰고 싶을 때 재사용. battery_init() 호출 후에만 유효 */
adc_oneshot_unit_handle_t battery_get_adc_handle(void);

#ifdef __cplusplus
}
#endif
