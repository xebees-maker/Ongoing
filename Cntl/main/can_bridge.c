#include "can_bridge.h"
#include "can_bridge_link.h"

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
 * BRIDGE_TO_CNTL로 수신). CONTROL 카테고리 없음(브와 동일 이유 — 1차 개발 범위엔 필요 없음).
 */

static const char *TAG = "can_bridge";

#define CAN_BRIDGE_TX_GPIO GPIO_NUM_15
#define CAN_BRIDGE_RX_GPIO GPIO_NUM_16
#define CAN_BRIDGE_BITRATE 500000

static twai_node_handle_t s_node = NULL;
static can_bridge_ctx_t *s_data_ctx;
static can_bridge_reassembly_t s_data_reasm;
static can_bridge_queue_t s_data_complete_q;
static can_bridge_recv_cb_t s_recv_cb;

/* 실제 CAN 송신(ISO-TP)은 세션 1개 원칙이라 동시 호출 금지 — reliable_request가 여러
 * 워커 태스크(기기별)에서 동시에 불릴 수 있으므로 송신 구간만 뮤텍스로 직렬화 */
static SemaphoreHandle_t s_send_mutex;

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

static twai_frame_t s_rx_frame;
static uint8_t       s_rx_buf[8];

static IRAM_ATTR bool on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    if (twai_node_receive_from_isr(handle, &s_rx_frame) == ESP_OK) {
        raw_frame_t rf;
        rf.id = s_rx_frame.header.id;
        rf.len = (uint8_t)s_rx_frame.buffer_len;
        if (rf.len > 8) rf.len = 8;
        memcpy(rf.data, s_rx_frame.buffer, rf.len);
        xQueueSendFromISR(s_raw_frame_q, &rf, &woken);
    }
    return woken == pdTRUE;
}

static IRAM_ATTR bool on_error(twai_node_handle_t handle, const twai_error_event_data_t *edata, void *ctx)
{
    ESP_EARLY_LOGW(TAG, "버스 에러: 0x%x", (unsigned)edata->err_flags.val);
    return false;
}

static IRAM_ATTR bool on_state_change(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *ctx)
{
    static const char *names[] = {"error_active", "error_warning", "error_passive", "bus_off"};
    ESP_EARLY_LOGW(TAG, "상태 전이: %s -> %s", names[edata->old_sta], names[edata->new_sta]);
    return false;
}

static void can_rx_task(void *arg)
{
    (void)arg;
    raw_frame_t rf;
    for (;;) {
        if (xQueueReceive(s_raw_frame_q, &rf, portMAX_DELAY) != pdTRUE) continue;
        if (rf.id != CAN_BRIDGE_ID_BRIDGE_TO_CNTL_DATA) continue;

        uint8_t pci = (rf.len >= 1) ? ((rf.data[0] >> 4) & 0x0F) : 0xFF;
        if (pci == ISO_TP_PCI_FC) { can_bridge_ctx_notify_fc(s_data_ctx, rf.data); continue; }

        uint8_t fc[8];
        if (can_bridge_reassembly_feed(&s_data_reasm, rf.data, rf.len, fc, &s_data_complete_q)) {
            twai_frame_t f = { .header.id = CAN_BRIDGE_ID_CNTL_TO_BRIDGE_DATA, .buffer = fc, .buffer_len = 8 };
            twai_node_transmit(s_node, &f, CAN_BRIDGE_DEFAULT_TIMEOUT_MS);
        }
    }
}

