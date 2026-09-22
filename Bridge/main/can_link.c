#include "can_link.h"
#include "bridge_esp_now.h"
#include "ui_screen.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

/**
 * 2026-09-22 — 콘의 can_test.c에서 실기 검증된 ISO-TP 엔진(can_bridge_link)을 그대로 쓰되,
 * 이 프로젝트는 항상 BRIDGE 역할 고정(MAC 기반 역할판정 로직 제거 — 벤치테스트 때와 달리
 * 이제 콘 소스를 안 쓰므로 애초에 그 로직 자체가 없음).
 */

static const char *TAG = "can_link";

#define CAN_LINK_TX_GPIO GPIO_NUM_15
#define CAN_LINK_RX_GPIO GPIO_NUM_16
#define CAN_LINK_BITRATE 500000

static twai_node_handle_t s_node = NULL;

static can_bridge_ctx_t *s_ctrl_ctx;
static can_bridge_ctx_t *s_data_ctx;
static can_bridge_reassembly_t s_ctrl_reasm;
static can_bridge_reassembly_t s_data_reasm;
static can_bridge_queue_t s_ctrl_complete_q;
static can_bridge_queue_t s_data_complete_q;

can_bridge_ctx_t *can_link_get_data_ctx(void) { return s_data_ctx; }
can_bridge_ctx_t *can_link_get_ctrl_ctx(void) { return s_ctrl_ctx; }

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
        uint8_t pci = (rf.len >= 1) ? ((rf.data[0] >> 4) & 0x0F) : 0xFF;

        if (rf.id == CAN_BRIDGE_ID_CNTL_TO_BRIDGE_CONTROL) {
            if (pci == ISO_TP_PCI_FC) { can_bridge_ctx_notify_fc(s_ctrl_ctx, rf.data); continue; }
            uint8_t fc[8];
            if (can_bridge_reassembly_feed(&s_ctrl_reasm, rf.data, rf.len, fc, &s_ctrl_complete_q)) {
                twai_frame_t f = { .header.id = CAN_BRIDGE_ID_BRIDGE_TO_CNTL_CONTROL, .buffer = fc, .buffer_len = 8 };
                twai_node_transmit(s_node, &f, CAN_BRIDGE_DEFAULT_TIMEOUT_MS);
            }
        } else if (rf.id == CAN_BRIDGE_ID_CNTL_TO_BRIDGE_DATA) {
            if (pci == ISO_TP_PCI_FC) { can_bridge_ctx_notify_fc(s_data_ctx, rf.data); continue; }
            uint8_t fc[8];
            if (can_bridge_reassembly_feed(&s_data_reasm, rf.data, rf.len, fc, &s_data_complete_q)) {
                twai_frame_t f = { .header.id = CAN_BRIDGE_ID_BRIDGE_TO_CNTL_DATA, .buffer = fc, .buffer_len = 8 };
                twai_node_transmit(s_node, &f, CAN_BRIDGE_DEFAULT_TIMEOUT_MS);
            }
        }
        /* 그 외(내가 보낸 것의 echo 등)는 무시 */
    }
}

