#include "can_bridge.h"
#include "can_bridge_link.h"
#include "photo_rx.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

/**
 * 2026-09-23 — 콘 쪽 CAN 링크. 브의 can_link.c와 방향만 반대(콘: CNTL_TO_BRIDGE로 송신,
 * BRIDGE_TO_CNTL로 수신).
 * 2026-09-26(설계 Docs/설계_CAN링크_2026-09-26.md §2~3, 2-②) — CONTROL/DATA 두 경로 수신.
 * can_rx가 ID로 나눠 각자의 조립기·완성 큐에 넣고, 경로마다 소비 태스크가 따로 돔(Control 17 /
 * Data 15) — 사진 청크 처리 중에도 WAKE_HELLO/결과가 기다리지 않게.
 */

static const char *TAG = "can_bridge";

#define CAN_BRIDGE_TX_GPIO GPIO_NUM_15
#define CAN_BRIDGE_RX_GPIO GPIO_NUM_16
#define CAN_BRIDGE_BITRATE 1000000  /* 2026-09-26(사진 전송 CAN 개선 2단계) — 500k -> 1M, 브와 반드시 같은 값 */

static twai_node_handle_t s_node = NULL;
static can_bridge_ctx_t *s_data_ctx;
static can_bridge_reassembly_t s_data_reasm;
static can_bridge_queue_t s_data_complete_q;
static can_bridge_ctx_t *s_ctrl_ctx;
static can_bridge_reassembly_t s_ctrl_reasm;
static can_bridge_queue_t s_ctrl_complete_q;
static can_bridge_recv_cb_t s_recv_cb;


/* ---- 미결 reliable 요청 테이블 — MAC당 1개 원칙(esp_now_reliable과 동일 전제), 여러
 * 기기(다른 MAC)는 동시에 대기 가능 ---- */
#define CAN_BRIDGE_MAX_PENDING 8
typedef struct {
    uint8_t  in_use;
    uint8_t  mac[6];
    SemaphoreHandle_t done_sem;
    uint8_t  ok;
    uint8_t *reply_out;
    size_t   reply_out_cap;
    size_t   reply_len;
} pending_request_t;

static pending_request_t s_pending[CAN_BRIDGE_MAX_PENDING];
static SemaphoreHandle_t s_pending_mutex;

typedef struct {
    uint32_t id;
    uint8_t  len;
    uint8_t  data[8];
} raw_frame_t;
static QueueHandle_t s_raw_frame_q;
static volatile uint32_t s_raw_drop = 0;  /* ISR 원시 프레임 큐가 차서 버린 수 */
/* 2026-09-26(버스 에러 조사) — 에러를 트래픽 양으로 나눠 비교하려고 셈 */
static volatile uint32_t s_rx_frames = 0, s_tx_frames = 0, s_bus_errs = 0, s_state_changes = 0;

static TaskHandle_t s_status_task = NULL;  /* 버스 오프 시 on_state_change가 깨움 */

static twai_frame_t s_rx_frame;
static uint8_t       s_rx_buf[8];

static bool on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    if (twai_node_receive_from_isr(handle, &s_rx_frame) == ESP_OK) {
        s_rx_frames++;
        raw_frame_t rf;
        rf.id = s_rx_frame.header.id;
        rf.len = (uint8_t)s_rx_frame.buffer_len;
        if (rf.len > 8) rf.len = 8;
        memcpy(rf.data, s_rx_frame.buffer, rf.len);
        /* 2026-09-26 — 큐(16칸)가 차면 프레임이 조용히 버려져 CF 순번 어긋남으로 나타날 수 있음 → 셈 */
        if (xQueueSendFromISR(s_raw_frame_q, &rf, &woken) != pdTRUE) s_raw_drop++;
    }
    return woken == pdTRUE;
}

/* 송신 완료(성공/실패 무관) — 송신 풀 슬롯 반납 */
static bool on_tx_done(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *ctx)
{
    (void)handle;
    (void)ctx;
    s_tx_frames++;
    return can_bridge_tx_pool_on_done_isr(edata);
}

