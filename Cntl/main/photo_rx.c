#include "photo_rx.h"
#include "esp_now_link.h"
#include "node_hub.h"
#include "node_request.h"
#include "can_bridge.h"
#include "ui_log.h"
#include "device_config.h"
#include "photo_storage.h"

#include <string.h>
#include "esp_now.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "photo_rx";

/* 모듈 전체 상태를 하나의 뮤텍스로 보호 — ESP-NOW 태스크(recv_cb 경유)와 LVGL 워커
 * 태스크(UI) 양쪽에서 건드리는데, 호출 빈도가 낮아서(초당 몇 번 수준) 필드별로 락을
 * 쪼갤 실익이 없음 */
static SemaphoreHandle_t s_mutex;

/* 2026-09-04(사용자 설계: "이벤트로 처리해") — 사진 수신 완료(성공/실패) 이벤트. 웹은
 * 세마포어로 블로킹 대기(폴링 아님), 앱은 등록된 콜백으로 즉시 통지받음(이 모듈은 LVGL을
 * 몰라서 콜백 안에서 lv_async_call()로 미루는 건 콜백 구현부 책임) */
static SemaphoreHandle_t s_photo_event_sem = NULL;
static photo_rx_event_cb_t s_photo_ready_cb = NULL;

static void fire_photo_event(void)
{
    xSemaphoreGive(s_photo_event_sem);
    if (s_photo_ready_cb) s_photo_ready_cb();
}

/* ────────────────────────────────────────────────────────────
 * 1. 단일 사진 수신(capture_now/fetch_by_id 공용)
 * ──────────────────────────────────────────────────────────── */
/* 부팅 시 한 번만 할당하고 절대 free/realloc 안 함(2026-08-01, recv 버퍼/캐시 슬롯/판넬
 * 디코드 버퍼 모두 동일 원칙 — 반복 사용하는 버퍼는 처음에 한 번만 잡고 계속 재사용).
 * 720KB로는 실기에서 큰 사진(고엔트로피 장면)이 넘쳐서 3001 에러가 남 — 720KB->1024KB로
 * 확장(2026-08-02). 캐시 슬롯이 2->1로 줄면서(아래 참고) recv_buf+캐시슬롯 합이 이제 버퍼
 * 3개가 아니라 2개라, 실측 여유PSRAM(캐시슬롯[1] 할당 직후 161,560B, 구성 3버퍼 기준)으로
 * 역산한 전체 가용치(~2.42MB) 안에서 1024KB씩 잡아도 ~270KB 여유가 남음(과거 6.7MB 기준
 * 계산은 틀렸었으니 이 실측치를 기준으로 삼음) */
#define PHOTO_RECV_BUF_CAP (1024 * 1024)
static uint8_t  *s_recv_buf = NULL;   /* ESP-NOW 태스크만 건드림, 밖으로 포인터가 안 나감 */
static size_t     s_recv_cap = 0;
static uint32_t   s_file_id = 0;
static uint32_t   s_total_size = 0;
static uint16_t   s_total_chunks = 0;
static uint32_t   s_expected_crc = 0;
static uint16_t   s_chunks_received = 0;
static uint8_t    s_photo_cam_mac[6] = { 0 };  /* NACK을 돌려보낼 대상 — 요청 시점에 저장 */
static uint8_t    s_recv_kind = 0;  /* 2026-09-18(SD 제거 재설계) — META의 kind('M'/'T'),
                                        DONE에서 photo_storage_save()의 파일명 접두사로 씀 */

/* 청크 신뢰성 재설계(2026-08-03) — "핸드셰이크처럼 청크마다 응답을 기다리는데 정작 그
 * 응답이 로컬 라디오 ACK일 뿐이라 진짜 확인이 아니었던" 예전 방식을 버리고, 신뢰도 높은
 * 브로드캐스팅에서 쓰는 스트리밍+선택적 재전송(NACK) 방식으로 교체(사용자 설계 지시,
 * esp_now_link.h의 esp_now_photo_chunk_nack_t 주석 참고). 어느 chunk_idx를 받았는지
 * 비트맵으로 추적해뒀다가 DONE 도착 시 빠진 것만 콕 집어 CAM에 재전송 요청 */
#define PHOTO_MAX_CHUNKS ((PHOTO_RECV_BUF_CAP + ESP_NOW_PHOTO_CHUNK_DATA_LEN - 1) / ESP_NOW_PHOTO_CHUNK_DATA_LEN)
static uint8_t    s_chunk_bitmap[(PHOTO_MAX_CHUNKS + 7) / 8];
static int        s_nack_rounds_used = 0;

static inline void chunk_bitmap_clear(void) { memset(s_chunk_bitmap, 0, sizeof(s_chunk_bitmap)); }
static inline void chunk_bitmap_set(uint16_t idx)
{
    if (idx >= PHOTO_MAX_CHUNKS) return;
    s_chunk_bitmap[idx / 8] |= (uint8_t)(1u << (idx % 8));
}
static inline bool chunk_bitmap_test(uint16_t idx)
{
    if (idx >= PHOTO_MAX_CHUNKS) return false;
    return (s_chunk_bitmap[idx / 8] >> (idx % 8)) & 1;
}

