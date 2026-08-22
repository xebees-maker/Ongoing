#pragma once

/**
 * CAM 사진 프로토콜 — 세 갈래:
 *  1. 단일 사진 수신(META/CHUNK/DONE 재조립 + CRC32 검증) — 목록에서 특정 사진 보기
 *     (fetch_by_id) 전용. 촬영(capture_now)은 이 상태머신을 안 씀(2026-08-01, 아래 참고).
 *  2. 지금촬영 진행 단계(CAPTURE_STATUS) — Cntl UI 진행 팝업 표시용.
 *  3. 사진 "목록"만 가져오기(LIST_REQUEST/ENTRY/DONE, 내용 전송 없음) + 삭제.
 *
 * recv_cb(ESP-NOW/WiFi 드라이버 태스크)에서 esp_now_photo_on_recv()로 넘겨받아 처리한다.
 * 검증된 사진(압축 JPEG 원본 그대로)은 file_id별 캐시에 저장되고, UI는 esp_now_photo_cache_get()
 * 으로 포인터만 빌려서(소유권 안 넘어옴) 필요한 해상도로 직접 디코드한다 — 판넬은 작게,
 * 뷰어는 크게, 웹은 원본 그대로 서빙하는 식으로 셋이 같은 캐시를 다른 용도로 씀
 * (2026-08-01, 원본 해상도가 Cntl 힙보다 커서 통짜로 못 들고 있는 문제 때문에 압축본만
 * 캐시하고 표시할 때마다 필요한 만큼만 디코드하는 구조로 바꿈).
 *
 * 2026-08-01: 촬영과 전송을 분리했다 — 예전엔 지금촬영 성공 시 CAM이 곧바로 그 사진을
 * 1번 섹션 상태머신으로 자동 전송했는데, 전송이 느리고(당시 청크 200바이트) 실기에서
 * 자주 실패해서 지금촬영 팝업이 끝나지 않는 문제가 반복됐다. 이제 지금촬영은 CAPTURE_STATUS
 * (성공/실패)까지만 보고 끝 — 사진을 실제로 보려면 목록에서 선택해 fetch_by_id로 따로
 * 받아야 한다(그 팝업은 UI 쪽에서 별도로 진행률/ETA를 보여줌, get_chunk_progress() 참고).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_now_photo_init(void);

/* esp_now_hub.c의 recv_cb가 PHOTO_ 계열/CAPTURE_STATUS 메시지를 여기로 넘겨줌.
 * src_mac: 보낸 CAM의 MAC(2026-08-05 추가 — CAPTURE_STATUS_ACK을 그 자리에서 돌려보내려면
 * 필요, esp_now_photo.c의 handle_capture_status() 참고) */
void esp_now_photo_on_recv(uint8_t msg_type, const uint8_t *src_mac, const uint8_t *data, int len);

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

/* READY 상태일 때 방금 완료돼서 캐시에 들어간 file_id — 이걸로 esp_now_photo_cache_get() 조회 */
uint32_t esp_now_photo_get_ready_file_id(void);

/* READY 확인 후 IDLE로 되돌릴 때 사용(list_ack()와 동일 패턴) */
void esp_now_photo_ready_ack(void);

/* ERROR 상태를 확인 후 IDLE로 되돌릴 때 사용 */
void esp_now_photo_clear(void);

/* 압축 JPEG 원본 캐시 조회 — 소유권은 안 넘어옴(내부 캐시가 계속 들고 있다가 꽉 차면
 * 오래된 것부터 교체), 호출부는 포인터로 그때그때 디코드만 해서 씀. 없으면 false */
bool esp_now_photo_cache_get(uint32_t file_id, const uint8_t **out_data, size_t *out_len);

/* RECEIVING 중 진행률(청크 수신/전체) — fetch 진행 팝업이 퍼센트/ETA 표시에 씀.
 * RECEIVING이 아닐 땐 의미 없는 값일 수 있으니 호출부가 state를 먼저 확인할 것 */
void esp_now_photo_get_chunk_progress(uint16_t *received, uint16_t *total);

/* ────────────────────────────────────────────────────────────
 * 2. 지금촬영 — 진행 팝업 단계 추적(사진 전송은 안 함, 위 헤더 설명 참고)
 * ──────────────────────────────────────────────────────────── */