static bool on_error(twai_node_handle_t handle, const twai_error_event_data_t *edata, void *ctx)
{
    /* 0x1(arb_lost, 중재 패배)는 CAN 정상 동작(동시 송신 시 ID 높은 쪽이 양보) — 에러 수에서 제외 */
    if (edata->err_flags.val & ~0x1u) s_bus_errs++;
    return false;
}

static bool on_state_change(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *ctx)
{
    /* 2026-09-26 — ISR 안에서 로그 안 찍음(콘솔 출력 대기로 ISR이 붙잡힘). 현재 상태는 can_status_task가 찍음 */
    s_state_changes++;
    BaseType_t woken = pdFALSE;
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        /* 드라이버가 버린 송신 중 프레임 정리(can_bridge_tx_pool_on_bus_off_isr 주석) + 복구를 5초 주기 확인에
         * 맡기지 않고 상태 태스크를 바로 깨워 twai_node_recover() */
        if (can_bridge_tx_pool_on_bus_off_isr()) woken = pdTRUE;
        if (s_status_task) vTaskNotifyGiveFromISR(s_status_task, &woken);
    }
    return woken == pdTRUE;
}

static void can_rx_task(void *arg)
{
    (void)arg;
    raw_frame_t rf;
    for (;;) {
        if (xQueueReceive(s_raw_frame_q, &rf, portMAX_DELAY) != pdTRUE) continue;

        /* 2026-09-26 — ID로 경로 선택. 받는 쪽 FC는 자기 경로의 송신 ID로(현행 원칙) */
        can_bridge_ctx_t *ctx;
        can_bridge_reassembly_t *reasm;
        can_bridge_queue_t *complete_q;
        uint32_t fc_id;
        if (rf.id == CAN_BRIDGE_ID_BRIDGE_TO_CNTL_CONTROL) {
            ctx = s_ctrl_ctx; reasm = &s_ctrl_reasm; complete_q = &s_ctrl_complete_q;
            fc_id = CAN_BRIDGE_ID_CNTL_TO_BRIDGE_CONTROL;
        } else if (rf.id == CAN_BRIDGE_ID_BRIDGE_TO_CNTL_DATA) {
            ctx = s_data_ctx; reasm = &s_data_reasm; complete_q = &s_data_complete_q;
            fc_id = CAN_BRIDGE_ID_CNTL_TO_BRIDGE_DATA;
        } else {
            continue;  /* 콘→브 방향 ID(자기 송신 에코 없음)나 모르는 ID */
        }

        uint8_t pci = (rf.len >= 1) ? ((rf.data[0] >> 4) & 0x0F) : 0xFF;
        if (pci == ISO_TP_PCI_FC) { can_bridge_ctx_notify_fc(ctx, rf.data); continue; }

        uint8_t fc[8];
        if (can_bridge_reassembly_feed(reasm, rf.data, rf.len, fc, complete_q)) {
            /* 2026-09-25 — 지역 프레임을 드라이버에 넘기면 반환 뒤 ISR이 덮인 스택을 읽음(실기 크래시).
             * PSRAM 송신 풀 슬롯에 복사해서 보냄 */
            esp_err_t fc_err = can_bridge_tx_frame(s_node, fc_id, fc, 8, CAN_BRIDGE_DEFAULT_TIMEOUT_MS);
            if (fc_err != ESP_OK) {
                ESP_LOGW(TAG, "FC 송신 실패(id=0x%x): %s", (unsigned)fc_id, esp_err_to_name(fc_err));
            }
        }
    }
}