static volatile photo_rx_state_t s_state = PHOTO_RX_STATE_IDLE;
static uint32_t s_ready_file_id = 0;  /* READY 상태일 때 방금 캐시에 들어간 file_id */

/* 압축 JPEG 원본 캐시 — 방금 수신 완료된 사진 1장을 판넬/웹이 재조회 없이 디코드해 쓸
 * 수 있게 담아두는 "완료본 보관소". 슬롯 버퍼는 recv 버퍼와 같은 이유로 부팅 시 고정
 * 할당해두고 매번 memcpy만 함, malloc/free 없음(2026-08-01).
 * 슬롯 수 1개로 축소(2026-08-02, 기존 2개) — 예전엔 "이미 선택했던 사진을 다시 선택하면
 * 재요청 안 함" 용도로 최근 2장을 들고 있었는데, 사용자가 그 설계를 뒤집음: "탭은
 * Select하기 위한 것일 뿐, 실제 action(가져오기)은 select가 바뀔 때만, 그리고 바뀌면
 * 무조건 새로 가져온다 — 이전 사진을 들고 있는 개념 자체가 없다"(ui_main.c의
 * reconcile_selection 참고). 즉 "여러 장을 기억해뒀다 재사용"할 일이 이제 없어서 슬롯은
 * "지금 막 도착한 사진 1장"만 있으면 충분 — 이 슬롯은 재요청 회피용이 아니라 순전히
 * display_photo/웹 다운로드가 읽어가는 데이터 소스 역할만 함. 여기서 아낀 만큼
 * PHOTO_RECV_BUF_CAP을 키우는 데 씀(위 참고) */
#define PHOTO_CACHE_SLOTS 1
#define PHOTO_CACHE_SLOT_CAP PHOTO_RECV_BUF_CAP
typedef struct {
    bool     used;
    uint32_t file_id;
    uint8_t *data;   /* 부팅 시 고정 할당, 절대 free 안 함 */
    size_t   len;
} photo_cache_slot_t;
static photo_cache_slot_t s_cache[PHOTO_CACHE_SLOTS];
static int s_cache_next = 0;  /* 다음에 (필요하면 덮어)쓸 슬롯 */

/* 이미 있으면 그 슬롯에, 없으면 s_cache_next 슬롯에 memcpy로 덮어쓰고 한 칸 전진 —
 * 호출부가 뮤텍스 잡고 불러야 함. 슬롯 버퍼가 고정 크기라 len이 넘치면 버림(실측
 * 크기 대비 여유를 크게 뒀으니 정상 상황에서는 안 일어나야 함) */
static void cache_insert_locked(uint32_t file_id, const uint8_t *data, size_t len)
{
    if (len > PHOTO_CACHE_SLOT_CAP) {
        ESP_LOGE(TAG, "cache_insert: 사진이 캐시 슬롯보다 큼(%u > %u) — 버림",
                 (unsigned)len, (unsigned)PHOTO_CACHE_SLOT_CAP);
        ui_log_add_err(UI_ERR_CACHE_TOO_BIG, "Photo save failed (too big) file_id=%u len=%u", (unsigned)file_id, (unsigned)len);
        return;
    }

    for (int i = 0; i < PHOTO_CACHE_SLOTS; i++) {
        if (s_cache[i].used && s_cache[i].file_id == file_id) {
            memcpy(s_cache[i].data, data, len);
            s_cache[i].len = len;
            ui_log_add("CACHE slot[%d] updated file_id=%u len=%u", i, (unsigned)file_id, (unsigned)len);
            return;
        }
    }
    photo_cache_slot_t *slot = &s_cache[s_cache_next];
    if (!slot->data) {
        ESP_LOGE(TAG, "cache_insert: 슬롯 버퍼 없음(초기 할당 실패?) — 버림");
        ui_log_add_err(UI_ERR_CACHE_NO_BUF, "Photo save failed (out of memory) file_id=%u", (unsigned)file_id);
        s_cache_next = (s_cache_next + 1) % PHOTO_CACHE_SLOTS;
        return;
    }
    ui_log_add("CACHE slot[%d] new (prev file_id=%u) -> file_id=%u len=%u",
               s_cache_next, (unsigned)slot->file_id, (unsigned)file_id, (unsigned)len);
    memcpy(slot->data, data, len);
    slot->used    = true;
    slot->file_id = file_id;
    slot->len     = len;
    s_cache_next = (s_cache_next + 1) % PHOTO_CACHE_SLOTS;
}

/* ────────────────────────────────────────────────────────────
 * 2. 지금촬영 진행 단계
 * ──────────────────────────────────────────────────────────── */
