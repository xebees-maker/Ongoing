#include "can_link.h"
#include "bridge_esp_now.h"
#include "ui_screen.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_now_reliable.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

/**
 * 2026-09-23 — 1차 개발 범위(사용자 확정: 환경설정/화면/브 고유 초기화/I2C->CAN 교체/캠이
 * 브에 붙는 것 확인)에 맞춰 정리. 콘의 can_test.c에서 검증된 ISO-TP 엔진(can_bridge_link)을
 * 그대로 쓰되, CONTROL 카테고리(PING/PONG 등)는 이번 범위에 없어서 뺐음 — 브는 CASK 판단을
 * 전혀 안 하는 투명 릴레이(DATA 카테고리 하나)라 그것만 있으면 됨. 이전에 can_test.c의
 * PING/PONG 자동응답 로직을 그대로 옮겨왔던 건, 콘을 정리하기 전 옛 테스트 트래픽을 실제
 * ESP-NOW 전송으로 잘못 릴레이하는 부작용을 냈음(사용자 지적) — 필요 없는 걸 가져온 게
 * 원인이라 아예 제거.
 */

static const char *TAG = "can_link";

#define CAN_LINK_TX_GPIO GPIO_NUM_15
#define CAN_LINK_RX_GPIO GPIO_NUM_16
#define CAN_LINK_BITRATE 500000

static twai_node_handle_t s_node = NULL;

static can_bridge_ctx_t *s_data_ctx;
static can_bridge_reassembly_t s_data_reasm;
static can_bridge_queue_t s_data_complete_q;

can_bridge_ctx_t *can_link_get_data_ctx(void) { return s_data_ctx; }

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
        if (rf.id != CAN_BRIDGE_ID_CNTL_TO_BRIDGE_DATA) continue;  /* CONTROL 등 그 외는 무시(범위 밖) */

        uint8_t pci = (rf.len >= 1) ? ((rf.data[0] >> 4) & 0x0F) : 0xFF;
        if (pci == ISO_TP_PCI_FC) { can_bridge_ctx_notify_fc(s_data_ctx, rf.data); continue; }

        uint8_t fc[8];
        if (can_bridge_reassembly_feed(&s_data_reasm, rf.data, rf.len, fc, &s_data_complete_q)) {
            twai_frame_t f = { .header.id = CAN_BRIDGE_ID_BRIDGE_TO_CNTL_DATA, .buffer = fc, .buffer_len = 8 };
            twai_node_transmit(s_node, &f, CAN_BRIDGE_DEFAULT_TIMEOUT_MS);
        }
    }
}

/* 2026-09-23(1단계 — 콘의 esp_now_reliable_request()를 브가 대행) — 기존 esp_now_reliable
 * 컴포넌트를 그대로 호출(재구현 안 함, 사용자 지시). 여기서 블로킹(최대 timeout_ms*max_attempts)
 * 되는데, can_consume_task 하나가 DATA 전체를 순차 처리하는 기존 모델 그대로(이 링크는
 * 어차피 세션 1개 원칙) — 그 동안 다른 DATA 처리가 밀리는 건 의도된 동작 */