static void deliver_relay(const can_bridge_app_header_t *hdr, const uint8_t *payload, size_t payload_len)
{
    if (!s_recv_cb) return;
    /* 예전 I2C 브릿지 설계에서 이미 확인된 사실 그대로: hub.c의 recv_cb는 info->src_addr와
     * info->rx_ctrl->rssi 두 필드만 읽음. 나머지는 0으로 채워도 무방.
     * 2026-09-26 — 소비 태스크가 경로별 2개라 동시에 불릴 수 있음 → static 대신 지역 변수(작음) */
    wifi_pkt_rx_ctrl_t rx_ctrl;
    memset(&rx_ctrl, 0, sizeof(rx_ctrl));
    rx_ctrl.rssi = (int8_t)hdr->flags;

    esp_now_recv_info_t info;
    memset(&info, 0, sizeof(info));
    uint8_t src_addr[6];
    memcpy(src_addr, hdr->mac, 6);
    info.src_addr = src_addr;
    info.rx_ctrl = &rx_ctrl;

    s_recv_cb(&info, payload, (int)payload_len);
}

static void deliver_reliable_result(const can_bridge_app_header_t *hdr, const uint8_t *body, size_t body_len)
{
    if (body_len < sizeof(can_bridge_reliable_result_hdr_t)) return;
    can_bridge_reliable_result_hdr_t result_hdr;
    memcpy(&result_hdr, body, sizeof(result_hdr));
    const uint8_t *reply = body + sizeof(result_hdr);
    size_t reply_len = body_len - sizeof(result_hdr);

    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    for (int i = 0; i < CAN_BRIDGE_MAX_PENDING; i++) {
        if (s_pending[i].in_use && memcmp(s_pending[i].mac, hdr->mac, 6) == 0) {
            s_pending[i].ok = result_hdr.ok;
            if (result_hdr.ok && s_pending[i].reply_out && reply_len > 0) {
                size_t copy_len = reply_len < s_pending[i].reply_out_cap ? reply_len : s_pending[i].reply_out_cap;
                memcpy(s_pending[i].reply_out, reply, copy_len);
                s_pending[i].reply_len = copy_len;
            }
            xSemaphoreGive(s_pending[i].done_sem);
            break;
        }
    }
    xSemaphoreGive(s_pending_mutex);
}

/* 2026-09-25(사용자 지시 — 이벤트 방식) — 예전엔 큐가 비면 vTaskDelay(20ms) 후 다시 확인하는
 * 폴링이었음(지시는 이벤트였는데 잘못 구현). 이제 큐를 비운 뒤 태스크 알림을 기다림 — 알림은
 * can_bridge_queue_push()가 완성된 메시지를 넣을 때만 보냄(can_bridge_queue_set_notify_task) */