static volatile esp_now_capture_stage_t s_capture_stage = PHOTO_RX_CAPTURE_STAGE_NONE;
/* 2026-08-26(사용자 지시) — 어느 mac을 대상으로 진행 중인지 기록(s_photo_cam_mac과 동일
 * 이유). 없으면 여러 기기가 붙어있을 때 캠1 지금촬영 중에 캠2까지 "통신 중"으로 오판해서
 * 불필요하게 안 재우는 버그가 됨(photo_rx_is_transacting_with 참고) */
static uint8_t s_capture_cam_mac[6] = { 0 };


void photo_rx_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_photo_event_sem = xSemaphoreCreateBinary();

    ui_log_add("INIT free PSRAM(start)=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));


    s_recv_buf = heap_caps_malloc(PHOTO_RECV_BUF_CAP, MALLOC_CAP_SPIRAM);
    if (s_recv_buf) {
        s_recv_cap = PHOTO_RECV_BUF_CAP;
        ui_log_add("INIT recv_buf=OK free PSRAM=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        ESP_LOGE(TAG, "수신 버퍼 초기 할당 실패(%u bytes) — 사진 수신 불가", (unsigned)PHOTO_RECV_BUF_CAP);
        ui_log_add_err(UI_ERR_RECV_BUF_ALLOC, "Recv buffer alloc failed - cannot receive photos");
    }

    for (int i = 0; i < PHOTO_CACHE_SLOTS; i++) {
        s_cache[i].data = heap_caps_malloc(PHOTO_CACHE_SLOT_CAP, MALLOC_CAP_SPIRAM);
        if (s_cache[i].data) {
            ui_log_add("INIT cache slot[%d]=OK free PSRAM=%u", i, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else {
            ESP_LOGE(TAG, "캐시 슬롯[%d] 초기 할당 실패(%u bytes)", i, (unsigned)PHOTO_CACHE_SLOT_CAP);
            ui_log_add_err(UI_ERR_CACHE_SLOT_ALLOC, "Cache slot[%d] alloc failed - cannot store multiple photos", i);
        }
    }
}


/* 촬영과 전송은 완전히 분리(2026-08-01) — CAM에 "지금 찍어라"만 보내고 CAPTURE_STATUS로
 * 결과만 확인함. 사진 자체는 CAM이 촬영 직후 따로 푸시해옴(2026-09-18 SD 제거 재설계 —
 * 예전의 "목록에서 골라 fetch_by_id" 경로는 2026-09-26 삭제) */
void photo_rx_capture_now(const uint8_t *cam_mac)
{
    node_hub_note_user_action();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capture_stage = PHOTO_RX_CAPTURE_STAGE_SENT;
    memcpy(s_capture_cam_mac, cam_mac, sizeof(s_capture_cam_mac));
    xSemaphoreGive(s_mutex);

    esp_now_photo_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_REQUEST,
        .mode     = PHOTO_REQUEST_MODE_CAPTURE_NOW,
        .param    = 0,
    };
    /* 2026-08-05 Layer 1 -> 2026-08-26 CASK 큐 — CAPTURE_STATUS(RECEIVED)를 기다리는 건
     * 여전히 다음 CASK "할일" 단계에서 node_request_enqueue가 함. 촬영 자체의 최종 결과
     * (SUCCESS/FAILED)는 이후 별도 비동기 CAPTURE_STATUS로 옴 — 그건 기존처럼
     * recv_cb -> handle_capture_status()가 처리(여기서 안 기다림) */
    static const uint8_t s_capture_status_types[] = { ESP_NOW_MSG_CAPTURE_STATUS };
    node_hub_queue_action(cam_mac, &req, sizeof(req), s_capture_status_types, 1, 500, 3, "Capture now");
    ESP_LOGI(TAG, "PHOTO_REQUEST(mode=CAPTURE_NOW) 큐잉됨");
}

static void handle_meta(const uint8_t *src_mac, const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_meta_t)) return;
    const esp_now_photo_meta_t *meta = (const esp_now_photo_meta_t *)data;
    ESP_LOGI(TAG, "PHOTO_META 수신: file_id=%u, total_size=%u, chunks=%u",
             (unsigned)meta->file_id, (unsigned)meta->total_size, (unsigned)meta->total_chunks);
    ui_log_add("META file_id=%u size=%u chunks=%u",
               (unsigned)meta->file_id, (unsigned)meta->total_size, (unsigned)meta->total_chunks);

    /* s_state 하나만 뮤텍스로 짧게 감싸고 s_file_id/s_total_chunks 등 나머지 필드는 밖에서
     * 건드리던 게 진짜 경합이었음(2026-08-03, 사용자 지적: "CNTL의 수신단 구현이 이상한 것
     * 같아") — LVGL UI 태스크(photo_rx_fetch_by_id, 새 선택 시 s_file_id를 미리 바꿈)와
     * 이 함수(ESP-NOW 콜백 태스크)가 같은 필드들을 서로 다른 락 구간에서 만지고 있어서
     * "확인"과 "그 확인을 근거로 쓰기"가 원자적이지 않았음. 이제 관련 필드 전부를 하나의
     * 락 구간 안에서 같이 바꿈 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (meta->total_size > s_recv_cap || !s_recv_buf) {
        s_state = PHOTO_RX_STATE_ERROR;
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "사진이 고정 수신 버퍼보다 큼(%u > %u bytes) — 버림",
                 (unsigned)meta->total_size, (unsigned)s_recv_cap);
        ui_log_add_err(UI_ERR_META_TOO_BIG, "Photo receive failed (too big) %u > %u", (unsigned)meta->total_size, (unsigned)s_recv_cap);
        fire_photo_event();
        return;
    }
    s_file_id         = meta->file_id;
    s_recv_kind        = meta->kind;
    s_total_size       = meta->total_size;
    s_total_chunks     = meta->total_chunks;
    s_expected_crc     = meta->crc32;
    s_chunks_received  = 0;
    chunk_bitmap_clear();
    s_nack_rounds_used = 0;
    s_state            = PHOTO_RX_STATE_RECEIVING;
    /* 응답(WINDOW_STATUS_ACK/DONE_ACK) 보낼 대상을 실제 발신자 MAC으로 갱신(2026-08-05,
     * Selective Repeat 벤치마크로 발견) — 원래는 start_single_receive()가 Cntl이 먼저
     * PHOTO_REQUEST를 보낼 때 미리 채워뒀는데, XFER_BENCH 모드는 CAM이 요청 없이 먼저
     * 밀어서(META부터 시작) s_photo_cam_mac이 한 번도 안 채워진 채로 남아있었음(초기값
     * 전부 0) — can_bridge_relay_send(0-MAC, ...)이 ESP_ERR_ESPNOW_NOT_FOUND로 항상 실패해서 응답이
     * CAM에 전혀 안 갔던 게 원인. META를 실제로 누가 보냈는지가 항상 진짜 정답이므로
     * 여기서 갱신하는 게 요청 경로 여부와 무관하게 맞음 */
    if (src_mac) memcpy(s_photo_cam_mac, src_mac, sizeof(s_photo_cam_mac));
    xSemaphoreGive(s_mutex);

    /* 2026-08-21 — META를 reliable로 전환(esp_now_link.h의 ESP_NOW_MSG_PHOTO_META_ACK 주석
     * 참고) — CAM은 이 ACK을 받을 때까지 청크 전송을 시작 안 하므로, 여기서 반드시 응답해야
     * CAM이 다음 단계로 진행함(용량초과로 위에서 이미 ERROR 처리하고 return한 경우는 응답
     * 안 함 — 어차피 못 받을 전송이라 CAM이 재시도 끝에 스스로 포기하게 두는 게 대역폭
     * 낭비가 적음) */
    esp_now_photo_done_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_META_ACK };
    can_bridge_relay_send(src_mac, (const uint8_t *)&ack, sizeof(ack));
}

