#include "esp_now_reliable.h"

#include <string.h>
#include <stdlib.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "esp_now_reliable";

/* 2026-09-26(설계 Docs/설계_CAN링크_2026-09-26.md §4, 3단계) — 노드별 슬롯 + 이벤트로 움직이는 상태 기계.
 * 예전엔 대기 슬롯 1개 + API 뮤텍스라 한 번에 요청 하나만 기다릴 수 있었고, 브가 콘의 RELIABLE_SEND를
 * 대행할 때 캠 A의 대기(최대 timeout×시도)가 캠 B 요청과 그 뒤의 모든 메시지를 막았음(head-of-line).
 *
 * - 슬롯: 노드(MAC)당 1개. 서로 다른 노드는 동시에 진행.
 * - 서비스 태스크 1개가 알림으로만 깨어나 송신/재전송/완료 처리: 새 요청, 슬롯 타이머 만료, 송신 큐
 *   자리 생김(esp_now_reliable_on_send_done), 응답 도착(esp_now_reliable_on_recv). 폴링 없음.
 * - 응답 대조는 recv_cb 문맥(on_recv)에서 MAC+msg_type으로 슬롯을 찾아 복사만 하고 서비스를 깨움.
 * - 완료 콜백은 서비스 태스크에서 불림 — 블로킹 금지(결과를 큐에 넣고 알림만).
 * - 동기 esp_now_reliable_request()는 비동기 + 세마포어 대기 포장. 예전처럼 동기 호출끼리는 API
 *   뮤텍스로 차례로 처리(캠/센스는 허브 하나하고만 통신하므로 동작 그대로) */

#define SLOT_COUNT        8
#define REQ_BUF_CAP       250    /* 신뢰 요청은 전부 수십 바이트 — 넘으면 거절(ESP_ERR_INVALID_SIZE) */
#define REPLY_BUF_CAP     1470   /* ESP-NOW v2 한도 — DONE_ACK/WINDOW_STATUS_ACK(누락 목록)가 가장 큼 */
#define ACCEPT_MAX        8

/* NO_MEM 뒤 송신 완료 이벤트를 기다리는 최대 시간(이벤트가 끝내 안 올 때의 상한). 정상이면 다른
 * 프레임이 끝나는 즉시(수 ms) 깨어남. 이만큼씩 TX_SLOT_MAX_TRIES번 못 나가면 그 시도는 보낸 걸로
 * 치고 응답 대기로 넘어감(예전 동작과 동일) */
#define TX_SLOT_WAIT_MAX_MS 100
#define TX_SLOT_MAX_TRIES   6

typedef enum {
    SLOT_FREE = 0,
    SLOT_NEED_SEND,     /* 서비스가 보내야 함 */
    SLOT_WAIT_TX,       /* NO_MEM — 송신 큐 자리(send_done) 또는 짧은 타이머 대기 */
    SLOT_WAIT_REPLY,    /* 보냄 — 응답 또는 timeout_ms 타이머 대기 */
} slot_state_t;

typedef struct {
    slot_state_t state;
    uint8_t  mac[6];
    uint8_t *req;               /* REQ_BUF_CAP, 첫 사용 때 할당 후 계속 재사용 */
    size_t   req_len;
    uint8_t  accept[ACCEPT_MAX];
    size_t   accept_count;
    uint32_t timeout_ms;
    int      attempts_total;
    int      attempts_left;
    int      nomem_tries;
    bool     timer_fired;
    bool     matched;
    esp_timer_handle_t timer;
    uint8_t *reply;             /* REPLY_BUF_CAP, 첫 사용 때 할당 후 계속 재사용 */
    size_t   reply_len;
    esp_now_reliable_done_cb_t cb;
    void    *cb_ctx;
} slot_t;

