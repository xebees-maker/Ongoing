#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_now_link.h"

/**
 * ESP-NOW 노드 신원 + 광고(advertise) 브로드캐스트 + 허브 페어링.
 * 페어링되면 advertise를 멈추고 같은 주기로 허브에 SENSOR_DATA를 유니캐스트한다.
 * 페어링이 끊기거나(연속 전송 실패) 장시간 페어링에 실패하면 자동으로 다시 advertise로
 * 돌아가서 무한 재시도한다 — 사용자가 리셋할 필요 없음.
 */

void esp_now_node_init(void);
const char *esp_now_node_get_name(void);
bool esp_now_node_is_paired(void);

/**
 * @brief 매 샘플 주기마다 앱(sensor-c6.c / sensor_node.c)이 호출 — 다음 SENSOR_DATA
 *        전송 타이머가 돌 때 이 값을 그대로 실어보낸다. chan_count는
 *        ESP_NOW_MAX_CHANNELS를 넘으면 안 됨.
 */
void esp_now_node_set_readings(sensor_kind_t sensor_kind, uint8_t chan_count,
                                const uint8_t *chan_type, const uint8_t *chan_ok,
                                const float *chan_val,
                                bool batt_ok, int batt_pct, bool powered);

/**
 * @brief Green 상태 LED GPIO 등록 (esp_now_node_init() 이전에 호출).
 *        생략하면(호출 안 하면) LED 갱신 없이 동작.
 */
void esp_now_node_set_status_led(gpio_num_t pin);
