#include "photo_rx.h"
#include "esp_now_link.h"
#include "node_hub.h"
#include "node_request.h"
#include "can_bridge.h"
#include "ui_log.h"
#include "device_config.h"
#include "photo_storage.h"
#include "sd_health.h"
#include "sd_storage.h"
#include "can_bridge_link.h"
#include "esp_mac.h"

#include <string.h>
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "PHOTO";

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

/* ────────────────────────────────────────────────────────────
 * 2026-10-20(사용자 결정 — 사진 버퍼 재설계) — 캠별 사진 수신 버퍼
 * ────────────────────────────────────────────────────────────
 * 예전엔 사진마다 128KB로 시작해 256K→512K로 재할당했는데, ESP-IDF 힙은 재할당을 잠금(크리티컬 섹션) 안에서 하고 블록을
 * 옮길 때 내용을 그 안에서 복사함 → 수백 KB 복사 동안 인터럽트가 막혀 콘 CAN 수신 FIFO가 넘쳤음(10-19 청크 222 손실).
 * 이제 캠마다 버퍼를 하나 미리 잡고 받는 동안은 할당이 없음. 크기는 해상도로 정함:
 *   해상도 단계(UXGA → SXGA → XGA → SVGA) 중 모든 캠이 같은 단계(센서 최대를 넘지 않게)일 때 버퍼 합이 PSRAM에 들어가는
 *   가장 높은 단계. 남길 PSRAM(CAMBUF_PSRAM_RESERVE)은 사진 보기·통계·웹이 최악에 쓰는 양.
 *   버퍼 = 가로×세로×2바이트 ÷ 16(q=12 최악 실측 약 1/19에 여유) — UXGA 약 240KB.
 * 캠은 PAIR_ACK로 센서 PID·최대 해상도를 알려 주고, 콘은 정한 해상도를 CAM_CONFIG_SET.frame_size로 보냄. 정보를 안 보내는 옛
 * 펌웨어 캠은 해상도를 정해 줄 수 없어 UXGA 크기 버퍼만 잡음(그 캠이 더 크게 찍으면 거절됨). 버퍼보다 큰 사진은 받지 않음 */
#define CAMBUF_MAX            4
#define CAMBUF_PSRAM_RESERVE  (512 * 1024)
typedef struct {
    bool     used;
    uint8_t  mac[6];
    uint8_t  max_fs;        /* 센서 최대(framesize_t), 0 = 모름(옛 펌웨어) */
    uint8_t  fs;            /* 콘이 정한 해상도, ESP_NOW_CAM_FRAME_SIZE_DEFAULT = 정해 줄 수 없음 */
    uint8_t *buf;
    size_t   cap;
    size_t   want_cap;      /* 받는 중이라 아직 못 바꾼 새 크기(0 = 없음) */
} cam_buf_t;
static cam_buf_t s_cambufs[CAMBUF_MAX];
static const struct { uint8_t fs; uint16_t w, h; } s_res_ladder[] = {
    { 15, 1600, 1200 },   /* UXGA */
    { 14, 1280, 1024 },   /* SXGA */
    { 12, 1024,  768 },   /* XGA */
    { 11,  800,  600 },   /* SVGA */
};
#define RES_LEVELS ((int)(sizeof(s_res_ladder) / sizeof(s_res_ladder[0])))

static size_t res_buf_bytes(int level)
{
    size_t b = (size_t)s_res_ladder[level].w * s_res_ladder[level].h * 2 / 16;
    return (b + 4095) & ~(size_t)4095;
}

/* 단계 level에서 이 캠이 쓸 단계 — 센서 최대보다 높으면 센서 최대 이하의 첫 단계 */
static int cam_level(const cam_buf_t *c, int level)
{
    for (int l = level; l < RES_LEVELS; l++) {
        if (c->max_fs == 0 || s_res_ladder[l].fs <= c->max_fs) return l;
    }
    return RES_LEVELS - 1;
}

static bool session_active_for_locked(const uint8_t *mac);