static void handle_chunk(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_chunk_t)) return;
    const esp_now_photo_chunk_t *chunk = (const esp_now_photo_chunk_t *)data;

    /* 확인(state/file_id 일치)과 실제 쓰기(memcpy)를 같은 락 구간 안에서 — 그 사이에
     * photo_rx_fetch_by_id()가 끼어들어 s_file_id/s_state를 새 요청으로 바꿔버리면,
     * 이 청크가 이미 낡은 요청 것인데도 그 사실을 놓치고 새 수신버퍼에 잘못 쓰일 수 있었음 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state != PHOTO_RX_STATE_RECEIVING || chunk->file_id != s_file_id || !s_recv_buf) {
        xSemaphoreGive(s_mutex);
        return;
    }
    size_t offset = (size_t)chunk->chunk_idx * ESP_NOW_PHOTO_CHUNK_DATA_LEN;
    if (offset + chunk->chunk_len > s_recv_cap) {
        xSemaphoreGive(s_mutex);
        return;  /* 손상된/엉뚱한 청크 — 무시 */
    }
    memcpy(s_recv_buf + offset, chunk->data, chunk->chunk_len);
    /* NACK 재전송 라운드에서 같은 청크가 다시 올 수 있음(예: 재전송분과 뒤늦은 원본이
     * 둘 다 도착) — 비트맵으로 "새로 받은 것"만 카운트해서 중복 집계 방지 */
    if (!chunk_bitmap_test(chunk->chunk_idx)) {
        chunk_bitmap_set(chunk->chunk_idx);
        s_chunks_received++;
    }
    uint8_t recv_kind = s_recv_kind;
    xSemaphoreGive(s_mutex);

    /* 2026-08-10 — 적응형 반응시간의 "마지막 사용자 조작" 시각을 청크마다 갱신. 예전엔
     * require_paired()가 요청 "시작" 시점에 한 번만 갱신해서, 전송이 몇 초 걸리면 그 시간이
     * 고스란히 조용한 시간으로 카운트돼버림 — 전송 도중에 이미 적응형 임계값을 넘겨 SLEEP_NOW가
     * CAM에 큐잉되고, 전송이 끝나 busy가 풀리자마자(사용자가 결과를 볼 틈도 없이) 바로 잠드는
     * 문제로 실사용 중 확인됨(사용자 분석: "통신 완료 후가 아니라 통신을 시작한 입력에 의해
     * 카운터가 진행됨"). 청크가 계속 들어오는 동안은 "활동 중"이 맞으므로 매 청크 갱신 —
     * 단, 2026-09-19(주기촬영 CAM 자율 푸시 재설계 이후, 사용자 지시로 조사) 'T'(주기촬영)는
     * 사람이 화면을 보거나 조작한 게 전혀 아닌 CAM 자율 전송이므로 여기서 제외. 안 그러면
     * 캡처주기<적응형임계값일 때 매 주기촬영 전송이 조용시간을 계속 리셋해서 캠이 영영
     * 못 자는 버그가 됨(실사용 중 5분간 미절전으로 확인) */
    if (recv_kind != 'T') {
        node_hub_note_user_action();
    }
}

