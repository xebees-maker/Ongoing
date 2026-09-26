#include "can_bridge_link.h"
#include "esp_now_link.h"  /* 경로 분류용 ESP-NOW msg_type */
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "can_bridge_link";

/* PSRAM 우선, 없으면 내부 RAM 폴백(사용자 지시 — feedback_prefer_psram_for_buffers 정책을
 * 브릿지에도 동일 적용, PSRAM 없는 후보 보드 대비 폴백 필수) */
static void *psram_or_internal_alloc(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(len, MALLOC_CAP_8BIT);
    }
    return p;
}

/* ============================== FIFO 큐 — 드롭 금지 ============================== */

void can_bridge_queue_init(can_bridge_queue_t *q)
{
    if (!q) {
        ESP_LOGE(TAG, "queue_init: q=NULL");
        return;
    }
    memset(q, 0, sizeof(*q));
    portMUX_INITIALIZE(&q->lock);
    q->inited = 1;
}

void can_bridge_queue_set_notify_task(can_bridge_queue_t *q, TaskHandle_t task)
{
    if (!q || !q->inited) {
        ESP_LOGE(TAG, "queue_set_notify_task: 미초기화 큐(q=%p)", (void *)q);
        return;
    }
    taskENTER_CRITICAL(&q->lock);
    q->notify_task = task;
    taskEXIT_CRITICAL(&q->lock);
}

void can_bridge_queue_push(can_bridge_queue_t *q, const uint8_t *data, size_t len)
{
    if (!q || !q->inited) {
        /* 코드 버그(미초기화 큐에 push) — 드롭 금지 정책상 조용히 버리지 않고 즉시 드러나게 중단 */
        ESP_LOGE(TAG, "queue_push: 미초기화 큐(q=%p) — 메시지 드롭 금지 정책상 중단", (void *)q);
        abort();
    }
    if (len == 0 || !data) {
        /* 길이 0짜리는 메시지가 아님(재조립기는 len>=1만 넘김) — 넣을 게 없으니 무시 */
        ESP_LOGE(TAG, "queue_push: 잘못된 인자(data=%p len=%u) — 무시", (const void *)data, (unsigned)len);
        return;
    }

    /* 할당/복사는 락 밖에서(크리티컬 섹션 안에서 malloc 금지) */
    can_bridge_queue_entry_t *e = (can_bridge_queue_entry_t *)psram_or_internal_alloc(sizeof(can_bridge_queue_entry_t) + len);
    if (!e) {
        /* 사용자 지시: 이 링크에서 메시지 유실은 절대 허용 안 함 — 드롭 대신 즉시 드러나게 abort.
         * (PSRAM+내부RAM 둘 다 실패할 정도면 이미 메모리 설계 자체가 잘못된 상태) */
        ESP_LOGE(TAG, "큐 항목 할당 실패(len=%u) — 메시지 드롭 금지 정책상 중단", (unsigned)len);
        abort();
    }
    e->next = NULL;
    e->len = len;
    memcpy(e->data, data, len);

    taskENTER_CRITICAL(&q->lock);
    if (q->tail) {
        q->tail->next = e;
    } else {
        q->head = e;
    }
    q->tail = e;
    q->count++;
    if (q->count > q->high_water_mark) {
        q->high_water_mark = q->count;
    }
    TaskHandle_t notify_task = q->notify_task;
    taskEXIT_CRITICAL(&q->lock);

    /* 완성된 메시지 1개 도착 = 소비 태스크가 처리할 일이 생긴 때 — 이때만 깨움 */
    if (notify_task) {
        xTaskNotifyGive(notify_task);
    }
}

