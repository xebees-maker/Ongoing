#include "esp_now_photo.h"
#include "esp_now_link.h"
#include "ui_log.h"

#include <string.h>
#include "esp_now.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "esp_now_photo";

/* 모듈 전체 상태를 하나의 뮤텍스로 보호 — ESP-NOW 태스크(recv_cb 경유)와 LVGL 워커
 * 태스크(UI) 양쪽에서 건드리는데, 호출 빈도가 낮아서(초당 몇 번 수준) 필드별로 락을
 * 쪼갤 실익이 없음 */
static SemaphoreHandle_t s_mutex;

/* ────────────────────────────────────────────────────────────
 * 1. 단일 사진 수신(capture_now/fetch_by_id 공용)
 * ──────────────────────────────────────────────────────────── */
/* 실측 JPEG 크기 650~670KB에 여유를 둔 고정 상한 — 부팅 시 한 번만 할당하고 절대
 * free/realloc 안 함(2026-08-01, recv 버퍼/캐시 슬롯/판넬·뷰어 디코드 버퍼 모두 동일 원칙 —
 * 반복 사용하는 버퍼는 처음에 한 번만 잡고 계속 재사용, 매번 free+malloc 하지 않기).
 * 800KB였던 걸 720KB로 축소(2026-08-01) — 실기 로그로 이 보드의 실제 여유 PSRAM이
 * 폰트 로드 직후 기준 ~2.4MB뿐임을 확인(기존에 참고하던 "6.7MB usable"은 이 빌드
 * 기준 틀린 수치였음, 화면 프레임버퍼+폰트가 이미 5.5MB 이상을 씀) — recv_buf+캐시
 * 슬롯 2개(같은 크기) 셋이 이 예산 안에 다 들어가야 해서 실측 최대치(670KB) 대비
 * 여유를 줄임 */
#define PHOTO_RECV_BUF_CAP (720 * 1024)
static uint8_t  *s_recv_buf = NULL;   /* ESP-NOW 태스크만 건드림, 밖으로 포인터가 안 나감 */
static size_t     s_recv_cap = 0;
static uint32_t   s_file_id = 0;
static uint32_t   s_total_size = 0;
static uint16_t   s_total_chunks = 0;
static uint32_t   s_expected_crc = 0;
static uint16_t   s_chunks_received = 0;

static volatile esp_now_photo_state_t s_state = ESP_NOW_PHOTO_STATE_IDLE;
static uint32_t s_ready_file_id = 0;  /* READY 상태일 때 방금 캐시에 들어간 file_id */

/* 압축 JPEG 원본 캐시 — 판넬(작게)/뷰어(크게)/웹(원본 그대로) 셋이 각자 필요한 만큼
 * 이 압축본에서 디코드해서 씀. 슬롯 버퍼도 recv 버퍼와 같은 이유로 부팅 시 고정
 * 할당(PHOTO_RECV_BUF_CAP과 동일 용량 — 캐시에 들어가는 것도 결국 같은 종류의 사진
 * 압축본이라 같은 상한이면 충분)해두고 매번 memcpy만 함, malloc/free 없음(2026-08-01
 * — 예전엔 handle_done()에서 완료될 때마다 heap_caps_malloc으로 새 복사본을 만들었는데,
 * 이게 판넬(320x240)/뷰어(1600x960, ~3.3MB) 고정버퍼까지 도입하고 나서 PSRAM 여유가
 * 빠듯해지자 두 번째 사진부터 바로 실패했음 — 실기 로그로 확인: "완료본 버퍼 할당 실패").
 * 슬롯 수는 6→3→2로 축소 — 뷰어 버퍼(~3.3MB)가 이미 크게 자리를 차지해서 여유를
 * 더 뒀음(FIFO 교체, LRU까지는 불필요) */
#define PHOTO_CACHE_SLOTS 2
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
static void start_single_receive(const uint8_t *cam_mac, uint8_t mode, uint32_t param)
{
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
    /* file_id도 요청 시점에 바로 갱신(현재 유일한 호출부인 fetch_by_id는 mode=BY_ID,
     * param=file_id) — META 도착 전까지 s_file_id가 "이전" 요청 값 그대로 남아있으면
     * 그 사이에 이전 요청의 뒤늦은 CHUNK/DONE이 도착했을 때 file_id가 우연히 일치해서
     * 지금 받는 중인 걸로 잘못 받아들여지는 경우가 있었음(2026-08-01, 실기에서 다른
     * 사진을 선택해도 엉뚱한 사진이 뜨는 문제로 확인) */
    s_file_id = param;
    xSemaphoreGive(s_mutex);

    esp_now_photo_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_REQUEST,
        .mode     = mode,
        .param    = param,
    };
    esp_err_t err = esp_now_send(cam_mac, (const uint8_t *)&req, sizeof(req));
    ESP_LOGI(TAG, "PHOTO_REQUEST(mode=%d, param=%u) 전송: %s", mode, (unsigned)param, esp_err_to_name(err));
    if (err == ESP_OK) {
        ui_log_add("REQUEST mode=%d param=%u OK", mode, (unsigned)param);
    } else {
        ui_log_add_err(UI_ERR_SEND_PHOTO_REQ, "사진 요청 전송 실패: %s", esp_err_to_name(err));
    }
}