static void deliver_relay(const can_bridge_app_header_t *hdr, const uint8_t *payload, size_t payload_len)
{
    if (!s_recv_cb) return;
    /* 예전 I2C 브릿지 설계에서 이미 확인된 사실 그대로: hub.c의 recv_cb는 info->src_addr와
     * info->rx_ctrl->rssi 두 필드만 읽음. 나머지는 0으로 채워도 무방 */
    static wifi_pkt_rx_ctrl_t rx_ctrl;
    memset(&rx_ctrl, 0, sizeof(rx_ctrl));
    rx_ctrl.rssi = (int8_t)hdr->flags;

    static esp_now_recv_info_t info;
    memset(&info, 0, sizeof(info));
    static uint8_t src_addr[6];
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

static void can_consume_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t *data; size_t len;
        while (can_bridge_queue_pop(&s_data_complete_q, &data, &len)) {
            if (len > CAN_BRIDGE_APP_HEADER_LEN) {
                can_bridge_app_header_t hdr;
                memcpy(&hdr, data, sizeof(hdr));
                const uint8_t *body = data + CAN_BRIDGE_APP_HEADER_LEN;
                size_t body_len = len - CAN_BRIDGE_APP_HEADER_LEN;

                if (hdr.msg_type == CAN_DATA_RELAY) {
                    deliver_relay(&hdr, body, body_len);
                } else if (hdr.msg_type == CAN_DATA_RELIABLE_RESULT) {
                    deliver_reliable_result(&hdr, body, body_len);
                }
            }
            can_bridge_queue_pop_free(data);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void can_status_task(void *arg)
{
    (void)arg;
    for (;;) {
        twai_node_status_t status;
        if (twai_node_get_info(s_node, &status, NULL) == ESP_OK) {
            static const char *names[] = {"error_active", "error_warning", "error_passive", "bus_off"};
            ESP_LOGI(TAG, "상태=%s TEC=%u REC=%u DATA큐=%u(최대%u)",
                     names[status.state], (unsigned)status.tx_error_count, (unsigned)status.rx_error_count,
                     (unsigned)s_data_complete_q.count, (unsigned)s_data_complete_q.high_water_mark);
            if (status.state == TWAI_ERROR_BUS_OFF) twai_node_recover(s_node);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
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

    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    esp_err_t err = can_bridge_send(s_data_ctx, msg, CAN_BRIDGE_APP_HEADER_LEN + len);
    xSemaphoreGive(s_send_mutex);

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

    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    esp_err_t send_err = can_bridge_send(s_data_ctx, msg, off);
    xSemaphoreGive(s_send_mutex);
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
    s_send_mutex = xSemaphoreCreateMutex();
    s_pending_mutex = xSemaphoreCreateMutex();
    memset(s_pending, 0, sizeof(s_pending));

    static StaticQueue_t s_raw_frame_q_struct;
    uint8_t *raw_frame_q_storage = (uint8_t *)heap_caps_malloc(16 * sizeof(raw_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_raw_frame_q = xQueueCreateStatic(16, sizeof(raw_frame_t), raw_frame_q_storage, &s_raw_frame_q_struct);

    s_rx_frame.buffer = s_rx_buf;
    s_rx_frame.buffer_len = sizeof(s_rx_buf);

    can_bridge_queue_init(&s_data_complete_q);

    #define CAN_BRIDGE_DATA_BUF_LEN 1536
    uint8_t *data_buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_DATA_BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data_buf) data_buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_DATA_BUF_LEN, MALLOC_CAP_8BIT);
    can_bridge_reassembly_init(&s_data_reasm, data_buf, CAN_BRIDGE_DATA_BUF_LEN);

    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx = CAN_BRIDGE_TX_GPIO,
            .rx = CAN_BRIDGE_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing.bitrate = CAN_BRIDGE_BITRATE,
        .fail_retry_cnt = 3,
        .tx_queue_depth = 8,
    };
    esp_err_t err = twai_new_node_onchip(&node_cfg, &s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_new_node_onchip 실패: %s", esp_err_to_name(err));
        return;
    }

    twai_event_callbacks_t cbs = {
        .on_rx_done = on_rx_done,
        .on_error = on_error,
        .on_state_change = on_state_change,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(s_node, &cbs, NULL));
    ESP_ERROR_CHECK(twai_node_enable(s_node));

    s_data_ctx = can_bridge_ctx_create(s_node, CAN_BRIDGE_ID_CNTL_TO_BRIDGE_DATA);

    static StaticTask_t s_can_rx_tcb, s_can_consume_tcb, s_can_status_tcb;
    StackType_t *can_rx_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StackType_t *can_consume_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StackType_t *can_status_stack = (StackType_t *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    /* 2026-09-25(사용자 설계 — 통신/UI 코어 분리) — LVGL은 코어 0, CAN 통신은 코어 1에 고정.
     * 코어 1 안에서는 CAN(수신/소비)이 가장 높아야 함 — 통신 등급 17(project_cntl_task_priority_scheme),
     * power_relay(15)보다 위. can_status는 5초 주기 상태 로그뿐이라 낮은 5 유지 */
    xTaskCreateStaticPinnedToCore(can_rx_task, "can_rx", 4096 / sizeof(StackType_t), NULL, 17, can_rx_stack, &s_can_rx_tcb, 1);
    xTaskCreateStaticPinnedToCore(can_consume_task, "can_consume", 4096 / sizeof(StackType_t), NULL, 17, can_consume_stack, &s_can_consume_tcb, 1);
    xTaskCreateStaticPinnedToCore(can_status_task, "can_status", 3072 / sizeof(StackType_t), NULL, 5, can_status_stack, &s_can_status_tcb, 1);

    ESP_LOGI(TAG, "콘 CAN 링크 시작됨(TX=%d RX=%d %dbps)", CAN_BRIDGE_TX_GPIO, CAN_BRIDGE_RX_GPIO, CAN_BRIDGE_BITRATE);
}
