#include "photo_rx.h"
#include "esp_now_link.h"
#include "node_hub.h"
#include "node_request.h"
#include "can_bridge.h"
#include "ui_log.h"
#include "device_config.h"
#include "photo_storage.h"
#include "can_bridge_link.h"
#include "esp_mac.h"

#include <string.h>
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
 * 1. 사진 수신 — 브가 보내는 순서 맞춘 SR 스트림(설계 §4, 4단계)
 * ────────────────────────────────────────────────────────────
 * 2026-09-26 — 예전엔 콘이 SR의 끝점(META/청크/윈도 상태/DONE을 직접 처리, 사진 전체를 PSRAM 1MB
 * 버퍼에 모은 뒤 DONE 때 SD에 한 번에 저장)이었음. 이제 브가 SR 끝점이고, 콘은 브가 순서를 맞춰 흘려주는
 * CAN_DATA_SR_META/CHUNK/DONE만 받음 — 캠별 세션(두 캠이 동시에 보내도 섞이지 않음), 받는 대로 SD 임시파일에
 * 이어 쓰기(SR_WRITE_BLOCK씩 모아서) + CRC 누적, DONE에서 검증 후 최종 이름으로. PSRAM 사진 캐시는 없앰
 * (볼 일이 드물어 필요할 때 SD에서 읽음 — 사용자 결정) */
#define SR_SESSIONS     4
#define SR_WRITE_BLOCK  (16 * 1024)   /* SD 쓰기 단위 — 섹터(512B) 배수, 청크(1200B)마다 쓰지 않게 모아서 */

typedef struct {
    bool     active;
    uint8_t  mac[6];
    uint8_t  kind;
    uint32_t file_id;
    uint32_t total_size;
    uint16_t total_chunks;
    uint16_t next_idx;
    uint32_t expected_crc;
    uint32_t crc;
    uint32_t received;
    photo_storage_writer_t *writer;   /* NULL이면 저장 불가(SD 미마운트 등) — 스트림은 끝까지 받고 실패 처리 */
    uint8_t *wbuf;                    /* SR_WRITE_BLOCK, 부팅 때 1회 할당(PSRAM) — 세션을 비워도 유지 */
    size_t   wlen;
} sr_session_t;
static sr_session_t s_sessions[SR_SESSIONS];

/* UI 통지용 — 마지막 수신 결과(여러 캠이 받아도 "방금 한 장 끝남/실패"만 알리면 됨, ui_main.c 참고) */
static volatile photo_rx_state_t s_state = PHOTO_RX_STATE_IDLE;

/* ────────────────────────────────────────────────────────────
 * 2. 지금촬영 진행 단계
 * ──────────────────────────────────────────────────────────── */
static volatile esp_now_capture_stage_t s_capture_stage = PHOTO_RX_CAPTURE_STAGE_NONE;
/* 2026-08-26(사용자 지시) — 어느 mac을 대상으로 진행 중인지 기록(사진 수신 세션의 mac과 동일
 * 이유). 없으면 여러 기기가 붙어있을 때 캠1 지금촬영 중에 캠2까지 "통신 중"으로 오판해서
 * 불필요하게 안 재우는 버그가 됨(photo_rx_is_transacting_with 참고) */
static uint8_t s_capture_cam_mac[6] = { 0 };


