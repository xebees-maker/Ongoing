#include "esp_now_photo.h"
#include "esp_now_link.h"
#include "esp_now_hub.h"
#include "esp_now_tx.h"
#include "ui_log.h"

#include <string.h>
#include "esp_now.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "esp_now_photo";

/* 모듈 전체 상태를 하나의 뮤텍스로 보호 — ESP-NOW 태스크(recv_cb 경유)와 LVGL 워커
 * 태스크(UI) 양쪽에서 건드리는데, 호출 빈도가 낮아서(초당 몇 번 수준) 필드별로 락을
 * 쪼갤 실익이 없음 */
static SemaphoreHandle_t s_mutex;

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

/* 청크 신뢰성 재설계(2026-08-03) — "핸드셰이크처럼 청크마다 응답을 기다리는데 정작 그
 * 응답이 로컬 라디오 ACK일 뿐이라 진짜 확인이 아니었던" 예전 방식을 버리고, 신뢰도 높은
 * 브로드캐스팅에서 쓰는 스트리밍+선택적 재전송(NACK) 방식으로 교체(사용자 설계 지시,
 * esp_now_link.h의 esp_now_photo_chunk_nack_t 주석 참고). 어느 chunk_idx를 받았는지
 * 비트맵으로 추적해뒀다가 DONE 도착 시 빠진 것만 콕 집어 CAM에 재전송 요청 */
#define PHOTO_MAX_CHUNKS ((PHOTO_RECV_BUF_CAP + ESP_NOW_PHOTO_CHUNK_DATA_LEN - 1) / ESP_NOW_PHOTO_CHUNK_DATA_LEN)
static uint8_t    s_chunk_bitmap[(PHOTO_MAX_CHUNKS + 7) / 8];
#define PHOTO_NACK_MAX_ROUNDS 3
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

