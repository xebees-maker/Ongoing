#include "can_test.h"

#include "can_bridge_link.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>

/**
 * 2026-09-22 — "대화하자" 세션에서 설계한 콘<->브릿지 CAN 프로토콜(can_bridge_link)을
 * 실제로 검증하는 2단계 테스트. 콘-콘 CAN 배선 자체(종단저항 접촉불량 원인으로 확인/해결됨)에
 * 이어서, 이번엔 원시 카운터가 아니라 실제 설계한 프로토콜(ISO-TP SF/FF/CF/FC, CAN ID 4개,
 * DATA/CONTROL 분리, PSRAM FIFO 큐)이 두 보드 사이에서 실제로 동작하는지 확인.
 *
 * 아직 안 된 것(다음 단계, 의도적으로 이번 패스 범위 밖) — 실제 ESP-NOW 릴레이(브릿지가 CAM과
 * 통신하며 이 CAN 링크로 중계하는 것), 브릿지 화면 좌우 분할 로그 UI, 콘 설정화면의 큐 깊이
 * 노출. 지금은 두 콘 보드가 이 프로토콜 자체로 PING/PONG(CONTROL)과 가짜 CASK 모양 페이로드
 * (DATA)를 주고받는 것까지만 검증.
 *
 * 역할(MAC 기반, 기존 방식 확장) — 한쪽은 CNTL_TO_BRIDGE_*로 송신/BRIDGE_TO_CNTL_*로 수신,
 * 다른 쪽은 반대. 두 보드 다 accept-all 필터(HAL v1 기본값, 이전 세션에 확인됨)라 버스의
 * 모든 프레임을 받고 ID로 소프트웨어에서 구분.
 */

static const char *TAG = "can_test";

#define CAN_TEST_TX_GPIO GPIO_NUM_15
#define CAN_TEST_RX_GPIO GPIO_NUM_16
#define CAN_TEST_BITRATE 500000

/* 원시 TWAI 레벨 진단 카운터 — 종단저항 문제 진단 때 쓰던 것, 그대로 유지(화면에도 계속 표시) */
static volatile uint32_t s_tx_count = 0;      /* twai_node_transmit() ESP_OK(프레임 하나 큐잉 성공) */
static volatile uint32_t s_rx_count = 0;      /* on_rx_done 호출 횟수(프레임 하나 수신) */
static volatile uint32_t s_tx_done_ok = 0;    /* on_tx_done, is_tx_success=true */
static volatile uint32_t s_tx_done_fail = 0;

static twai_node_handle_t s_node = NULL;

static can_bridge_role_t s_role;
static uint32_t s_my_ctrl_tx_id, s_my_ctrl_rx_id, s_my_data_tx_id, s_my_data_rx_id;
static uint8_t  s_my_mac[6];

static can_bridge_ctx_t *s_ctrl_ctx;
static can_bridge_ctx_t *s_data_ctx;
static can_bridge_reassembly_t s_ctrl_reasm;
static can_bridge_reassembly_t s_data_reasm;
static can_bridge_queue_t s_ctrl_complete_q;
static can_bridge_queue_t s_data_complete_q;

/* 프로토콜 레벨 진단 카운터 — 콘 요약화면에 큐 깊이를 노출하라는 사용자 지시 대비,
 * 큐 자체(count/high_water_mark)는 can_bridge_queue_t 안에 이미 있음 */
static volatile uint32_t s_ctrl_msgs_completed = 0;
static volatile uint32_t s_data_msgs_completed = 0;

/* ISR -> 처리 태스크로 원시 프레임을 넘기는 큐(작고 고정크기라 PSRAM 정책 대상 아님 —
 * FreeRTOS 큐 자체 내부 저장은 xQueueCreate가 내부 RAM에 잡음, 이 프로젝트 관례상 큐 핸들
 * 자체는 예외로 취급) */
typedef struct {
    uint32_t id;
    uint8_t  len;
    uint8_t  data[8];
} raw_frame_t;
static QueueHandle_t s_raw_frame_q;

static SemaphoreHandle_t s_rx_sem;
static twai_frame_t      s_rx_frame;
static uint8_t            s_rx_buf[8];

uint32_t can_test_get_tx_count(void) { return s_tx_count; }
uint32_t can_test_get_rx_count(void) { return s_rx_count; }
uint32_t can_test_get_tx_done_ok(void) { return s_tx_done_ok; }
uint32_t can_test_get_tx_done_fail(void) { return s_tx_done_fail; }

