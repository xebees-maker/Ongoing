#pragma once

/**
 * 2026-09-21(설계 — project_cntl_i2c_bridge_design_2026_09_21 메모리) — CNTL은 더 이상 ESP-NOW
 * 라디오를 직접 쓰지 않는다. 별도 Bridge 보드(XIAO Seeed, ESP32-C3)가 ESP-NOW 드라이버 전체를
 * 대신 갖고, CNTL과는 isolated I2C 4단자로만 연결된다(CNTL이 마스터, Bridge가 슬레이브).
 *
 * 이 모듈은 esp_now_hub.c/esp_now_tx.c/esp_now_photo.c가 지금까지 esp_now_send()/
 * esp_now_reliable_request()/esp_now_init()/esp_now_register_recv_cb()를 직접 부르던 자리에
 * 그대로 꽂아넣을 수 있는 신호 호환(signature-compatible) 대체 함수들을 제공한다 — CASK
 * 상태머신 등 그 위의 로직은 전혀 안 바뀐다(브릿지는 프로토콜을 모르는 투명 중계).
 *
 * TODO(하드웨어 확인 필요): 아래 SDA/SCL 핀은 CNTL 보드의 "isolated I2C 4단자" 실제 GPIO
 * 번호를 아직 모름(스키매틱에 없음, 대화로만 확인된 존재) — 플레이스홀더로 취급할 것.
 */

#include "esp_err.h"
#include "esp_now.h"  /* esp_now_recv_cb_t, esp_now_recv_info_t 재사용(시그니처 호환 목적) */

/* esp_now_init()+esp_now_register_recv_cb(recv_cb)를 대체 — I2C 마스터를 세팅하고, 브릿지가
 * 올려주는 INCOMING_MSG를 recv_cb에 그대로(esp_now_recv_info_t 형태로 재구성해서) 전달함 */
void i2c_bridge_init(esp_now_recv_cb_t recv_cb);

/* esp_now_send()를 대체(fire-and-forget, 재시도 없음) — 브릿지에게 1회 전송을 맡기고 큐잉
 * 결과만 돌려줌(esp_now_send()와 동일한 "로컬 접수됐다"는 의미, 실제 무선 송출 성공/실패는
 * 여기서 알 수 없음 — 기존 esp_now_send() 호출부들도 원래 그 반환값만 봤음) */
esp_err_t bridge_send(const uint8_t *mac, const void *data, size_t len);

/* esp_now_reliable_request()를 대체 — 시그니처 동일(esp_now_tx.c의 tx_worker_task가 그대로
 * 재사용). 내부적으로 브릿지에 RELIABLE_SEND를 보내고, 브릿지가 자기 라디오 옆에서 재시도
 * 루프를 돈 뒤 I2C로 돌려주는 RELIABLE_RESULT를 기다림(블로킹, 시그니처는 동일하지만 내부
 * 구현은 완전히 다름 — 재시도 자체는 이제 브릿지 쪽에서 일어남) */
esp_err_t bridge_reliable_request(const uint8_t *peer_mac,
                                   const void *req, size_t req_len,
                                   const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                                   uint32_t timeout_ms, int max_attempts,
                                   void *reply_out, size_t reply_out_cap, size_t *reply_out_len);

/* CNTL 자신의 WiFi 채널이 정해지면(또는 바뀌면) 브릿지에게 알려줌 — 브릿지가 그 채널에
 * 앉아있어야 Sens/CAM의 채널스캔이 브릿지를 찾을 수 있음(esp_now_hub_get_wifi_channel() 참고).
 * TODO — 지금은 esp_now_hub_init()에서 1회만 호출됨. 세션 중 채널이 바뀌는 경우(공유기
 * 자동채널전환/CSA)는 아직 이 함수를 다시 부르는 지점이 없음 — 후속 작업으로 등록 */
void i2c_bridge_set_channel(uint8_t channel);