/* 2026-08-21 핸드셰이크 재설계(사용자 설계) — INIT_NEEDED/INIT_DONE/CAPTURING 추가, 각각
 * 독립된 타임아웃으로 판정(project_cntl_cam_capture_now_handshake_redesign 메모리 참고).
 * 카메라 초기화가 필요 없는 경우 INIT_NEEDED/INIT_DONE은 아예 안 오고 ACKED에서 바로
 * CAPTURING으로 넘어감 — 팝업도 그 두 단계를 건너뛰고 표시함(ui_main.c 참고) */
typedef enum {
    ESP_NOW_CAPTURE_STAGE_NONE = 0,
    ESP_NOW_CAPTURE_STAGE_SENT,             /* 1단계: Cntl -> CAM 요청 보냄, 접수 대기 */
    ESP_NOW_CAPTURE_STAGE_ACKED,            /* 1단계 완료: CAM이 접수 확인 */
    ESP_NOW_CAPTURE_STAGE_INIT_NEEDED,      /* 카메라 초기화 시작 */
    ESP_NOW_CAPTURE_STAGE_INIT_DONE,        /* 카메라 초기화 완료 */
    ESP_NOW_CAPTURE_STAGE_CAPTURING,        /* 실제 촬영(워밍업+실샷) 시작 */
    ESP_NOW_CAPTURE_STAGE_CAPTURED,         /* 최종: 촬영 성공 */
    ESP_NOW_CAPTURE_STAGE_CAPTURE_FAILED,   /* 최종: 촬영 실패(초기화 실패 포함) */
} esp_now_capture_stage_t;

/* Cntl -> CAM: 즉시 새로 촬영하라는 요청 — 사진은 안 받음(CAPTURE_STATUS까지만) */
void esp_now_photo_capture_now(const uint8_t *cam_mac);

esp_now_capture_stage_t esp_now_photo_get_capture_stage(void);

/* 팝업이 최종 단계(CAPTURED/CAPTURE_FAILED)를 확인한 뒤 NONE으로 되돌릴 때 사용 —
 * 다음 지금촬영 요청 전에 호출 안 해도 capture_now()가 알아서 SENT로 덮어씀 */
void esp_now_photo_capture_stage_clear(void);

/* 목록에서 고른 특정 사진 요청 — 결과는 위 1번 섹션의 state/consume()/get_chunk_progress()로
 * 받음(캡처 단계 추적과 무관, capture_stage는 안 건드림) */
void esp_now_photo_fetch_by_id(const uint8_t *cam_mac, uint32_t file_id);

/* ────────────────────────────────────────────────────────────
 * 3. 사진 목록(내용 없이 file_id+크기만) + 삭제
 * ──────────────────────────────────────────────────────────── */
#define ESP_NOW_PHOTO_LIST_MAX 64  /* CAM SD엔 최대 500장까지 있을 수 있지만 화면 리스트라 제한 */

/* 2026-08-11 — esp_now_link.h에 배치 프로토콜용 동명(wire) 구조체가 새로 생겨서 이름 충돌
 * 방지로 개명(esp_now_photo_list_item_t -> _view_item_t). 필드는 화면 표시에 필요한 것만 남긴
 * 로컬 저장용이라 wire 구조체의 index는 없음(배열 위치 자체가 index 역할) */
typedef struct {
    uint32_t file_id;       /* CAM의 M/T 공용 순번(더 이상 타임스탬프 아님, 2026-08-01 재설계) */
    uint8_t  kind;           /* 'M' 또는 'T' */
    uint32_t capture_time;   /* 파일의 FAT 수정시각(유닉스 타임스탬프) — 촬영시각 표시용 */
    uint32_t file_size;
} esp_now_photo_list_view_item_t;

typedef enum {
    ESP_NOW_PHOTO_LIST_STATE_IDLE = 0,
    ESP_NOW_PHOTO_LIST_STATE_REQUESTING,
    ESP_NOW_PHOTO_LIST_STATE_READY,
    ESP_NOW_PHOTO_LIST_STATE_ERROR,  /* 2026-08-11 — DONE이 이미 아는 개수보다 적게 받은 상태로
                                        조기도착(개수 불일치). get_progress()의 received/expected가
                                        실패 시점 값 그대로 남아있어 팝업이 사유를 표시할 수 있음 */
} esp_now_photo_list_state_t;