/* 2026-09-26 — 경로마다 하나씩(arg = 그 경로의 완성 큐). 처리 내용은 같음(RELAY/RESULT) */
static void can_consume_task(void *arg)
{
    can_bridge_queue_t *q = (can_bridge_queue_t *)arg;
    for (;;) {
        uint8_t *data; size_t len;
        while (can_bridge_queue_pop(q, &data, &len)) {
            if (len > CAN_BRIDGE_APP_HEADER_LEN) {
                can_bridge_app_header_t hdr;
                memcpy(&hdr, data, sizeof(hdr));
                const uint8_t *body = data + CAN_BRIDGE_APP_HEADER_LEN;
                size_t body_len = len - CAN_BRIDGE_APP_HEADER_LEN;

                if (hdr.msg_type == CAN_DATA_RELAY) {
                    deliver_relay(&hdr, body, body_len);
                } else if (hdr.msg_type == CAN_DATA_RELIABLE_RESULT) {
                    deliver_reliable_result(&hdr, body, body_len);
                } else if (hdr.msg_type == CAN_DATA_SR_META || hdr.msg_type == CAN_DATA_SR_CHUNK ||
                           hdr.msg_type == CAN_DATA_SR_DONE) {
                    /* 2026-09-26(설계 §4, 4단계) — 브가 순서를 맞춘 사진 스트림(Data 경로 → data_consume) */
                    photo_rx_on_sr_stream(hdr.msg_type, hdr.mac, body, body_len);
                }
            }
            can_bridge_queue_pop_free(data);
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

static void can_status_task(void *arg)
{
    (void)arg;
    for (;;) {
        twai_node_status_t status;
        if (twai_node_get_info(s_node, &status, NULL) == ESP_OK) {
            static const char *names[] = {"error_active", "error_warning", "error_passive", "bus_off"};
            uint32_t q_count, q_hwm, cq_count, cq_hwm;
            can_bridge_queue_get_stats(&s_data_complete_q, &q_count, &q_hwm);
            can_bridge_queue_get_stats(&s_ctrl_complete_q, &cq_count, &cq_hwm);
            ESP_LOGI(TAG, "상태=%s TEC=%u REC=%u CTRL큐=%u(최대%u) DATA큐=%u(최대%u) 수신버림=%u RX=%u TX=%u 버스에러=%u 상태전이=%u",
                     names[status.state], (unsigned)status.tx_error_count, (unsigned)status.rx_error_count,
                     (unsigned)cq_count, (unsigned)cq_hwm, (unsigned)q_count, (unsigned)q_hwm, (unsigned)s_raw_drop,
                     (unsigned)s_rx_frames, (unsigned)s_tx_frames, (unsigned)s_bus_errs, (unsigned)s_state_changes);
            if (status.state == TWAI_ERROR_BUS_OFF) twai_node_recover(s_node);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));  /* 5초 주기, 버스 오프면 즉시 */
    }
}

/* 2026-09-26(설계 §2, 2-④) — 콘→브 송신: 경로 분류(can_bridge_path_for_app_msg)로 Control/Data ctx
 * 선택. ISO-TP 세션 1개 원칙은 ctx마다의 송신 뮤텍스(can_bridge_send 내부)가 지킴 — 예전의 별도
 * s_send_mutex는 그 중복이라 제거(두 경로가 서로를 기다리지 않게) */
/* 2026-09-26(사용자 설계 — CAN 관련은 전부 코어 1) — CAN 송신은 TWAI 인터럽트와 같은 코어 1에 고정된 태스크에서만
 * 해야 함(can_bridge_node_start_on_core 주석). 콘은 node_request 작업 태스크(코어 미고정), UI 등 여러 곳에서
 * 보내므로, 코어 1에 고정되지 않은 호출은 경로별 코어 1 송신 태스크(Control 17 / Data 15)에 넘기고 결과를 기다림.
 * 요청은 호출자 스택에 있고 완료 신호까지 호출자가 기다리므로 복사 없음 */
typedef struct {
    can_bridge_ctx_t *ctx;
    const uint8_t *msg;
    size_t len;
    esp_err_t err;
    SemaphoreHandle_t done;
    StaticSemaphore_t done_buf;
} tx_req_t;
static QueueHandle_t s_ctrl_tx_q, s_data_tx_q;

static void can_tx_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    for (;;) {
        tx_req_t *r;
        if (xQueueReceive(q, &r, portMAX_DELAY) == pdTRUE) {
            r->err = can_bridge_send(r->ctx, r->msg, r->len);
            xSemaphoreGive(r->done);
        }
    }
}

static esp_err_t send_app_msg(const uint8_t *msg, size_t len)
{
    bool control = (can_bridge_path_for_app_msg(msg, len) == CAN_BRIDGE_CAT_CONTROL);
    can_bridge_ctx_t *ctx = control ? s_ctrl_ctx : s_data_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;
    if (xTaskGetCoreID(xTaskGetCurrentTaskHandle()) == 1) return can_bridge_send(ctx, msg, len);

    tx_req_t r = { .ctx = ctx, .msg = msg, .len = len, .err = ESP_FAIL };
    r.done = xSemaphoreCreateBinaryStatic(&r.done_buf);
    tx_req_t *rp = &r;
    xQueueSend(control ? s_ctrl_tx_q : s_data_tx_q, &rp, portMAX_DELAY);
    xSemaphoreTake(r.done, portMAX_DELAY);
    vSemaphoreDelete(r.done);
    return r.err;
}

esp_err_t can_bridge_relay_send(const uint8_t *mac, const void *data, size_t len)
{
    uint8_t *msg = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!msg) msg = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + len, MALLOC_CAP_8BIT);
    if (!msg) return ESP_ERR_NO_MEM;

    can_bridge_app_header_t hdr = { .msg_type = CAN_DATA_RELAY, .flags = 0 };
    memcpy(hdr.mac, mac, 6);
    memcpy(msg, &hdr, CAN_BRIDGE_APP_HEADER_LEN);
    memcpy(msg + CAN_BRIDGE_APP_HEADER_LEN, data, len);

    esp_err_t err = send_app_msg(msg, CAN_BRIDGE_APP_HEADER_LEN + len);

    free(msg);
    return err;
}