static volatile esp_now_photo_state_t s_state = ESP_NOW_PHOTO_STATE_IDLE;
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
        ui_log_add_err(UI_ERR_CACHE_TOO_BIG, "사진 저장 실패(용량초과) file_id=%u len=%u", (unsigned)file_id, (unsigned)len);
        return;
    }

    for (int i = 0; i < PHOTO_CACHE_SLOTS; i++) {
        if (s_cache[i].used && s_cache[i].file_id == file_id) {
            memcpy(s_cache[i].data, data, len);
            s_cache[i].len = len;
            ui_log_add("CACHE 슬롯[%d] 갱신 file_id=%u len=%u", i, (unsigned)file_id, (unsigned)len);
            return;
        }
    }
    photo_cache_slot_t *slot = &s_cache[s_cache_next];
    if (!slot->data) {
        ESP_LOGE(TAG, "cache_insert: 슬롯 버퍼 없음(초기 할당 실패?) — 버림");
        ui_log_add_err(UI_ERR_CACHE_NO_BUF, "사진 저장 실패(메모리 부족) file_id=%u", (unsigned)file_id);
        s_cache_next = (s_cache_next + 1) % PHOTO_CACHE_SLOTS;
        return;
    }
    ui_log_add("CACHE 슬롯[%d] 신규(이전 file_id=%u) -> file_id=%u len=%u",
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
static volatile esp_now_capture_stage_t s_capture_stage = ESP_NOW_CAPTURE_STAGE_NONE;

/* ────────────────────────────────────────────────────────────
 * 3. 사진 목록
 * ──────────────────────────────────────────────────────────── */
static esp_now_photo_list_item_t s_list_items[ESP_NOW_PHOTO_LIST_MAX];
static int                        s_list_count = 0;
static volatile esp_now_photo_list_state_t s_list_state = ESP_NOW_PHOTO_LIST_STATE_IDLE;
static uint32_t                   s_sd_total_kb = 0;  /* 최근 목록 응답에 실려온 CAM SD 용량 */
static uint32_t                   s_sd_used_kb  = 0;

/* 목록 항목(PHOTO_LIST_ENTRY)은 청크와 달리 ACK/재전송이 없는 단발성 esp_now_send라
 * 유실될 수 있음 — 유실된 게 하필 마지막 항목(=file_id 오름차순으로 보내므로 항상
 * 최신 사진)이면 Cntl 목록에서 최신 사진이 통째로 빠지고, 그 앞의(이미 봤던) 사진이
 * index 0으로 보여서 "선택은 되는데 안 가져옴"처럼 보임(2026-08-02 실기에서 확인).
 * LIST_DONE의 count와 실제 수신 개수를 대조해 다르면 재요청 — 청크의 3회 재시도와
 * 같은 원칙 */
#define PHOTO_LIST_MAX_RETRIES 2
static uint8_t  s_list_cam_mac[6];
static int      s_list_retry_count = 0;

void esp_now_photo_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    ui_log_add("INIT 여유PSRAM(시작)=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    s_recv_buf = heap_caps_malloc(PHOTO_RECV_BUF_CAP, MALLOC_CAP_SPIRAM);
    if (s_recv_buf) {
        s_recv_cap = PHOTO_RECV_BUF_CAP;
        ui_log_add("INIT recv_buf=OK 여유PSRAM=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        ESP_LOGE(TAG, "수신 버퍼 초기 할당 실패(%u bytes) — 사진 수신 불가", (unsigned)PHOTO_RECV_BUF_CAP);
        ui_log_add_err(UI_ERR_RECV_BUF_ALLOC, "수신버퍼 할당 실패 — 사진 수신 불가");
    }

    for (int i = 0; i < PHOTO_CACHE_SLOTS; i++) {
        s_cache[i].data = heap_caps_malloc(PHOTO_CACHE_SLOT_CAP, MALLOC_CAP_SPIRAM);
        if (s_cache[i].data) {
            ui_log_add("INIT 캐시슬롯[%d]=OK 여유PSRAM=%u", i, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else {
            ESP_LOGE(TAG, "캐시 슬롯[%d] 초기 할당 실패(%u bytes)", i, (unsigned)PHOTO_CACHE_SLOT_CAP);
            ui_log_add_err(UI_ERR_CACHE_SLOT_ALLOC, "캐시 슬롯[%d] 할당 실패 — 여러 장 저장 불가", i);
        }
    }
}

/* ════════════════════════════════════════════════════════════
 * 단일 사진 수신 — 내부 공용 로직
 * ════════════════════════════════════════════════════════════ */
/* 통신 시도 전 항상 지금 이 기기를 아는 상태인지 확인(2026-08-04, 사용자 지시로 도입,
 * 2026-08-10 connectionless 모델로 재정의) — ESP-NOW는 connectionless라 "연결/연결끊김"
 * 자체가 없는 개념이었음(사용자 정정). WAITING(한 번도 페어링 안 됐거나 자동 재연결이
 * 정상 범위를 넘겨 실패 중)일 때만 막고, PAIRED/ACTIVE는 CAM이 딥슬립 사이 무선
 * 무응답 구간에 있어도 그냥 통과시킴 — 실제 전송이 응답을 못 받으면 그건 이 요청
 * 자체의 무응답 에러(UI_ERR_*_NORESPONSE 등, 기존 개별 처리)로 자연스럽게 드러남.
 * 재페어링 자체는 esp_now_hub.c의 ADVERTISE 핸들러가 사용자 액션과 무관하게 매 딥슬립
 * 사이클마다 알아서 재시도하므로 여기서 따로 안 건드림.
 * 5개 액션 함수(지금촬영/목록갱신/삭제/전체삭제/사진선택-fetch) 전부 이 함수를 거쳐가므로,
 * 적응형 반응시간(2026-08-10)의 "마지막 사용자 조작" 시각도 여기서 한 곳에서 갱신함 —
 * select_camera()의 자동 최초 목록조회도 이 경로를 타지만, 그 정도는 "활동"으로 봐도
 * 무해함(어차피 드물게 발생, 과설계 방지) */
static bool require_paired(const uint8_t *cam_mac, const char *what)
{
    (void)what;
    esp_now_hub_note_user_action();
    return esp_now_hub_get_conn_state(cam_mac) != HUB_CONN_STATE_WAITING;
}

static void start_single_receive(const uint8_t *cam_mac, uint8_t mode, uint32_t param)
{
    if (!require_paired(cam_mac, "사진 요청")) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == ESP_NOW_PHOTO_STATE_RECEIVING) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "이미 수신 중 — 새 요청 무시");
        ui_log_add_err(UI_ERR_REQUEST_BUSY, "요청 무시됨(이미 수신중) param=%u", (unsigned)param);
        return;
    }
    s_state = ESP_NOW_PHOTO_STATE_RECEIVING;
    /* 이전 전송의 진행률 카운터가 새 META 도착 전까지 남아있으면 fetch 진행 팝업이
     * 그 낡은 값으로 퍼센트/ETA를 잘못 계산함(2026-08-01 실기에서 확인: 67%부터 시작,
     * 남은시간이 거꾸로 증가) — 새 요청 시작 시점에 바로 지움 */
    s_chunks_received = 0;
    s_total_chunks    = 0;
    chunk_bitmap_clear();
    s_nack_rounds_used = 0;
    /* file_id도 요청 시점에 바로 갱신(현재 유일한 호출부인 fetch_by_id는 mode=BY_ID,
     * param=file_id) — META 도착 전까지 s_file_id가 "이전" 요청 값 그대로 남아있으면
     * 그 사이에 이전 요청의 뒤늦은 CHUNK/DONE이 도착했을 때 file_id가 우연히 일치해서
     * 지금 받는 중인 걸로 잘못 받아들여지는 경우가 있었음(2026-08-01, 실기에서 다른
     * 사진을 선택해도 엉뚱한 사진이 뜨는 문제로 확인) */
    s_file_id = param;
    memcpy(s_photo_cam_mac, cam_mac, sizeof(s_photo_cam_mac));
    xSemaphoreGive(s_mutex);

    /* 2026-08-05 Layer 1 — esp_now_tx로 큐잉, PHOTO_META를 기다림(재시도는 레이어가 대신 함).
     * CAM 쪽 recv_cb가 mode+param 동일 요청은 dedup 처리하므로(esp_now_cam.c 참고) 재시도로
     * 같은 요청이 여러 번 도착해도 안전 */
    esp_now_photo_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_REQUEST,
        .mode     = mode,
        .param    = param,
    };
    static const uint8_t s_meta_types[] = { ESP_NOW_MSG_PHOTO_META };
    esp_now_tx_enqueue(cam_mac, &req, sizeof(req), s_meta_types, 1, 500, 3, "사진 요청");
    ESP_LOGI(TAG, "PHOTO_REQUEST(mode=%d, param=%u) 큐잉됨", mode, (unsigned)param);
    ui_log_add("REQUEST mode=%d param=%u 큐잉됨", mode, (unsigned)param);
}