static slot_t            s_slots[SLOT_COUNT];
static SemaphoreHandle_t s_lock = NULL;       /* 슬롯 표 보호 — 안에서는 복사·상태 변경만 */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_api_mutex = NULL;  /* 동기 호출끼리 차례로(예전 동작 유지) */
static StaticSemaphore_t s_api_mutex_buf;
static TaskHandle_t      s_service_task = NULL;
static volatile bool     s_any_wait_tx = false;

static void notify_service(void)
{
    if (s_service_task) xTaskNotifyGive(s_service_task);
}

static void slot_timer_cb(void *arg)
{
    slot_t *s = (slot_t *)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s->timer_fired = true;
    xSemaphoreGive(s_lock);
    notify_service();
}

static void add_peer_if_needed(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

static void *alloc_buf(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(len, MALLOC_CAP_8BIT);
    return p;
}

/* 슬롯 처리 한 번 — 서비스 태스크에서만 부름. esp_now_send()와 콜백은 잠금 밖에서 */
static void service_slot(slot_t *s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s->state == SLOT_FREE) { xSemaphoreGive(s_lock); return; }

    /* 1) 응답 도착 → 성공 완료 */
    if (s->matched) {
        esp_timer_stop(s->timer);
        esp_now_reliable_done_cb_t cb = s->cb;
        void *cb_ctx = s->cb_ctx;
        s->timer_fired = false;
        xSemaphoreGive(s_lock);
        /* 슬롯을 비우기 전에 콜백 — reply 포인터는 콜백 동안만 유효. 콜백 중엔 state가 아직 FREE가
         * 아니라서 같은 노드에 새 요청이 끼어들 수 없음 */
        if (cb) cb(cb_ctx, ESP_OK, s->reply, s->reply_len);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s->state = SLOT_FREE;
        s->matched = false;
        xSemaphoreGive(s_lock);
        return;
    }

    /* 2) 타이머 만료 */
    if (s->timer_fired) {
        s->timer_fired = false;
        if (s->state == SLOT_WAIT_REPLY) {
            s->attempts_left--;
            if (s->attempts_left <= 0) {
                esp_now_reliable_done_cb_t cb = s->cb;
                void *cb_ctx = s->cb_ctx;
                int total = s->attempts_total;
                xSemaphoreGive(s_lock);
                ESP_LOGW(TAG, "요청 타임아웃(%d회 시도 모두 무응답)", total);
                if (cb) cb(cb_ctx, ESP_ERR_TIMEOUT, NULL, 0);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s->state = SLOT_FREE;
                xSemaphoreGive(s_lock);
                return;
            }
            s->state = SLOT_NEED_SEND;
        } else if (s->state == SLOT_WAIT_TX) {
            s->state = SLOT_NEED_SEND;  /* send_done이 안 와도 상한 시간 뒤 다시 시도 */
        }
    }

    /* 3) 송신 */
    if (s->state == SLOT_NEED_SEND) {
        uint8_t mac[6];
        memcpy(mac, s->mac, 6);
        size_t req_len = s->req_len;
        xSemaphoreGive(s_lock);

        /* req 버퍼는 이 슬롯이 끝날 때까지 안 바뀜(요청 함수는 FREE 슬롯에만 씀) — 잠금 밖에서 읽어도 됨 */
        esp_err_t err = esp_now_send(mac, s->req, req_len);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s->state != SLOT_NEED_SEND) { xSemaphoreGive(s_lock); return; }  /* 그사이 응답으로 끝난 경우 등 */
        if (err == ESP_ERR_ESPNOW_NO_MEM && s->nomem_tries + 1 < TX_SLOT_MAX_TRIES) {
            s->nomem_tries++;
            s->state = SLOT_WAIT_TX;
            s_any_wait_tx = true;
            esp_timer_stop(s->timer);
            esp_timer_start_once(s->timer, (uint64_t)TX_SLOT_WAIT_MAX_MS * 1000);
            xSemaphoreGive(s_lock);
            return;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_now_send 실패(시도 %d/%d): %s",
                     s->attempts_total - s->attempts_left + 1, s->attempts_total, esp_err_to_name(err));
        }
        /* 보냈거나(또는 NO_MEM 재시도 소진) — 이번 시도의 응답 대기 */
        s->nomem_tries = 0;
        s->state = SLOT_WAIT_REPLY;
        esp_timer_stop(s->timer);
        esp_timer_start_once(s->timer, (uint64_t)s->timeout_ms * 1000);
        /* 응답이 esp_now_send() 반환 전에 이미 와서 matched가 섰다면 다음 알림에서 처리됨(on_recv가 알림을 줌) */
    }
    xSemaphoreGive(s_lock);
}