/* missing_count==0이면 완료를 뜻하는 PHOTO_DONE_ACK를 항상 1번만 보냄(2026-08-05, Layer 1
 * 재설계) — 예전엔 "문제 있을 때만" NACK을 보내서 CAM이 정상종료인지 무응답인지 구분할
 * 방법이 없었음. 이제 CAM은 esp_now_reliable_request()로 이 응답을 기다리므로, 응답이 안
 * 오면 CAM 쪽에서 알아서 DONE을 재전송함 — Cntl은 3번씩 반복 전송할 필요가 없어짐(레이어가
 * 대신 재시도해줌) */
static void send_done_ack(uint16_t missing_count, const uint16_t *missing_idx)
{
    static esp_now_photo_chunk_nack_t ack;  /* static — 800B+ 구조체를 스택에 안 둠(2026-08-03
                                                스택 오버플로우 사고 이후 원칙) */
    ack.version       = ESP_NOW_LINK_VERSION;
    ack.msg_type      = ESP_NOW_MSG_PHOTO_DONE_ACK;
    ack.file_id       = s_file_id;
    ack.missing_count = missing_count;
    if (missing_count > 0) memcpy(ack.missing_idx, missing_idx, missing_count * sizeof(uint16_t));
    esp_err_t err = can_bridge_relay_send(s_photo_cam_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "PHOTO_DONE_ACK 전송(누락 %u개) file_id=%u: %s", missing_count, (unsigned)s_file_id, esp_err_to_name(err));
}