/* 캠 버퍼 크기를 실제로 바꿈(받는 중이면 끝난 뒤로 미룸). 호출부가 s_mutex를 쥔 상태 */
static void cambuf_apply_locked(cam_buf_t *c, size_t cap)
{
    if (c->cap == cap && c->buf) { c->want_cap = 0; return; }
    if (c->buf && session_active_for_locked(c->mac)) { c->want_cap = cap; return; }   /* 버퍼가 없으면 쓰는 중일 수 없음 */
    heap_caps_free(c->buf);
    c->buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    c->cap = c->buf ? cap : 0;
    c->want_cap = 0;
    if (!c->buf) {
        ESP_LOGE(TAG, "Photo buffer alloc failed for " MACSTR " (%u bytes) - photos from this cam rejected", MAC2STR(c->mac), (unsigned)cap);
        ui_log_add_err(UI_ERR_RECV_BUF_ALLOC, "Recv buffer alloc failed - cannot receive photos");
    }
}

/* 모든 캠의 해상도·버퍼를 다시 정함. 호출부가 s_mutex를 쥔 상태 */
static void cambuf_plan_locked(void)
{
    size_t held = 0;
    for (int i = 0; i < CAMBUF_MAX; i++) if (s_cambufs[i].used) held += s_cambufs[i].cap;
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t budget = (free_now + held > CAMBUF_PSRAM_RESERVE) ? free_now + held - CAMBUF_PSRAM_RESERVE : 0;
    int level = RES_LEVELS - 1;
    for (int l = 0; l < RES_LEVELS; l++) {
        size_t need = 0;
        for (int i = 0; i < CAMBUF_MAX; i++) if (s_cambufs[i].used) need += res_buf_bytes(cam_level(&s_cambufs[i], l));
        if (need <= budget) { level = l; break; }
    }
    for (int i = 0; i < CAMBUF_MAX; i++) {
        cam_buf_t *c = &s_cambufs[i];
        if (!c->used) continue;
        int cl = cam_level(c, level);
        c->fs = c->max_fs ? s_res_ladder[cl].fs : ESP_NOW_CAM_FRAME_SIZE_DEFAULT;
        cambuf_apply_locked(c, res_buf_bytes(cl));
        ESP_LOGI(TAG, "Photo buffer " MACSTR ": res %d (%ux%u), %u bytes%s (PSRAM free %u, budget %u)", MAC2STR(c->mac),
                 (int)s_res_ladder[cl].fs, s_res_ladder[cl].w, s_res_ladder[cl].h, (unsigned)res_buf_bytes(cl),
                 c->max_fs ? "" : " [cam sends no info - res not controlled]",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)budget);
    }
}

static cam_buf_t *cambuf_find_locked(const uint8_t *mac)
{
    for (int i = 0; i < CAMBUF_MAX; i++) if (s_cambufs[i].used && memcmp(s_cambufs[i].mac, mac, 6) == 0) return &s_cambufs[i];
    return NULL;
}

/* 없으면 만듦(max_fs 0 = 모름). 자리가 없으면 NULL. 호출부가 s_mutex를 쥔 상태 */
static cam_buf_t *cambuf_get_locked(const uint8_t *mac, bool *created)
{
    *created = false;
    cam_buf_t *c = cambuf_find_locked(mac);
    if (c) return c;
    for (int i = 0; i < CAMBUF_MAX; i++) {
        if (!s_cambufs[i].used) {
            c = &s_cambufs[i];
            memset(c, 0, sizeof(*c));
            c->used = true;
            memcpy(c->mac, mac, 6);
            c->fs = ESP_NOW_CAM_FRAME_SIZE_DEFAULT;
            *created = true;
            return c;
        }
    }
    return NULL;
}

static void capw_on_pair(const uint8_t *mac);          /* 아래(주기 촬영 실패 감시) */
static void capw_note_auto_photo(const uint8_t *mac);

void photo_rx_cam_info(const uint8_t *mac, uint16_t pid, uint8_t max_fs)
{
    capw_on_pair(mac);   /* 2026-10-22 — 연결된 캠부터 주기 촬영 감시(기준 = 지금) */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool created = false;
    cam_buf_t *c = cambuf_get_locked(mac, &created);
    if (c && (created || c->max_fs != max_fs)) {
        ESP_LOGI(TAG, "Cam " MACSTR " reports sensor PID 0x%04x, max res %u", MAC2STR(mac), (unsigned)pid, (unsigned)max_fs);
        c->max_fs = max_fs;
        cambuf_plan_locked();
    }
    xSemaphoreGive(s_mutex);
}

