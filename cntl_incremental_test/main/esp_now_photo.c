#include "esp_now_photo.h"
#include "esp_now_link.h"

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
static uint8_t  *s_recv_buf = NULL;   /* ESP-NOW 태스크만 건드림, 밖으로 포인터가 안 나감 */
static size_t     s_recv_cap = 0;
static uint32_t   s_file_id = 0;
static uint32_t   s_total_size = 0;
static uint16_t   s_total_chunks = 0;
static uint32_t   s_expected_crc = 0;
static uint16_t   s_chunks_received = 0;

static uint8_t *s_ready_buf = NULL;   /* 검증 완료본 — consume()으로 소유권 이전 */
static size_t    s_ready_len = 0;

static volatile esp_now_photo_state_t s_state = ESP_NOW_PHOTO_STATE_IDLE;

/* 지금 진행 중인 단일 수신이 지금촬영 흐름인지(true) 목록에서 고른 단순 조회인지(false) —
 * true일 때만 아래 capture_stage를 같이 갱신함 */
static bool s_is_capture_flow = false;

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

void esp_now_photo_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
}

/* ════════════════════════════════════════════════════════════
 * 단일 사진 수신 — 내부 공용 로직
 * ════════════════════════════════════════════════════════════ */
static void start_single_receive(const uint8_t *cam_mac, uint8_t mode, uint32_t param, bool is_capture_flow)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == ESP_NOW_PHOTO_STATE_RECEIVING) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "이미 수신 중 — 새 요청 무시");
        return;
    }
    s_state = ESP_NOW_PHOTO_STATE_RECEIVING;
    s_is_capture_flow = is_capture_flow;
    xSemaphoreGive(s_mutex);

    esp_now_photo_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_REQUEST,
        .mode     = mode,
        .param    = param,
    };
    esp_err_t err = esp_now_send(cam_mac, (const uint8_t *)&req, sizeof(req));
    ESP_LOGI(TAG, "PHOTO_REQUEST(mode=%d) 전송: %s", mode, esp_err_to_name(err));
}

void esp_now_photo_capture_now(const uint8_t *cam_mac)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capture_stage = ESP_NOW_CAPTURE_STAGE_SENT;
    xSemaphoreGive(s_mutex);
    start_single_receive(cam_mac, PHOTO_REQUEST_MODE_CAPTURE_NOW, 0, true);
}

void esp_now_photo_fetch_by_id(const uint8_t *cam_mac, uint32_t file_id)
{
    start_single_receive(cam_mac, PHOTO_REQUEST_MODE_BY_ID, file_id, false);
}