/* 촬영과 전송은 완전히 분리(2026-08-01) — CAM에 "지금 찍어라"만 보내고 CAPTURE_STATUS로
 * 결과만 확인함. 사진 자체는 여기서 안 받음(단일수신 상태머신을 아예 안 씀) — 실제로
 * 보려면 목록에서 선택해서 fetch_by_id로 따로 받아야 함 */
void esp_now_photo_capture_now(const uint8_t *cam_mac)
{
    if (!require_paired(cam_mac, "지금촬영")) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capture_stage = ESP_NOW_CAPTURE_STAGE_SENT;
    xSemaphoreGive(s_mutex);

    esp_now_photo_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_REQUEST,
        .mode     = PHOTO_REQUEST_MODE_CAPTURE_NOW,
        .param    = 0,
    };
    /* 2026-08-05 Layer 1 — CAPTURE_STATUS(RECEIVED)를 기다림(재시도는 레이어가 대신 함).
     * 촬영 자체의 최종 결과(SUCCESS/FAILED)는 이후 별도 비동기 CAPTURE_STATUS로 옴 —
     * 그건 기존처럼 recv_cb -> handle_capture_status()가 처리(여기서 안 기다림) */
    static const uint8_t s_capture_status_types[] = { ESP_NOW_MSG_CAPTURE_STATUS };
    esp_now_tx_enqueue(cam_mac, &req, sizeof(req), s_capture_status_types, 1, 500, 3, "지금촬영");
    ESP_LOGI(TAG, "PHOTO_REQUEST(mode=CAPTURE_NOW) 큐잉됨");
}