uint8_t photo_rx_cam_frame_size(const uint8_t *mac)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cam_buf_t *c = cambuf_find_locked(mac);
    uint8_t fs = c ? c->fs : ESP_NOW_CAM_FRAME_SIZE_DEFAULT;
    xSemaphoreGive(s_mutex);
    return fs;
}

void photo_rx_cam_forget(const uint8_t *mac)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cam_buf_t *c = cambuf_find_locked(mac);
    if (c && !session_active_for_locked(mac)) {
        heap_caps_free(c->buf);
        memset(c, 0, sizeof(*c));
        ESP_LOGI(TAG, "Photo buffer for " MACSTR " released - replanning", MAC2STR(mac));
        cambuf_plan_locked();
    }
    xSemaphoreGive(s_mutex);
}

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
            ESP_LOGE(TAG, "SR write buffer[%d] alloc failed (%u bytes)", i, (unsigned)SR_WRITE_BLOCK);
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
    ESP_LOGD(TAG, "PHOTO_REQUEST(mode=CAPTURE_NOW) queued");
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
    /* 2026-10-20 — 받는 동안 미뤄 둔 버퍼 크기 변경을 지금 적용(cambuf_apply_locked 참고) */
    cam_buf_t *c = cambuf_find_locked(s->mac);
    if (c && c->want_cap) cambuf_apply_locked(c, c->want_cap);
}

