#pragma once

/**
 * 2026-09-21 — CNTL과의 I2C 링크(브릿지 = 슬레이브). CNTL이 I2C 마스터로서 이 브릿지에
 * RELIABLE_SEND/FIRE_AND_FORGET/SET_CHANNEL/PING/RESET을 쓰고(마스터 write 트랜잭션),
 * 주기적으로 읽어서(마스터 read 트랜잭션) INCOMING_MSG/RELIABLE_RESULT/PONG을 가져감 —
 * bridge_link.h(Common/components/bridge_link)의 고정크기 패킷 하나를 그대로 주고받음.
 *
 * 2026-09-21 — 실제 보드는 XIAO ESP32-**C6**(사용자가 처음엔 "C3"로 잘못 말했다가 정정,
 * 출처: https://wiki.seeedstudio.com/xiao_esp32c6_getting_started). I2C 핀은 사용자가 직접
 * 확인: SDA=D4=GPIO22, SCL=D5=GPIO23 — 아래 값은 더 이상 플레이스홀더가 아님.
 */

#include "esp_err.h"
#include "bridge_link.h"

void i2c_slave_link_init(void);

/* ESP-NOW recv_cb에서 호출 — 수신한 원본 프레임(mac/rssi/payload)을 CNTL에게 넘길 INCOMING_MSG로
 * 큐잉함(비ISR 컨텍스트에서 부르는 걸 전제 — esp_now recv_cb는 ESP-NOW 태스크 컨텍스트라 안전) */
void i2c_slave_link_queue_incoming(const uint8_t *mac, int8_t rssi, const uint8_t *data, size_t len);

/* 2026-09-22(임시 진단) — on_request ISR이 실제로 tx 태스크를 깨우는지 확인용 카운터.
 * 확인 끝나면 diag_task와 함께 제거할 것 */
void i2c_slave_link_get_diag_counters(uint32_t *on_request, uint32_t *on_request_woken, uint32_t *tx_notified);