int can_bridge_queue_pop(can_bridge_queue_t *q, uint8_t **out_data, size_t *out_len)
{
    if (!q || !q->inited || !out_data || !out_len) {
        ESP_LOGE(TAG, "queue_pop: 잘못된 인자(q=%p inited=%u out_data=%p out_len=%p)",
                 (void *)q, q ? (unsigned)q->inited : 0u, (void *)out_data, (void *)out_len);
        return 0;
    }

    taskENTER_CRITICAL(&q->lock);
    can_bridge_queue_entry_t *e = q->head;
    if (e) {
        q->head = e->next;
        if (!q->head) q->tail = NULL;
        if (q->count > 0) q->count--;
    }
    taskEXIT_CRITICAL(&q->lock);

    if (!e) return 0;

    /* entry 헤더(next/len)는 반환하는 data 포인터보다 앞에 있으니, data 시작주소를 돌려주고
     * free는 pop_free에서 그 앞의 entry 헤더까지 포함해 처리 */
    e->next = NULL;
    *out_data = e->data;
    *out_len = e->len;
    return 1;
}

void can_bridge_queue_pop_free(uint8_t *popped_data)
{
    if (!popped_data) return;
    /* data[]는 flexible array member라 entry 시작주소 = data - offsetof(...,data) */
    can_bridge_queue_entry_t *e = (can_bridge_queue_entry_t *)(popped_data - offsetof(can_bridge_queue_entry_t, data));
    free(e);
}

void can_bridge_queue_get_stats(can_bridge_queue_t *q, uint32_t *out_count, uint32_t *out_high_water_mark)
{
    uint32_t count = 0, hwm = 0;
    if (q && q->inited) {
        taskENTER_CRITICAL(&q->lock);
        count = q->count;
        hwm = q->high_water_mark;
        taskEXIT_CRITICAL(&q->lock);
    }
    if (out_count) *out_count = count;
    if (out_high_water_mark) *out_high_water_mark = hwm;
}

/* ============================== ISO-TP 재조립(수신) ============================== */

void can_bridge_reassembly_init(can_bridge_reassembly_t *r, uint8_t *psram_buf, size_t buf_cap)
{
    memset(r, 0, sizeof(*r));
    r->buf = psram_buf;
    r->buf_cap = buf_cap;
}

int can_bridge_reassembly_feed(can_bridge_reassembly_t *r, const uint8_t *frame_data, uint8_t frame_len,
                                uint8_t fc_frame_out[8], can_bridge_queue_t *complete_queue)
{
    if (frame_len < 1) return 0;
    uint8_t pci = (frame_data[0] >> 4) & 0x0F;

    if (pci == ISO_TP_PCI_SF) {
        uint8_t len = frame_data[0] & 0x0F;
        if (len == 0 || len > ISO_TP_SF_MAX_LEN || (size_t)len > r->buf_cap) {
            ESP_LOGW(TAG, "SF 길이 이상(%u) — 폐기", len);
            return 0;
        }
        memcpy(r->buf, &frame_data[1], len);
        can_bridge_queue_push(complete_queue, r->buf, len);
        r->in_progress = 0;
        return 0; /* SF는 FC 불필요 */
    }

    if (pci == ISO_TP_PCI_FF) {
        if (frame_len < 2) return 0;
        size_t total_len = (((size_t)(frame_data[0] & 0x0F)) << 8) | frame_data[1];
        if (total_len > r->buf_cap) {
            ESP_LOGE(TAG, "FF total_len(%u) > 버퍼(%u) — 폐기(발신측 재시도 기대)",
                     (unsigned)total_len, (unsigned)r->buf_cap);
            r->in_progress = 0;
            return 0;
        }
        size_t first_chunk = total_len < ISO_TP_FF_FIRST_LEN ? total_len : ISO_TP_FF_FIRST_LEN;
        memcpy(r->buf, &frame_data[2], first_chunk);
        r->total_len = total_len;
        r->received_len = first_chunk;
        r->next_seq = 1;
        r->in_progress = 1;

        /* BS=0("남은 거 다 보내"), STmin=0 — 사용자 지시 없는 세부는 최소구현(단순 P2P 링크라
         * 별도 페이싱 협상 불필요) */
        fc_frame_out[0] = (uint8_t)((ISO_TP_PCI_FC << 4) | ISO_TP_FC_STATUS_CTS);
        fc_frame_out[1] = 0x00; /* BlockSize=0 */
        fc_frame_out[2] = 0x00; /* STmin=0 */
        memset(&fc_frame_out[3], 0, 5);
        return 1;
    }

    if (pci == ISO_TP_PCI_CF) {
        if (!r->in_progress) {
            ESP_LOGW(TAG, "진행 중인 FF 없이 CF 도착 — 폐기");
            return 0;
        }
        uint8_t seq = frame_data[0] & 0x0F;
        if (seq != r->next_seq) {
            ESP_LOGW(TAG, "CF 순번 어긋남(기대=%u 수신=%u) — 이 메시지 폐기(발신측 재시도 기대)",
                     r->next_seq, seq);
            r->in_progress = 0;
            return 0;
        }
        size_t remain = r->total_len - r->received_len;
        size_t chunk = (size_t)(frame_len - 1);
        if (chunk > remain) chunk = remain;
        if (r->received_len + chunk > r->buf_cap) {
            ESP_LOGE(TAG, "CF로 버퍼 한도 초과 — 폐기");
            r->in_progress = 0;
            return 0;
        }
        memcpy(r->buf + r->received_len, &frame_data[1], chunk);
        r->received_len += chunk;
        r->next_seq = (uint8_t)((r->next_seq + 1) & 0x0F);

        if (r->received_len >= r->total_len) {
            can_bridge_queue_push(complete_queue, r->buf, r->total_len);
            r->in_progress = 0;
        }
        return 0;
    }

    /* FC는 재조립 상태머신이 아니라 송신측(can_bridge_send)이 직접 처리 — 여기로 오면 무시 */
    return 0;
}