static void service_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_any_wait_tx = false;
        for (int i = 0; i < SLOT_COUNT; i++) {
            service_slot(&s_slots[i]);
        }
        /* 한 바퀴 도는 동안 새로 생긴 이벤트는 그 알림이 다음 루프를 깨움 */
    }
}

static void ensure_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_api_mutex = xSemaphoreCreateMutexStatic(&s_api_mutex_buf);
    for (int i = 0; i < SLOT_COUNT; i++) {
        const esp_timer_create_args_t targs = { .callback = slot_timer_cb, .arg = &s_slots[i], .name = "reliable" };
        esp_timer_create(&targs, &s_slots[i].timer);
    }
    /* Wi-Fi(ESP-NOW)와 같은 코어 0, 통신 등급 17(설계 §3). 단일 코어(C3)도 코어 0 */
    xTaskCreatePinnedToCore(service_task, "reliable_svc", 3072, NULL, 17, &s_service_task, 0);
}

esp_err_t esp_now_reliable_request_async(const uint8_t *peer_mac,
                                          const void *req, size_t req_len,
                                          const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                                          uint32_t timeout_ms, int max_attempts,
                                          esp_now_reliable_done_cb_t cb, void *cb_ctx)
{
    if (!peer_mac || !req || req_len == 0 || !accept_reply_types || accept_reply_types_count == 0) return ESP_ERR_INVALID_ARG;
    if (req_len > REQ_BUF_CAP || accept_reply_types_count > ACCEPT_MAX) return ESP_ERR_INVALID_SIZE;
    ensure_init();
    add_peer_if_needed(peer_mac);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *free_slot = NULL;
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (s_slots[i].state != SLOT_FREE && memcmp(s_slots[i].mac, peer_mac, 6) == 0) {
            xSemaphoreGive(s_lock);
            ESP_LOGE(TAG, "이 노드에 진행 중인 요청이 이미 있음(노드당 1개 원칙 위반) — %02X%02X%02X%02X%02X%02X",
                     peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
            return ESP_ERR_INVALID_STATE;
        }
        if (!free_slot && s_slots[i].state == SLOT_FREE) free_slot = &s_slots[i];
    }
    if (!free_slot) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "슬롯 부족(%d)", SLOT_COUNT);
        return ESP_ERR_NO_MEM;
    }
    if (!free_slot->req) free_slot->req = (uint8_t *)alloc_buf(REQ_BUF_CAP);
    if (!free_slot->reply) free_slot->reply = (uint8_t *)alloc_buf(REPLY_BUF_CAP);
    if (!free_slot->req || !free_slot->reply) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    memcpy(free_slot->mac, peer_mac, 6);
    memcpy(free_slot->req, req, req_len);
    free_slot->req_len = req_len;
    memcpy(free_slot->accept, accept_reply_types, accept_reply_types_count);
    free_slot->accept_count = accept_reply_types_count;
    free_slot->timeout_ms = timeout_ms;
    free_slot->attempts_total = (max_attempts > 0) ? max_attempts : 1;
    free_slot->attempts_left = free_slot->attempts_total;
    free_slot->nomem_tries = 0;
    free_slot->timer_fired = false;
    free_slot->matched = false;
    free_slot->reply_len = 0;
    free_slot->cb = cb;
    free_slot->cb_ctx = cb_ctx;
    free_slot->state = SLOT_NEED_SEND;
    xSemaphoreGive(s_lock);

    notify_service();
    return ESP_OK;
}