void esp_now_photo_list_request(const uint8_t *cam_mac);
esp_now_photo_list_state_t esp_now_photo_list_get_state(void);

/* PHOTO_LIST_COUNT 응답을 받았는지(=CAM이 요청을 실제로 받아 처리를 시작했는지) — 진행팝업의
 * "명령 전송" 단계 완료 판정용(2026-08-11). get_progress()의 total>0을 대신 쓰면 CAM의 실제
 * 목록이 0개인 경우(예: 방금 모두지우기 직후)를 "아직 응답 없음"으로 잘못 판정하게 되므로
 * 별도 플래그로 분리함 */
bool esp_now_photo_list_count_received(void);

/* READY일 때 목록을 out에 채워서 개수 반환(최대 ESP_NOW_PHOTO_LIST_MAX, 최신순) */
int esp_now_photo_list_get_items(esp_now_photo_list_view_item_t *out, int max);

/* REQUESTING 중일 때만 의미 있음 — 진행팝업의 정체 감지용(get_chunk_progress()와 동일 패턴,
 * 2026-08-10). total은 LIST_DONE 도착 전까지 0(아직 몇 개인지 모름) */
void esp_now_photo_list_get_progress(uint16_t *received, uint16_t *total);

/* UI가 READY 목록을 화면에 반영한 뒤 호출 — 상태를 IDLE로 돌려서 다음 폴링 때 같은
 * 목록을 중복 처리하지 않게 함 */
void esp_now_photo_list_ack(void);

/* 가장 최근 목록 응답에 같이 실려온 CAM SD카드 전체/사용 용량(KB) — 목록 옆에 사용량 %로
 * 보여주는 용도(2026-08-01). 아직 한 번도 목록을 못 받았거나 CAM이 조회 실패했으면 0/0 */
void esp_now_photo_list_get_sd_usage(uint32_t *out_total_kb, uint32_t *out_used_kb);

/* Cntl -> CAM: 특정 사진 삭제(fire-and-forget) — UI가 이후 list_request()로 목록 갱신 */
void esp_now_photo_delete(const uint8_t *cam_mac, uint32_t file_id);

/* Cntl -> CAM: 전체 삭제 요청 — ACK(성공여부+삭제개수) 도착 여부/내용은 아래 상태로 폴링.
 * ACK을 받았든 못 받았든(무응답) UI는 항상 list_request()로 실제 남은 목록을 다시 확인해야
 * 함(부분 실패 가능성 때문에 로컬 상태를 신뢰하면 안 됨) — 그 판단은 UI 쪽 책임. */
void esp_now_photo_delete_all(const uint8_t *cam_mac);

typedef enum {
    ESP_NOW_DELETE_ALL_STATE_NONE = 0,
    ESP_NOW_DELETE_ALL_STATE_REQUESTED,
    ESP_NOW_DELETE_ALL_STATE_RECEIVED,  /* 2026-08-21 — CAM이 접수하고 지울 개수를 알려온 상태.
                                            아직 실제 삭제 완료는 아님 — ACKED와 구분 */
    ESP_NOW_DELETE_ALL_STATE_ACKED,
} esp_now_delete_all_state_t;

esp_now_delete_all_state_t esp_now_photo_delete_all_get_state(void);

/* RECEIVED 이상(RECEIVED/ACKED) 상태일 때만 의미 있음 — CAM이 삭제 시작 전에 보고한,
 * "지금부터 지울" 개수(완료 대기 예산 계산용). ACKED 상태의 deleted_count(실제로 지운
 * 개수, 아래 get_count())와는 다른 값 — 정상이면 같겠지만 의미상 분리해둠 */
uint16_t esp_now_photo_delete_all_get_received_count(void);

/* ACKED 상태일 때만 의미 있음 */
bool     esp_now_photo_delete_all_get_success(void);
uint16_t esp_now_photo_delete_all_get_count(void);

/* 팝업이 ACKED를 확인한 뒤 NONE으로 되돌릴 때 사용(capture_stage_clear와 동일 패턴) */
void esp_now_photo_delete_all_clear(void);

#ifdef __cplusplus
}
#endif
