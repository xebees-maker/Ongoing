#include "bridge_sr.h"
#include "bridge_esp_now.h"
#include "ui_screen.h"
#include "esp_now_link.h"
#include "can_bridge_link.h"

#include <string.h>
#include <stddef.h>
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bridge_sr";

#define SR_SESSIONS         4
/* 순서가 어긋난 청크 보관 개수 — 설계는 "최대 윈도우 1개(16)"인데, 캠은 윈도우의 누락분을 재전송한 뒤 그 재전송이
 * 도착했는지 확인하지 않고 곧장 다음 윈도우를 보냄. 재전송이 또 빠지면 다음 윈도우 청크가 16칸 범위를 넘어 전부
 * 버려지고 DONE 라운드에서 다시 보내야 해서, 윈도우 2개(32청크, 약 38KB/캠, PSRAM)로 잡음 */
#define SR_HOLD             32
#define SR_FLOW_HIGH        48      /* Data 송신 큐가 이만큼 쌓이면 WINDOW_STATUS_ACK를 미룸(캠 속도를 CAN에 맞춤) */
#define SR_FLOW_LOW         16      /* 이만큼 빠지면 미뤄 둔 응답을 보냄 */
#define SR_IDLE_TIMEOUT_US  (10LL * 1000 * 1000)  /* 캠이 조용히 포기한 전송 정리(콘의 세션·임시파일도 같이 닫힘) */

typedef struct {
    bool     active;
    bool     completed;             /* 마지막 사진을 끝까지 보냄 — DONE_ACK가 유실돼 캠이 DONE을 다시 보내면 0으로 재응답 */
    uint8_t  mac[6];
    uint32_t file_id;
    uint16_t total_chunks;
    uint16_t next_idx;              /* 다음에 콘으로 보낼 청크 번호(이보다 작은 건 이미 보냄) */
    uint8_t *hold;                  /* SR_HOLD × ESP_NOW_PHOTO_CHUNK_DATA_LEN, 부팅 때 1회 할당 */
    uint16_t hold_idx[SR_HOLD];
    uint16_t hold_len[SR_HOLD];
    bool     hold_valid[SR_HOLD];
    int64_t  last_us;
    bool     status_pending;        /* 흐름 제어로 미룬 WINDOW_STATUS 요청 */
    uint16_t st_start, st_count;
} sr_cam_t;

static sr_cam_t s_cams[SR_SESSIONS];
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_idle_timer;
/* CAN 메시지 조립 버퍼(잠금 안에서만 씀) — app 헤더 + 청크 헤더 + 청크 데이터 */
static uint8_t s_msg[CAN_BRIDGE_APP_HEADER_LEN + sizeof(can_bridge_sr_chunk_hdr_t) + ESP_NOW_PHOTO_CHUNK_DATA_LEN];
static esp_now_photo_chunk_nack_t s_nack;  /* 800B+ — 스택에 안 둠 */

/* ---- 콘으로(CAN Data) ---- */
static void push_to_cntl(uint8_t app_type, const uint8_t *mac, const void *body, size_t body_len,
                         const uint8_t *extra, size_t extra_len)
{
    can_bridge_app_header_t hdr = { .msg_type = app_type, .flags = 0 };
    memcpy(hdr.mac, mac, 6);
    memcpy(s_msg, &hdr, CAN_BRIDGE_APP_HEADER_LEN);
    memcpy(s_msg + CAN_BRIDGE_APP_HEADER_LEN, body, body_len);
    if (extra_len) memcpy(s_msg + CAN_BRIDGE_APP_HEADER_LEN + body_len, extra, extra_len);
    bridge_esp_now_queue_to_cntl(s_msg, CAN_BRIDGE_APP_HEADER_LEN + body_len + extra_len);
}

static void push_chunk(sr_cam_t *c, uint16_t idx, const uint8_t *data, uint16_t len)
{
    can_bridge_sr_chunk_hdr_t h = { .file_id = c->file_id, .chunk_idx = idx, .len = len };
    push_to_cntl(CAN_DATA_SR_CHUNK, c->mac, &h, sizeof(h), data, len);
}

