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
 *         윈도우 판정(cam_node.c)에 씀 */
bool esp_now_cam_is_busy(void);

/** @brief 2026-08-25(CASK 재설계) — 알려진 CNTL에 광고 없이 곧장 유니캐스트로 재연결 시도.
 *         성공하면 true(PAIRED 상태까지 전환됨), 실패하면 채널스캔 폴백을 알아서 시작(또는
 *         이미 돌고 있으면 재개)하고 false 리턴 — cam_node.c의 웨이크 루프가 침묵
 *         타임아웃으로 재시도할 때도 이 함수 하나만 다시 부르면 됨(esp_now_cam_init()도
 *         내부적으로 이 함수를 씀) */
bool esp_now_cam_reconnect(void);

/** @brief Green 상태 LED GPIO 등록 (esp_now_cam_init() 이전에 호출) */
void esp_now_cam_set_status_led(gpio_num_t pin);