/* ============================== 송신 프레임 풀 ============================== */

typedef struct {
    twai_frame_t frame;
    uint8_t data[8];
    volatile uint8_t in_use;
} tx_slot_t;

static tx_slot_t *s_tx_slots;            /* PSRAM, CAN_BRIDGE_TX_POOL_SIZE개 */
static SemaphoreHandle_t s_tx_free_sem;  /* 빈 슬롯 개수(카운팅) */
/* 2026-09-26 — 드라이버에 동시에 넣는 프레임을 1개로 제한(이전 프레임의 on_tx_done이 줌). IDF v6.0.2 onchip
 * 드라이버는 하드웨어가 바쁠 때 태스크가 프레임을 내부 큐에 넣은 직후, 그 사이 송신 완료 ISR이 그 프레임을
 * 꺼내 보내고 끝내 버리면 태스크의 "second chance" 확인이 빈 큐를 보고 assert(esp_twai_onchip.c:607 "should
 * always get frame at this moment")로 죽음(실기 크래시, 태스크 간 뮤텍스로도 재현 — 태스크↔ISR 경합).
 * 한 번에 1개만 넣으면 드라이버는 항상 "하드웨어가 비어 있어 바로 보냄" 경로만 탐 */
static SemaphoreHandle_t s_tx_inflight_sem;
static StaticSemaphore_t s_tx_inflight_sem_buf;
static portMUX_TYPE s_tx_lock = portMUX_INITIALIZER_UNLOCKED;
/* 2026-09-26 — 지금 드라이버에 들어가 있는 프레임(s_tx_inflight_sem을 쥔 프레임). on_tx_done이나 버스 오프
 * 정리(can_bridge_tx_pool_on_bus_off_isr) 중 먼저 온 쪽만 반납 — 둘 다 오더라도 한 번만 반납되게 s_tx_lock
 * 안에서 확인하고 비움 */
static tx_slot_t *s_inflight_slot = NULL;
/* CAN 노드(=TWAI 인터럽트)가 잡힌 코어 — can_bridge_node_start_on_core()가 기록. -1이면 미지정 */
static int s_tx_core = -1;

void can_bridge_tx_pool_init(void)
{
    if (s_tx_slots) return;
    s_tx_slots = (tx_slot_t *)psram_or_internal_alloc(sizeof(tx_slot_t) * CAN_BRIDGE_TX_POOL_SIZE);
    s_tx_free_sem = xSemaphoreCreateCounting(CAN_BRIDGE_TX_POOL_SIZE, CAN_BRIDGE_TX_POOL_SIZE);
    s_tx_inflight_sem = xSemaphoreCreateBinaryStatic(&s_tx_inflight_sem_buf);
    xSemaphoreGive(s_tx_inflight_sem);  /* 처음엔 비어 있음(보낼 수 있음) */
    if (!s_tx_slots || !s_tx_free_sem) {
        /* 송신 불가 상태로 조용히 두지 않음(드롭 금지 정책과 같은 취지) */
        ESP_LOGE(TAG, "송신 프레임 풀 할당 실패(slots=%p sem=%p) — 중단", (void *)s_tx_slots, (void *)s_tx_free_sem);
        abort();
    }
    memset(s_tx_slots, 0, sizeof(tx_slot_t) * CAN_BRIDGE_TX_POOL_SIZE);
}

