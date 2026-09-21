#include "i2c_slave_link.h"

#include <string.h>
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "driver/i2c_slave.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_now_reliable.h"

static const char *TAG = "i2c_slave_link";

/* 2026-09-21 — XIAO ESP32-C6 공식 핀아웃(사용자 확인, wiki.seeedstudio.com/xiao_esp32c6_getting_started):
 * SDA=D4=GPIO22, SCL=D5=GPIO23 */
#define BRIDGE_I2C_PORT    0
#define BRIDGE_I2C_SDA_PIN 22
#define BRIDGE_I2C_SCL_PIN 23
#define BRIDGE_I2C_ADDR    0x42   /* TODO — 임의값, 충돌 확인 필요(7비트 주소) */

/* 마스터(CNTL)가 쓴 걸 받아서 처리할 큐, 마스터가 읽어갈 걸 올려둘 큐 — 둘 다 ISR에서
 * FromISR API로 넣고, 전용 태스크가 일반 컨텍스트에서 소비함(i2c_slave_write()가
 * IRAM_ATTR 아니라 ISR 콜백에서 직접 부르면 안 됨 — esp_driver_i2c/i2c_slave.c 확인) */
#define I2C_QUEUE_DEPTH 8
static QueueHandle_t s_rx_queue = NULL;   /* CNTL -> 브릿지 (완성된 패킷) */
static QueueHandle_t s_tx_queue = NULL;   /* 브릿지 -> CNTL (완성된 패킷, 드레인 대상) */
static TaskHandle_t  s_tx_task_handle = NULL;

static i2c_slave_dev_handle_t s_i2c_slave = NULL;

/* on_receive 콜백은 하드웨어 FIFO 한도 이상은 여러 번 나눠서 올 수 있음(드라이버 링버퍼 주석
 * 참고) — sizeof(bridge_link_packet_t)만큼 쌓일 때까지 이 버퍼에 이어붙임. ISR에서만 건드림 */
static uint8_t s_rx_accum[sizeof(bridge_link_packet_t)];
static size_t  s_rx_accum_len = 0;

static IRAM_ATTR bool on_receive(i2c_slave_dev_handle_t i2c_slave,
                                  const i2c_slave_rx_done_event_data_t *evt_data, void *arg)
{
    BaseType_t hp_task_woken = pdFALSE;
    size_t n = evt_data->length;
    size_t space = sizeof(s_rx_accum) - s_rx_accum_len;
    if (n > space) n = space;  /* 넘치면 버림(손상된 프레임 — CRC가 어차피 걸러냄) */
    memcpy(s_rx_accum + s_rx_accum_len, evt_data->buffer, n);
    s_rx_accum_len += n;

    if (s_rx_accum_len >= sizeof(bridge_link_packet_t)) {
        xQueueSendFromISR(s_rx_queue, s_rx_accum, &hp_task_woken);
        s_rx_accum_len = 0;
    }
    return hp_task_woken == pdTRUE;
}

/* 마스터가 읽으려는데 FIFO가 비어있을 때(=지금 올려줄 게 없음) 호출됨 — tx 태스크를 깨워서
 * NONE 패킷을 새로 스테이징하게 함(이 콜백 자신은 i2c_slave_write()를 직접 못 부름, ISR임) */
static IRAM_ATTR bool on_request(i2c_slave_dev_handle_t i2c_slave,
                                  const i2c_slave_request_event_data_t *evt_data, void *arg)
{
    BaseType_t hp_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_tx_task_handle, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

/* 브릿지 -> CNTL 큐를 소비해서 실제 I2C 슬레이브 FIFO에 스테이징. 큐가 비었는데 on_request로
 * 깨워졌으면(마스터가 폴링했는데 줄 게 없음) 빈 NONE 패킷을 대신 스테이징함 */
static void i2c_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        bridge_link_packet_t pkt;
        if (xQueueReceive(s_tx_queue, &pkt, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (ulTaskNotifyTake(pdTRUE, 0) == 0) continue;
            memset(&pkt, 0, sizeof(pkt));
            pkt.msg_type = BRIDGE_MSG_NONE;
        }
        bridge_link_seal(&pkt);
        uint32_t written = 0;
        esp_err_t err = i2c_slave_write(s_i2c_slave, (const uint8_t *)&pkt, sizeof(pkt), &written, pdMS_TO_TICKS(200));
        if (err != ESP_OK || written != sizeof(pkt)) {
            ESP_LOGW(TAG, "i2c_slave_write 실패(%s, %u/%u바이트)", esp_err_to_name(err),
                     (unsigned)written, (unsigned)sizeof(pkt));
        }
    }
}

