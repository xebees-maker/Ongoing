#pragma once

/**
 * CAM 사진 프로토콜 — 세 갈래:
 *  1. 단일 사진 수신(META/CHUNK/DONE 재조립 + CRC32 검증) — 지금촬영(capture_now)과
 *     목록에서 특정 사진 보기(fetch_by_id)가 공유해서 씀.
 *  2. 지금촬영 진행 단계(CAPTURE_STATUS) — Cntl UI 진행 팝업 표시용.
 *  3. 사진 "목록"만 가져오기(LIST_REQUEST/ENTRY/DONE, 내용 전송 없음) + 삭제.
 *
 * recv_cb(ESP-NOW/WiFi 드라이버 태스크)에서 esp_now_photo_on_recv()로 넘겨받아 처리하고,
 * 검증된 사진은 esp_now_photo_consume()으로 소유권째 LVGL 워커 태스크(UI)에 넘긴다 —
 * 그래야 UI가 화면에 표시 중인 이전 버퍼를 다른 태스크가 free()하는 일이 없다.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_now_photo_init(void);

/* esp_now_hub.c의 recv_cb가 PHOTO_ 계열/CAPTURE_STATUS 메시지를 여기로 넘겨줌 */
void esp_now_photo_on_recv(uint8_t msg_type, const uint8_t *data, int len);

/* ────────────────────────────────────────────────────────────
 * 1. 단일 사진 수신 — capture_now/fetch_by_id 공용
 * ──────────────────────────────────────────────────────────── */
typedef enum {
    ESP_NOW_PHOTO_STATE_IDLE = 0,
    ESP_NOW_PHOTO_STATE_RECEIVING,
    ESP_NOW_PHOTO_STATE_READY,
    ESP_NOW_PHOTO_STATE_ERROR,
} esp_now_photo_state_t;

esp_now_photo_state_t esp_now_photo_get_state(void);

/* READY일 때만 성공 — 성공하면 버퍼 소유권이 호출자에게 넘어가고(직접 heap_caps_free
 * 해야 함), 상태는 IDLE로 돌아감. UI(LVGL 워커 태스크)에서만 호출할 것 */
bool esp_now_photo_consume(const uint8_t **out_data, size_t *out_len);

/* ERROR 상태를 확인 후 IDLE로 되돌릴 때 사용 */
void esp_now_photo_clear(void);

/* ────────────────────────────────────────────────────────────
 * 2. 지금촬영 — 진행 팝업 단계 추적
 * ──────────────────────────────────────────────────────────── */
typedef enum {
    ESP_NOW_CAPTURE_STAGE_NONE = 0,
    ESP_NOW_CAPTURE_STAGE_SENT,             /* 1단계: Cntl -> CAM 요청 보냄, 접수 대기 */
    ESP_NOW_CAPTURE_STAGE_ACKED,            /* 1단계 완료: CAM이 접수 확인 */
    ESP_NOW_CAPTURE_STAGE_CAPTURED,         /* 2단계 완료(성공): 전송 대기/진행 중(3단계로) */
    ESP_NOW_CAPTURE_STAGE_CAPTURE_FAILED,   /* 2단계 완료(실패) */
    ESP_NOW_CAPTURE_STAGE_TRANSFER_DONE,    /* 3단계 완료: 사진 수신+검증 성공 */
    ESP_NOW_CAPTURE_STAGE_TRANSFER_FAILED,  /* 3단계 완료(실패): CRC 불일치/청크 누락 등 */
} esp_now_capture_stage_t;

/* Cntl -> CAM: 기존 파일 무시하고 즉시 새로 촬영해서 그 1장만 보내라는 요청.
 * 이미 진행 중이면 무시(동시에 두 개를 안 받음) */
void esp_now_photo_capture_now(const uint8_t *cam_mac);

esp_now_capture_stage_t esp_now_photo_get_capture_stage(void);

/* 팝업이 최종 단계(TRANSFER_DONE/FAILED)를 확인한 뒤 NONE으로 되돌릴 때 사용 —
 * 다음 지금촬영 요청 전에 호출 안 해도 capture_now()가 알아서 SENT로 덮어씀 */
void esp_now_photo_capture_stage_clear(void);

/* 목록에서 고른 특정 사진 요청 — 결과는 위 1번 섹션의 state/consume()으로 받음(캡처 단계
 * 추적과 무관, capture_stage는 안 건드림) */
void esp_now_photo_fetch_by_id(const uint8_t *cam_mac, uint32_t file_id);

/* ────────────────────────────────────────────────────────────
 * 3. 사진 목록(내용 없이 file_id+크기만) + 삭제
 * ──────────────────────────────────────────────────────────── */
#define ESP_NOW_PHOTO_LIST_MAX 64  /* CAM SD엔 최대 500장까지 있을 수 있지만 화면 리스트라 제한 */

typedef struct {
    uint32_t file_id;    /* 촬영 시각의 유닉스 타임스탬프 겸 고유 id */
    uint32_t file_size;
} esp_now_photo_list_item_t;

typedef enum {
    ESP_NOW_PHOTO_LIST_STATE_IDLE = 0,
    ESP_NOW_PHOTO_LIST_STATE_REQUESTING,
    ESP_NOW_PHOTO_LIST_STATE_READY,
} esp_now_photo_list_state_t;

void esp_now_photo_list_request(const uint8_t *cam_mac);
esp_now_photo_list_state_t esp_now_photo_list_get_state(void);

/* READY일 때 목록을 out에 채워서 개수 반환(최대 ESP_NOW_PHOTO_LIST_MAX, 최신순) */
int esp_now_photo_list_get_items(esp_now_photo_list_item_t *out, int max);

/* UI가 READY 목록을 화면에 반영한 뒤 호출 — 상태를 IDLE로 돌려서 다음 폴링 때 같은
 * 목록을 중복 처리하지 않게 함 */
void esp_now_photo_list_ack(void);

/* Cntl -> CAM: 특정 사진 삭제(fire-and-forget) — UI가 이후 list_request()로 목록 갱신 */
void esp_now_photo_delete(const uint8_t *cam_mac, uint32_t file_id);

#ifdef __cplusplus
}
#endif