/* 촬영과 전송은 완전히 분리(2026-08-01) — CAM에 "지금 찍어라"만 보내고 CAPTURE_STATUS로
 * 결과만 확인함. 사진 자체는 여기서 안 받음(단일수신 상태머신을 아예 안 씀) — 실제로
 * 보려면 목록에서 선택해서 fetch_by_id로 따로 받아야 함 */
void esp_now_photo_capture_now(const uint8_t *cam_mac)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capture_stage = ESP_NOW_CAPTURE_STAGE_SENT;
    xSemaphoreGive(s_mutex);

    esp_now_photo_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_REQUEST,
        .mode     = PHOTO_REQUEST_MODE_CAPTURE_NOW,
        .param    = 0,
    };
    esp_err_t err = esp_now_send(cam_mac, (const uint8_t *)&req, sizeof(req));
    ESP_LOGI(TAG, "PHOTO_REQUEST(mode=CAPTURE_NOW) 전송: %s", esp_err_to_name(err));
    if (err != ESP_OK) ui_log_add_err(UI_ERR_SEND_CAPTURE_REQ, "지금촬영 요청 전송 실패: %s", esp_err_to_name(err));
}

void esp_now_photo_fetch_by_id(const uint8_t *cam_mac, uint32_t file_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* 이전 요청이 끝까지(PHOTO_DONE) 못 가고 걸려있었어도, 목록에서 새로 선택한 이상
     * 사용자 의도는 "새로 시작"이므로 강제로 흘려보냄(capture_now에서 겪었던 것과 같은
     * 이유 — RECEIVING에 타임아웃이 없어서 한번 걸리면 이후 요청이 계속 씹힘) */
    s_state = ESP_NOW_PHOTO_STATE_IDLE;
    xSemaphoreGive(s_mutex);
    start_single_receive(cam_mac, PHOTO_REQUEST_MODE_BY_ID, file_id);
}

static void handle_meta(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_meta_t)) return;
    const esp_now_photo_meta_t *meta = (const esp_now_photo_meta_t *)data;
    ESP_LOGI(TAG, "PHOTO_META 수신: file_id=%u, total_size=%u, chunks=%u",
             (unsigned)meta->file_id, (unsigned)meta->total_size, (unsigned)meta->total_chunks);
    ui_log_add("META file_id=%u size=%u chunks=%u",
               (unsigned)meta->file_id, (unsigned)meta->total_size, (unsigned)meta->total_chunks);

    if (meta->total_size > s_recv_cap || !s_recv_buf) {
        ESP_LOGE(TAG, "사진이 고정 수신 버퍼보다 큼(%u > %u bytes) — 버림",
                 (unsigned)meta->total_size, (unsigned)s_recv_cap);
        ui_log_add_err(UI_ERR_META_TOO_BIG, "사진 수신 실패(용량초과) %u > %u", (unsigned)meta->total_size, (unsigned)s_recv_cap);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_ERROR;
        xSemaphoreGive(s_mutex);
        return;
    }
    s_file_id         = meta->file_id;
    s_total_size       = meta->total_size;
    s_total_chunks     = meta->total_chunks;
    s_expected_crc     = meta->crc32;
    s_chunks_received  = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = ESP_NOW_PHOTO_STATE_RECEIVING;
    xSemaphoreGive(s_mutex);
}