void esp_now_photo_fetch_by_id(const uint8_t *cam_mac, uint32_t file_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* 이전 요청이 끝까지(PHOTO_DONE) 못 가고 걸려있었어도, 목록에서 새로 선택한 이상
     * 사용자 의도는 "새로 시작"이므로 강제로 흘려보냄(capture_now에서 겪었던 것과 같은
     * 이유 — RECEIVING에 타임아웃이 없어서 한번 걸리면 이후 요청이 계속 씹힘) */
    s_state = ESP_NOW_PHOTO_STATE_IDLE;
    /* s_file_id를 여기서 바로 새 file_id로 맞춰둠(META 도착 전에 미리) — CAM은 취소
     * 프로토콜이 없어서 사용자가 빠르게 다른 사진을 다시 선택하면 CAM이 이전 요청을
     * 여전히 전송 중일 수 있음(2026-08-02, CAM 쪽엔 세대번호로 스스로 중단하게 고침).
     * 그 사이 이전 file_id의 뒤늦은 청크가 도착했을 때 s_file_id가 아직 옛 값 그대로면
     * handle_chunk()의 file_id 일치 검사를 통과해서 새 수신버퍼에 잘못 섞여 들어갈 수
     * 있음 — 미리 새 file_id로 바꿔두면 옛 청크는 자동으로 불일치 처리되어 버려짐 */
    s_file_id = file_id;
    xSemaphoreGive(s_mutex);
    start_single_receive(cam_mac, PHOTO_REQUEST_MODE_BY_ID, file_id);
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
     * 같아") — LVGL UI 태스크(esp_now_photo_fetch_by_id, 새 선택 시 s_file_id를 미리 바꿈)와
     * 이 함수(ESP-NOW 콜백 태스크)가 같은 필드들을 서로 다른 락 구간에서 만지고 있어서
     * "확인"과 "그 확인을 근거로 쓰기"가 원자적이지 않았음. 이제 관련 필드 전부를 하나의
     * 락 구간 안에서 같이 바꿈 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (meta->total_size > s_recv_cap || !s_recv_buf) {
        s_state = ESP_NOW_PHOTO_STATE_ERROR;
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "사진이 고정 수신 버퍼보다 큼(%u > %u bytes) — 버림",
                 (unsigned)meta->total_size, (unsigned)s_recv_cap);
        ui_log_add_err(UI_ERR_META_TOO_BIG, "사진 수신 실패(용량초과) %u > %u", (unsigned)meta->total_size, (unsigned)s_recv_cap);
        return;
    }
    s_file_id         = meta->file_id;
    s_total_size       = meta->total_size;
    s_total_chunks     = meta->total_chunks;
    s_expected_crc     = meta->crc32;
    s_chunks_received  = 0;
    chunk_bitmap_clear();
    s_nack_rounds_used = 0;
    s_state            = ESP_NOW_PHOTO_STATE_RECEIVING;
    /* 응답(WINDOW_STATUS_ACK/DONE_ACK) 보낼 대상을 실제 발신자 MAC으로 갱신(2026-08-05,
     * Selective Repeat 벤치마크로 발견) — 원래는 start_single_receive()가 Cntl이 먼저
     * PHOTO_REQUEST를 보낼 때 미리 채워뒀는데, XFER_BENCH 모드는 CAM이 요청 없이 먼저
     * 밀어서(META부터 시작) s_photo_cam_mac이 한 번도 안 채워진 채로 남아있었음(초기값
     * 전부 0) — esp_now_send(0-MAC, ...)이 ESP_ERR_ESPNOW_NOT_FOUND로 항상 실패해서 응답이
     * CAM에 전혀 안 갔던 게 원인. META를 실제로 누가 보냈는지가 항상 진짜 정답이므로
     * 여기서 갱신하는 게 요청 경로 여부와 무관하게 맞음 */
    if (src_mac) memcpy(s_photo_cam_mac, src_mac, sizeof(s_photo_cam_mac));
    xSemaphoreGive(s_mutex);
}

static void handle_chunk(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_chunk_t)) return;
    const esp_now_photo_chunk_t *chunk = (const esp_now_photo_chunk_t *)data;

    /* 확인(state/file_id 일치)과 실제 쓰기(memcpy)를 같은 락 구간 안에서 — 그 사이에
     * esp_now_photo_fetch_by_id()가 끼어들어 s_file_id/s_state를 새 요청으로 바꿔버리면,
     * 이 청크가 이미 낡은 요청 것인데도 그 사실을 놓치고 새 수신버퍼에 잘못 쓰일 수 있었음 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state != ESP_NOW_PHOTO_STATE_RECEIVING || chunk->file_id != s_file_id || !s_recv_buf) {
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
    xSemaphoreGive(s_mutex);

    /* 2026-08-10 — 적응형 반응시간의 "마지막 사용자 조작" 시각을 청크마다 갱신. 예전엔
     * require_paired()가 요청 "시작" 시점에 한 번만 갱신해서, 전송이 몇 초 걸리면 그 시간이
     * 고스란히 조용한 시간으로 카운트돼버림 — 전송 도중에 이미 적응형 임계값을 넘겨 SLEEP_NOW가
     * CAM에 큐잉되고, 전송이 끝나 busy가 풀리자마자(사용자가 결과를 볼 틈도 없이) 바로 잠드는
     * 문제로 실사용 중 확인됨(사용자 분석: "통신 완료 후가 아니라 통신을 시작한 입력에 의해
     * 카운터가 진행됨"). 청크가 계속 들어오는 동안은 "활동 중"이 맞으므로 매 청크 갱신 */
    esp_now_hub_note_user_action();
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
    esp_err_t err = esp_now_send(s_photo_cam_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "PHOTO_DONE_ACK 전송(누락 %u개) file_id=%u: %s", missing_count, (unsigned)s_file_id, esp_err_to_name(err));
}

