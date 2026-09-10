#pragma once

/**
 * Cntl 전용 송신 스케줄러(Layer 1, 2026-08-05 도입 -> 2026-09-10 재설계). UI가 부르는
 * esp_now_hub_pair()/esp_now_photo_*() 요청 함수들은 그대로 즉시 리턴(요청을 여기로 큐잉만
 * 함, 지금까지와 동일한 UX — UI 안 얼어붙음).
 *
 * 2026-09-10 재설계(사용자 설계, TX_QUEUE_FULL 근본원인 조사 후) — 예전엔 "큐 하나 + 소비
 * 태스크 하나"였는데, PAIR_REQUEST 하나가 응답 없을 때 최대 6.5초까지 재시도하며 그 태스크를
 * 독점해서 무관한 다른 기기의 전송까지 같이 막히는 head-of-line blocking이 있었음(콘 리붓
 * 직후 재페어링 폭주 시 32칸 큐가 넘치는 근본원인). 지금은 입구 큐(esp_now_tx_enqueue가
 * 넣는 곳)를 "디스패처" 태스크 하나만 소비하는데, 디스패처는 무선 전송을 직접 하지 않고
 * mac별로 존재하는(또는 새로 만드는) 전용 워커 태스크의 개인 큐로 넘기기만 함(매우 빠름,
 * 절대 블로킹 안 됨). 실제 esp_now_reliable_request() 블로킹 호출은 각 워커 태스크 안에서만
 * 일어나므로 기기별로 독립적/병렬적으로 진행됨. 워커는 할 일이 떨어지면(TX_WORKER_IDLE_MS
 * 동안 유휴) 스스로 vTaskDelete(NULL)로 종료 — 고정 태스크 상시 할당(영구 RAM 낭비)도,
 * 단일 태스크 큐셋(진짜 동시성 없이 공정성만 있는 방식, 사용자가 명시적으로 기각)도 아닌
 * 동적 스폰/자진종료 방식.
 *
 * 이 모듈은 상태를 전혀 안 건드림 — 순수 "재시도 포함 전송 스케줄러"다. 응답이 왔을 때의
 * 상태 반영(n->paired=true, s_capture_stage 갱신 등)은 지금처럼 recv_cb -> 각 handle_*()가
 * 그대로 담당한다(esp_now_reliable_on_recv()는 대기 중인 해당 워커 태스크를 깨우기만 하고, 그
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
