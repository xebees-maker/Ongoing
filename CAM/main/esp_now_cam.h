#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_now_link.h"

/**
 * CAM의 ESP-NOW 리프 노드 로직 — Sens/main/esp_now_node.c와 광고/채널스캔/페어링 부분은
 * 거의 동일(같은 세션에서 막 바꾼 로직이라 실기 검증 전이라 공유 컴포넌트로 뽑지 않고
 * 각자 사본으로 둠 — 나중에 둘 다 검증되면 Common/components/esp_now_peer로 통합 고려).
 * 페어링 후에는 Sens의 SENSOR_DATA 주기 전송 대신, Cntl의 PHOTO_REQUEST에 응답해서
 * SD에 저장된 사진을 청크로 잘라 보낸다.
 */

void esp_now_cam_init(void);
const char *esp_now_cam_get_name(void);
bool esp_now_cam_is_paired(void);

/** @brief 사진전송/목록조회/삭제 큐가 대기 중이거나 처리 중인가 — Deep Sleep 진입 전 대기
 *         윈도우 판정(cam_node.c의 cam_node_wake_window_done())에 씀 */
bool esp_now_cam_is_busy(void);

/** @brief 페어링됐는데 SLEEP_NOW를 아직 못 받아서 재요청(ESP_NOW_MSG_SLEEP_NOW_REQUEST)을
 *         보냄(2026-08-11) — cam_node_wake_window_done()이 주기적으로 호출 */
void esp_now_cam_send_sleep_now_request(void);

/** @brief Green 상태 LED GPIO 등록 (esp_now_cam_init() 이전에 호출) */
void esp_now_cam_set_status_led(gpio_num_t pin);