static void handle_done(const uint8_t *data, int len)
{
    (void)data; (void)len;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_photo_state_t st = s_state;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "PHOTO_DONE 수신: state=%d, chunks=%u/%u", st, s_chunks_received, s_total_chunks);
    ui_log_add("DONE state=%d chunks=%u/%u file_id=%u", st, s_chunks_received, s_total_chunks, (unsigned)s_file_id);
    if (st != ESP_NOW_PHOTO_STATE_RECEIVING) {
        /* RECEIVING이 아니면 이 거래는 CAM 쪽에서 이미 다른 시도로 대체됐거나(세대번호) 이
         * 요청 자체를 우리가 모름(META를 못 받은 상태) — 뭘 요청받았는지조차 몰라서 의미
         * 있는 ACK를 만들 방법이 없으므로 응답 안 함. CAM은 reliable_request 타임아웃으로
         * 알아서 포기함 */
        ui_log_add("DONE 무시(state!=RECEIVING)");
        return;
    }

    if (s_total_chunks == 0 && s_chunks_received == 0) {
        /* META가 아예 안 왔던 경우(해당 file_id가 없음 등) — 에러가 아니라 그냥
         * "보낼 게 없었다"로 조용히 종료. 위와 같은 이유로 응답 생략 */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_IDLE;
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

        if (s_nack_rounds_used < PHOTO_NACK_MAX_ROUNDS) {
            s_nack_rounds_used++;
            ESP_LOGW(TAG, "청크 누락(%u/%u) — 재전송 요청(%u개, 라운드 %d/%d)",
                     s_chunks_received, s_total_chunks, n, s_nack_rounds_used, PHOTO_NACK_MAX_ROUNDS);
            ui_log_add("DONE_ACK 누락 %u개 통보(라운드 %d/%d) file_id=%u", n, s_nack_rounds_used,
                       PHOTO_NACK_MAX_ROUNDS, (unsigned)s_file_id);
            return;
        }
        ESP_LOGW(TAG, "청크 누락(%u/%u) — NACK 라운드 소진, 사진 버림", s_chunks_received, s_total_chunks);
        ui_log_add_err(UI_ERR_CHUNK_MISSING, "사진 수신 실패(청크 누락 %u/%u, 재전송 후에도)", s_chunks_received, s_total_chunks);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_ERROR;
        xSemaphoreGive(s_mutex);
        return;
    }

    uint32_t crc = esp_rom_crc32_le(0, s_recv_buf, s_total_size);
    if (crc != s_expected_crc) {
        ESP_LOGW(TAG, "CRC 불일치 — 사진 버림(재조립 실패)");
        ui_log_add_err(UI_ERR_CRC_MISMATCH, "사진 수신 실패(CRC 불일치) file_id=%u", (unsigned)s_file_id);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_ERROR;
        xSemaphoreGive(s_mutex);
        return;
    }

    /* 검증된 압축 JPEG 원본을 캐시 슬롯(고정 버퍼)으로 memcpy — 픽셀 디코드는 안 함
     * (판넬/뷰어/웹이 필요할 때마다 각자 해상도로 디코드). s_recv_buf는 다음 요청 때
     * 재사용되므로 캐시는 별도 고정 버퍼에 복사해둠(malloc 없음 — cache_insert_locked 참고) */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cache_insert_locked(s_file_id, s_recv_buf, s_total_size);
    s_ready_file_id = s_file_id;
    s_state = ESP_NOW_PHOTO_STATE_READY;
    xSemaphoreGive(s_mutex);

    send_done_ack(0, NULL);  /* 완료 통보(2026-08-05, Layer 1) — missing_count=0 */
    ESP_LOGI(TAG, "사진 수신 완료: file_id=%u, %u bytes", (unsigned)s_file_id, (unsigned)s_total_size);
    ui_log_add("READY file_id=%u %u bytes", (unsigned)s_file_id, (unsigned)s_total_size);
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
    bool ok = (s_state == ESP_NOW_PHOTO_STATE_RECEIVING && req->file_id == s_file_id);
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
    esp_err_t err = esp_now_send(s_photo_cam_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "WINDOW_STATUS_ACK [%u,%u) 누락 %u개: %s",
             req->range_start, end, n, esp_err_to_name(err));
}

esp_now_photo_state_t esp_now_photo_get_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_photo_state_t st = s_state;
    xSemaphoreGive(s_mutex);
    return st;
}

/* RECEIVING 중일 때만 의미 있음 — fetch 진행 팝업의 퍼센트/ETA 계산용(락 없이 읽음,
 * ESP-NOW 태스크만 쓰고 여긴 표시용으로만 읽어서 uint16 tearing 정도는 무해) */
void esp_now_photo_get_chunk_progress(uint16_t *received, uint16_t *total)
{
    *received = s_chunks_received;
    *total    = s_total_chunks;
}

uint32_t esp_now_photo_get_ready_file_id(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t id = s_ready_file_id;
    xSemaphoreGive(s_mutex);
    return id;
}

void esp_now_photo_ready_ack(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == ESP_NOW_PHOTO_STATE_READY) {
        s_state = ESP_NOW_PHOTO_STATE_IDLE;
    }
    xSemaphoreGive(s_mutex);
}

bool esp_now_photo_cache_get(uint32_t file_id, const uint8_t **out_data, size_t *out_len)
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

void esp_now_photo_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == ESP_NOW_PHOTO_STATE_ERROR) {
        s_state = ESP_NOW_PHOTO_STATE_IDLE;
    }
    xSemaphoreGive(s_mutex);
}