static void handle_done(const uint8_t *data, int len)
{
    (void)data; (void)len;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    photo_rx_state_t st = s_state;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "PHOTO_DONE 수신: state=%d, chunks=%u/%u", st, s_chunks_received, s_total_chunks);
    ui_log_add("DONE state=%d chunks=%u/%u file_id=%u", st, s_chunks_received, s_total_chunks, (unsigned)s_file_id);
    if (st != PHOTO_RX_STATE_RECEIVING) {
        /* RECEIVING이 아니면 이 거래는 CAM 쪽에서 이미 다른 시도로 대체됐거나(세대번호) 이
         * 요청 자체를 우리가 모름(META를 못 받은 상태) — 뭘 요청받았는지조차 몰라서 의미
         * 있는 ACK를 만들 방법이 없으므로 응답 안 함. CAM은 reliable_request 타임아웃으로
         * 알아서 포기함 */
        ui_log_add("DONE ignored (state!=RECEIVING)");
        return;
    }

    if (s_total_chunks == 0 && s_chunks_received == 0) {
        /* META가 아예 안 왔던 경우(해당 file_id가 없음 등) — 에러가 아니라 그냥
         * "보낼 게 없었다"로 조용히 종료. 위와 같은 이유로 응답 생략 */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = PHOTO_RX_STATE_IDLE;
        xSemaphoreGive(s_mutex);
        return;
    }

    if (s_chunks_received != s_total_chunks || s_total_size == 0 || !s_recv_buf) {
        /* 청크 누락(2026-08-03 재설계) — 빠진 chunk_idx를 정확히 짚어서 DONE_ACK에 실어
         * 재전송을 요청함(신뢰도 높은 브로드캐스팅 방식, 사용자 설계 지시). 라운드가
         * 남아있으면 RECEIVING 상태를 유지한 채 여기서 그냥 리턴 — CAM이 재전송 후 다시
         * 보내는 DONE이 이 함수를 다시 호출함. 라운드를 다 썼으면 그 사실도 DONE_ACK로
         * 알려주고(2026-08-05, 이전엔 응답 자체를 안 보내서 CAM이 자기 타임아웃까지
         * 기다려야 했음) 진짜 실패 처리 */
        static uint16_t missing_idx[ESP_NOW_PHOTO_NACK_MAX_INDICES];
        uint16_t n = 0;
        for (uint16_t idx = 0; idx < s_total_chunks && n < ESP_NOW_PHOTO_NACK_MAX_INDICES; idx++) {
            if (!chunk_bitmap_test(idx)) missing_idx[n++] = idx;
        }
        send_done_ack(n, missing_idx);

        /* 2026-08-21 off-by-one 수정 — CAM(esp_now_cam.c)의 재전송 루프는 정확히
         * nack_max_rounds번만 DONE을 보내고 그 후엔 조용히 포기함(그 다음 DONE은 절대 안 옴).
         * 그런데 여기 카운터는 증가 *전*에 검사해서, 마지막 DONE을 받고도 "아직 여유 있다"고
         * 오판하고 다음 DONE을 기다렸음 — CAM 쪽 마지막 라운드와 Cntl 쪽 "포기 조건"이 하나
         * 어긋나 있던 것. 실기에서 재현: CAM이 "재전송 라운드 소진"으로 조용히 끝냈는데 Cntl은
         * 계속 대기하다 stall 타임아웃(3006)으로만 빠짐 — 정작 원인은 청크 누락(3002)인데
         * 엉뚱한 무응답 에러로 보였음. 먼저 증가시키고 그 값으로 판단해야 CAM의 마지막
         * 라운드(=이 함수의 마지막 호출)에서 곧바로 실패 처리로 넘어감.
         *
         * 이 라운드 수는 CAM/Cntl 둘 다 "몇 라운드째인가"를 각자 판단 기준으로 쓰므로
         * 반드시 같은 숫자여야 함(둘 다 하드코딩했다가 이 버그로 처음 어긋났던 걸 발견,
         * feedback_cntl_owns_mutually_judged_values 메모리 참고) — 이제 CNTL이 유일한
         * 소유자(device_config_get_nack_max_rounds)이고 CAM_CONFIG_SET으로 CAM에도 같은 값을
         * 전달함(node_hub.c push_cam_config_to 참고) */
        int nack_max_rounds = (int)device_config_get_nack_max_rounds();
        s_nack_rounds_used++;
        if (s_nack_rounds_used < nack_max_rounds) {
            ESP_LOGW(TAG, "청크 누락(%u/%u) — 재전송 요청(%u개, 라운드 %d/%d)",
                     s_chunks_received, s_total_chunks, n, s_nack_rounds_used, nack_max_rounds);
            ui_log_add("DONE_ACK reporting %u missing (round %d/%d) file_id=%u", n, s_nack_rounds_used,
                       nack_max_rounds, (unsigned)s_file_id);
            return;
        }
        ESP_LOGW(TAG, "청크 누락(%u/%u) — NACK 라운드 소진, 사진 버림", s_chunks_received, s_total_chunks);
        ui_log_add_err(UI_ERR_CHUNK_MISSING, "Photo receive failed (chunk missing %u/%u even after resend)", s_chunks_received, s_total_chunks);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = PHOTO_RX_STATE_ERROR;
        xSemaphoreGive(s_mutex);
        fire_photo_event();
        return;
    }

    uint32_t crc = esp_rom_crc32_le(0, s_recv_buf, s_total_size);
    if (crc != s_expected_crc) {
        ESP_LOGW(TAG, "CRC 불일치 — 사진 버림(재조립 실패)");
        ui_log_add_err(UI_ERR_CRC_MISMATCH, "Photo receive failed (CRC mismatch) file_id=%u", (unsigned)s_file_id);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = PHOTO_RX_STATE_ERROR;
        xSemaphoreGive(s_mutex);
        fire_photo_event();
        return;
    }

    /* 검증된 압축 JPEG 원본을 캐시 슬롯(고정 버퍼)으로 memcpy — 픽셀 디코드는 안 함
     * (판넬/뷰어/웹이 필요할 때마다 각자 해상도로 디코드). s_recv_buf는 다음 요청 때
     * 재사용되므로 캐시는 별도 고정 버퍼에 복사해둠(malloc 없음 — cache_insert_locked 참고) */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cache_insert_locked(s_file_id, s_recv_buf, s_total_size);
    s_ready_file_id = s_file_id;
    s_state = PHOTO_RX_STATE_READY;
    xSemaphoreGive(s_mutex);

    /* 2026-09-18(SD 제거 재설계 — "찍을 때마다 항상 콘에 가져와서 콘의 SD에 저장") — 검증된
     * 사진을 콘 SD의 카메라별 폴더에 영구 저장. 실패해도(SD 미마운트 등) 치명적이지 않음 —
     * 캐시 슬롯엔 이미 들어갔으므로 방금 받은 사진 자체는 화면/웹에서 정상 표시됨, 다음
     * refresh_storage_status_label()이 SD 상태를 알려줌(stats_store 실패와 동일 톨러런스) */
    uint32_t seq = 0;
    if (photo_storage_save(s_photo_cam_mac, s_recv_kind, s_recv_buf, s_total_size, &seq)) {
        ui_log_add("SD저장 완료 file_id=%u -> seq=%u", (unsigned)s_file_id, (unsigned)seq);
    } else {
        ui_log_add_err(UI_ERR_SD_MOUNT_FAILED, "Photo SD save failed file_id=%u", (unsigned)s_file_id);
    }

    send_done_ack(0, NULL);  /* 완료 통보(2026-08-05, Layer 1) — missing_count=0 */
    ESP_LOGI(TAG, "사진 수신 완료: file_id=%u, %u bytes", (unsigned)s_file_id, (unsigned)s_total_size);
    ui_log_add("READY file_id=%u %u bytes", (unsigned)s_file_id, (unsigned)s_total_size);
    fire_photo_event();
}