esp_err_t can_bridge_reliable_request(const uint8_t *peer_mac,
                                       const void *req, size_t req_len,
                                       const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                                       uint32_t timeout_ms, int max_attempts,
                                       void *reply_out, size_t reply_out_cap, size_t *reply_out_len)
{
    if (accept_reply_types_count > CAN_BRIDGE_RELIABLE_MAX_ACCEPT_TYPES) return ESP_ERR_INVALID_ARG;

    /* 미결 테이블에 슬롯 등록 */
    int slot = -1;
    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    for (int i = 0; i < CAN_BRIDGE_MAX_PENDING; i++) {
        if (!s_pending[i].in_use) {
            slot = i;
            s_pending[i].in_use = 1;
            memcpy(s_pending[i].mac, peer_mac, 6);
            s_pending[i].ok = 0;
            s_pending[i].reply_out = (uint8_t *)reply_out;
            s_pending[i].reply_out_cap = reply_out_cap;
            s_pending[i].reply_len = 0;
            if (!s_pending[i].done_sem) s_pending[i].done_sem = xSemaphoreCreateBinary();
            xSemaphoreTake(s_pending[i].done_sem, 0);  /* 혹시 남아있던 신호 비움 */
            break;
        }
    }
    xSemaphoreGive(s_pending_mutex);
    if (slot < 0) {
        ESP_LOGE(TAG, "미결 요청 테이블 가득참(%d) — mac=%02X%02X%02X%02X%02X%02X",
                 CAN_BRIDGE_MAX_PENDING, peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
        return ESP_ERR_NO_MEM;
    }

    size_t msg_len = CAN_BRIDGE_APP_HEADER_LEN + sizeof(can_bridge_reliable_send_hdr_t) + req_len;
    uint8_t *msg = (uint8_t *)heap_caps_malloc(msg_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!msg) msg = (uint8_t *)heap_caps_malloc(msg_len, MALLOC_CAP_8BIT);
    if (!msg) {
        s_pending[slot].in_use = 0;
        return ESP_ERR_NO_MEM;
    }

    can_bridge_app_header_t app_hdr = { .msg_type = CAN_DATA_RELIABLE_SEND, .flags = 0 };
    memcpy(app_hdr.mac, peer_mac, 6);
    can_bridge_reliable_send_hdr_t send_hdr = {
        .timeout_ms = timeout_ms,
        .max_attempts = (uint8_t)max_attempts,
        .accept_reply_types_count = (uint8_t)accept_reply_types_count,
    };
    memcpy(send_hdr.accept_reply_types, accept_reply_types, accept_reply_types_count);

    size_t off = 0;
    memcpy(msg + off, &app_hdr, CAN_BRIDGE_APP_HEADER_LEN); off += CAN_BRIDGE_APP_HEADER_LEN;
    memcpy(msg + off, &send_hdr, sizeof(send_hdr)); off += sizeof(send_hdr);
    memcpy(msg + off, req, req_len); off += req_len;

    esp_err_t send_err = send_app_msg(msg, off);
    free(msg);

    if (send_err != ESP_OK) {
        s_pending[slot].in_use = 0;
        return send_err;
    }

    /* 브가 내부에서 timeout_ms*max_attempts까지 걸릴 수 있으니, CAN 쪽 대기는 여유를 둠 */
    TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)timeout_ms * (uint32_t)(max_attempts > 0 ? max_attempts : 1) + 2000);
    esp_err_t ret;
    if (xSemaphoreTake(s_pending[slot].done_sem, wait_ticks) == pdTRUE) {
        if (s_pending[slot].ok) {
            if (reply_out_len) *reply_out_len = s_pending[slot].reply_len;
            ret = ESP_OK;
        } else {
            ret = ESP_ERR_TIMEOUT;
        }
    } else {
        ESP_LOGW(TAG, "CAN 레벨 타임아웃(브 응답 없음) — mac=%02X%02X%02X%02X%02X%02X",
                 peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
        ret = ESP_ERR_TIMEOUT;
    }
    s_pending[slot].in_use = 0;
    return ret;
}