static void tx_slot_release(tx_slot_t *slot)
{
    taskENTER_CRITICAL(&s_tx_lock);
    slot->in_use = 0;
    taskEXIT_CRITICAL(&s_tx_lock);
    xSemaphoreGive(s_tx_free_sem);
}

esp_err_t can_bridge_tx_frame(twai_node_handle_t node, uint32_t id, const uint8_t *data, uint8_t len, int timeout_ms)
{
    if (!s_tx_slots || !s_tx_free_sem) {
        ESP_LOGE(TAG, "tx_frame: 풀 미초기화(can_bridge_tx_pool_init 누락)");
        return ESP_ERR_INVALID_STATE;
    }
    if (!node || len > 8 || (len > 0 && !data)) return ESP_ERR_INVALID_ARG;
    if (s_tx_core >= 0 && xTaskGetCoreID(xTaskGetCurrentTaskHandle()) != s_tx_core) {
        /* 설계 위반(can_bridge_node_start_on_core 주석 참고) — 한 번만 알림 */
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            ESP_LOGE(TAG, "CAN 송신이 코어 %d에 고정되지 않은 태스크(%s)에서 호출됨 — 송신 정지 위험", s_tx_core, pcTaskGetName(NULL));
        }
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t total = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_tx_free_sem, total) != pdTRUE) {
        ESP_LOGW(TAG, "송신 프레임 풀 빈 슬롯 없음(%dms) — 송신 실패", timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    tx_slot_t *slot = NULL;
    taskENTER_CRITICAL(&s_tx_lock);
    for (int i = 0; i < CAN_BRIDGE_TX_POOL_SIZE; i++) {
        if (!s_tx_slots[i].in_use) {
            s_tx_slots[i].in_use = 1;
            slot = &s_tx_slots[i];
            break;
        }
    }
    taskEXIT_CRITICAL(&s_tx_lock);
    if (!slot) {
        /* 세마포어 개수와 in_use가 어긋난 경우(논리 버그) — 개수는 되돌려 둠 */
        ESP_LOGE(TAG, "tx_frame: 세마포어는 얻었는데 빈 슬롯 없음(논리 버그)");
        xSemaphoreGive(s_tx_free_sem);
        return ESP_FAIL;
    }

    memset(&slot->frame, 0, sizeof(slot->frame));
    if (len > 0) memcpy(slot->data, data, len);
    slot->frame.header.id = id;
    slot->frame.buffer = slot->data;
    slot->frame.buffer_len = len;

    int remain_ms = -1;
    if (timeout_ms >= 0) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        TickType_t left = (elapsed < total) ? (total - elapsed) : 0;
        remain_ms = (int)pdTICKS_TO_MS(left);
    }
    /* 이전 프레임의 송신 완료(on_tx_done)를 기다림 — 위 s_tx_inflight_sem 주석 참고 */
    TickType_t inflight_wait = (remain_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(remain_ms);
    if (xSemaphoreTake(s_tx_inflight_sem, inflight_wait) != pdTRUE) {
        tx_slot_release(slot);
        return ESP_ERR_TIMEOUT;
    }
    taskENTER_CRITICAL(&s_tx_lock);
    s_inflight_slot = slot;
    taskEXIT_CRITICAL(&s_tx_lock);
    esp_err_t err = twai_node_transmit(node, &slot->frame, 0);
    if (err != ESP_OK) {
        /* 드라이버에 못 들어감(버스 오프 중이면 INVALID_STATE) — ISR이 이 슬롯을 볼 일이 없으니 바로 반납,
         * 송신 권한도 돌려줌 */
        taskENTER_CRITICAL(&s_tx_lock);
        bool mine = (s_inflight_slot == slot);
        if (mine) s_inflight_slot = NULL;
        taskEXIT_CRITICAL(&s_tx_lock);
        if (mine) {  /* 아니면 그 사이 버스 오프 정리가 이미 반납함 */
            xSemaphoreGive(s_tx_inflight_sem);
            tx_slot_release(slot);
        }
    }
    return err;
}

/* s_inflight_slot이 slot(NULL이면 무엇이든)이면 비우고 반납 — on_tx_done과 버스 오프 정리 공용 */
static bool release_inflight_isr(tx_slot_t *slot)
{
    portENTER_CRITICAL_ISR(&s_tx_lock);
    tx_slot_t *cur = s_inflight_slot;
    bool mine = cur && (!slot || cur == slot);
    if (mine) {
        s_inflight_slot = NULL;
        cur->in_use = 0;
    }
    portEXIT_CRITICAL_ISR(&s_tx_lock);
    if (!mine) return false;  /* 이미 다른 쪽이 반납함 — 슬롯은 재사용 중일 수 있으니 건드리지 않음 */
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_tx_free_sem, &woken);
    xSemaphoreGiveFromISR(s_tx_inflight_sem, &woken);  /* 다음 프레임 넣어도 됨 */
    return woken == pdTRUE;
}