static void handle_meta(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_meta_t)) return;
    const esp_now_photo_meta_t *meta = (const esp_now_photo_meta_t *)data;

    if (meta->total_size > s_recv_cap) {
        uint8_t *new_buf = heap_caps_malloc(meta->total_size, MALLOC_CAP_SPIRAM);
        if (!new_buf) {
            ESP_LOGE(TAG, "수신 버퍼 할당 실패(%u bytes)", (unsigned)meta->total_size);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_state = ESP_NOW_PHOTO_STATE_ERROR;
            if (s_is_capture_flow) s_capture_stage = ESP_NOW_CAPTURE_STAGE_TRANSFER_FAILED;
            xSemaphoreGive(s_mutex);
            return;
        }
        if (s_recv_buf) heap_caps_free(s_recv_buf);
        s_recv_buf = new_buf;
        s_recv_cap = meta->total_size;
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
    if (st != ESP_NOW_PHOTO_STATE_RECEIVING) return;

    if (s_total_chunks == 0 && s_chunks_received == 0) {
        /* META가 아예 안 왔던 경우 — 지금촬영이면 촬영 실패로 CAPTURE_STATUS가 이미
         * 따로 알려줬을 것이므로 에러가 아니라 그냥 "보낼 게 없었다"로 조용히 종료 */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_IDLE;
        xSemaphoreGive(s_mutex);
        return;
    }

    if (s_chunks_received != s_total_chunks || s_total_size == 0 || !s_recv_buf) {
        ESP_LOGW(TAG, "청크 누락(%u/%u) — 사진 버림", s_chunks_received, s_total_chunks);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_ERROR;
        if (s_is_capture_flow) s_capture_stage = ESP_NOW_CAPTURE_STAGE_TRANSFER_FAILED;
        xSemaphoreGive(s_mutex);
        return;
    }

    uint32_t crc = esp_rom_crc32_le(0, s_recv_buf, s_total_size);
    if (crc != s_expected_crc) {
        ESP_LOGW(TAG, "CRC 불일치 — 사진 버림(재조립 실패)");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_ERROR;
        if (s_is_capture_flow) s_capture_stage = ESP_NOW_CAPTURE_STAGE_TRANSFER_FAILED;
        xSemaphoreGive(s_mutex);
        return;
    }

    /* 검증된 사진을 별도 버퍼로 복사 — UI가 아직 이전 사진(예전에 consume()해 간 버퍼)을
     * 화면에 그리고 있을 수 있으니, 이번에 받은 s_recv_buf는 다음 요청 때 그냥
     * 재할당/재사용해도 안전하도록 분리해둠 */
    uint8_t *copy = heap_caps_malloc(s_total_size, MALLOC_CAP_SPIRAM);
    if (!copy) {
        ESP_LOGE(TAG, "완료본 버퍼 할당 실패");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_ERROR;
        if (s_is_capture_flow) s_capture_stage = ESP_NOW_CAPTURE_STAGE_TRANSFER_FAILED;
        xSemaphoreGive(s_mutex);
        return;
    }
    memcpy(copy, s_recv_buf, s_total_size);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_ready_buf) {
        heap_caps_free(s_ready_buf);  /* UI가 아직 이전 READY본을 안 가져갔으면 버림 */
    }
    s_ready_buf = copy;
    s_ready_len = s_total_size;
    s_state = ESP_NOW_PHOTO_STATE_READY;
    if (s_is_capture_flow) s_capture_stage = ESP_NOW_CAPTURE_STAGE_TRANSFER_DONE;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "사진 수신 완료: %u bytes", (unsigned)s_total_size);
}

esp_now_photo_state_t esp_now_photo_get_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_photo_state_t st = s_state;
    xSemaphoreGive(s_mutex);
    return st;
}

bool esp_now_photo_consume(const uint8_t **out_data, size_t *out_len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = (s_state == ESP_NOW_PHOTO_STATE_READY && s_ready_buf != NULL);
    if (ok) {
        *out_data   = s_ready_buf;
        *out_len    = s_ready_len;
        s_ready_buf = NULL;
        s_ready_len = 0;
        s_state     = ESP_NOW_PHOTO_STATE_IDLE;
    }
    xSemaphoreGive(s_mutex);
    return ok;
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
}

static void handle_list_entry(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_list_entry_t)) return;
    const esp_now_photo_list_entry_t *entry = (const esp_now_photo_list_entry_t *)data;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_list_state == ESP_NOW_PHOTO_LIST_STATE_REQUESTING && s_list_count < ESP_NOW_PHOTO_LIST_MAX) {
        s_list_items[s_list_count].file_id   = entry->file_id;
        s_list_items[s_list_count].file_size = entry->file_size;
        s_list_count++;
    }
    xSemaphoreGive(s_mutex);
}

static void handle_list_done(const uint8_t *data, int len)
{
    (void)data; (void)len;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_list_state == ESP_NOW_PHOTO_LIST_STATE_REQUESTING) {
        s_list_state = ESP_NOW_PHOTO_LIST_STATE_READY;
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

void esp_now_photo_delete(const uint8_t *cam_mac, uint32_t file_id)
{
    esp_now_photo_delete_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_DELETE_REQUEST,
        .file_id  = file_id,
    };
    esp_err_t err = esp_now_send(cam_mac, (const uint8_t *)&req, sizeof(req));
    ESP_LOGI(TAG, "PHOTO_DELETE_REQUEST(id=%u) 전송: %s", (unsigned)file_id, esp_err_to_name(err));
}

static void handle_delete_ack(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_delete_ack_t)) return;
    const esp_now_photo_delete_ack_t *ack = (const esp_now_photo_delete_ack_t *)data;
    ESP_LOGI(TAG, "PHOTO_DELETE_ACK id=%u: %s", (unsigned)ack->file_id, ack->success ? "성공" : "실패");
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
        default: break;
    }
}