uint32_t can_test_get_ctrl_queue_depth(void) { return s_ctrl_complete_q.count; }
uint32_t can_test_get_data_queue_depth(void) { return s_data_complete_q.count; }
uint32_t can_test_get_ctrl_queue_hwm(void) { return s_ctrl_complete_q.high_water_mark; }
uint32_t can_test_get_data_queue_hwm(void) { return s_data_complete_q.high_water_mark; }
uint32_t can_test_get_ctrl_msgs_completed(void) { return s_ctrl_msgs_completed; }
uint32_t can_test_get_data_msgs_completed(void) { return s_data_msgs_completed; }

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

static IRAM_ATTR bool on_tx_done(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *ctx)
{
    if (edata->is_tx_success) s_tx_done_ok++;
    else                      s_tx_done_fail++;
    return false;
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

/* 원시 프레임 처리 태스크 — ISO-TP 라우팅/재조립/FC 응답을 전부 여기(태스크 컨텍스트)에서
 * 처리. twai_node_transmit()이 블로킹 가능이라 ISR에서 직접 부르면 안 되므로 이렇게 분리 */
static void can_rx_task(void *arg)
{
    (void)arg;
    raw_frame_t rf;
    for (;;) {
        if (xQueueReceive(s_raw_frame_q, &rf, portMAX_DELAY) != pdTRUE) continue;
        s_rx_count++;

        uint8_t pci = (rf.len >= 1) ? ((rf.data[0] >> 4) & 0x0F) : 0xFF;

        if (rf.id == s_my_ctrl_rx_id) {
            if (pci == ISO_TP_PCI_FC) {
                can_bridge_ctx_notify_fc(s_ctrl_ctx, rf.data);
                continue;
            }
            uint8_t fc[8];
            if (can_bridge_reassembly_feed(&s_ctrl_reasm, rf.data, rf.len, fc, &s_ctrl_complete_q)) {
                twai_frame_t f = { .header.id = s_my_ctrl_tx_id, .buffer = fc, .buffer_len = 8 };
                esp_err_t fcerr = twai_node_transmit(s_node, &f, CAN_BRIDGE_DEFAULT_TIMEOUT_MS);
                if (fcerr != ESP_OK) ESP_LOGW(TAG, "CTRL FC 전송 실패: %s", esp_err_to_name(fcerr));
            }
        } else if (rf.id == s_my_data_rx_id) {
            if (pci == ISO_TP_PCI_FC) {
                can_bridge_ctx_notify_fc(s_data_ctx, rf.data);
                continue;
            }
            uint8_t fc[8];
            if (can_bridge_reassembly_feed(&s_data_reasm, rf.data, rf.len, fc, &s_data_complete_q)) {
                twai_frame_t f = { .header.id = s_my_data_tx_id, .buffer = fc, .buffer_len = 8 };
                esp_err_t fcerr = twai_node_transmit(s_node, &f, CAN_BRIDGE_DEFAULT_TIMEOUT_MS);
                if (fcerr != ESP_OK) ESP_LOGW(TAG, "DATA FC 전송 실패: %s", esp_err_to_name(fcerr));
            }
        }
        /* 그 외 ID(내가 보낸 프레임의 echo 등)는 무시 */
    }
}

/* 완성된 메시지를 큐에서 꺼내 로그로 찍는 소비 태스크 — CONTROL/DATA 각각 */
static void can_consume_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t *data; size_t len;
        while (can_bridge_queue_pop(&s_ctrl_complete_q, &data, &len)) {
            s_ctrl_msgs_completed++;
            if (len >= CAN_BRIDGE_APP_HEADER_LEN) {
                can_bridge_app_header_t hdr;
                memcpy(&hdr, data, sizeof(hdr));
                ESP_LOGI(TAG, "CONTROL 수신: type=%u mac=%02X%02X%02X%02X%02X%02X flags=%u len=%u",
                         hdr.msg_type, hdr.mac[0], hdr.mac[1], hdr.mac[2], hdr.mac[3], hdr.mac[4], hdr.mac[5],
                         hdr.flags, (unsigned)len);
                /* PING 받으면 즉시 PONG(사용자 설계: 브릿지 생존확인 왕복) */
                if (hdr.msg_type == CAN_CTRL_PING) {
                    can_bridge_app_header_t pong = { .msg_type = CAN_CTRL_PONG, .flags = 0 };
                    memcpy(pong.mac, s_my_mac, 6);
                    can_bridge_send(s_ctrl_ctx, (const uint8_t *)&pong, sizeof(pong));
                }
            }
            can_bridge_queue_pop_free(data);
        }
        while (can_bridge_queue_pop(&s_data_complete_q, &data, &len)) {
            s_data_msgs_completed++;
            if (len >= CAN_BRIDGE_APP_HEADER_LEN) {
                can_bridge_app_header_t hdr;
                memcpy(&hdr, data, sizeof(hdr));
                ESP_LOGI(TAG, "DATA 수신: type=%u mac=%02X%02X%02X%02X%02X%02X payload_len=%u 전체len=%u",
                         hdr.msg_type, hdr.mac[0], hdr.mac[1], hdr.mac[2], hdr.mac[3], hdr.mac[4], hdr.mac[5],
                         (unsigned)(len - CAN_BRIDGE_APP_HEADER_LEN), (unsigned)len);
            }
            can_bridge_queue_pop_free(data);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void can_tx_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    for (;;) {
        /* CONTROL: 5초마다 PING(브릿지 생존확인 시나리오 재현) */
        if (tick % 5 == 0) {
            can_bridge_app_header_t ping = { .msg_type = CAN_CTRL_PING, .flags = 0 };
            memcpy(ping.mac, s_my_mac, 6);
            esp_err_t err = can_bridge_send(s_ctrl_ctx, (const uint8_t *)&ping, sizeof(ping));
            if (err != ESP_OK) ESP_LOGW(TAG, "CONTROL PING 전송 실패: %s", esp_err_to_name(err));
            else s_tx_count++;
        }

        /* DATA: 10초마다 가짜 CASK 모양 페이로드(투명 릴레이 시나리오 재현) — FF+CF 여러 개
         * 필요한 크기(8바이트 헤더 + 24바이트 더미)로 일부러 크게 잡아 멀티프레임 경로 검증.
         * 2026-09-22(실기 디버깅) — 두 보드가 동시에 이 tick에 도달하면 양방향 DATA가 겹쳐서
         * 버스 에러(REC 급증)로 CF 유실이 실측됨. 역할별로 3틱 어긋나게 해서 겹침을 줄임 */
        uint32_t data_tick_offset = (s_role == CAN_BRIDGE_ROLE_CNTL) ? 5 : 8;
        if (tick % 10 == data_tick_offset) {
            uint8_t msg[CAN_BRIDGE_APP_HEADER_LEN + 24];
            can_bridge_app_header_t hdr = { .msg_type = CAN_DATA_RELAY, .flags = 0 };
            memcpy(hdr.mac, s_my_mac, 6);
            memcpy(msg, &hdr, CAN_BRIDGE_APP_HEADER_LEN);
            for (int i = 0; i < 24; i++) msg[CAN_BRIDGE_APP_HEADER_LEN + i] = (uint8_t)(tick + i);
            esp_err_t err = can_bridge_send(s_data_ctx, msg, sizeof(msg));
            if (err != ESP_OK) ESP_LOGW(TAG, "DATA 전송 실패: %s", esp_err_to_name(err));
            else s_tx_count++;
        }

        twai_node_status_t status;
        if (twai_node_get_info(s_node, &status, NULL) == ESP_OK) {
            static const char *names[] = {"error_active", "error_warning", "error_passive", "bus_off"};
            ESP_LOGI(TAG, "상태=%s TEC=%u REC=%u tx_ok=%u tx_fail=%u | CTRL큐=%u(최대%u) DATA큐=%u(최대%u) 완료 C=%u D=%u",
                     names[status.state], (unsigned)status.tx_error_count, (unsigned)status.rx_error_count,
                     (unsigned)s_tx_done_ok, (unsigned)s_tx_done_fail,
                     (unsigned)s_ctrl_complete_q.count, (unsigned)s_ctrl_complete_q.high_water_mark,
                     (unsigned)s_data_complete_q.count, (unsigned)s_data_complete_q.high_water_mark,
                     (unsigned)s_ctrl_msgs_completed, (unsigned)s_data_msgs_completed);
            if (status.state == TWAI_ERROR_BUS_OFF) {
                ESP_LOGW(TAG, "bus-off — 복구 시도");
                twai_node_recover(s_node);
            }
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void can_test_init(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    memcpy(s_my_mac, mac, 6);

    /* 역할 배정 — 2026-09-22(버그 수정): mac[5]의 최하위 1비트로 나눴더니 지금 테스트 중인
     * 두 보드(COM19=e8:f6:0a:8e:86:68, COM18=44:1b:f6:ca:48:c0)가 둘 다 짝수라 똑같이
     * BRIDGE로 배정되는 충돌이 실기에서 발생(양쪽 다 FC 타임아웃, 메시지 완료 0건으로 확인).
     * 지금은 이 두 물리보드로만 검증하는 임시 벤치테스트 단계라, COM19의 실제 MAC과 정확히
     * 일치하면 CNTL, 아니면 BRIDGE로 명시적 하드코딩(실제 배포에선 콘/브릿지가 별도 하드웨어/
     * 펌웨어로 고정되니 이 분기 자체가 없어짐 — 지금만의 벤치테스트 전용 임시 조치) */
    static const uint8_t s_com19_mac[6] = {0xe8, 0xf6, 0x0a, 0x8e, 0x86, 0x68};
    s_role = (memcmp(mac, s_com19_mac, 6) == 0) ? CAN_BRIDGE_ROLE_CNTL : CAN_BRIDGE_ROLE_BRIDGE;
    if (s_role == CAN_BRIDGE_ROLE_CNTL) {
        s_my_ctrl_tx_id = CAN_BRIDGE_ID_CNTL_TO_BRIDGE_CONTROL;
        s_my_ctrl_rx_id = CAN_BRIDGE_ID_BRIDGE_TO_CNTL_CONTROL;
        s_my_data_tx_id = CAN_BRIDGE_ID_CNTL_TO_BRIDGE_DATA;
        s_my_data_rx_id = CAN_BRIDGE_ID_BRIDGE_TO_CNTL_DATA;
    } else {
        s_my_ctrl_tx_id = CAN_BRIDGE_ID_BRIDGE_TO_CNTL_CONTROL;
        s_my_ctrl_rx_id = CAN_BRIDGE_ID_CNTL_TO_BRIDGE_CONTROL;
        s_my_data_tx_id = CAN_BRIDGE_ID_BRIDGE_TO_CNTL_DATA;
        s_my_data_rx_id = CAN_BRIDGE_ID_CNTL_TO_BRIDGE_DATA;
    }

    s_raw_frame_q = xQueueCreate(16, sizeof(raw_frame_t));
    s_rx_sem = xSemaphoreCreateBinary();
    s_rx_frame.buffer = s_rx_buf;
    s_rx_frame.buffer_len = sizeof(s_rx_buf);

    can_bridge_queue_init(&s_ctrl_complete_q);
    can_bridge_queue_init(&s_data_complete_q);

    /* 재조립 버퍼 — PSRAM(feedback_prefer_psram_for_buffers, 콘/브릿지 공통 원칙) */
    uint8_t *ctrl_buf = (uint8_t *)heap_caps_malloc(256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctrl_buf) ctrl_buf = (uint8_t *)heap_caps_malloc(256, MALLOC_CAP_8BIT);
    uint8_t *data_buf = (uint8_t *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data_buf) data_buf = (uint8_t *)heap_caps_malloc(1024, MALLOC_CAP_8BIT);
    can_bridge_reassembly_init(&s_ctrl_reasm, ctrl_buf, 256);
    can_bridge_reassembly_init(&s_data_reasm, data_buf, 1024);

    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx = CAN_TEST_TX_GPIO,
            .rx = CAN_TEST_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing.bitrate = CAN_TEST_BITRATE,
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
        .on_tx_done = on_tx_done,
        .on_error = on_error,
        .on_state_change = on_state_change,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(s_node, &cbs, NULL));
    ESP_ERROR_CHECK(twai_node_enable(s_node));

    s_ctrl_ctx = can_bridge_ctx_create(s_node, s_my_ctrl_tx_id);
    s_data_ctx = can_bridge_ctx_create(s_node, s_my_data_tx_id);

    xTaskCreate(can_rx_task, "can_rx_test", 4096, NULL, 10, NULL);
    xTaskCreate(can_consume_task, "can_consume", 4096, NULL, 10, NULL);
    xTaskCreate(can_tx_task, "can_tx_test", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "CAN 프로토콜 테스트 시작됨 (역할=%s, TX=%d RX=%d, %dbps)",
             s_role == CAN_BRIDGE_ROLE_CNTL ? "CNTL" : "BRIDGE",
             CAN_TEST_TX_GPIO, CAN_TEST_RX_GPIO, CAN_TEST_BITRATE);
}