static void push_done(sr_cam_t *c, uint8_t status)
{
    can_bridge_sr_done_t d = { .file_id = c->file_id, .status = status };
    push_to_cntl(CAN_DATA_SR_DONE, c->mac, &d, sizeof(d), NULL, 0);
}

/* ---- 캠으로(ESP-NOW) — 응답은 짧은 쪽만 보냄(missing_idx는 실제 개수만큼) ---- */
static void reply(const uint8_t *mac, const void *msg, size_t len)
{
    bridge_esp_now_ensure_peer(mac);
    esp_err_t err = esp_now_send(mac, (const uint8_t *)msg, len);
    if (err != ESP_OK) ESP_LOGW(TAG, "응답 송신 실패(type=%u): %s", ((const uint8_t *)msg)[1], esp_err_to_name(err));
}

static void reply_nack(const uint8_t *mac, uint8_t msg_type, uint32_t file_id, uint16_t n)
{
    s_nack.version = ESP_NOW_LINK_VERSION;
    s_nack.msg_type = msg_type;
    s_nack.file_id = file_id;
    s_nack.missing_count = n;
    reply(mac, &s_nack, offsetof(esp_now_photo_chunk_nack_t, missing_idx) + (size_t)n * sizeof(uint16_t));
}

/* ---- 상태 ---- */
static inline bool is_held(const sr_cam_t *c, uint16_t idx)
{
    int slot = idx % SR_HOLD;
    return c->hold_valid[slot] && c->hold_idx[slot] == idx;
}

static sr_cam_t *find_cam(const uint8_t *mac)
{
    for (int i = 0; i < SR_SESSIONS; i++) {
        if ((s_cams[i].active || s_cams[i].completed) && memcmp(s_cams[i].mac, mac, 6) == 0) return &s_cams[i];
    }
    return NULL;
}

static sr_cam_t *alloc_cam(const uint8_t *mac)
{
    sr_cam_t *c = find_cam(mac);
    if (c) return c;
    for (int i = 0; i < SR_SESSIONS; i++) if (!s_cams[i].active && !s_cams[i].completed) return &s_cams[i];
    for (int i = 0; i < SR_SESSIONS; i++) if (!s_cams[i].active) return &s_cams[i];  /* 완료 기록만 남은 칸 재사용 */
    return NULL;
}

static void reset_holds(sr_cam_t *c)
{
    memset(c->hold_valid, 0, sizeof(c->hold_valid));
}

/* 필요한 만큼 보관분을 이어서 보냄 */
static void drain_holds(sr_cam_t *c)
{
    while (c->next_idx < c->total_chunks && is_held(c, c->next_idx)) {
        int slot = c->next_idx % SR_HOLD;
        push_chunk(c, c->next_idx, c->hold + (size_t)slot * ESP_NOW_PHOTO_CHUNK_DATA_LEN, c->hold_len[slot]);
        c->hold_valid[slot] = false;
        c->next_idx++;
    }
}

static uint16_t collect_missing(const sr_cam_t *c, uint16_t start, uint16_t end)
{
    uint16_t n = 0;
    if (end > c->total_chunks) end = c->total_chunks;
    for (uint16_t idx = start; idx < end && n < ESP_NOW_PHOTO_NACK_MAX_INDICES; idx++) {
        if (idx >= c->next_idx && !is_held(c, idx)) s_nack.missing_idx[n++] = idx;
    }
    return n;
}

static void answer_window_status(sr_cam_t *c)
{
    uint16_t n = collect_missing(c, c->st_start, (uint16_t)(c->st_start + c->st_count));
    reply_nack(c->mac, ESP_NOW_MSG_PHOTO_WINDOW_STATUS_ACK, c->file_id, n);
    c->status_pending = false;
}