/* Selective Repeat 실험(2026-08-05) — handle_done()과 같은 원칙(모르는 거래엔 무응답, CAM의
 * reliable_request 타임아웃/재시도에 맡김)이지만 훨씬 단순함: range 하나 안에서만 누락을
 * 찾으면 되고(파일 전체를 매번 다시 스캔하지 않음), range_count가 SR_WINDOW_SIZE(CAM 쪽)
 * 이하로 고정되니 missing_count가 400 상한을 넘을 일이 없음 — 새 구조체 대신
 * esp_now_photo_chunk_nack_t를 msg_type만 바꿔 그대로 재사용(DONE_ACK와 동일 이유) */
static void handle_window_status_request(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_window_status_req_t)) return;
    const esp_now_photo_window_status_req_t *req = (const esp_now_photo_window_status_req_t *)data;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = (s_state == PHOTO_RX_STATE_RECEIVING && req->file_id == s_file_id);
    xSemaphoreGive(s_mutex);
    if (!ok) return;

    static esp_now_photo_chunk_nack_t ack;  /* static — 800B+ 구조체 스택 회피(기존 원칙) */
    uint16_t end = req->range_start + req->range_count;
    uint16_t n = 0;
    for (uint16_t idx = req->range_start; idx < end && n < ESP_NOW_PHOTO_NACK_MAX_INDICES; idx++) {
        if (!chunk_bitmap_test(idx)) ack.missing_idx[n++] = idx;
    }
    ack.version       = ESP_NOW_LINK_VERSION;
    ack.msg_type      = ESP_NOW_MSG_PHOTO_WINDOW_STATUS_ACK;
    ack.file_id       = req->file_id;
    ack.missing_count = n;
    esp_err_t err = can_bridge_relay_send(s_photo_cam_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "WINDOW_STATUS_ACK [%u,%u) 누락 %u개: %s",
             req->range_start, end, n, esp_err_to_name(err));
}

photo_rx_state_t photo_rx_get_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    photo_rx_state_t st = s_state;
    xSemaphoreGive(s_mutex);
    return st;
}

/* RECEIVING 중일 때만 의미 있음 — fetch 진행 팝업의 퍼센트/ETA 계산용(락 없이 읽음,
 * ESP-NOW 태스크만 쓰고 여긴 표시용으로만 읽어서 uint16 tearing 정도는 무해) */
void photo_rx_get_chunk_progress(uint16_t *received, uint16_t *total)
{
    *received = s_chunks_received;
    *total    = s_total_chunks;
}

uint32_t photo_rx_get_ready_file_id(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t id = s_ready_file_id;
    xSemaphoreGive(s_mutex);
    return id;
}

void photo_rx_ready_ack(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == PHOTO_RX_STATE_READY) {
        s_state = PHOTO_RX_STATE_IDLE;
    }
    xSemaphoreGive(s_mutex);
}

bool photo_rx_cache_get(uint32_t file_id, const uint8_t **out_data, size_t *out_len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < PHOTO_CACHE_SLOTS; i++) {
        if (s_cache[i].used && s_cache[i].file_id == file_id) {
            *out_data = s_cache[i].data;
            *out_len  = s_cache[i].len;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return found;
}

void photo_rx_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == PHOTO_RX_STATE_ERROR) {
        s_state = PHOTO_RX_STATE_IDLE;
    }
    xSemaphoreGive(s_mutex);
}

void photo_rx_set_ready_cb(photo_rx_event_cb_t cb)
{
    s_photo_ready_cb = cb;
}

bool photo_rx_wait_cached(uint32_t file_id, uint32_t timeout_ms,
                                const uint8_t **out_data, size_t *out_len)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        if (photo_rx_cache_get(file_id, out_data, out_len)) return true;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        /* 2026-09-05 버그수정 — file_id 비교 없이 전역 s_state만 봐서, 지금 기다리는 것과
         * 무관한 다른 요청(예: 그 사이 desync로 인한 실패)이 ERROR를 스치기만 해도 즉시
         * "내 요청 실패"로 오판했음(실기 확인: 다른 사진 전송 직후 desync가 나면, 그와
         * 무관한 사진 요청도 누르자마자 실패로 뜸) — 단일슬롯 현재 대상(s_file_id)이 내가
         * 기다리는 file_id와 같을 때만 내 실패로 인정 */
        bool errored = (s_state == PHOTO_RX_STATE_ERROR && s_file_id == file_id);
        xSemaphoreGive(s_mutex);
        if (errored) return false;  /* 실패도 이벤트로 즉시 나옴 — 타임아웃까지 안 기다림 */

        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) return false;
        xSemaphoreTake(s_photo_event_sem, deadline - now);
    }
}