static void handle_chunk(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_chunk_t)) return;
    const esp_now_photo_chunk_t *chunk = (const esp_now_photo_chunk_t *)data;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_photo_state_t st = s_state;
    xSemaphoreGive(s_mutex);
    if (st != ESP_NOW_PHOTO_STATE_RECEIVING || chunk->file_id != s_file_id || !s_recv_buf) return;

    size_t offset = (size_t)chunk->chunk_idx * ESP_NOW_PHOTO_CHUNK_DATA_LEN;
    if (offset + chunk->chunk_len > s_recv_cap) return;  /* 손상된/엉뚱한 청크 — 무시 */
    memcpy(s_recv_buf + offset, chunk->data, chunk->chunk_len);
    s_chunks_received++;
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
        ui_log_add("DONE 무시(state!=RECEIVING)");
        return;
    }

    if (s_total_chunks == 0 && s_chunks_received == 0) {
        /* META가 아예 안 왔던 경우(해당 file_id가 없음 등) — 에러가 아니라 그냥
         * "보낼 게 없었다"로 조용히 종료 */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_IDLE;
        xSemaphoreGive(s_mutex);
        return;
    }

    if (s_chunks_received != s_total_chunks || s_total_size == 0 || !s_recv_buf) {
        ESP_LOGW(TAG, "청크 누락(%u/%u) — 사진 버림", s_chunks_received, s_total_chunks);
        ui_log_add_err(UI_ERR_CHUNK_MISSING, "사진 수신 실패(청크 누락 %u/%u)", s_chunks_received, s_total_chunks);
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

    ESP_LOGI(TAG, "사진 수신 완료: file_id=%u, %u bytes", (unsigned)s_file_id, (unsigned)s_total_size);
    ui_log_add("READY file_id=%u %u bytes", (unsigned)s_file_id, (unsigned)s_total_size);
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
static void handle_capture_status(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_capture_status_t)) return;
    const esp_now_capture_status_t *msg = (const esp_now_capture_status_t *)data;
    ESP_LOGI(TAG, "CAPTURE_STATUS 수신: status=%d", msg->status);

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
void esp_now_photo_list_request(const uint8_t *cam_mac)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_list_state = ESP_NOW_PHOTO_LIST_STATE_REQUESTING;
    s_list_count = 0;
    xSemaphoreGive(s_mutex);

    esp_now_photo_list_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_LIST_REQUEST,
    };
    esp_err_t err = esp_now_send(cam_mac, (const uint8_t *)&req, sizeof(req));
    ESP_LOGI(TAG, "PHOTO_LIST_REQUEST 전송: %s", esp_err_to_name(err));
    if (err != ESP_OK) ui_log_add_err(UI_ERR_SEND_LIST_REQ, "목록 요청 전송 실패: %s", esp_err_to_name(err));
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
}

static void handle_list_done(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_list_done_t)) return;
    const esp_now_photo_list_done_t *done = (const esp_now_photo_list_done_t *)data;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_list_state == ESP_NOW_PHOTO_LIST_STATE_REQUESTING) {
        s_list_state  = ESP_NOW_PHOTO_LIST_STATE_READY;
        s_sd_total_kb = done->sd_total_kb;
        s_sd_used_kb  = done->sd_used_kb;
    }
    xSemaphoreGive(s_mutex);
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
    esp_now_photo_delete_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_DELETE_REQUEST,
        .file_id  = file_id,
    };
    esp_err_t err = esp_now_send(cam_mac, (const uint8_t *)&req, sizeof(req));
    ESP_LOGI(TAG, "PHOTO_DELETE_REQUEST(id=%u) 전송: %s", (unsigned)file_id, esp_err_to_name(err));
    if (err != ESP_OK) ui_log_add_err(UI_ERR_SEND_DELETE_REQ, "삭제 요청 전송 실패: %s", esp_err_to_name(err));
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
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_delete_all_state = ESP_NOW_DELETE_ALL_STATE_REQUESTED;
    xSemaphoreGive(s_mutex);

    esp_now_photo_delete_all_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_DELETE_ALL_REQUEST,
    };
    esp_err_t err = esp_now_send(cam_mac, (const uint8_t *)&req, sizeof(req));
    ESP_LOGI(TAG, "PHOTO_DELETE_ALL_REQUEST 전송: %s", esp_err_to_name(err));
    if (err != ESP_OK) ui_log_add_err(UI_ERR_SEND_DELETE_ALL_REQ, "전체삭제 요청 전송 실패: %s", esp_err_to_name(err));
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
void esp_now_photo_on_recv(uint8_t msg_type, const uint8_t *data, int len)
{
    switch (msg_type) {
        case ESP_NOW_MSG_PHOTO_META:      handle_meta(data, len);           break;
        case ESP_NOW_MSG_PHOTO_CHUNK:      handle_chunk(data, len);          break;
        case ESP_NOW_MSG_PHOTO_DONE:       handle_done(data, len);           break;
        case ESP_NOW_MSG_CAPTURE_STATUS:   handle_capture_status(data, len); break;
        case ESP_NOW_MSG_PHOTO_LIST_ENTRY: handle_list_entry(data, len);     break;
        case ESP_NOW_MSG_PHOTO_LIST_DONE:  handle_list_done(data, len);      break;
        case ESP_NOW_MSG_PHOTO_DELETE_ACK: handle_delete_ack(data, len);     break;
        case ESP_NOW_MSG_PHOTO_DELETE_ALL_ACK: handle_delete_all_ack(data, len); break;
        default: break;
    }
}
