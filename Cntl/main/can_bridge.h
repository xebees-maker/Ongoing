#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_now.h"  /* esp_now_recv_info_t 타입만 재사용(콘의 recv_cb 시그니처 호환용) —
                          esp_now_init/send 등 실제 드라이버 함수는 전혀 안 씀 */

/**
 * 2026-09-23 — 콘은 ESP-NOW를 전혀 모름(사용자 지시: "콘에 ESP-NOW관련된 어떠한 기능도
 * 없어"). 이 모듈이 콘 쪽에서 예전 esp_now_send()/esp_now_reliable_request()/
 * esp_now_init()+register_recv_cb() 자리를 그대로 대신함 — 시그니처를 최대한 똑같이 맞춰서
 * hub.c/tx.c 등 호출부 수정을 함수 이름 교체 수준으로 최소화(예전 I2C 브릿지(i2c_bridge.c)
 * 설계와 동일한 이유).
 *
 * 실제 무선/재시도는 전부 브(Bridge)가 함 — 콘은 CAN을 통해 브의 기능을 "호출"만 함(IPC
 * 개념, 사용자 설계: "IFC에서 IPC처럼 바뀌는 거지"). reliable 재시도는 브가 기존
 * esp_now_reliable 컴포넌트를 그대로 써서 수행 — 콘 쪽엔 재시도 로직이 전혀 없음.
 */

typedef void (*can_bridge_recv_cb_t)(const esp_now_recv_info_t *info, const uint8_t *data, int len);

/* esp_now_init()+esp_now_register_recv_cb(recv_cb) 자리. recv_cb는 브가 CAN_DATA_RELAY로
 * 중계한(즉 브가 ESP-NOW로 실제 받은) 프레임마다 한 번씩 불림 — info->src_addr/rx_ctrl->rssi만
 * 유효(그 외 필드는 0으로 채움, 기존 hub.c recv_cb가 이 두 필드만 씀 — 예전 I2C 설계에서
 * 이미 확인된 사실 그대로) */
void can_bridge_init(can_bridge_recv_cb_t recv_cb);

/* esp_now_send(mac, data, len) 자리 — fire-and-forget, 브가 큐잉만 하고 바로 반환.
 * ADVERTISE_ACK처럼 응답을 기다리지 않는 단발 전송용.
 * 이름이 can_bridge_send가 아니라 can_bridge_relay_send인 이유: 공유 라이브러리
 * (Common/components/can_bridge_link)에 이미 같은 이름의 저수준 ISO-TP 송신 함수가 있어서
 * 이름 충돌을 피함 — 이 함수는 그 위에 얹힌 콘 전용 고수준 래퍼 */
esp_err_t can_bridge_relay_send(const uint8_t *mac, const void *data, size_t len);

/* esp_now_reliable_request(...)와 완전히 같은 시그니처 — 내부적으로 CAN을 통해 브에
 * "이거 reliable로 보내고 결과 줘"라고 요청, 브가 esp_now_reliable_request()를 실제로 실행한
 * 뒤 결과를 CAN으로 돌려줄 때까지 이 호출이 블로킹됨(호출부 입장에선 예전과 동일하게 보임) */
esp_err_t can_bridge_reliable_request(const uint8_t *peer_mac,
                                       const void *req, size_t req_len,
                                       const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                                       uint32_t timeout_ms, int max_attempts,
                                       void *reply_out, size_t reply_out_cap, size_t *reply_out_len);
