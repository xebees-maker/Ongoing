#pragma once

/**
 * Cntl 전용 송신 태스크(Layer 1, 2026-08-05) — CAM의 photo_tx와 같은 패턴: 큐 하나 + 태스크
 * 하나. UI가 부르는 esp_now_hub_pair()/esp_now_photo_*() 요청 함수들은 그대로 즉시 리턴
 * (요청을 여기로 큐잉만 함, 지금까지와 동일한 UX — UI 안 얼어붙음). 실제
 * esp_now_reliable_request() 블로킹 호출은 이 태스크 안에서만 일어남.
 *
 * 이 모듈은 상태를 전혀 안 건드림 — 순수 "재시도 포함 전송 스케줄러"다. 응답이 왔을 때의
 * 상태 반영(n->paired=true, s_capture_stage 갱신 등)은 지금처럼 recv_cb -> 각 handle_*()가
 * 그대로 담당한다(esp_now_reliable_on_recv()는 대기 중인 이 태스크를 깨우기만 하고, 그
 * 뒤에도 recv_cb의 나머지 dispatch가 정상적으로 이어서 실행됨 — 두 경로가 같은 메시지를
 * 각자의 목적으로 병행 처리). 실패(응답 없음)는 진단용 로그로만 남김(2026-08-10 —
 * 예전엔 여기서 일괄로 UI_ERR_NOT_PAIRED 토스트도 띄웠는데, 이 모듈은 어떤 요청이든
 * 다 거쳐가는 범용 계층이라 "왜 실패했는지"에 대한 UI 피드백은 호출부마다 자기 상황에
 * 맞는 전용 에러코드로 이미 처리하고 있어서(UI_ERR_FETCH_NORESPONSE 등) 여기서 또
 * 일괄 처리하면 중복/오해석 에러가 뜸 — 실사용 중 발견).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_now_tx_init(void);

/* mac으로 req(req_len바이트, TX_REQ_MAX_LEN 이하)를 큐잉 — esp_now_tx 태스크가 순서대로
 * esp_now_reliable_request()로 보냄. accept_reply_types는 반드시 static(또는 그에 준하게
 * 오래 유지되는) 배열이어야 함 — 태스크가 나중에(비동기로) 읽음. what은 실패 로그에 쓸
 * 짧은 설명(문자열 리터럴 등 정적 문자열 권장) */
void esp_now_tx_enqueue(const uint8_t *mac, const void *req, size_t req_len,
                         const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                         uint32_t timeout_ms, int max_attempts, const char *what);

#ifdef __cplusplus
}
#endif