/* ════════════════════════════════════════════════════════════
 * 지금촬영 진행 단계
 * ════════════════════════════════════════════════════════════ */
static void handle_capture_status(const uint8_t *src_mac, const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_capture_status_t)) return;
    const esp_now_capture_status_t *msg = (const esp_now_capture_status_t *)data;
    ESP_LOGI(TAG, "CAPTURE_STATUS 수신: status=%d", msg->status);

    /* 최종 결과(SUCCESS/FAILED)만 ACK — CAM이 esp_now_reliable_request()로 감싸서 기다리는
     * 건 이 둘뿐(2026-08-05, Layer 1). RECEIVED는 기존처럼 단발성 알림으로 유지 */
    if (src_mac && (msg->status == CAM_CAPTURE_STATUS_SUCCESS || msg->status == CAM_CAPTURE_STATUS_FAILED)) {
        esp_now_capture_status_ack_t ack = {
            .version  = ESP_NOW_LINK_VERSION,
            .msg_type = ESP_NOW_MSG_CAPTURE_STATUS_ACK,
        };
        esp_now_send(src_mac, (const uint8_t *)&ack, sizeof(ack));
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    switch (msg->status) {
        case CAM_CAPTURE_STATUS_RECEIVED:
            s_capture_stage = ESP_NOW_CAPTURE_STAGE_ACKED;
            break;
        case CAM_CAPTURE_STATUS_SUCCESS:
            s_capture_stage = ESP_NOW_CAPTURE_STAGE_CAPTURED;
            break;
        case CAM_CAPTURE_STATUS_FAILED:
            s_capture_stage = ESP_NOW_CAPTURE_STAGE_CAPTURE_FAILED;
            ui_log_add_err(UI_ERR_CAPTURE_FAILED, "촬영 실패(CAM 응답)");
            break;
        default:
            break;
    }
    xSemaphoreGive(s_mutex);
}

esp_now_capture_stage_t esp_now_photo_get_capture_stage(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_capture_stage_t stage = s_capture_stage;
    xSemaphoreGive(s_mutex);
    return stage;
}

void esp_now_photo_capture_stage_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capture_stage = ESP_NOW_CAPTURE_STAGE_NONE;
    xSemaphoreGive(s_mutex);
}

/* ════════════════════════════════════════════════════════════
 * 사진 목록
 * ════════════════════════════════════════════════════════════ */
static void send_list_request_raw(const uint8_t *cam_mac)
{
    /* 2026-08-05 Layer 1 — esp_now_tx로 큐잉, 최종 신호인 PHOTO_LIST_DONE을 기다림(중간
     * LIST_ENTRY들은 그 사이 recv_cb -> handle_list_entry()로 정상 누적됨, 여기선 안 건드림).
     * CAM 쪽 s_list_request_pending으로 중복 요청 처리는 걸러짐(esp_now_cam.c 참고).
     * 타임아웃을 3초로 넉넉히 잡음 — 최대 500장 순회+개별 send()라 즉답형 메시지보다 오래 걸림 */
    esp_now_photo_list_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_LIST_REQUEST,
    };
    static const uint8_t s_list_done_types[] = { ESP_NOW_MSG_PHOTO_LIST_DONE };
    esp_now_tx_enqueue(cam_mac, &req, sizeof(req), s_list_done_types, 1, 3000, 3, "목록 요청");
    ESP_LOGI(TAG, "PHOTO_LIST_REQUEST 큐잉됨");
}

void esp_now_photo_list_request(const uint8_t *cam_mac)
{
    if (!require_paired(cam_mac, "목록 요청")) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_list_state = ESP_NOW_PHOTO_LIST_STATE_REQUESTING;
    s_list_count = 0;
    s_list_retry_count = 0;
    memcpy(s_list_cam_mac, cam_mac, sizeof(s_list_cam_mac));
    xSemaphoreGive(s_mutex);

    send_list_request_raw(cam_mac);
}

static void handle_list_entry(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_list_entry_t)) return;
    const esp_now_photo_list_entry_t *entry = (const esp_now_photo_list_entry_t *)data;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_list_state == ESP_NOW_PHOTO_LIST_STATE_REQUESTING && s_list_count < ESP_NOW_PHOTO_LIST_MAX) {
        s_list_items[s_list_count].file_id      = entry->file_id;
        s_list_items[s_list_count].kind         = entry->kind;
        s_list_items[s_list_count].capture_time = entry->capture_time;
        s_list_items[s_list_count].file_size    = entry->file_size;
        s_list_count++;
    }
    xSemaphoreGive(s_mutex);

    esp_now_hub_note_user_action();  /* 2026-08-10 — handle_chunk()와 동일 이유(목록도 최대
                                         500장이라 전송에 시간이 걸릴 수 있음) */
}