/* ════════════════════════════════════════════════════════════
 * 지금촬영 진행 단계
 * ════════════════════════════════════════════════════════════ */
static void handle_capture_status(const uint8_t *src_mac, const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_capture_status_t)) return;
    const esp_now_capture_status_t *msg = (const esp_now_capture_status_t *)data;
    ESP_LOGI(TAG, "CAPTURE_STATUS 수신: status=%d", msg->status);

    /* 2026-08-21 — RECEIVED만 recv_cb 컨텍스트의 fire-and-forget 예외, 나머지(INIT_NEEDED/
     * INIT_DONE/CAPTURING/SUCCESS/FAILED)는 전부 CAM이 esp_now_reliable_request()로 감싸서
     * 기다리는 reliable이라 다 ACK 필요(feedback_default_to_reliable_messaging 메모리 참고,
     * 예전엔 SUCCESS/FAILED만 ACK했음) */
    if (src_mac && msg->status != CAM_CAPTURE_STATUS_RECEIVED) {
        esp_now_capture_status_ack_t ack = {
            .version  = ESP_NOW_LINK_VERSION,
            .msg_type = ESP_NOW_MSG_CAPTURE_STATUS_ACK,
        };
        can_bridge_relay_send(src_mac, (const uint8_t *)&ack, sizeof(ack));
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    switch (msg->status) {
        case CAM_CAPTURE_STATUS_RECEIVED:
            s_capture_stage = PHOTO_RX_CAPTURE_STAGE_ACKED;
            break;
        case CAM_CAPTURE_STATUS_INIT_NEEDED:
            s_capture_stage = PHOTO_RX_CAPTURE_STAGE_INIT_NEEDED;
            break;
        case CAM_CAPTURE_STATUS_INIT_DONE:
            s_capture_stage = PHOTO_RX_CAPTURE_STAGE_INIT_DONE;
            break;
        case CAM_CAPTURE_STATUS_CAPTURING:
            s_capture_stage = PHOTO_RX_CAPTURE_STAGE_CAPTURING;
            break;
        case CAM_CAPTURE_STATUS_SUCCESS:
            s_capture_stage = PHOTO_RX_CAPTURE_STAGE_CAPTURED;
            break;
        case CAM_CAPTURE_STATUS_FAILED:
            s_capture_stage = PHOTO_RX_CAPTURE_STAGE_CAPTURE_FAILED;
            ui_log_add_err(UI_ERR_CAPTURE_FAILED, "Capture failed (CAM response)");
            break;
        default:
            break;
    }
    xSemaphoreGive(s_mutex);
}

esp_now_capture_stage_t photo_rx_get_capture_stage(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_capture_stage_t stage = s_capture_stage;
    xSemaphoreGive(s_mutex);
    return stage;
}

void photo_rx_capture_stage_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capture_stage = PHOTO_RX_CAPTURE_STAGE_NONE;
    xSemaphoreGive(s_mutex);
}

/* 2026-08-26(사용자 지시) — photo_rx.h 주석 참고. "통신 중엔 안 재운다" 판단의 근거.
 * 트랜잭션마다 대상 mac을 저장해두므로(s_photo_cam_mac/s_capture_cam_mac) 여러 기기가 동시에
 * 붙어있어도 각자 자기 것만 "통신 중"으로 정확히 매칭됨 — 다른 기기 것 때문에 잘못 안 재우는
 * 일이 없음. 2026-09-26 — 캠 SD 제거로 목록/전체삭제 트랜잭션은 삭제됨(사진 수신+지금촬영만 남음) */
bool photo_rx_is_transacting_with(const uint8_t *mac)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = (s_state == PHOTO_RX_STATE_RECEIVING && memcmp(s_photo_cam_mac, mac, 6) == 0)
             || (s_capture_stage != PHOTO_RX_CAPTURE_STAGE_NONE
                 && s_capture_stage != PHOTO_RX_CAPTURE_STAGE_CAPTURED
                 && s_capture_stage != PHOTO_RX_CAPTURE_STAGE_CAPTURE_FAILED
                 && memcmp(s_capture_cam_mac, mac, 6) == 0);
    xSemaphoreGive(s_mutex);
    return busy;
}

/* ════════════════════════════════════════════════════════════
 * 디스패치
 * ════════════════════════════════════════════════════════════ */
void photo_rx_on_recv(uint8_t msg_type, const uint8_t *src_mac, const uint8_t *data, int len)
{
    switch (msg_type) {
        case ESP_NOW_MSG_PHOTO_META:      handle_meta(src_mac, data, len);  break;
        case ESP_NOW_MSG_PHOTO_CHUNK:      handle_chunk(data, len);          break;
        case ESP_NOW_MSG_PHOTO_DONE:       handle_done(data, len);           break;
        case ESP_NOW_MSG_CAPTURE_STATUS:   handle_capture_status(src_mac, data, len); break;
        case ESP_NOW_MSG_PHOTO_WINDOW_STATUS_REQUEST: handle_window_status_request(data, len); break;
        default: break;
    }
}