bool can_bridge_tx_pool_on_done_isr(const twai_tx_done_event_data_t *edata)
{
    if (!edata || !edata->done_tx_frame || !s_tx_slots || !s_tx_free_sem) return false;
    const twai_frame_t *f = edata->done_tx_frame;
    tx_slot_t *slot = NULL;
    for (int i = 0; i < CAN_BRIDGE_TX_POOL_SIZE; i++) {
        if (&s_tx_slots[i].frame == f) { slot = &s_tx_slots[i]; break; }
    }
    if (!slot) return false;  /* 이 풀에서 나간 프레임이 아님 */
    return release_inflight_isr(slot);
}

bool can_bridge_tx_pool_on_bus_off_isr(void)
{
    if (!s_tx_slots || !s_tx_free_sem) return false;
    return release_inflight_isr(NULL);
}

typedef struct {
    const twai_onchip_node_config_t *cfg;
    const twai_event_callbacks_t *cbs;
    twai_node_handle_t node;
    esp_err_t err;
    TaskHandle_t waiter;
} node_start_args_t;

static void node_start_task(void *arg)
{
    node_start_args_t *a = (node_start_args_t *)arg;
    a->err = twai_new_node_onchip(a->cfg, &a->node);
    if (a->err == ESP_OK) {
        can_bridge_tx_pool_init();
        a->err = twai_node_register_event_callbacks(a->node, a->cbs, NULL);
        if (a->err == ESP_OK) a->err = twai_node_enable(a->node);
    }
    xTaskNotifyGive(a->waiter);
    vTaskDelete(NULL);
}

esp_err_t can_bridge_node_start_on_core(const twai_onchip_node_config_t *node_cfg, const twai_event_callbacks_t *cbs,
                                        int core, twai_node_handle_t *out_node)
{
    if (!node_cfg || !cbs || !out_node) return ESP_ERR_INVALID_ARG;
    node_start_args_t a = { .cfg = node_cfg, .cbs = cbs, .node = NULL, .err = ESP_FAIL, .waiter = xTaskGetCurrentTaskHandle() };
    if (xTaskCreatePinnedToCore(node_start_task, "can_node_start", 3072, &a, 17, NULL, core) != pdPASS) return ESP_ERR_NO_MEM;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (a.err != ESP_OK) return a.err;
    s_tx_core = core;
    *out_node = a.node;
    ESP_LOGI(TAG, "TWAI 노드 시작(인터럽트 코어 %d)", core);
    return ESP_OK;
}

/* ============================== 송신(ISO-TP 분할) ============================== */

