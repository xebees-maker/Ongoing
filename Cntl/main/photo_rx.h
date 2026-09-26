#pragma once

/**
 * CAM 사진 프로토콜(콘 쪽) — 두 갈래:
 *  1. 사진 수신(META/CHUNK/DONE 재조립 + CRC32 검증) — CAM이 촬영 즉시 푸시해오는 사진
 *     (2026-09-18 SD 제거 재설계). 검증된 사진은 photo_storage로 저장.
 *  2. 지금촬영 진행 단계(CAPTURE_STATUS) — Cntl UI 진행 팝업 표시용.
 *
 * 2026-09-26 — 캠 SD 제거로 목록(LIST_*)/삭제(DELETE_*)/특정 사진 요청(fetch_by_id)은 삭제.
 * 수신 경로 자체는 설계(Docs/설계_CAN링크_2026-09-26.md)대로 SR을 브로 옮기면서 다시 짬.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void photo_rx_init(void);

/* 2026-08-26(사용자 지시) — "통신 중엔 안 재운다": SLEEP_NOW를 0이 아닌 값으로 보내는 조건
 * 셋 중 하나(node_hub.c의 send_cask_sleep_now() 참고) — 이 노드 대상으로 진행 중인
 * 트랜잭션(사진 수신/지금촬영)이 있으면 true. 시작부터 끝까지(사진을 다
 * 가져오는 등) 하나의 트랜잭션으로 취급 — 접수 ack만 받고 안 끝난 것도
 * 포함(지금촬영은 접수 ack != 진짜 완료). 트랜잭션마다 대상 mac을
 * 저장해두고 정확히 매칭함(s_photo_cam_mac/s_capture_cam_mac) — 여러 기기가 동시에
 * 붙어있어도 캠1 트랜잭션 때문에 캠2가 잘못 안 재워지는 일이 없음(2026-08-26, 사용자 지적:
 * "여러 기기와 물리면 변수를 혼동해서 짤까봐") */
bool photo_rx_is_transacting_with(const uint8_t *mac);

/* node_hub.c의 recv_cb가 PHOTO_ 계열/CAPTURE_STATUS 메시지를 여기로 넘겨줌.
 * src_mac: 보낸 CAM의 MAC(2026-08-05 추가 — CAPTURE_STATUS_ACK을 그 자리에서 돌려보내려면
 * 필요, photo_rx.c의 handle_capture_status() 참고) */
void photo_rx_on_recv(uint8_t msg_type, const uint8_t *src_mac, const uint8_t *data, int len);

/* ────────────────────────────────────────────────────────────
 * 1. 단일 사진 수신 — capture_now/fetch_by_id 공용
 * ──────────────────────────────────────────────────────────── */
typedef enum {
    PHOTO_RX_STATE_IDLE = 0,
    PHOTO_RX_STATE_RECEIVING,
    PHOTO_RX_STATE_READY,
    PHOTO_RX_STATE_ERROR,
} photo_rx_state_t;

photo_rx_state_t photo_rx_get_state(void);

/* READY 상태일 때 방금 완료돼서 캐시에 들어간 file_id — 이걸로 photo_rx_cache_get() 조회 */
uint32_t photo_rx_get_ready_file_id(void);

/* READY 확인 후 IDLE로 되돌릴 때 사용(list_ack()와 동일 패턴) */
void photo_rx_ready_ack(void);

/* ERROR 상태를 확인 후 IDLE로 되돌릴 때 사용 */
void photo_rx_clear(void);

/* 압축 JPEG 원본 캐시 조회 — 소유권은 안 넘어옴(내부 캐시가 계속 들고 있다가 꽉 차면
 * 오래된 것부터 교체), 호출부는 포인터로 그때그때 디코드만 해서 씀. 없으면 false */
bool photo_rx_cache_get(uint32_t file_id, const uint8_t **out_data, size_t *out_len);

/* RECEIVING 중 진행률(청크 수신/전체) — fetch 진행 팝업이 퍼센트/ETA 표시에 씀.
 * RECEIVING이 아닐 땐 의미 없는 값일 수 있으니 호출부가 state를 먼저 확인할 것 */