void photo_rx_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_photo_event_sem = xSemaphoreCreateBinary();
    for (int i = 0; i < SR_SESSIONS; i++) {
        s_sessions[i].wbuf = heap_caps_malloc(SR_WRITE_BLOCK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_sessions[i].wbuf) {
            ESP_LOGE(TAG, "SR 쓰기 버퍼[%d] 할당 실패(%u bytes)", i, (unsigned)SR_WRITE_BLOCK);
            ui_log_add_err(UI_ERR_RECV_BUF_ALLOC, "Recv buffer alloc failed - cannot receive photos");
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

/* 호출부가 s_mutex를 쥔 상태 */
static sr_session_t *find_session_locked(const uint8_t *mac)
{
    for (int i = 0; i < SR_SESSIONS; i++) {
        if (s_sessions[i].active && memcmp(s_sessions[i].mac, mac, 6) == 0) return &s_sessions[i];
    }
    return NULL;
}

static bool flush_wbuf(sr_session_t *s)
{
    if (s->wlen == 0) return true;
    bool ok = s->writer && photo_storage_append(s->writer, s->wbuf, s->wlen);
    s->wlen = 0;
    return ok;
}

/* 세션을 끝냄 — 남은 저장기는 abort(임시파일 삭제). 호출부가 s_mutex를 쥔 상태 */
static void drop_session_locked(sr_session_t *s)
{
    if (s->writer) photo_storage_abort(s->writer);
    s->writer = NULL;
    s->wlen = 0;
    s->active = false;
}

static void finish_event(photo_rx_state_t st)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = st;
    xSemaphoreGive(s_mutex);
    fire_photo_event();
}

/* META/CHUNK/DONE은 모두 Data 경로라 data_consume 태스크 하나만 부름 — 세션 필드는 그 태스크만 바꾸고,
 * s_mutex는 다른 태스크가 보는 active/mac(photo_rx_is_transacting_with)과 세션 할당에만 씀 */
static void handle_sr_meta(const uint8_t *mac, const uint8_t *body, size_t len)
{
    if (len < sizeof(can_bridge_sr_meta_t)) return;
    can_bridge_sr_meta_t meta;
    memcpy(&meta, body, sizeof(meta));
    ESP_LOGI(TAG, "SR_META " MACSTR ": file_id=%u size=%u chunks=%u kind=%c", MAC2STR(mac),
             (unsigned)meta.file_id, (unsigned)meta.total_size, (unsigned)meta.total_chunks, (char)meta.kind);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sr_session_t *s = find_session_locked(mac);
    if (s) {
        ESP_LOGW(TAG, "SR_META: 이전 사진(file_id=%u) 미완료 — 버림", (unsigned)s->file_id);
        drop_session_locked(s);
    } else {
        for (int i = 0; i < SR_SESSIONS && !s; i++) {
            if (!s_sessions[i].active) s = &s_sessions[i];
        }
    }
    if (!s || !s->wbuf) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "SR_META: 수신 세션 없음(동시 %d개 초과 또는 버퍼 없음) — 버림", SR_SESSIONS);
        return;
    }
    s->active       = true;
    memcpy(s->mac, mac, 6);
    s->kind         = meta.kind;
    s->file_id      = meta.file_id;
    s->total_size   = meta.total_size;
    s->total_chunks = meta.total_chunks;
    s->next_idx     = 0;
    s->expected_crc = meta.crc32;
    s->crc          = 0;
    s->received     = 0;
    s->writer       = NULL;
    s->wlen         = 0;
    xSemaphoreGive(s_mutex);

    /* SD 열기는 잠금 밖에서(수 ms~) */
    s->writer = photo_storage_begin(mac, meta.kind);
    if (!s->writer) ui_log_add_err(UI_ERR_SD_MOUNT_FAILED, "Photo SD save failed file_id=%u", (unsigned)meta.file_id);
    ui_log_add("META file_id=%u size=%u chunks=%u", (unsigned)meta.file_id, (unsigned)meta.total_size, (unsigned)meta.total_chunks);
}

static void handle_sr_chunk(const uint8_t *mac, const uint8_t *body, size_t len)
{
    if (len < sizeof(can_bridge_sr_chunk_hdr_t)) return;
    can_bridge_sr_chunk_hdr_t h;
    memcpy(&h, body, sizeof(h));
    const uint8_t *data = body + sizeof(h);
    if (len - sizeof(h) < h.len) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sr_session_t *s = find_session_locked(mac);
    xSemaphoreGive(s_mutex);
    if (!s || h.file_id != s->file_id) return;
    if (h.chunk_idx != s->next_idx || s->received + h.len > s->total_size || h.len > SR_WRITE_BLOCK) {
        /* 브가 순서를 보장하므로 여기 오면 스트림이 깨진 것 — 이 사진은 버림 */
        ESP_LOGW(TAG, "SR_CHUNK 순서/크기 어긋남(받은 %u, 기대 %u) — 사진 버림", h.chunk_idx, s->next_idx);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        drop_session_locked(s);
        xSemaphoreGive(s_mutex);
        ui_log_add_err(UI_ERR_CHUNK_MISSING, "Photo receive failed (stream order) file_id=%u", (unsigned)h.file_id);
        finish_event(PHOTO_RX_STATE_ERROR);
        return;
    }
    if (s->wlen + h.len > SR_WRITE_BLOCK) flush_wbuf(s);
    memcpy(s->wbuf + s->wlen, data, h.len);
    s->wlen     += h.len;
    s->crc       = esp_rom_crc32_le(s->crc, data, h.len);
    s->received += h.len;
    s->next_idx++;

    /* 2026-08-10 — 적응형 반응시간의 "마지막 사용자 조작"을 청크마다 갱신('T' 주기촬영은 CAM 자율 전송이라
     * 제외 — 안 그러면 캠이 영영 못 잠) */
    if (s->kind != 'T') node_hub_note_user_action();
}