static void add_peer_if_needed(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        ESP_LOGW(TAG, "피어 등록 실패");
        return;
    }
    /* CNTL의 esp_now_hub.c add_peer_if_needed()와 동일한 레이트 설정 — RF 동작이 이 보드로
     * 옮겨왔으니 이 값도 같이 옮겨와야 캠/센스 쪽에서 보는 링크 특성이 그대로 유지됨 */
    esp_now_rate_config_t rate_cfg = { .phymode = WIFI_PHY_MODE_HT20, .rate = WIFI_PHY_RATE_MCS0_LGI, .ersu = false, .dcm = false };
    esp_now_set_peer_rate_config(mac, &rate_cfg);
}

static void handle_reliable_send(const bridge_link_packet_t *req)
{
    bridge_link_packet_t result = { 0 };
    result.msg_type = BRIDGE_MSG_RELIABLE_RESULT;
    memcpy(result.mac, req->mac, 6);

    add_peer_if_needed(req->mac);
    size_t reply_len = 0;
    esp_err_t err = esp_now_reliable_request(req->mac, req->payload, req->payload_len,
                                              req->accept_reply_types, req->accept_reply_types_count,
                                              req->timeout_ms, req->max_attempts,
                                              result.payload, sizeof(result.payload), &reply_len);
    result.ok = (err == ESP_OK) ? 1 : 0;
    result.payload_len = (uint16_t)reply_len;
    if (xQueueSend(s_tx_queue, &result, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "RELIABLE_RESULT 큐잉 실패(tx 큐 포화)");
    }
}

static void handle_fire_and_forget(const bridge_link_packet_t *req)
{
    add_peer_if_needed(req->mac);
    esp_err_t err = esp_now_send(req->mac, req->payload, req->payload_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FIRE_AND_FORGET 전송 실패: %s", esp_err_to_name(err));
    }
}

/* reliable 실행은 esp_now_reliable_request()가 블로킹(재시도 루프)이라, 이 처리를 rx 태스크
 * 본체에서 직접 하면 그동안 다른 CNTL 요청(FIRE_AND_FORGET 등)을 못 받음 — 전용 워커 태스크로
 * 넘김. Bridge는 지금 CNTL 하나만 상대하므로 CNTL의 esp_now_tx.c처럼 기기별 워커 풀을 둘
 * 필요는 없고, reliable 전용 태스크 하나로 충분(같은 우선순위 원칙, 통신 계층=17) */
#define RELIABLE_QUEUE_DEPTH 4
static QueueHandle_t s_reliable_queue = NULL;

static void reliable_worker_task(void *arg)
{
    (void)arg;
    bridge_link_packet_t req;
    for (;;) {
        if (xQueueReceive(s_reliable_queue, &req, portMAX_DELAY) == pdTRUE) {
            handle_reliable_send(&req);
        }
    }
}

