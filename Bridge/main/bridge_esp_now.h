#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * 2026-09-22 — 브(브릿지)가 ESP-NOW 라디오를 직접 소유. 받은 건 뭐든 해석 없이 CAN DATA로
 * 콘에 그대로 중계(투명 릴레이), 콘이 CAN DATA로 준 것도 뭐든 그대로 ESP-NOW로 내보냄 —
 * project_cntl_i2c_bridge_design_2026_09_21 설계 그대로(전송 계층만 I2C에서 CAN으로).
 */

void bridge_esp_now_init(void);

/* 2026-09-26 — ESP-NOW 초기화가 끝났으면 true(그 전에 온 콘의 무선 송신 요청은 버림) */
bool bridge_esp_now_is_ready(void);

/* 2026-10-18(설계_보브_보릴 §4.3) — 콘이 정한 채널로 라디오를 옮김(CAN_CTRL_SET_CHANNEL). 이웃 채널 광고 필터도 새 채널 기준.
 * 이 브에 붙은 노드는 채널을 잃고 다시 훑어 찾음. 지금 채널은 bridge_esp_now_channel() — PONG에 실어 콘에 알림 */
void bridge_esp_now_set_channel(uint8_t channel);
uint8_t bridge_esp_now_channel(void);
bool bridge_esp_now_channel_set_by_cntl(void);

/* can_link.c가 콘에서 받은 DATA 완성 메시지를 실제 ESP-NOW로 내보낼 때 씀 */
void bridge_esp_now_send_raw(const uint8_t mac[6], const uint8_t *data, uint16_t len);

/* can_link.c가 RELIABLE_SEND 처리 시 esp_now_reliable_request()를 직접 부르기 전에 씀 —
 * ESP-NOW는 peer로 등록 안 된 MAC에는 esp_now_send() 자체가 즉시 실패(ESP_ERR_ESPNOW_NOT_FOUND)
 * 하므로 반드시 먼저 호출해야 함(bridge_esp_now_send_raw 내부의 add_peer_if_needed와 동일 로직) */
void bridge_esp_now_ensure_peer(const uint8_t mac[6]);

/* 2026-09-26 — 콘으로 보낼 app 메시지(app_header+body)를 경로별 송신 큐에 넣음(복사, 블로킹 없음).
 * RELIABLE_SEND 대행 완료 콜백처럼 CAN 송신을 직접 하면 안 되는 문맥에서 씀 */
void bridge_esp_now_queue_to_cntl(const uint8_t *msg, size_t len);

/* 2026-09-26(4단계) — CAN Data 송신 큐에 쌓인 메시지 수(SR 흐름 제어용) */
uint32_t bridge_esp_now_data_backlog(void);