/* ---- 동기 포장 ---- */
typedef struct {
    SemaphoreHandle_t done;
    esp_err_t result;
    void    *reply_out;
    size_t   reply_out_cap;
    size_t  *reply_out_len;
} sync_wait_t;

static void sync_done_cb(void *ctx, esp_err_t result, const uint8_t *reply, size_t reply_len)
{
    sync_wait_t *w = (sync_wait_t *)ctx;
    w->result = result;
    if (result == ESP_OK) {
        if (w->reply_out && w->reply_out_cap > 0 && reply) {
            size_t copy_len = reply_len < w->reply_out_cap ? reply_len : w->reply_out_cap;
            memcpy(w->reply_out, reply, copy_len);
            if (w->reply_out_len) *w->reply_out_len = copy_len;
        } else if (w->reply_out_len) {
            *w->reply_out_len = reply_len;
        }
    }
    xSemaphoreGive(w->done);
}

esp_err_t esp_now_reliable_request(const uint8_t *peer_mac,
                                    const void *req, size_t req_len,
                                    const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                                    uint32_t timeout_ms, int max_attempts,
                                    void *reply_out, size_t reply_out_cap, size_t *reply_out_len)
{
    ensure_init();
    xSemaphoreTake(s_api_mutex, portMAX_DELAY);

    StaticSemaphore_t done_buf;
    sync_wait_t w = {
        .done = xSemaphoreCreateBinaryStatic(&done_buf),
        .result = ESP_ERR_TIMEOUT,
        .reply_out = reply_out,
        .reply_out_cap = reply_out_cap,
        .reply_out_len = reply_out_len,
    };
    esp_err_t err = esp_now_reliable_request_async(peer_mac, req, req_len,
                                                    accept_reply_types, accept_reply_types_count,
                                                    timeout_ms, max_attempts, sync_done_cb, &w);
    if (err == ESP_OK) {
        xSemaphoreTake(w.done, portMAX_DELAY);  /* 서비스가 timeout×시도 안에 반드시 콜백을 부름 */
        err = w.result;
    }
    vSemaphoreDelete(w.done);
    xSemaphoreGive(s_api_mutex);
    return err;
}

void esp_now_reliable_on_recv(uint8_t msg_type, const uint8_t *src_mac,
                               const uint8_t *data, int len)
{
    if (!s_lock || !src_mac || !data || len <= 0) return;
    bool hit = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SLOT_COUNT; i++) {
        slot_t *s = &s_slots[i];
        if (s->state == SLOT_FREE || s->matched) continue;
        if (memcmp(s->mac, src_mac, 6) != 0) continue;
        bool type_ok = false;
        for (size_t k = 0; k < s->accept_count; k++) {
            if (s->accept[k] == msg_type) { type_ok = true; break; }
        }
        if (!type_ok) continue;
        size_t copy_len = (size_t)len > REPLY_BUF_CAP ? REPLY_BUF_CAP : (size_t)len;
        memcpy(s->reply, data, copy_len);
        s->reply_len = copy_len;
        s->matched = true;
        hit = true;
        break;  /* 노드당 슬롯 1개 */
    }
    xSemaphoreGive(s_lock);
    if (hit) notify_service();
}

void esp_now_reliable_on_send_done(void)
{
    /* send_cb는 Wi-Fi 태스크 문맥(ISR 아님). NO_MEM으로 기다리는 슬롯이 있을 때만 서비스를 깨움 —
     * 기다리던 슬롯은 짧은 타이머도 걸려 있지만, 이 이벤트로 즉시 재시도 */
    if (!s_any_wait_tx) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (s_slots[i].state == SLOT_WAIT_TX) {
            esp_timer_stop(s_slots[i].timer);
            s_slots[i].state = SLOT_NEED_SEND;
        }
    }
    xSemaphoreGive(s_lock);
    notify_service();
}