static void can_consume_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t *data; size_t len;
        while (can_bridge_queue_pop(&s_ctrl_complete_q, &data, &len)) {
            if (len >= CAN_BRIDGE_APP_HEADER_LEN) {
                can_bridge_app_header_t hdr;
                memcpy(&hdr, data, sizeof(hdr));
                ui_screen_log_can("CTRL rx type=%u mac=%02X%02X%02X%02X%02X%02X",
                                   hdr.msg_type, hdr.mac[0], hdr.mac[1], hdr.mac[2], hdr.mac[3], hdr.mac[4], hdr.mac[5]);
                if (hdr.msg_type == CAN_CTRL_PING) {
                    can_bridge_app_header_t pong = { .msg_type = CAN_CTRL_PONG, .flags = 0 };
                    can_bridge_send(s_ctrl_ctx, (const uint8_t *)&pong, sizeof(pong));
                }
            }
            can_bridge_queue_pop_free(data);
        }
        while (can_bridge_queue_pop(&s_data_complete_q, &data, &len)) {
            if (len > CAN_BRIDGE_APP_HEADER_LEN) {
                can_bridge_app_header_t hdr;
                memcpy(&hdr, data, sizeof(hdr));
                uint16_t payload_len = (uint16_t)(len - CAN_BRIDGE_APP_HEADER_LEN);
                ui_screen_log_can("DATA rx mac=%02X%02X%02X%02X%02X%02X len=%u -> ESP-NOW 전송",
                                   hdr.mac[0], hdr.mac[1], hdr.mac[2], hdr.mac[3], hdr.mac[4], hdr.mac[5], payload_len);
                /* 콘이 CAM에 보내라고 준 원본 프레임 — 그대로 ESP-NOW로 내보냄(투명 릴레이) */
                bridge_esp_now_send_raw(hdr.mac, data + CAN_BRIDGE_APP_HEADER_LEN, payload_len);
            }
            can_bridge_queue_pop_free(data);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void can_status_task(void *arg)
{
    (void)arg;
    for (;;) {
        twai_node_status_t status;
        if (twai_node_get_info(s_node, &status, NULL) == ESP_OK) {
            static const char *names[] = {"error_active", "error_warning", "error_passive", "bus_off"};
            ESP_LOGI(TAG, "상태=%s TEC=%u REC=%u CTRL큐=%u(최대%u) DATA큐=%u(최대%u)",
                     names[status.state], (unsigned)status.tx_error_count, (unsigned)status.rx_error_count,
                     (unsigned)s_ctrl_complete_q.count, (unsigned)s_ctrl_complete_q.high_water_mark,
                     (unsigned)s_data_complete_q.count, (unsigned)s_data_complete_q.high_water_mark);
            if (status.state == TWAI_ERROR_BUS_OFF) twai_node_recover(s_node);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void can_link_init(void)
{
    s_raw_frame_q = xQueueCreate(16, sizeof(raw_frame_t));
    s_rx_frame.buffer = s_rx_buf;
    s_rx_frame.buffer_len = sizeof(s_rx_buf);

    can_bridge_queue_init(&s_ctrl_complete_q);
    can_bridge_queue_init(&s_data_complete_q);

    uint8_t *ctrl_buf = (uint8_t *)heap_caps_malloc(256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctrl_buf) ctrl_buf = (uint8_t *)heap_caps_malloc(256, MALLOC_CAP_8BIT);
    /* ESP-NOW v2 최대 프레임(1470B) + 앱헤더(8B) — bridge_esp_now.c와 맞춤 */
    #define CAN_LINK_DATA_BUF_LEN 1536
    uint8_t *data_buf = (uint8_t *)heap_caps_malloc(CAN_LINK_DATA_BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data_buf) data_buf = (uint8_t *)heap_caps_malloc(CAN_LINK_DATA_BUF_LEN, MALLOC_CAP_8BIT);
    can_bridge_reassembly_init(&s_ctrl_reasm, ctrl_buf, 256);
    can_bridge_reassembly_init(&s_data_reasm, data_buf, CAN_LINK_DATA_BUF_LEN);

    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx = CAN_LINK_TX_GPIO,
            .rx = CAN_LINK_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing.bitrate = CAN_LINK_BITRATE,
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

    s_ctrl_ctx = can_bridge_ctx_create(s_node, CAN_BRIDGE_ID_BRIDGE_TO_CNTL_CONTROL);
    s_data_ctx = can_bridge_ctx_create(s_node, CAN_BRIDGE_ID_BRIDGE_TO_CNTL_DATA);

    xTaskCreate(can_rx_task, "can_rx", 4096, NULL, 10, NULL);
    xTaskCreate(can_consume_task, "can_consume", 4096, NULL, 10, NULL);
    xTaskCreate(can_status_task, "can_status", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "브 CAN 링크 시작됨(TX=%d RX=%d %dbps)", CAN_LINK_TX_GPIO, CAN_LINK_RX_GPIO, CAN_LINK_BITRATE);
}