struct can_bridge_ctx {
    twai_node_handle_t node;
    uint32_t tx_id;
    SemaphoreHandle_t fc_sem;
    volatile uint8_t fc_status;
    volatile uint8_t fc_pending;
    /* 2026-09-25(실기 — 브에서 RL 결과 전송(can_consume)과 캠 프레임 릴레이(esp_now_relay)가
     * 같은 ctx로 동시에 can_bridge_send를 불러 두 메시지의 FF/CF가 섞임 -> 콘 "CF 순번 어긋남",
     * 브 "FC 타임아웃") — "ctx당 세션 1개" 규칙을 호출자에게 맡기지 않고 여기서 보장 */
    SemaphoreHandle_t send_mutex;
};

can_bridge_ctx_t *can_bridge_ctx_create(twai_node_handle_t node, uint32_t tx_id)
{
    can_bridge_ctx_t *ctx = (can_bridge_ctx_t *)psram_or_internal_alloc(sizeof(can_bridge_ctx_t));
    if (!ctx) {
        ESP_LOGE(TAG, "ctx 할당 실패");
        abort();
    }
    ctx->node = node;
    ctx->tx_id = tx_id;
    ctx->fc_sem = xSemaphoreCreateBinary();
    ctx->send_mutex = xSemaphoreCreateMutex();
    if (!ctx->fc_sem || !ctx->send_mutex) {
        ESP_LOGE(TAG, "ctx 세마포어/뮤텍스 생성 실패");
        abort();
    }
    ctx->fc_status = ISO_TP_FC_STATUS_CTS;
    ctx->fc_pending = 0;
    return ctx;
}

/* CAN RX 콜백 쪽에서, 이 ctx가 기다리는 FC 프레임을 받았을 때 호출 — can_test.c의
 * on_rx_done에서 PCI==FC이고 기대하던 방향이면 이걸 불러줘야 함 */
void can_bridge_ctx_notify_fc(can_bridge_ctx_t *ctx, const uint8_t *frame_data)
{
    if (!ctx->fc_pending) return;
    ctx->fc_status = frame_data[0] & 0x0F;
    ctx->fc_pending = 0;
    xSemaphoreGive(ctx->fc_sem);
}

static esp_err_t send_one_frame(can_bridge_ctx_t *ctx, const uint8_t data[8], uint8_t len)
{
    /* 2026-09-25 — 지역 프레임 포인터를 드라이버에 넘기지 않고 PSRAM 풀 슬롯에 복사해서 보냄
     * (can_bridge_tx_frame 주석 참고) */
    return can_bridge_tx_frame(ctx->node, ctx->tx_id, data, len, CAN_BRIDGE_DEFAULT_TIMEOUT_MS);
}

/* msg(app_header+payload, len바이트)를 통째로 ISO-TP로 쪼개 보냄. 성공 시 ESP_OK.
 * len<=7이면 SF 하나로 끝(FC 불필요). 그보다 크면 FF -> FC 대기 -> CF들 순서(BS=0/STmin=0
 * 고정이라 FC는 딱 한 번만 기다리면 나머지 CF는 곧바로 연속 전송) */