static void i2c_rx_task(void *arg)
{
    (void)arg;
    bridge_link_packet_t pkt;
    for (;;) {
        if (xQueueReceive(s_rx_queue, &pkt, portMAX_DELAY) != pdTRUE) continue;
        if (!bridge_link_verify(&pkt)) {
            ESP_LOGW(TAG, "CRC 불일치 — 프레임 버림(msg_type=%u)", pkt.msg_type);
            continue;
        }
        switch (pkt.msg_type) {
        case BRIDGE_MSG_RELIABLE_SEND:
            if (xQueueSend(s_reliable_queue, &pkt, 0) != pdTRUE) {
                ESP_LOGW(TAG, "reliable 큐 포화 — 요청 버림");
            }
            break;
        case BRIDGE_MSG_FIRE_AND_FORGET:
            handle_fire_and_forget(&pkt);
            break;
        case BRIDGE_MSG_SET_CHANNEL: {
            uint8_t ch = pkt.mac[0];
            esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            ESP_LOGI(TAG, "SET_CHANNEL(%u): %s", ch, esp_err_to_name(err));
            break;
        }
        case BRIDGE_MSG_PING: {
            bridge_link_packet_t pong = { 0 };
            pong.msg_type = BRIDGE_MSG_PONG;
            if (xQueueSend(s_tx_queue, &pong, pdMS_TO_TICKS(200)) != pdTRUE) {
                ESP_LOGW(TAG, "PONG 큐잉 실패(tx 큐 포화)");
            }
            break;
        }
        case BRIDGE_MSG_RESET:
            ESP_LOGW(TAG, "CNTL 요청으로 재시작");
            vTaskDelay(pdMS_TO_TICKS(50));  /* 로그 플러시 여유 */
            esp_restart();
            break;
        default:
            ESP_LOGW(TAG, "알 수 없는 msg_type=%u — 무시", pkt.msg_type);
            break;
        }
    }
}

void i2c_slave_link_queue_incoming(const uint8_t *mac, int8_t rssi, const uint8_t *data, size_t len)
{
    if (len > BRIDGE_LINK_MAX_PAYLOAD) len = BRIDGE_LINK_MAX_PAYLOAD;
    bridge_link_packet_t pkt = { 0 };
    pkt.msg_type = BRIDGE_MSG_INCOMING;
    memcpy(pkt.mac, mac, 6);
    pkt.rssi = rssi;
    pkt.payload_len = (uint16_t)len;
    memcpy(pkt.payload, data, len);
    if (xQueueSend(s_tx_queue, &pkt, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "INCOMING_MSG 큐잉 실패(tx 큐 포화) — CNTL이 이 수신을 못 봄");
    }
}

void i2c_slave_link_init(void)
{
    s_rx_queue = xQueueCreate(I2C_QUEUE_DEPTH, sizeof(bridge_link_packet_t));
    s_tx_queue = xQueueCreate(I2C_QUEUE_DEPTH, sizeof(bridge_link_packet_t));
    s_reliable_queue = xQueueCreate(RELIABLE_QUEUE_DEPTH, sizeof(bridge_link_packet_t));

    /* 통신 계층 우선순위 17 — CNTL 쪽 tx_dispatcher/worker와 동일 원칙(project_cntl_task_priority_scheme) */
    xTaskCreate(i2c_rx_task, "i2c_rx", 4096, NULL, 17, NULL);
    xTaskCreate(reliable_worker_task, "reliable_worker", 4096, NULL, 17, NULL);
    xTaskCreate(i2c_tx_task, "i2c_tx", 3072, NULL, 17, &s_tx_task_handle);

    i2c_slave_config_t cfg = {
        .i2c_port      = BRIDGE_I2C_PORT,
        .sda_io_num    = BRIDGE_I2C_SDA_PIN,
        .scl_io_num    = BRIDGE_I2C_SCL_PIN,
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = sizeof(bridge_link_packet_t) * 2,
        .receive_buf_depth = sizeof(bridge_link_packet_t) * 2,
        .slave_addr    = BRIDGE_I2C_ADDR,
        .addr_bit_len  = I2C_ADDR_BIT_LEN_7,
    };
    ESP_ERROR_CHECK(i2c_new_slave_device(&cfg, &s_i2c_slave));

    i2c_slave_event_callbacks_t cbs = {
        .on_receive = on_receive,
        .on_request = on_request,
    };
    ESP_ERROR_CHECK(i2c_slave_register_event_callbacks(s_i2c_slave, &cbs, NULL));

    ESP_LOGI(TAG, "I2C 슬레이브 시작됨 (SDA=%d SCL=%d addr=0x%02x)",
             BRIDGE_I2C_SDA_PIN, BRIDGE_I2C_SCL_PIN, BRIDGE_I2C_ADDR);
}