void photo_rx_get_chunk_progress(uint16_t *received, uint16_t *total);

/* 2026-09-04(사용자 설계: "이벤트로 처리해") — 사진 수신이 완료(성공/실패 둘 다)되는 바로 그
 * 지점에서 발생. 앱(UI)과 웹(httpd) 둘 다 폴링 대신 이 시점에 반응해야 함:
 * - 앱: ui_main.c가 photo_rx_set_ready_cb()로 콜백을 등록해두면, 완료 시 그 콜백이
 *   즉시(같은 호출 스택에서, 어느 태스크든) 불림 — 콜백 안에서 LVGL을 건드릴 거면 콜백
 *   구현부가 스스로 lv_async_call()로 미뤄야 함(이 모듈은 LVGL을 모름, 위 헤더 설명 참고)
 * - 웹: photo_rx_wait_cached()가 내부적으로 이 이벤트를 기다림(폴링 아님) */
typedef void (*photo_rx_event_cb_t)(void);
void photo_rx_set_ready_cb(photo_rx_event_cb_t cb);

/* file_id가 캐시에 들어올 때까지(성공) 또는 이번 수신이 실패로 끝날 때까지 이벤트로
 * 블로킹 대기(폴링 아님, xSemaphoreTake) — timeout_ms 안에 못 받으면 false.
 * httpd 태스크에서 부르는 용도(main.c) */
bool photo_rx_wait_cached(uint32_t file_id, uint32_t timeout_ms,
                                const uint8_t **out_data, size_t *out_len);

/* ────────────────────────────────────────────────────────────
 * 2. 지금촬영 — 진행 팝업 단계 추적(사진 전송은 안 함, 위 헤더 설명 참고)
 * ──────────────────────────────────────────────────────────── */
/* 2026-08-21 핸드셰이크 재설계(사용자 설계) — INIT_NEEDED/INIT_DONE/CAPTURING 추가, 각각
 * 독립된 타임아웃으로 판정(project_cntl_cam_capture_now_handshake_redesign 메모리 참고).
 * 카메라 초기화가 필요 없는 경우 INIT_NEEDED/INIT_DONE은 아예 안 오고 ACKED에서 바로
 * CAPTURING으로 넘어감 — 팝업도 그 두 단계를 건너뛰고 표시함(ui_main.c 참고) */
typedef enum {
    PHOTO_RX_CAPTURE_STAGE_NONE = 0,
    PHOTO_RX_CAPTURE_STAGE_SENT,             /* 1단계: Cntl -> CAM 요청 보냄, 접수 대기 */
    PHOTO_RX_CAPTURE_STAGE_ACKED,            /* 1단계 완료: CAM이 접수 확인 */
    PHOTO_RX_CAPTURE_STAGE_INIT_NEEDED,      /* 카메라 초기화 시작 */
    PHOTO_RX_CAPTURE_STAGE_INIT_DONE,        /* 카메라 초기화 완료 */
    PHOTO_RX_CAPTURE_STAGE_CAPTURING,        /* 실제 촬영(워밍업+실샷) 시작 */
    PHOTO_RX_CAPTURE_STAGE_CAPTURED,         /* 최종: 촬영 성공 */
    PHOTO_RX_CAPTURE_STAGE_CAPTURE_FAILED,   /* 최종: 촬영 실패(초기화 실패 포함) */
} esp_now_capture_stage_t;

/* Cntl -> CAM: 즉시 새로 촬영하라는 요청 — 사진은 안 받음(CAPTURE_STATUS까지만) */
void photo_rx_capture_now(const uint8_t *cam_mac);

esp_now_capture_stage_t photo_rx_get_capture_stage(void);

/* 팝업이 최종 단계(CAPTURED/CAPTURE_FAILED)를 확인한 뒤 NONE으로 되돌릴 때 사용 —
 * 다음 지금촬영 요청 전에 호출 안 해도 capture_now()가 알아서 SENT로 덮어씀 */
void photo_rx_capture_stage_clear(void);


#ifdef __cplusplus
}
#endif
