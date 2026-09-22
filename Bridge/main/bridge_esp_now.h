#pragma once

#include <stdint.h>

/**
 * 2026-09-22 — 브(브릿지)가 ESP-NOW 라디오를 직접 소유. 받은 건 뭐든 해석 없이 CAN DATA로
 * 콘에 그대로 중계(투명 릴레이), 콘이 CAN DATA로 준 것도 뭐든 그대로 ESP-NOW로 내보냄 —
 * project_cntl_i2c_bridge_design_2026_09_21 설계 그대로(전송 계층만 I2C에서 CAN으로).
 */

void bridge_esp_now_init(void);

/* can_link.c가 콘에서 받은 DATA 완성 메시지를 실제 ESP-NOW로 내보낼 때 씀 */
void bridge_esp_now_send_raw(const uint8_t mac[6], const uint8_t *data, uint16_t len);