/* PHOTO_LIST_DONE_ACK를 항상 보냄(2026-08-05, Layer 1) — DONE_ACK와 동일한 원칙. CAM은
 * esp_now_reliable_request()로 이 응답을 기다리므로 재요청이 필요하면 CAM이 알아서 LIST_DONE을
 * 다시 보냄(레이어가 재시도를 대신함) — Cntl이 3번 반복 전송할 필요 없음 */
static void send_list_done_ack(void)
{
    esp_now_photo_list_done_t ack = {
        .version     = ESP_NOW_LINK_VERSION,
        .msg_type    = ESP_NOW_MSG_PHOTO_LIST_DONE_ACK,
        .count       = (uint16_t)s_list_count,
        .sd_total_kb = 0,
        .sd_used_kb  = 0,
    };
    esp_err_t err = esp_now_send(s_list_cam_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "PHOTO_LIST_DONE_ACK 전송(count=%d): %s", s_list_count, esp_err_to_name(err));
}

static void handle_list_done(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_list_done_t)) return;
    const esp_now_photo_list_done_t *done = (const esp_now_photo_list_done_t *)data;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_list_state == ESP_NOW_PHOTO_LIST_STATE_REQUESTING) {
        if (done->count != s_list_count && s_list_retry_count < PHOTO_LIST_MAX_RETRIES) {
            s_list_retry_count++;
            int got = s_list_count, want = done->count, retry = s_list_retry_count;
            uint8_t mac_copy[6];
            memcpy(mac_copy, s_list_cam_mac, sizeof(mac_copy));
            xSemaphoreGive(s_mutex);
            send_list_done_ack();  /* 재요청 전에도 일단 지금까지 받은 결과는 확인해줌 */
            ui_log_add_err(UI_ERR_LIST_COUNT_MISMATCH, "목록 %d/%d개만 수신 — 재요청(%d/%d)",
                            got, want, retry, PHOTO_LIST_MAX_RETRIES);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_list_count = 0;  /* 재요청이라 처음부터 새로 받음 */
            xSemaphoreGive(s_mutex);
            send_list_request_raw(mac_copy);
            return;
        }
        if (done->count != s_list_count) {
            ui_log_add_err(UI_ERR_LIST_COUNT_MISMATCH, "목록 %d/%d개만 수신 — 재시도 포기, 있는 것만 표시",
                            s_list_count, done->count);
        }
        s_list_state  = ESP_NOW_PHOTO_LIST_STATE_READY;
        s_sd_total_kb = done->sd_total_kb;
        s_sd_used_kb  = done->sd_used_kb;
    }
    xSemaphoreGive(s_mutex);
    send_list_done_ack();
}

esp_now_photo_list_state_t esp_now_photo_list_get_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_photo_list_state_t st = s_list_state;
    xSemaphoreGive(s_mutex);
    return st;
}

int esp_now_photo_list_get_items(esp_now_photo_list_item_t *out, int max)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_list_count;
    if (count > max) count = max;
    /* CAM은 오래된 것부터(오름차순) 보내는데, 화면엔 최신이 위로 오는 게 자연스러워서 뒤집어서 복사 */
    for (int i = 0; i < count; i++) {
        out[i] = s_list_items[s_list_count - 1 - i];
    }
    xSemaphoreGive(s_mutex);
    return count;
}

void esp_now_photo_list_ack(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_list_state == ESP_NOW_PHOTO_LIST_STATE_READY) {
        s_list_state = ESP_NOW_PHOTO_LIST_STATE_IDLE;
    }
    xSemaphoreGive(s_mutex);
}

void esp_now_photo_list_get_sd_usage(uint32_t *out_total_kb, uint32_t *out_used_kb)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out_total_kb = s_sd_total_kb;
    *out_used_kb  = s_sd_used_kb;
    xSemaphoreGive(s_mutex);
}

void esp_now_photo_delete(const uint8_t *cam_mac, uint32_t file_id)
{
    if (!require_paired(cam_mac, "사진 삭제")) return;

    esp_now_photo_delete_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_DELETE_REQUEST,
        .file_id  = file_id,
    };
    /* 2026-08-05 Layer 1 — PHOTO_DELETE_ACK를 기다림(재시도는 레이어가 대신 함) */
    static const uint8_t s_delete_ack_types[] = { ESP_NOW_MSG_PHOTO_DELETE_ACK };
    esp_now_tx_enqueue(cam_mac, &req, sizeof(req), s_delete_ack_types, 1, 500, 3, "사진 삭제");
    ESP_LOGI(TAG, "PHOTO_DELETE_REQUEST(id=%u) 큐잉됨", (unsigned)file_id);
}

