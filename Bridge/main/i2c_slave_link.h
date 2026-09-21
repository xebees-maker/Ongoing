#pragma once

/**
 * 2026-09-21 — CNTL과의 I2C 링크(브릿지 = 슬레이브). CNTL이 I2C 마스터로서 이 브릿지에
 * RELIABLE_SEND/FIRE_AND_FORGET/SET_CHANNEL/PING/RESET을 쓰고(마스터 write 트랜잭션),
 * 주기적으로 읽어서(마스터 read 트랜잭션) INCOMING_MSG/RELIABLE_RESULT/PONG을 가져감 —
 * bridge_link.h(Common/components/bridge_link)의 고정크기 패킷 하나를 그대로 주고받음.
 *
 * TODO(하드웨어 확인 필요): 아래 SDA/SCL 핀은 Seeed XIAO ESP32-C3 실크스크린 D4/D5(공식
 * 핀아웃 기준 GPIO6/GPIO7) 추정값이다. 실제 보드 배선 확인 전까지는 플레이스홀더로 취급할 것.
 */

#include "esp_err.h"
#include "bridge_link.h"

void i2c_slave_link_init(void);

/* ESP-NOW recv_cb에서 호출 — 수신한 원본 프레임(mac/rssi/payload)을 CNTL에게 넘길 INCOMING_MSG로
 * 큐잉함(비ISR 컨텍스트에서 부르는 걸 전제 — esp_now recv_cb는 ESP-NOW 태스크 컨텍스트라 안전) */
void i2c_slave_link_queue_incoming(const uint8_t *mac, int8_t rssi, const uint8_t *data, size_t len);
