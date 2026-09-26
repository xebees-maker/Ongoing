#pragma once

/* 2026-09-26(설계 Docs/설계_CAN링크_2026-09-26.md §4, 4단계) — 브가 사진 전송(SR)의 끝점.
 * 캠의 META/청크/윈도 상태 요청/DONE을 브가 직접 처리하고 응답(META_ACK, WINDOW_STATUS_ACK, DONE_ACK)도 브가
 * 만듦. 콘에는 순서를 맞춘 스트림(CAN_DATA_SR_META/CHUNK/DONE)만 Data 경로로 보냄. 캠별 상태, 순서가 어긋난
 * 청크는 SR_HOLD개까지만 잠깐 보관(사진 전체는 안 들고 있음). */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* bridge_esp_now_init()에서 recv_cb 등록 전에 1회 */
void bridge_sr_init(void);

/* recv_cb에서 매 수신마다 — SR 7종이면 처리하고 true(콘으로 RELAY하지 않음), 아니면 false */
bool bridge_sr_on_recv(const uint8_t *src_mac, const uint8_t *data, int len);

/* Data 릴레이 태스크가 CAN으로 하나 보낼 때마다 — 흐름 제어로 미뤄 둔 WINDOW_STATUS_ACK를 큐가 빠지면 보냄 */
void bridge_sr_on_tx_progress(void);

#ifdef __cplusplus
}
#endif