static void handle_sr_done(const uint8_t *mac, const uint8_t *body, size_t len)
{
    if (len < sizeof(can_bridge_sr_done_t)) return;
    can_bridge_sr_done_t d;
    memcpy(&d, body, sizeof(d));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sr_session_t *s = find_session_locked(mac);
    xSemaphoreGive(s_mutex);
    if (!s || d.file_id != s->file_id) return;

    bool complete = (d.status == CAN_BRIDGE_SR_DONE_COMPLETE && s->next_idx == s->total_chunks &&
                     s->received == s->total_size);
    bool crc_ok = complete && (s->crc == s->expected_crc);
    uint32_t seq = 0;
    bool saved = false;
    if (crc_ok && s->writer && flush_wbuf(s)) {
        saved = photo_storage_finish(s->writer, &seq);  /* 성공/실패 모두 writer 해제 */
        s->writer = NULL;
    }
    uint32_t file_id = s->file_id, size = s->total_size;
    uint16_t got = s->next_idx, total = s->total_chunks;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    drop_session_locked(s);   /* 실패 경로에 남은 writer는 abort */
    xSemaphoreGive(s_mutex);

    if (saved) {
        ESP_LOGI(TAG, "사진 수신 완료: " MACSTR " file_id=%u, %u bytes -> seq=%u", MAC2STR(mac),
                 (unsigned)file_id, (unsigned)size, (unsigned)seq);
        ui_log_add("READY file_id=%u %u bytes seq=%u", (unsigned)file_id, (unsigned)size, (unsigned)seq);
        finish_event(PHOTO_RX_STATE_READY);
        return;
    }
    if (d.status != CAN_BRIDGE_SR_DONE_COMPLETE) {
        ESP_LOGW(TAG, "사진 전송 중단(브 통지) file_id=%u", (unsigned)file_id);
        ui_log_add_err(UI_ERR_CHUNK_MISSING, "Photo receive failed (transfer aborted) file_id=%u", (unsigned)file_id);
    } else if (!complete) {
        ESP_LOGW(TAG, "사진 불완전(%u/%u 청크) file_id=%u", (unsigned)got, (unsigned)total, (unsigned)file_id);
        ui_log_add_err(UI_ERR_CHUNK_MISSING, "Photo receive failed (incomplete) file_id=%u", (unsigned)file_id);
    } else if (!crc_ok) {
        ESP_LOGW(TAG, "CRC 불일치 — 사진 버림 file_id=%u", (unsigned)file_id);
        ui_log_add_err(UI_ERR_CRC_MISMATCH, "Photo receive failed (CRC mismatch) file_id=%u", (unsigned)file_id);
    } else {
        ui_log_add_err(UI_ERR_SD_MOUNT_FAILED, "Photo SD save failed file_id=%u", (unsigned)file_id);
    }
    finish_event(PHOTO_RX_STATE_ERROR);
}

void photo_rx_on_sr_stream(uint8_t app_type, const uint8_t *mac, const uint8_t *body, size_t len)
{
    switch (app_type) {
        case CAN_DATA_SR_META:  handle_sr_meta(mac, body, len);  break;
        case CAN_DATA_SR_CHUNK: handle_sr_chunk(mac, body, len); break;
        case CAN_DATA_SR_DONE:  handle_sr_done(mac, body, len);  break;
        default: break;
    }
}

photo_rx_state_t photo_rx_get_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    photo_rx_state_t st = s_state;
    xSemaphoreGive(s_mutex);
    return st;
}

void photo_rx_ready_ack(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == PHOTO_RX_STATE_READY) s_state = PHOTO_RX_STATE_IDLE;
    xSemaphoreGive(s_mutex);
}

void photo_rx_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == PHOTO_RX_STATE_ERROR) s_state = PHOTO_RX_STATE_IDLE;
    xSemaphoreGive(s_mutex);
}

void photo_rx_set_ready_cb(photo_rx_event_cb_t cb)
{
    s_photo_ready_cb = cb;
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
 * 트랜잭션마다 대상 mac을 저장해두므로(사진 세션 mac/s_capture_cam_mac) 여러 기기가 동시에
 * 붙어있어도 각자 자기 것만 "통신 중"으로 정확히 매칭됨 — 다른 기기 것 때문에 잘못 안 재우는
 * 일이 없음. 2026-09-26 — 캠 SD 제거로 목록/전체삭제 트랜잭션은 삭제됨(사진 수신+지금촬영만 남음) */
bool photo_rx_is_transacting_with(const uint8_t *mac)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = (find_session_locked(mac) != NULL)
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
    /* 2026-09-26(4단계) — SR 7종은 브가 끝점이라 콘으로 안 옴(사진은 photo_rx_on_sr_stream으로) */
    if (msg_type == ESP_NOW_MSG_CAPTURE_STATUS) handle_capture_status(src_mac, data, len);
}