static esp_err_t can_bridge_send_locked(can_bridge_ctx_t *ctx, const uint8_t *msg, size_t len)
{
    if (len <= ISO_TP_SF_MAX_LEN) {
        uint8_t frame[8] = {0};
        frame[0] = (uint8_t)((ISO_TP_PCI_SF << 4) | len);
        memcpy(&frame[1], msg, len);
        return send_one_frame(ctx, frame, (uint8_t)(1 + len));
    }

    /* First Frame */
    uint8_t frame[8] = {0};
    frame[0] = (uint8_t)((ISO_TP_PCI_FF << 4) | ((len >> 8) & 0x0F));
    frame[1] = (uint8_t)(len & 0xFF);
    memcpy(&frame[2], msg, ISO_TP_FF_FIRST_LEN);
    xSemaphoreTake(ctx->fc_sem, 0);  /* 이전 세션에서 남았을 수 있는 FC 신호 비움 */
    ctx->fc_pending = 1;
    esp_err_t err = send_one_frame(ctx, frame, 8);
    if (err != ESP_OK) { ctx->fc_pending = 0; return err; }

    if (xSemaphoreTake(ctx->fc_sem, pdMS_TO_TICKS(CAN_BRIDGE_DEFAULT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "FC 타임아웃 — 전송 중단");
        ctx->fc_pending = 0;
        return ESP_ERR_TIMEOUT;
    }
    if (ctx->fc_status != ISO_TP_FC_STATUS_CTS) {
        ESP_LOGW(TAG, "FC status=%u(CTS 아님) — 전송 중단", ctx->fc_status);
        return ESP_FAIL;
    }

    size_t sent = ISO_TP_FF_FIRST_LEN;
    uint8_t seq = 1;
    while (sent < len) {
        size_t chunk = (len - sent) < ISO_TP_CF_MAX_LEN ? (len - sent) : ISO_TP_CF_MAX_LEN;
        uint8_t cf[8] = {0};
        cf[0] = (uint8_t)((ISO_TP_PCI_CF << 4) | (seq & 0x0F));
        memcpy(&cf[1], msg + sent, chunk);
        err = send_one_frame(ctx, cf, (uint8_t)(1 + chunk));
        if (err != ESP_OK) return err;
        sent += chunk;
        seq = (uint8_t)((seq + 1) & 0x0F);
        /* 2026-09-22에 CF마다 2ms 대기를 넣었음(연속 전송 시 버스 에러로 CF 유실 관측).
         * 2026-09-26(사진 전송 CAN 개선 1단계, 사용자 지시) — 제거하고 측정. 당시엔 지역변수
         * 프레임 포인터를 송신 큐에 넣던 버그(e3b8caf에서 수정)가 있어서, 그 증상이 이 버그
         * 때문이었을 수 있음. 청크 하나(CF 약 174개)에 이 대기만 약 350ms였음 */
    }
    return ESP_OK;
}

esp_err_t can_bridge_send(can_bridge_ctx_t *ctx, const uint8_t *msg, size_t len)
{
    if (!ctx || !msg) return ESP_ERR_INVALID_ARG;
    if (len == 0 || len > ISO_TP_MAX_MSG_LEN) return ESP_ERR_INVALID_ARG;

    /* ctx당 세션 1개 — 다른 태스크가 같은 ctx로 보내는 중이면 끝날 때까지 대기(메시지 드롭 금지
     * 정책이라 타임아웃 없이 기다림, 한 세션은 FC 타임아웃 500ms + CF 전송으로 유한시간 안에 끝남) */
    xSemaphoreTake(ctx->send_mutex, portMAX_DELAY);
    esp_err_t err = can_bridge_send_locked(ctx, msg, len);
    xSemaphoreGive(ctx->send_mutex);
    return err;
}

/* ---- 경로 분류(설계 Docs/설계_CAN링크_2026-09-26.md §2) ---- */
can_bridge_category_t can_bridge_path_for_esp_now_msg(uint8_t esp_now_msg_type)
{
    switch (esp_now_msg_type) {
        case ESP_NOW_MSG_PHOTO_META:
        case ESP_NOW_MSG_PHOTO_META_ACK:
        case ESP_NOW_MSG_PHOTO_CHUNK:
        case ESP_NOW_MSG_PHOTO_WINDOW_STATUS_REQUEST:
        case ESP_NOW_MSG_PHOTO_WINDOW_STATUS_ACK:
        case ESP_NOW_MSG_PHOTO_DONE:
        case ESP_NOW_MSG_PHOTO_DONE_ACK:
            return CAN_BRIDGE_CAT_DATA;
        default:
            return CAN_BRIDGE_CAT_CONTROL;
    }
}

can_bridge_category_t can_bridge_path_for_app_msg(const uint8_t *msg, size_t len)
{
    if (!msg || len < CAN_BRIDGE_APP_HEADER_LEN) return CAN_BRIDGE_CAT_CONTROL;
    if (msg[0] == CAN_DATA_RELAY && len >= CAN_BRIDGE_APP_HEADER_LEN + 2) {
        return can_bridge_path_for_esp_now_msg(msg[CAN_BRIDGE_APP_HEADER_LEN + 1]);
    }
    return CAN_BRIDGE_CAT_CONTROL;
}