/* ---- 메시지별 ---- */
static void on_meta(const uint8_t *mac, const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_meta_t)) return;
    esp_now_photo_meta_t meta;
    memcpy(&meta, data, sizeof(meta));
    sr_cam_t *c = alloc_cam(mac);
    if (!c || !c->hold) {
        ESP_LOGE(TAG, "SR 세션 없음(동시 %d개 초과) — META 무시", SR_SESSIONS);
        return;
    }
    if (c->active && c->file_id == meta.file_id) {
        /* META_ACK가 유실돼 캠이 META를 다시 보냄 — 응답만 다시 */
        esp_now_photo_done_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_META_ACK };
        reply(mac, &ack, sizeof(ack));
        return;
    }
    if (c->active) {
        ESP_LOGW(TAG, MACSTR " 이전 사진(file_id=%u) 미완료 — 중단 통지", MAC2STR(mac), (unsigned)c->file_id);
        push_done(c, CAN_BRIDGE_SR_DONE_ABORTED);
    }
    c->active = true;
    c->completed = false;
    memcpy(c->mac, mac, 6);
    c->file_id = meta.file_id;
    c->total_chunks = meta.total_chunks;
    c->next_idx = 0;
    c->status_pending = false;
    c->last_us = esp_timer_get_time();
    reset_holds(c);

    can_bridge_sr_meta_t m = {
        .file_id = meta.file_id, .total_size = meta.total_size, .total_chunks = meta.total_chunks,
        .crc32 = meta.crc32, .kind = meta.kind,
    };
    push_to_cntl(CAN_DATA_SR_META, mac, &m, sizeof(m), NULL, 0);
    esp_now_photo_done_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_META_ACK };
    reply(mac, &ack, sizeof(ack));

    char m6[7]; ui_screen_mac6(mac, m6);
    ui_screen_log_wireless("SR META(%s) id=%u %u chunks", m6, (unsigned)meta.file_id, (unsigned)meta.total_chunks);
}

static void on_chunk(const uint8_t *mac, const uint8_t *data, int len)
{
    if (len < (int)offsetof(esp_now_photo_chunk_t, data)) return;
    const esp_now_photo_chunk_t *ch = (const esp_now_photo_chunk_t *)data;
    sr_cam_t *c = find_cam(mac);
    if (!c || !c->active || ch->file_id != c->file_id) return;
    uint16_t idx = ch->chunk_idx, clen = ch->chunk_len;
    if (clen > ESP_NOW_PHOTO_CHUNK_DATA_LEN || len < (int)offsetof(esp_now_photo_chunk_t, data) + clen) return;
    if (idx >= c->total_chunks) return;
    c->last_us = esp_timer_get_time();

    if (idx < c->next_idx || is_held(c, idx)) return;  /* 중복 */
    if (idx == c->next_idx) {
        push_chunk(c, idx, ch->data, clen);
        c->next_idx++;
        drain_holds(c);
        return;
    }
    if (idx >= c->next_idx + SR_HOLD) return;  /* 보관 범위 밖 — 버림(윈도 상태/DONE 누락 목록으로 다시 옴) */
    int slot = idx % SR_HOLD;
    memcpy(c->hold + (size_t)slot * ESP_NOW_PHOTO_CHUNK_DATA_LEN, ch->data, clen);
    c->hold_idx[slot] = idx;
    c->hold_len[slot] = clen;
    c->hold_valid[slot] = true;
}

static void on_window_status(const uint8_t *mac, const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_window_status_req_t)) return;
    esp_now_photo_window_status_req_t req;
    memcpy(&req, data, sizeof(req));
    sr_cam_t *c = find_cam(mac);
    if (!c || !c->active || req.file_id != c->file_id) return;  /* 모르는 전송 — 무응답(캠이 다음으로 넘어감) */
    c->last_us = esp_timer_get_time();
    ESP_LOGD(TAG, MACSTR " WINDOW_STATUS [%u,+%u) next=%u backlog=%u", MAC2STR(mac), req.range_start, req.range_count,
             c->next_idx, (unsigned)bridge_esp_now_data_backlog());
    c->st_start = req.range_start;
    c->st_count = req.range_count;
    if (bridge_esp_now_data_backlog() > SR_FLOW_HIGH) {
        c->status_pending = true;   /* 흐름 제어 — bridge_sr_on_tx_progress()가 큐가 빠지면 응답 */
        return;
    }
    answer_window_status(c);
}