void can_bridge_init(can_bridge_recv_cb_t recv_cb)
{
    s_recv_cb = recv_cb;
    s_pending_mutex = xSemaphoreCreateMutex();
    memset(s_pending, 0, sizeof(s_pending));

    static StaticQueue_t s_raw_frame_q_struct;
    /* 2026-09-26 — 16칸은 두 경로가 동시에 흐를 때 모자라서 대량으로 버려짐(실기: 콘 2901개/90초 → CF 순번
         * 어긋남). 256칸으로 늘림.
         * 2026-09-26(사용자 설계 — 가급적 PSRAM) — PSRAM에 둠. TWAI 인터럽트는 IRAM 플래그 없이 할당돼서
         * 플래시 쓰기(캐시 꺼짐) 동안엔 IDF가 아예 막아두므로, 그 사이 PSRAM에 접근할 일이 없음 */
        #define RAW_FRAME_Q_LEN 256
        uint8_t *raw_frame_q_storage = (uint8_t *)heap_caps_malloc(RAW_FRAME_Q_LEN * sizeof(raw_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!raw_frame_q_storage) raw_frame_q_storage = (uint8_t *)heap_caps_malloc(RAW_FRAME_Q_LEN * sizeof(raw_frame_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_raw_frame_q = xQueueCreateStatic(RAW_FRAME_Q_LEN, sizeof(raw_frame_t), raw_frame_q_storage, &s_raw_frame_q_struct);

    s_rx_frame.buffer = s_rx_buf;
    s_rx_frame.buffer_len = sizeof(s_rx_buf);

    can_bridge_queue_init(&s_data_complete_q);
    can_bridge_queue_init(&s_ctrl_complete_q);

    #define CAN_BRIDGE_DATA_BUF_LEN 1536
    uint8_t *data_buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_DATA_BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data_buf) data_buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_DATA_BUF_LEN, MALLOC_CAP_8BIT);
    can_bridge_reassembly_init(&s_data_reasm, data_buf, CAN_BRIDGE_DATA_BUF_LEN);
    uint8_t *ctrl_buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_DATA_BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctrl_buf) ctrl_buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_DATA_BUF_LEN, MALLOC_CAP_8BIT);
    can_bridge_reassembly_init(&s_ctrl_reasm, ctrl_buf, CAN_BRIDGE_DATA_BUF_LEN);

    /* 2026-09-26(사용자 설계 — CAN 관련은 전부 코어 1) — TWAI 인터럽트를 코어 1에 잡음(노드를 코어 1 태스크에서
     * 생성). 이 함수는 node_hub_init()→app_main(코어 0)에서 불림 */
    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx = CAN_BRIDGE_TX_GPIO,
            .rx = CAN_BRIDGE_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing.bitrate = CAN_BRIDGE_BITRATE,
        .fail_retry_cnt = -1,  /* 2026-09-26(사용자 지시) — CAN 표준 동작: 성공할 때까지 하드웨어가 재전송(버스 오프는 on_state_change에서 정리+즉시 복구) */
        .tx_queue_depth = 8,
    };
    twai_event_callbacks_t cbs = {
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx_done,
        .on_error = on_error,
        .on_state_change = on_state_change,
    };
    esp_err_t err = can_bridge_node_start_on_core(&node_cfg, &cbs, 1, &s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAN 노드 시작 실패: %s", esp_err_to_name(err));
        return;
    }

    /* 코어 1 송신 태스크(send_app_msg 주석) — ctx보다 먼저 만들어 둠(ctx가 생기는 순간부터 send_app_msg가 쓸 수 있음) */
    s_ctrl_tx_q = xQueueCreate(8, sizeof(tx_req_t *));
    s_data_tx_q = xQueueCreate(8, sizeof(tx_req_t *));
    static StaticTask_t s_ctrl_tx_tcb, s_data_tx_tcb;
    StackType_t *ctrl_tx_stack = (StackType_t *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    StackType_t *data_tx_stack = (StackType_t *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    xTaskCreateStaticPinnedToCore(can_tx_task, "ctrl_tx", 3072 / sizeof(StackType_t), s_ctrl_tx_q, 17, ctrl_tx_stack, &s_ctrl_tx_tcb, 1);
    xTaskCreateStaticPinnedToCore(can_tx_task, "data_tx", 3072 / sizeof(StackType_t), s_data_tx_q, 15, data_tx_stack, &s_data_tx_tcb, 1);

    s_data_ctx = can_bridge_ctx_create(s_node, CAN_BRIDGE_ID_CNTL_TO_BRIDGE_DATA);
    s_ctrl_ctx = can_bridge_ctx_create(s_node, CAN_BRIDGE_ID_CNTL_TO_BRIDGE_CONTROL);

    static StaticTask_t s_can_rx_tcb, s_can_consume_tcb, s_ctrl_consume_tcb, s_can_status_tcb;
    StackType_t *can_rx_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StackType_t *can_consume_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StackType_t *ctrl_consume_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StackType_t *can_status_stack = (StackType_t *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    /* 2026-09-25(사용자 설계 — 통신/UI 코어 분리) — LVGL은 코어 0, CAN 통신은 코어 1에 고정.
     * 코어 1 안에서는 CAN(수신/소비)이 가장 높아야 함 — 통신 등급 17(project_cntl_task_priority_scheme),
     * power_relay(15)보다 위. can_status는 5초 주기 상태 로그뿐이라 낮은 5 유지 */
    /* 소비 태스크를 먼저 만들고 알림 대상으로 등록한 뒤에 수신 태스크를 띄움 — 첫 push부터
     * 알림이 가도록(등록 전에 들어온 게 있어도 소비 태스크의 첫 비우기에서 처리됨) */
    /* 2026-09-26(설계 §3) — 경로별 소비 태스크: Control 17, Data(SR) 15 */
    TaskHandle_t ctrl_task = xTaskCreateStaticPinnedToCore(can_consume_task, "ctrl_consume", 4096 / sizeof(StackType_t), &s_ctrl_complete_q, 17, ctrl_consume_stack, &s_ctrl_consume_tcb, 1);
    can_bridge_queue_set_notify_task(&s_ctrl_complete_q, ctrl_task);
    TaskHandle_t consume_task = xTaskCreateStaticPinnedToCore(can_consume_task, "data_consume", 4096 / sizeof(StackType_t), &s_data_complete_q, 15, can_consume_stack, &s_can_consume_tcb, 1);
    can_bridge_queue_set_notify_task(&s_data_complete_q, consume_task);
    xTaskCreateStaticPinnedToCore(can_rx_task, "can_rx", 4096 / sizeof(StackType_t), NULL, 17, can_rx_stack, &s_can_rx_tcb, 1);
    s_status_task = xTaskCreateStaticPinnedToCore(can_status_task, "can_status", 3072 / sizeof(StackType_t), NULL, 5, can_status_stack, &s_can_status_tcb, 1);

    ESP_LOGI(TAG, "콘 CAN 링크 시작됨(TX=%d RX=%d %dbps)", CAN_BRIDGE_TX_GPIO, CAN_BRIDGE_RX_GPIO, CAN_BRIDGE_BITRATE);
}