static void handle_delete_ack(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_delete_ack_t)) return;
    const esp_now_photo_delete_ack_t *ack = (const esp_now_photo_delete_ack_t *)data;
    ESP_LOGI(TAG, "PHOTO_DELETE_ACK id=%u: %s", (unsigned)ack->file_id, ack->success ? "성공" : "실패");
    if (!ack->success) ui_log_add_err(UI_ERR_DELETE_FAILED, "사진 삭제 실패 file_id=%u", (unsigned)ack->file_id);
}

/* ────────────────────────────────────────────────────────────
 * 4. 전체 삭제
 * ──────────────────────────────────────────────────────────── */
static volatile esp_now_delete_all_state_t s_delete_all_state = ESP_NOW_DELETE_ALL_STATE_NONE;
static bool     s_delete_all_success = false;
static uint16_t s_delete_all_count = 0;

void esp_now_photo_delete_all(const uint8_t *cam_mac)
{
    if (!require_paired(cam_mac, "전체삭제")) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_delete_all_state = ESP_NOW_DELETE_ALL_STATE_REQUESTED;
    xSemaphoreGive(s_mutex);

    esp_now_photo_delete_all_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_DELETE_ALL_REQUEST,
    };
    /* 2026-08-05 Layer 1 — PHOTO_DELETE_ALL_ACK를 기다림(재시도는 레이어가 대신 함).
     * 전체삭제는 CAM이 최대 500개 파일을 지워야 해서 시간이 걸릴 수 있음 — 타임아웃을
     * 넉넉히 잡음 */
    static const uint8_t s_delete_all_ack_types[] = { ESP_NOW_MSG_PHOTO_DELETE_ALL_ACK };
    esp_now_tx_enqueue(cam_mac, &req, sizeof(req), s_delete_all_ack_types, 1, 3000, 3, "전체삭제");
    ESP_LOGI(TAG, "PHOTO_DELETE_ALL_REQUEST 큐잉됨");
}

static void handle_delete_all_ack(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_delete_all_ack_t)) return;
    const esp_now_photo_delete_all_ack_t *ack = (const esp_now_photo_delete_all_ack_t *)data;
    ESP_LOGI(TAG, "PHOTO_DELETE_ALL_ACK 수신: 성공=%d, %u개", ack->success, (unsigned)ack->deleted_count);
    if (!ack->success) ui_log_add_err(UI_ERR_DELETE_ALL_FAILED, "전체삭제 실패");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_delete_all_state == ESP_NOW_DELETE_ALL_STATE_REQUESTED) {
        s_delete_all_success = ack->success;
        s_delete_all_count   = ack->deleted_count;
        s_delete_all_state   = ESP_NOW_DELETE_ALL_STATE_ACKED;
    }
    xSemaphoreGive(s_mutex);
}

esp_now_delete_all_state_t esp_now_photo_delete_all_get_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_delete_all_state_t st = s_delete_all_state;
    xSemaphoreGive(s_mutex);
    return st;
}

bool esp_now_photo_delete_all_get_success(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_delete_all_success;
    xSemaphoreGive(s_mutex);
    return ok;
}

uint16_t esp_now_photo_delete_all_get_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint16_t count = s_delete_all_count;
    xSemaphoreGive(s_mutex);
    return count;
}

void esp_now_photo_delete_all_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_delete_all_state = ESP_NOW_DELETE_ALL_STATE_NONE;
    xSemaphoreGive(s_mutex);
}

/* ════════════════════════════════════════════════════════════
 * 디스패치
 * ════════════════════════════════════════════════════════════ */
void esp_now_photo_on_recv(uint8_t msg_type, const uint8_t *src_mac, const uint8_t *data, int len)
{
    switch (msg_type) {
        case ESP_NOW_MSG_PHOTO_META:      handle_meta(src_mac, data, len);  break;
        case ESP_NOW_MSG_PHOTO_CHUNK:      handle_chunk(data, len);          break;
        case ESP_NOW_MSG_PHOTO_DONE:       handle_done(data, len);           break;
        case ESP_NOW_MSG_CAPTURE_STATUS:   handle_capture_status(src_mac, data, len); break;
        case ESP_NOW_MSG_PHOTO_LIST_ENTRY: handle_list_entry(data, len);     break;
        case ESP_NOW_MSG_PHOTO_LIST_DONE:  handle_list_done(data, len);      break;
        case ESP_NOW_MSG_PHOTO_DELETE_ACK: handle_delete_ack(data, len);     break;
        case ESP_NOW_MSG_PHOTO_DELETE_ALL_ACK: handle_delete_all_ack(data, len); break;
        case ESP_NOW_MSG_PHOTO_WINDOW_STATUS_REQUEST: handle_window_status_request(data, len); break;
        default: break;
    }
}
