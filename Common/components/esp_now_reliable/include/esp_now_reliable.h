#pragma once

/**
 * Reliable 모드(Layer 1) — CNTL↔노드 "요청 하나 보내고 응답 하나 받는" 핸드셰이크류 메시지용
 * Stop-and-Wait ARQ (2026-08-05).
 *
 * 오늘(2026-08-04) 세션 초반에 사용자와 함께 확정한 원칙 그대로 구현:
 *  - ESP-NOW는 UDP에 대응하는 무연결·비신뢰 원시 전송으로 본다. 그 위에 Stop-and-Wait ARQ를
 *    하부 추상화 계층으로 둔다.
 *  - 물리 ACK(802.11 MAC, esp_now_send()의 send_cb 성공/실패)는 "진짜 도달 확인"이 아니다
 *    (Two Generals Problem — 상대가 받았다는 사실 자체를 무선 프레임 하나로는 확정할 수
 *    없음). 애플리케이션 레벨 응답 메시지가 왔을 때만 "진짜 도달"로 인정한다.
 *  - 상위(요청을 보내는 쪽) 코드는 더 이상 "이 전송이 됐는지"를 개별적으로 의심/재시도하지
 *    않는다 — 이 레이어를 통해서만 보내면 성공(응답 페이로드) 또는 타임아웃만 돌려받는다.
 *
 * 범위: 청크(PHOTO_CHUNK)/LIST_ENTRY/ADVERTISE/CHANNEL_PING처럼 이미 자기 나름의 스트리밍
 * 또는 주기적 재시도 방식이 있는 메시지는 이 레이어를 안 씀 — "요청 1개 -> 응답 1개"로 끝나는
 * 핸드셰이크류(페어링, 목록조회, 지금촬영, 삭제, DONE/LIST_DONE 확인 등)만 대상.
 *
 * 동시에 하나의 요청만 대기하는 단순한 구조 — 호출부가 이미 전용 태스크 하나로 직렬화돼
 * 있다는 전제(Cntl의 esp_now_tx 태스크, CAM의 photo_tx 태스크)라 여러 요청을 동시에 걸
 * 필요가 없음. 여러 태스크에서 동시에 부르면 내부 뮤텍스로 그냥 순서대로 처리됨(느려질 뿐
 * 틀리진 않음).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* peer_mac으로 req(req_len바이트)를 보내고, accept_reply_types 중 하나가 peer_mac에서 올 때까지
 * 최대 timeout_ms 동안 기다린다. 안 오면 req를 다시 보내고 또 기다리기를 max_attempts번까지
 * 반복한다. 응답이 오면 reply_out(reply_out_cap바이트까지)에 복사하고 ESP_OK, 끝까지 못 받으면
 * ESP_ERR_TIMEOUT을 반환한다. reply_out/reply_out_len은 NULL 가능(응답 내용이 필요 없을 때). */
esp_err_t esp_now_reliable_request(const uint8_t *peer_mac,
                                    const void *req, size_t req_len,
                                    const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                                    uint32_t timeout_ms, int max_attempts,
                                    void *reply_out, size_t reply_out_cap, size_t *reply_out_len);

/* recv_cb(ESP-NOW 드라이버 태스크)에서 매 수신마다 호출 — 지금 대기 중인 요청이 있고
 * src_mac+msg_type이 맞으면 페이로드를 복사해두고 대기 태스크를 깨움. 대기 중인 요청이
 * 없거나 안 맞으면 조용히 무시하고 리턴하므로, 기존 dispatch 로직 앞단에 한 줄만 추가하면 됨
 * (esp_now_channelsync_on_recv()와 동일한 패턴) */
void esp_now_reliable_on_recv(uint8_t msg_type, const uint8_t *src_mac,
                               const uint8_t *data, int len);

#ifdef __cplusplus
}
#endif