static void on_done(const uint8_t *mac)
{
    sr_cam_t *c = find_cam(mac);
    ESP_LOGI(TAG, MACSTR " DONE 수신(세션 %s, next=%u/%u)", MAC2STR(mac), c ? (c->active ? "진행" : "완료") : "없음",
             c ? c->next_idx : 0, c ? c->total_chunks : 0);
    if (!c) return;
    if (!c->active) {
        /* 이미 끝까지 보냄 — DONE_ACK 유실로 캠이 다시 보낸 DONE */
        if (c->completed) reply_nack(mac, ESP_NOW_MSG_PHOTO_DONE_ACK, c->file_id, 0);
        return;
    }
    c->last_us = esp_timer_get_time();
    uint16_t n = collect_missing(c, c->next_idx, c->total_chunks);
    reply_nack(mac, ESP_NOW_MSG_PHOTO_DONE_ACK, c->file_id, n);
    if (n > 0) return;   /* 캠이 누락분을 다시 보내고 DONE을 다시 보냄 */
    push_done(c, CAN_BRIDGE_SR_DONE_COMPLETE);
    c->active = false;
    c->completed = true;
    char m6[7]; ui_screen_mac6(mac, m6);
    ui_screen_log_wireless("SR DONE(%s) id=%u", m6, (unsigned)c->file_id);
}

/* ---- 공개 ---- */
bool bridge_sr_on_recv(const uint8_t *src_mac, const uint8_t *data, int len)
{
    if (len < 2) return false;
    uint8_t t = data[1];
    if (t != ESP_NOW_MSG_PHOTO_META && t != ESP_NOW_MSG_PHOTO_CHUNK &&
        t != ESP_NOW_MSG_PHOTO_WINDOW_STATUS_REQUEST && t != ESP_NOW_MSG_PHOTO_DONE) {
        /* META_ACK/WINDOW_STATUS_ACK/DONE_ACK는 브가 만드는 쪽이라 캠에서 올 일이 없음 — 혹시 와도 콘으로 안 보냄 */
        return (t == ESP_NOW_MSG_PHOTO_META_ACK || t == ESP_NOW_MSG_PHOTO_WINDOW_STATUS_ACK ||
                t == ESP_NOW_MSG_PHOTO_DONE_ACK);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (t) {
        case ESP_NOW_MSG_PHOTO_META:                  on_meta(src_mac, data, len);          break;
        case ESP_NOW_MSG_PHOTO_CHUNK:                 on_chunk(src_mac, data, len);         break;
        case ESP_NOW_MSG_PHOTO_WINDOW_STATUS_REQUEST: on_window_status(src_mac, data, len); break;
        case ESP_NOW_MSG_PHOTO_DONE:                  on_done(src_mac);                     break;
        default: break;
    }
    xSemaphoreGive(s_lock);
    return true;
}

void bridge_sr_on_tx_progress(void)
{
    if (bridge_esp_now_data_backlog() > SR_FLOW_LOW) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SR_SESSIONS; i++) {
        if (s_cams[i].active && s_cams[i].status_pending) answer_window_status(&s_cams[i]);
    }
    xSemaphoreGive(s_lock);
}

static void idle_timer_cb(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SR_SESSIONS; i++) {
        sr_cam_t *c = &s_cams[i];
        if (c->active && now - c->last_us > SR_IDLE_TIMEOUT_US) {
            ESP_LOGW(TAG, MACSTR " SR 무응답 %llds — 중단(file_id=%u, %u/%u)", MAC2STR(c->mac),
                     (long long)(SR_IDLE_TIMEOUT_US / 1000000), (unsigned)c->file_id, c->next_idx, c->total_chunks);
            push_done(c, CAN_BRIDGE_SR_DONE_ABORTED);
            c->active = false;
            c->completed = false;
        }
    }
    xSemaphoreGive(s_lock);
    bridge_sr_on_tx_progress();  /* 큐가 빠졌는데 릴레이 태스크가 쉬는 경우 대비 */
}

void bridge_sr_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < SR_SESSIONS; i++) {
        s_cams[i].hold = heap_caps_malloc((size_t)SR_HOLD * ESP_NOW_PHOTO_CHUNK_DATA_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cams[i].hold) ESP_LOGE(TAG, "SR 보관 버퍼[%d] 할당 실패", i);
    }
    const esp_timer_create_args_t args = { .callback = idle_timer_cb, .name = "sr_idle" };
    esp_timer_create(&args, &s_idle_timer);
    esp_timer_start_periodic(s_idle_timer, 1000 * 1000);
}