static void handle_reliable_send(const can_bridge_app_header_t *hdr, const uint8_t *body, size_t body_len)
{
    if (body_len < sizeof(can_bridge_reliable_send_hdr_t)) return;
    can_bridge_reliable_send_hdr_t send_hdr;
    memcpy(&send_hdr, body, sizeof(send_hdr));
    const uint8_t *req = body + sizeof(send_hdr);
    size_t req_len = body_len - sizeof(send_hdr);

    /* 2026-09-23(실기 디버깅 — PAIR_REQUEST 13회 전부 즉시 실패로 확인) — ESP-NOW는 peer로
     * 등록 안 된 MAC에 esp_now_send()를 부르면 즉시 ESP_ERR_ESPNOW_NOT_FOUND라, 이걸 빠뜨리면
     * esp_now_reliable_request()가 매 시도마다 즉시(무선 딜레이 없이) 실패함 — RF 타이밍/채널
     * 문제가 아니라 이 한 줄이 빠진 것이었음 */
    bridge_esp_now_ensure_peer(hdr->mac);

    /* 응답 버퍼 — ESP-NOW 최대 프레임 크기, PSRAM, 태스크 수명 동안 재사용(매 호출 malloc 안 함) */
    static uint8_t *s_reply_buf = NULL;
    if (!s_reply_buf) {
        s_reply_buf = (uint8_t *)heap_caps_malloc(1470, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_reply_buf) s_reply_buf = (uint8_t *)heap_caps_malloc(1470, MALLOC_CAP_8BIT);
    }
    size_t reply_len = 0;
    esp_err_t err = esp_now_reliable_request(hdr->mac, req, req_len,
                                              send_hdr.accept_reply_types, send_hdr.accept_reply_types_count,
                                              send_hdr.timeout_ms, send_hdr.max_attempts,
                                              s_reply_buf, 1470, &reply_len);

    /* 2026-09-23(사용자 지적) — RL(구 RLBL)은 CAN에서 받은 명령을 "처리하는 코드 위치" 기준이
     * 아니라 실제로 무선(ESP-NOW)으로 캠과 주고받은 결과를 설명하는 내용이라 무선 창이 맞음 */
    char m6[7]; ui_screen_mac6(hdr->mac, m6);
    const char *type_name = req_len >= 2 ? ui_screen_msg_type_name(req[1]) : "?";
    ui_screen_log_wireless("RL(%c/%s/%u/%s) %s", (err == ESP_OK) ? 'S' : 'F', m6,
                            (unsigned)req_len, ui_screen_result_code(err), type_name);

    /* 결과를 콘에 CAN으로 돌려줌 — app_header + result_hdr + (성공시)응답 페이로드 */
    uint8_t *result_msg = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + sizeof(can_bridge_reliable_result_hdr_t) + 1470,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!result_msg) result_msg = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + sizeof(can_bridge_reliable_result_hdr_t) + 1470, MALLOC_CAP_8BIT);
    if (!result_msg) return;

    can_bridge_app_header_t result_app_hdr = { .msg_type = CAN_DATA_RELIABLE_RESULT, .flags = 0 };
    memcpy(result_app_hdr.mac, hdr->mac, 6);
    can_bridge_reliable_result_hdr_t result_hdr = { .ok = (err == ESP_OK) ? (uint8_t)1 : (uint8_t)0 };

    size_t off = 0;
    memcpy(result_msg + off, &result_app_hdr, CAN_BRIDGE_APP_HEADER_LEN); off += CAN_BRIDGE_APP_HEADER_LEN;
    memcpy(result_msg + off, &result_hdr, sizeof(result_hdr)); off += sizeof(result_hdr);
    if (result_hdr.ok && reply_len > 0) {
        memcpy(result_msg + off, s_reply_buf, reply_len); off += reply_len;
    }

    can_bridge_send(s_data_ctx, result_msg, off);
    free(result_msg);
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
                uint8_t *body = data + CAN_BRIDGE_APP_HEADER_LEN;
                size_t body_len = len - CAN_BRIDGE_APP_HEADER_LEN;

                if (hdr.msg_type == CAN_DATA_RELAY) {
                    /* 2026-09-23(사용자 확인 — "DATA rx가 무선에 찍혔다") — 다시 보니 이 줄
                     * 자체는 코드상 CAN 창에 정상적으로 찍히는 게 맞고, 실제로는 그 직후
                     * bridge_esp_now_send_raw()가 무선 창에 "TX(...)"를 바로 이어서 찍어서
                     * 같은 이벤트가 두 창에 연달아 나오다 보니 헷갈린 것으로 보임(버그 아님) —
                     * 어차피 TX(...) 줄이 결과를 알려주므로 여기선 mac+len만 짧게 */
                    char m6[7]; ui_screen_mac6(hdr.mac, m6);
                    const char *type_name = body_len >= 2 ? ui_screen_msg_type_name(body[1]) : "?";
                    ui_screen_log_can("DATA rx(%s/%u) %s", m6, (unsigned)body_len, type_name);
                    /* 콘이 CAM에 보내라고 준 원본 프레임 — 그대로 ESP-NOW로 내보냄(투명 릴레이) */
                    bridge_esp_now_send_raw(hdr.mac, body, (uint16_t)body_len);
                } else if (hdr.msg_type == CAN_DATA_RELIABLE_SEND) {
                    handle_reliable_send(&hdr, body, body_len);
                }
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
            ESP_LOGI(TAG, "상태=%s TEC=%u REC=%u DATA큐=%u(최대%u)",
                     names[status.state], (unsigned)status.tx_error_count, (unsigned)status.rx_error_count,
                     (unsigned)s_data_complete_q.count, (unsigned)s_data_complete_q.high_water_mark);
            if (status.state == TWAI_ERROR_BUS_OFF) twai_node_recover(s_node);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void can_link_init(void)
{
    {
        static StaticQueue_t s_raw_frame_q_struct;
        uint8_t *raw_frame_q_storage = (uint8_t *)heap_caps_malloc(16 * sizeof(raw_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_raw_frame_q = xQueueCreateStatic(16, sizeof(raw_frame_t), raw_frame_q_storage, &s_raw_frame_q_struct);
    }
    s_rx_frame.buffer = s_rx_buf;
    s_rx_frame.buffer_len = sizeof(s_rx_buf);

    can_bridge_queue_init(&s_data_complete_q);

    /* ESP-NOW v2 최대 프레임(1470B) + 앱헤더(8B) — bridge_esp_now.c와 맞춤 */
    #define CAN_LINK_DATA_BUF_LEN 1536
    uint8_t *data_buf = (uint8_t *)heap_caps_malloc(CAN_LINK_DATA_BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data_buf) data_buf = (uint8_t *)heap_caps_malloc(CAN_LINK_DATA_BUF_LEN, MALLOC_CAP_8BIT);
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

    s_data_ctx = can_bridge_ctx_create(s_node, CAN_BRIDGE_ID_BRIDGE_TO_CNTL_DATA);

    static StaticTask_t s_can_rx_tcb, s_can_consume_tcb, s_can_status_tcb;
    StackType_t *can_rx_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StackType_t *can_consume_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StackType_t *can_status_stack = (StackType_t *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    xTaskCreateStatic(can_rx_task, "can_rx", 4096 / sizeof(StackType_t), NULL, 10, can_rx_stack, &s_can_rx_tcb);
    xTaskCreateStatic(can_consume_task, "can_consume", 4096 / sizeof(StackType_t), NULL, 10, can_consume_stack, &s_can_consume_tcb);
    xTaskCreateStatic(can_status_task, "can_status", 3072 / sizeof(StackType_t), NULL, 5, can_status_stack, &s_can_status_tcb);

    ESP_LOGI(TAG, "브 CAN 링크 시작됨(TX=%d RX=%d %dbps, DATA 전용)", CAN_LINK_TX_GPIO, CAN_LINK_RX_GPIO, CAN_LINK_BITRATE);
}
