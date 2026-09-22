#include "can_test.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

/**
 * 2026-09-22(임시 진단) — CNTL 보드 두 대를 CANH-CANH/CANL-CANL로 직결해서 CAN 통신
 * 자체가 되는지 확인하는 최소 테스트. IO15=CANTX, IO16=CANRX (스키매틱 U12/TJA1051T/3
 * 확인됨). 양쪽 콘에 동일 펌웨어를 플래시 — 각자 1초마다 자기 카운터를 실어서 보내고,
 * 받은 프레임마다 rx 카운트를 올림. 종단저항(SW2)은 기본값이 이미 닫힘(연결)이라 별도
 * 조작 불필요(사용자 확인). ESP-IDF v6.0.2의 legacy driver/twai.h는 deprecated라 새
 * esp_twai.h(twai_node_*) API로 작성 — examples/peripherals/twai/twai_network 예제 참고.
 */

static const char *TAG = "can_test";

static volatile uint32_t s_tx_count = 0;
static volatile uint32_t s_rx_count = 0;

static twai_node_handle_t s_node = NULL;
static SemaphoreHandle_t  s_rx_sem = NULL;
static twai_frame_t       s_rx_frame;
static uint8_t             s_rx_buf[8];

#define CAN_TEST_TX_GPIO GPIO_NUM_15
#define CAN_TEST_RX_GPIO GPIO_NUM_16
#define CAN_TEST_BITRATE 500000

/* 2026-09-22(버그 수정 — 사용자 지적) — 두 보드가 완전히 동일한 펌웨어라 고정 ID(0x123)로
 * 보내면 같은 순간에 서로 충돌함(arb_lost/stuff_err 반복 확인됨). 보드마다 다른 MAC 하위
 * 바이트로 ID를 만들어서 두 보드가 서로 다른 ID로 송신하게 함(11비트 표준 ID 범위 내) */
static uint32_t s_tx_id = 0x123;

uint32_t can_test_get_tx_count(void) { return s_tx_count; }
uint32_t can_test_get_rx_count(void) { return s_rx_count; }

static IRAM_ATTR bool on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    if (twai_node_receive_from_isr(handle, &s_rx_frame) == ESP_OK) {
        xSemaphoreGiveFromISR(s_rx_sem, &woken);
    }
    return woken == pdTRUE;
}

static IRAM_ATTR bool on_error(twai_node_handle_t handle, const twai_error_event_data_t *edata, void *ctx)
{
    ESP_EARLY_LOGW(TAG, "버스 에러: 0x%x", (unsigned)edata->err_flags.val);
    return false;
}

/* 2026-09-22(임시 진단 — 계속되는 arb_lost/stuff_err 원인 확인) — 상태 전이와 TEC/REC를
 * 직접 찍어봄. bus-off까지 갔는데 자동 recover가 없으면 계속 실패만 반복될 것이므로 그
 * 경우 twai_node_recover()도 호출 */
static IRAM_ATTR bool on_state_change(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *ctx)
{
    static const char *names[] = {"error_active", "error_warning", "error_passive", "bus_off"};
    ESP_EARLY_LOGW(TAG, "상태 전이: %s -> %s", names[edata->old_sta], names[edata->new_sta]);
    return false;
}

static void can_rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_rx_sem, portMAX_DELAY) == pdTRUE) {
            s_rx_count++;
            uint32_t seq = 0;
            if (s_rx_frame.buffer_len >= sizeof(seq)) memcpy(&seq, s_rx_frame.buffer, sizeof(seq));
            ESP_LOGI(TAG, "수신: id=0x%03lx seq=%lu (누적 rx=%lu)",
                     (unsigned long)s_rx_frame.header.id, (unsigned long)seq, (unsigned long)s_rx_count);
        }
    }
}

static void can_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t seq = s_tx_count;
        twai_frame_t tx_frame = {
            .header.id = s_tx_id,
            .buffer = (uint8_t *)&seq,
            .buffer_len = sizeof(seq),
        };
        esp_err_t err = twai_node_transmit(s_node, &tx_frame, 500);
        if (err == ESP_OK) {
            s_tx_count++;
        } else {
            ESP_LOGW(TAG, "twai_node_transmit 실패: %s", esp_err_to_name(err));
        }

        twai_node_status_t status;
        if (twai_node_get_info(s_node, &status, NULL) == ESP_OK) {
            static const char *names[] = {"error_active", "error_warning", "error_passive", "bus_off"};
            ESP_LOGW(TAG, "상태=%s TEC=%u REC=%u txq_남음=%u", names[status.state],
                     (unsigned)status.tx_error_count, (unsigned)status.rx_error_count,
                     (unsigned)status.tx_queue_remaining);
            if (status.state == TWAI_ERROR_BUS_OFF) {
                ESP_LOGW(TAG, "bus-off — 복구 시도");
                twai_node_recover(s_node);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void can_test_init(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    /* 표준 11비트 ID 범위(0~0x7FF) 안에서 MAC 하위 바이트로 두 값 중 하나를 고름 —
     * 정확한 유일성보다 "이 두 보드가 서로 다르기만 하면 됨"이 목적 */
    s_tx_id = (mac[5] & 0x01) ? 0x123 : 0x456;

    s_rx_sem = xSemaphoreCreateBinary();
    s_rx_frame.buffer = s_rx_buf;
    s_rx_frame.buffer_len = sizeof(s_rx_buf);

    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx = CAN_TEST_TX_GPIO,
            .rx = CAN_TEST_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing.bitrate = CAN_TEST_BITRATE,
        .fail_retry_cnt = 3,
        .tx_queue_depth = 5,
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

    xTaskCreate(can_tx_task, "can_tx_test", 3072, NULL, 10, NULL);
    xTaskCreate(can_rx_task, "can_rx_test", 3072, NULL, 10, NULL);
    ESP_LOGI(TAG, "CAN 테스트 시작됨 (TX=%d RX=%d, %dbps, 내 송신id=0x%03lx)",
             CAN_TEST_TX_GPIO, CAN_TEST_RX_GPIO, CAN_TEST_BITRATE, (unsigned long)s_tx_id);
}