static bool session_active_for_locked(const uint8_t *mac)
{
    return find_session_locked(mac) != NULL;
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
    ESP_LOGD(TAG, "SR_META " MACSTR ": file_id=%u size=%u chunks=%u kind=%c", MAC2STR(mac),
             (unsigned)meta.file_id, (unsigned)meta.total_size, (unsigned)meta.total_chunks, (char)meta.kind);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sr_session_t *s = find_session_locked(mac);
    if (s && s->file_id == meta.file_id && s->next_idx == 0 && s->total_size == meta.total_size &&
        s->expected_crc == meta.crc32) {
        /* 브의 재전송으로 같은 META가 다시 옴(아직 청크 전) — 무시 */
        xSemaphoreGive(s_mutex);
        return;
    }
    if (s) {
        /* 새 META = 새 사진이 이전 사진을 대체(정상 소거, 2026-09-27 사용자 정의) — 보통은 브의 SUPERSEDED DONE이 먼저
         * 와서 여기 올 일이 없음 */
        ESP_LOGD(TAG, "SR_META: previous photo (file_id=%u) superseded - dropped", (unsigned)s->file_id);
        drop_session_locked(s);
    } else {
        for (int i = 0; i < SR_SESSIONS && !s; i++) {
            if (!s_sessions[i].active) s = &s_sessions[i];
        }
    }
    if (!s || !s->wbuf) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "SR_META: no receive session (>%d concurrent or no buffer) - dropped", SR_SESSIONS);
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
    /* 2026-10-20(사용자 결정 — 사진 버퍼 재설계) — 이 캠의 고정 버퍼(처음 보는 캠이면 지금 만듦). 사진이 버퍼보다 크면 받지 않음 */
    bool cb_created = false;
    cam_buf_t *cb = cambuf_get_locked(mac, &cb_created);
    if (cb && cb_created) cambuf_plan_locked();
    uint8_t *cb_buf = cb ? cb->buf : NULL;
    size_t cb_cap = cb ? cb->cap : 0;
    if (!cb_buf || meta.total_size > cb_cap) {
        drop_session_locked(s);
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "SR_META " MACSTR ": photo %u bytes > buffer %u - not received (cam resolution larger than planned?)",
                 MAC2STR(mac), (unsigned)meta.total_size, (unsigned)cb_cap);
        ui_log_add_err(UI_ERR_CHUNK_MISSING, "Photo receive failed (larger than buffer %u) file_id=%u", (unsigned)cb_cap, (unsigned)meta.file_id);
        return;
    }
    xSemaphoreGive(s_mutex);

    /* SD 열기는 잠금 밖에서(수 ms~) */
    s->writer = photo_storage_begin_ext(mac, meta.kind, cb_buf, cb_cap);
    /* 2026-10-14 — SD 고장 상태로 거부한 것이면 이미 5009·푸시로 알렸으니 엉뚱한 5008(마운트 실패)을 또 올리지 않음 */
    if (!s->writer && sd_health_writes_allowed() && !sd_storage_is_ejected()) ui_log_add_err(UI_ERR_SD_MOUNT_FAILED, "Photo SD save failed file_id=%u", (unsigned)meta.file_id);   /* 10-23 — SD를 꺼낸 동안은 알리지 않음 */
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
    /* 2026-09-26 — 브는 메시지 프레임이 버려지면 메시지 전체를 다시 보냄(can_bridge_send) → 이미 받은 청크가 다시 올 수
     * 있음. 중복은 무시 */
    if (h.chunk_idx < s->next_idx) return;
    if (h.chunk_idx != s->next_idx || s->received + h.len > s->total_size || h.len > SR_WRITE_BLOCK) {
        /* 브가 순서를 보장하므로 여기 오면 스트림이 깨진 것 — 이 사진은 버림 */
        ESP_LOGW(TAG, "SR_CHUNK order/size mismatch (got %u, expected %u) - photo dropped", h.chunk_idx, s->next_idx);
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

/* 2026-10-26(사용자 지시 — 오류 때 경과를 못 잡는 일 반복) — 캠이 실패 알림에 실어 보낸 전송 경과(캠별 마지막 것). 브의 중단·실패
 * 알림이 오면 그 글에 함께 붙임(캠 실패 알림이 먼저 옴 — 캠은 0.6초, 브 중단은 10초 뒤) */
#define CAMDIAG_SLOTS 4
static struct { bool valid; uint8_t mac[6]; int64_t us; esp_now_photo_tx_diag_t d; } s_camdiag[CAMDIAG_SLOTS];

static void camdiag_store(const uint8_t *mac, const esp_now_photo_tx_diag_t *d)
{
    int slot = 0;
    for (int i = 0; i < CAMDIAG_SLOTS; i++) {
        if (s_camdiag[i].valid && memcmp(s_camdiag[i].mac, mac, 6) == 0) { slot = i; break; }
        if (!s_camdiag[i].valid) slot = i;
    }
    s_camdiag[slot].valid = true;
    memcpy(s_camdiag[slot].mac, mac, 6);
    s_camdiag[slot].us = esp_timer_get_time();
    s_camdiag[slot].d = *d;
}

/* out에 경과 글 — 알림 글(144자)에 들어가게 짧게. 없으면 빈 글
 *  br<보낸 순서 조각>/<전체> r<받은 조각 프레임> d<중복> f<보관 범위 밖> w<창 상태 요청> m<알려 준 빠진 조각 합> D<DONE 받음>
 *    M<META 재전송> i<마지막 수신 뒤 ms>
 *  cam e<끝 이유 cam_tx_end_t> M<META 응답 받음> s<보낸 조각>/<전체> x<송신 거절>(<마지막 코드>) p<속도 조절 시간 초과>
 *    a<무선 수신 확인 실패>/<성공> w<창 상태 응답>/<요청> m<빠졌다고 들은 합> D<DONE 응답>/<보냄> L<마지막 빠진 수> h<허브에서 받은 프레임> t<ms> */
static void diag_text(const uint8_t *mac, const can_bridge_sr_rx_diag_t *br, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    if (br) {
        n += snprintf(out + n, cap - n, " br%u/%u r%u d%u f%u w%u m%u D%u M%u i%u",
                      br->next_idx, br->total, br->chunk_rx, br->chunk_dup, br->chunk_far, br->ws_rx, br->ws_missing,
                      br->done_rx, br->meta_resend, br->idle_ms);
    }
    for (int i = 0; i < CAMDIAG_SLOTS && n < cap; i++) {
        if (!s_camdiag[i].valid || memcmp(s_camdiag[i].mac, mac, 6) != 0) continue;
        if (esp_timer_get_time() - s_camdiag[i].us > 30000000LL) break;   /* 30초 넘은 건 다른 전송 */
        const esp_now_photo_tx_diag_t *c = &s_camdiag[i].d;
        snprintf(out + n, cap - n, " cam e%u M%u s%u/%u x%u(%d) p%u a%u/%u w%u/%u m%u D%u/%u L%u h%u t%u",
                 c->end, c->meta_ok, c->sent, c->total, c->send_err, c->last_err, c->pace_to, c->mac_fail, c->mac_ok,
                 c->ws_ack, c->ws_req, c->ws_missing, c->done_ack, c->done_try, c->last_missing, c->hub_rx, c->elapsed_ms);
        break;
    }
}

static void handle_sr_done(const uint8_t *mac, const uint8_t *body, size_t len)
{
    if (len < sizeof(can_bridge_sr_done_t)) return;
    can_bridge_sr_done_t d;
    memcpy(&d, body, sizeof(d));
    /* 2026-10-26 — 브가 붙인 세션 경과(옛 브는 없음) */
    can_bridge_sr_rx_diag_t br_diag;
    bool has_br = len >= sizeof(can_bridge_sr_done_t) + sizeof(br_diag);
    if (has_br) memcpy(&br_diag, body + sizeof(can_bridge_sr_done_t), sizeof(br_diag));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sr_session_t *s = find_session_locked(mac);
    xSemaphoreGive(s_mutex);
    if (!s || d.file_id != s->file_id) return;

    if (d.status == CAN_BRIDGE_SR_DONE_SUPERSEDED) {
        /* 2026-09-27(사용자 정의 — "에러가 아니고 정상적인 소거") — 새 촬영이 이 사진을 대체함. 받은 부분만 버리고 에러도
         * 사진 이벤트도 안 냄(이벤트를 내면 진행 중인 수동 촬영 팝업이 실패로 오판했음) */
        ESP_LOGD(TAG, "Photo superseded by newer photo file_id=%u (%u/%u chunks) - discarded", (unsigned)s->file_id,
                 (unsigned)s->next_idx, (unsigned)s->total_chunks);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        drop_session_locked(s);
        xSemaphoreGive(s_mutex);
        return;
    }

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
    uint8_t s_kind = s->kind;   /* 2026-10-22 — 아래 drop 뒤에도 쓰려고 */

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    drop_session_locked(s);   /* 실패 경로에 남은 writer는 abort */
    xSemaphoreGive(s_mutex);

    if (saved) {
        if (s_kind == 'T') capw_note_auto_photo(mac);   /* 2026-10-22 — 주기 촬영(재촬영 포함) 성공 — 실패 횟수 0으로 */
        ESP_LOGI(TAG, "Photo received: " MACSTR " file_id=%u, %u bytes -> seq=%u", MAC2STR(mac),
                 (unsigned)file_id, (unsigned)size, (unsigned)seq);
        finish_event(PHOTO_RX_STATE_READY);
        return;
    }
    static char s_dtext[320];   /* 2026-10-26 — 경과 글(스택에 안 둠, 수신 처리는 한 태스크) */
    diag_text(mac, has_br ? &br_diag : NULL, s_dtext, sizeof(s_dtext));
    if (d.status != CAN_BRIDGE_SR_DONE_COMPLETE) {
        ESP_LOGW(TAG, "Photo transfer aborted (Bridge notice) " MACSTR " file_id=%u%s", MAC2STR(mac), (unsigned)file_id, s_dtext);
        ui_log_add_err(UI_ERR_CHUNK_MISSING, "Photo receive failed (transfer aborted) file_id=%u%s", (unsigned)file_id, s_dtext);
    } else if (!complete) {
        ESP_LOGW(TAG, "Photo incomplete (%u/%u chunks) file_id=%u%s", (unsigned)got, (unsigned)total, (unsigned)file_id, s_dtext);
        ui_log_add_err(UI_ERR_CHUNK_MISSING, "Photo receive failed (incomplete) file_id=%u%s", (unsigned)file_id, s_dtext);
    } else if (!crc_ok) {
        ESP_LOGW(TAG, "CRC mismatch - photo dropped file_id=%u%s", (unsigned)file_id, s_dtext);
        ui_log_add_err(UI_ERR_CRC_MISMATCH, "Photo receive failed (CRC mismatch) file_id=%u%s", (unsigned)file_id, s_dtext);
    } else if (sd_health_writes_allowed() && !sd_storage_is_ejected()) {   /* 10-23 — SD를 꺼낸 동안은 알리지 않음 */
        ui_log_add_err(UI_ERR_SD_MOUNT_FAILED, "Photo SD save failed file_id=%u", (unsigned)file_id);
    } else {
        /* 2026-10-15 — SD 고장 상태라 저장을 거부한 것(시작 단계와 같은 이유, 93a2fd3) — 이미 5009·푸시로 알렸으니 5008을 또 올리지 않음 */
        ESP_LOGW(TAG, "Photo not saved - SD faulted file_id=%u", (unsigned)file_id);
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
    ESP_LOGD(TAG, "CAPTURE_STATUS received: status=%d", msg->status);

    /* 2026-08-21 — 캠이 캡처 상태를 esp_now_reliable_request()로 감싸 보내므로 ACK 필요(예전엔 SUCCESS/FAILED만 ACK).
     * 2026-09-26(5단계) — RECEIVED도 reliable로 바뀌어 예외 없음 */
    if (src_mac) {
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

void photo_rx_reset_sessions(uint8_t bridge)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < SR_SESSIONS; i++) {
        if (s_sessions[i].active && can_bridge_bridge_of(s_sessions[i].mac) == bridge) drop_session_locked(&s_sessions[i]);
    }
    if (can_bridge_bridge_of(s_capture_cam_mac) == bridge) s_capture_stage = PHOTO_RX_CAPTURE_STAGE_NONE;
    xSemaphoreGive(s_mutex);
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
 * 2026-10-22(사용자 설계 — 주기 촬영 실패 시 재촬영, 원래 1번 + 재촬영 2번, 3번 다 실패하면 오류·푸시, 수동 촬영은 해당 없음)
 * 실패를 아는 길 두 가지: ① 캠이 CAM_CAPTURE_FAIL로 알림 ② 콘이 "촬영 주기 + 여유" 안에 주기 촬영 사진(T)을 못 받음(캠이 멈추거나
 * 전송이 끊겨 알리지도 못한 경우 — 3002 포함). 실패마다 횟수를 올리고, 3번 안쪽이면 다음 체크인 때 재촬영(CAM_CAPTURE_RETRY)을
 * 보내고, 3번째면 오류(4005)·푸시 후 0부터. 주기 촬영 사진을 받으면 0으로. 오류는 사용자가 지움
 * ════════════════════════════════════════════════════════════ */
#define CAPW_ATTEMPTS        3          /* 원래 1번 + 재촬영 2번 */
#define CAPW_GRACE_SHORT_US  (60LL * 1000000)    /* 주기 30초 이하일 때 여유 */
#define CAPW_GRACE_LONG_US   (120LL * 1000000)   /* 그보다 긴 주기 */
typedef struct {
    bool    used;
    uint8_t mac[6];
    uint8_t fails;            /* 이번 주기 촬영의 연속 실패 수 */
    int64_t last_ok_us;       /* 마지막 주기 촬영 사진을 받은 때(없으면 기준을 잡은 때) */
} capw_t;
static capw_t s_capw[CAMBUF_MAX];
static portMUX_TYPE s_capw_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_capw_timer = NULL;

static capw_t *capw_get(const uint8_t *mac, bool create)
{
    for (int i = 0; i < CAMBUF_MAX; i++) if (s_capw[i].used && memcmp(s_capw[i].mac, mac, 6) == 0) return &s_capw[i];
    if (!create) return NULL;
    for (int i = 0; i < CAMBUF_MAX; i++) {
        if (!s_capw[i].used) {
            memset(&s_capw[i], 0, sizeof(s_capw[i]));
            s_capw[i].used = true;
            memcpy(s_capw[i].mac, mac, 6);
            s_capw[i].last_ok_us = esp_timer_get_time();
            return &s_capw[i];
        }
    }
    return NULL;
}

static const char *capfail_text(uint8_t reason)
{
    switch (reason) {
    case CAM_CAPFAIL_INIT:     return "camera init failed";
    case CAM_CAPFAIL_NO_FRAME: return "no frame";
    case CAM_CAPFAIL_PUSH:     return "send failed";
    case 0xFF:                 return "no photo in time";
    default:                   return "unknown";
    }
}

/* 실패 한 번 — 재촬영을 걸거나(3번 안쪽) 오류·푸시 */
static void capw_fail(const uint8_t *mac, uint8_t reason)
{
    /* 2026-10-22(사용자 지시 — 10-22 15:52~ SD 차단 중 4005가 66번 잘못 나감) — SD가 없거나 쓰기가 막혀 있으면 콘이 사진을
     * 저장 못 하는 것이지 캠 실패가 아님: 전송 실패·시간 안에 사진 없음은 세지 않음(캠 자체 실패 — 초기화·프레임 — 는 셈) */
    if ((reason == CAM_CAPFAIL_PUSH || reason == 0xFF) && (!sd_storage_is_mounted() || !sd_health_writes_allowed())) {
        ESP_LOGW(TAG, "Auto capture miss (%s) " MACSTR " not counted - SD not writable", capfail_text(reason), MAC2STR(mac));
        taskENTER_CRITICAL(&s_capw_mux);
        capw_t *c = capw_get(mac, true);
        if (c) c->last_ok_us = esp_timer_get_time();
        taskEXIT_CRITICAL(&s_capw_mux);
        return;
    }
    uint8_t fails;
    taskENTER_CRITICAL(&s_capw_mux);
    capw_t *c = capw_get(mac, true);
    if (!c) { taskEXIT_CRITICAL(&s_capw_mux); return; }
    c->fails++;
    fails = c->fails;
    c->last_ok_us = esp_timer_get_time();   /* 감시는 여기부터 다시 셈(재촬영 결과를 기다림) */
    if (fails >= CAPW_ATTEMPTS) c->fails = 0;
    taskEXIT_CRITICAL(&s_capw_mux);

    const char *alias = device_config_get_alias(mac);
    char name[40];
    if (alias && alias[0]) snprintf(name, sizeof(name), "%s", alias);
    else snprintf(name, sizeof(name), "C%02X%02X%02X", mac[3], mac[4], mac[5]);
    if (fails < CAPW_ATTEMPTS) {
        esp_now_cam_capture_ack_t req = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_CAM_CAPTURE_RETRY };
        static const uint8_t s_retry_ack_types[] = { ESP_NOW_MSG_CAM_CAPTURE_RETRY_ACK };
        node_hub_queue_action(mac, &req, sizeof(req), s_retry_ack_types, 1, 500, 3, "Capture retry");
        ESP_LOGW(TAG, "Auto capture failed (%s) " MACSTR " - retry %u/%u queued", capfail_text(reason), MAC2STR(mac),
                 (unsigned)fails, (unsigned)(CAPW_ATTEMPTS - 1));
        return;
    }
    ESP_LOGE(TAG, "Auto capture failed %u times (%s) " MACSTR, (unsigned)CAPW_ATTEMPTS, capfail_text(reason), MAC2STR(mac));
    /* 오류 목록에 올리면 알림 기록·푸시도 함께 나감(ui_log_add_err — 따로 alarm_post 하면 두 번 감, 10-22 시험) */
    ui_log_add_err(UI_ERR_AUTO_CAPTURE_GAVEUP, "%s auto capture failed %u times (%s)", name, (unsigned)CAPW_ATTEMPTS, capfail_text(reason));
}

static void capw_on_pair(const uint8_t *mac)
{
    taskENTER_CRITICAL(&s_capw_mux);
    capw_get(mac, true);
    taskEXIT_CRITICAL(&s_capw_mux);
}

static void capw_note_auto_photo(const uint8_t *mac)
{
    taskENTER_CRITICAL(&s_capw_mux);
    capw_t *c = capw_get(mac, true);
    if (c) { c->fails = 0; c->last_ok_us = esp_timer_get_time(); }
    taskEXIT_CRITICAL(&s_capw_mux);
}

/* 캠이 보낸 실패 알림 — ACK 하고 셈 */
static void handle_capture_fail(const uint8_t *src_mac, const uint8_t *data, int len)
{
    if (!src_mac || len < (int)ESP_NOW_CAM_CAPTURE_FAIL_BASE_LEN) return;
    const esp_now_cam_capture_fail_t *m = (const esp_now_cam_capture_fail_t *)data;
    /* 2026-10-26 — 캠의 전송 경과(옛 캠은 없음). 로그로 남기고 뒤따르는 브 중단 알림 글에 붙임 */
    if (len >= (int)sizeof(esp_now_cam_capture_fail_t) && m->reason == CAM_CAPFAIL_PUSH) {
        esp_now_photo_tx_diag_t dg;
        memcpy(&dg, &m->diag, sizeof(dg));
        camdiag_store(src_mac, &dg);
        static char s_ctext[200];
        diag_text(src_mac, NULL, s_ctext, sizeof(s_ctext));
        ESP_LOGW(TAG, "CAM tx failed " MACSTR "%s", MAC2STR(src_mac), s_ctext);
    }
    esp_now_cam_capture_ack_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_CAM_CAPTURE_FAIL_ACK };
    can_bridge_relay_send(src_mac, (const uint8_t *)&ack, sizeof(ack));
    /* 캠의 신뢰 전송이 ACK를 못 받아 같은 알림을 다시 보낸 경우 두 번 세지 않게(5초 안 같은 캠) */
    static uint8_t s_last_mac[6];
    static int64_t s_last_us = 0;
    int64_t now = esp_timer_get_time();
    if (s_last_us && now - s_last_us < 5000000LL && memcmp(s_last_mac, src_mac, 6) == 0) return;
    memcpy(s_last_mac, src_mac, 6);
    s_last_us = now;
    capw_fail(src_mac, m->reason);
}

/* 감시 타이머(10초) — 촬영 주기 + 여유 안에 주기 촬영 사진이 없으면 실패로 셈. 연결된 캠·주기 촬영 켠 캠만 */
static void capw_timer_cb(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < CAMBUF_MAX; i++) {
        uint8_t mac[6];
        int64_t last;
        taskENTER_CRITICAL(&s_capw_mux);
        bool used = s_capw[i].used;
        memcpy(mac, s_capw[i].mac, 6);
        last = s_capw[i].last_ok_us;
        taskEXIT_CRITICAL(&s_capw_mux);
        if (!used) continue;
        uint32_t interval = device_config_get_cam_capture_interval_sec(mac);
        hub_conn_state_t cs = node_hub_get_conn_state(mac);
        if (interval == 0 || cs == HUB_CONN_STATE_WAITING) {   /* 주기 촬영 끔·끊김 — 기준만 지금으로 */
            taskENTER_CRITICAL(&s_capw_mux);
            s_capw[i].last_ok_us = now;
            taskEXIT_CRITICAL(&s_capw_mux);
            continue;
        }
        int64_t grace = (interval <= 30) ? CAPW_GRACE_SHORT_US : CAPW_GRACE_LONG_US;
        if (now - last > (int64_t)interval * 1000000 + grace) capw_fail(mac, 0xFF);
    }
}

void photo_rx_capture_watch_start(void)
{
    if (s_capw_timer) return;
    const esp_timer_create_args_t ta = { .callback = capw_timer_cb, .name = "capw" };
    if (esp_timer_create(&ta, &s_capw_timer) == ESP_OK) esp_timer_start_periodic(s_capw_timer, 10LL * 1000000);
}

/* ════════════════════════════════════════════════════════════
 * 디스패치
 * ════════════════════════════════════════════════════════════ */
void photo_rx_on_recv(uint8_t msg_type, const uint8_t *src_mac, const uint8_t *data, int len)
{
    /* 2026-09-26(4단계) — SR 7종은 브가 끝점이라 콘으로 안 옴(사진은 photo_rx_on_sr_stream으로) */
    if (msg_type == ESP_NOW_MSG_CAPTURE_STATUS) handle_capture_status(src_mac, data, len);
    else if (msg_type == ESP_NOW_MSG_CAM_CAPTURE_FAIL) handle_capture_fail(src_mac, data, len);   /* 2026-10-22 */
}
