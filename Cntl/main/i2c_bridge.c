#include "i2c_bridge.h"
#include "bridge_link.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "i2c_bridge";

/* TODO(하드웨어 확인 필요) — CNTL의 isolated I2C 4단자 실제 GPIO 번호 미확인(스키매틱에
 * 없음, 대화로만 확인된 존재) — 배선 확인 전까지 플레이스홀더 */
#define BRIDGE_I2C_PORT       0
#define BRIDGE_I2C_SDA_PIN    4
#define BRIDGE_I2C_SCL_PIN    5
#define BRIDGE_I2C_ADDR       0x42   /* TODO — Bridge/main/i2c_slave_link.c와 반드시 일치해야 함 */
#define BRIDGE_I2C_TIMEOUT_MS 200

/* 한 MAC당 동시에 reliable 요청 하나만 떠있음(esp_now_tx.c의 기기별 워커 1개가 이 함수를
 * 직렬로 부름) — ESP_NOW_HUB_MAX_NODES와 같은 상한이면 충분하지만, 이 파일을 esp_now_hub.h에
 * 의존시키고 싶지 않아 값만 맞춰 독립 정의함 */
#define BRIDGE_MAX_PENDING 8

typedef struct {
    bool                 in_use;
    uint8_t              mac[6];
    SemaphoreHandle_t    done_sem;
    bridge_link_packet_t result;
} pending_slot_t;

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static esp_now_recv_cb_t       s_recv_cb = NULL;
static SemaphoreHandle_t        s_pending_mutex = NULL;
static pending_slot_t           s_pending[BRIDGE_MAX_PENDING];

static pending_slot_t *alloc_pending_locked(const uint8_t *mac)
{
    for (int i = 0; i < BRIDGE_MAX_PENDING; i++) {
        if (!s_pending[i].in_use) {
            s_pending[i].in_use = true;
            memcpy(s_pending[i].mac, mac, 6);
            if (!s_pending[i].done_sem) {
                s_pending[i].done_sem = xSemaphoreCreateBinary();
            } else {
                xSemaphoreTake(s_pending[i].done_sem, 0);  /* 이전 신호 잔여분 비움 */
            }
            return &s_pending[i];
        }
    }
    return NULL;
}

esp_err_t bridge_send(const uint8_t *mac, const void *data, size_t len)
{
    bridge_link_packet_t pkt = { 0 };
    pkt.msg_type = BRIDGE_MSG_FIRE_AND_FORGET;
    memcpy(pkt.mac, mac, 6);
    size_t n = len > BRIDGE_LINK_MAX_PAYLOAD ? BRIDGE_LINK_MAX_PAYLOAD : len;
    memcpy(pkt.payload, data, n);
    pkt.payload_len = (uint16_t)n;
    bridge_link_seal(&pkt);
    esp_err_t err = i2c_master_transmit(s_dev, (const uint8_t *)&pkt, sizeof(pkt), BRIDGE_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bridge_send 실패: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t bridge_reliable_request(const uint8_t *peer_mac,
                                   const void *req, size_t req_len,
                                   const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                                   uint32_t timeout_ms, int max_attempts,
                                   void *reply_out, size_t reply_out_cap, size_t *reply_out_len)
{
    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    pending_slot_t *slot = alloc_pending_locked(peer_mac);
    xSemaphoreGive(s_pending_mutex);
    if (!slot) {
        ESP_LOGE(TAG, "pending 슬롯 부족(%d개 초과) — 이론상 도달 불가(워커 수 상한과 동일)", BRIDGE_MAX_PENDING);
        return ESP_ERR_NO_MEM;
    }

    bridge_link_packet_t pkt = { 0 };
    pkt.msg_type = BRIDGE_MSG_RELIABLE_SEND;
    memcpy(pkt.mac, peer_mac, 6);
    pkt.timeout_ms = timeout_ms;
    pkt.max_attempts = (uint8_t)max_attempts;
    size_t types_n = accept_reply_types_count > BRIDGE_LINK_MAX_ACCEPT_TYPES ? BRIDGE_LINK_MAX_ACCEPT_TYPES : accept_reply_types_count;
    memcpy(pkt.accept_reply_types, accept_reply_types, types_n);
    pkt.accept_reply_types_count = (uint8_t)types_n;
    size_t payload_n = req_len > BRIDGE_LINK_MAX_PAYLOAD ? BRIDGE_LINK_MAX_PAYLOAD : req_len;
    memcpy(pkt.payload, req, payload_n);
    pkt.payload_len = (uint16_t)payload_n;
    bridge_link_seal(&pkt);

    esp_err_t tx_err = i2c_master_transmit(s_dev, (const uint8_t *)&pkt, sizeof(pkt), BRIDGE_I2C_TIMEOUT_MS);
    if (tx_err != ESP_OK) {
        xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
        slot->in_use = false;
        xSemaphoreGive(s_pending_mutex);
        ESP_LOGW(TAG, "브릿지에 RELIABLE_SEND 전송 실패: %s", esp_err_to_name(tx_err));
        return ESP_FAIL;
    }

    /* 브릿지 자신의 재시도 예산(timeout_ms*max_attempts, 브릿지 내부 esp_now_reliable_request()가
     * 그대로 소비) + I2C 폴링 주기/왕복 여유 마진(1초) */
    uint32_t wait_ms = timeout_ms * (uint32_t)max_attempts + 1000;
    BaseType_t got = xSemaphoreTake(slot->done_sem, pdMS_TO_TICKS(wait_ms));

    esp_err_t result;
    if (got == pdTRUE && slot->result.ok) {
        if (reply_out && reply_out_cap > 0) {
            size_t copy_len = slot->result.payload_len < reply_out_cap ? slot->result.payload_len : reply_out_cap;
            memcpy(reply_out, slot->result.payload, copy_len);
            if (reply_out_len) *reply_out_len = copy_len;
        } else if (reply_out_len) {
            *reply_out_len = slot->result.payload_len;
        }
        result = ESP_OK;
    } else {
        if (got != pdTRUE) {
            ESP_LOGW(TAG, "브릿지 응답 타임아웃(%lu ms 대기)", (unsigned long)wait_ms);
        }
        result = ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    slot->in_use = false;
    xSemaphoreGive(s_pending_mutex);
    return result;
}

static void deliver_incoming(const bridge_link_packet_t *pkt)
{
    if (!s_recv_cb) return;
    /* esp_now_recv_info_t/wifi_pkt_rx_ctrl_t를 최소한으로 복원 — 기존 recv_cb(esp_now_hub.c)는
     * info->src_addr과 info->rx_ctrl->rssi 딱 두 개만 읽으므로 그 외 필드는 0으로 둬도 안전
     * (실제 ESP-NOW 드라이버가 채우던 나머지 무선 진단 필드들은 이제 CNTL에서 볼 방법이 없음
     * — 브릿지가 rssi만 넘겨주도록 설계됨, project_cntl_i2c_bridge_design_2026_09_21 참고) */
    wifi_pkt_rx_ctrl_t rx_ctrl = { 0 };
    rx_ctrl.rssi = pkt->rssi;
    esp_now_recv_info_t info = { 0 };
    info.src_addr = (uint8_t *)pkt->mac;
    info.rx_ctrl  = &rx_ctrl;
    s_recv_cb(&info, pkt->payload, pkt->payload_len);
}

static void handle_reliable_result(const bridge_link_packet_t *pkt)
{
    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    for (int i = 0; i < BRIDGE_MAX_PENDING; i++) {
        if (s_pending[i].in_use && memcmp(s_pending[i].mac, pkt->mac, 6) == 0) {
            s_pending[i].result = *pkt;
            xSemaphoreGive(s_pending[i].done_sem);
            break;
        }
    }
    xSemaphoreGive(s_pending_mutex);
}

#define BRIDGE_PING_INTERVAL_US   (2 * 1000 * 1000)
#define BRIDGE_UNRESPONSIVE_US    (5 * 1000 * 1000)

static void i2c_bridge_poll_task(void *arg)
{
    (void)arg;
    int64_t last_activity_us = esp_timer_get_time();
    int64_t last_ping_us = 0;

    for (;;) {
        bridge_link_packet_t pkt;
        esp_err_t err = i2c_master_receive(s_dev, (uint8_t *)&pkt, sizeof(pkt), BRIDGE_I2C_TIMEOUT_MS);
        int64_t now_us = esp_timer_get_time();

        if (err == ESP_OK && bridge_link_verify(&pkt) && pkt.msg_type != BRIDGE_MSG_NONE) {
            last_activity_us = now_us;
            switch (pkt.msg_type) {
            case BRIDGE_MSG_INCOMING:        deliver_incoming(&pkt); break;
            case BRIDGE_MSG_RELIABLE_RESULT: handle_reliable_result(&pkt); break;
            case BRIDGE_MSG_PONG:            break;  /* 생존확인 목적 — last_activity_us 갱신이면 충분 */
            default: ESP_LOGW(TAG, "알 수 없는 msg_type=%u — 무시", pkt.msg_type); break;
            }
        } else if (err == ESP_OK) {
            last_activity_us = now_us;  /* NONE도 정상 응답(빈 폴)이므로 생존확인으로 인정 */
        }

        /* 생존확인 — 2초 이상 아무 트래픽 없으면 PING, 5초 이상 무응답이면 재시작 명령
         * 시도(최선노력 — 브릿지가 완전히 멎었으면 이 명령도 못 받을 수 있음, 브릿지 자체
         * 워치독이 최후 수단이라는 게 설계 전제, project_cntl_i2c_bridge_design_2026_09_21 참고) */
        if (now_us - last_ping_us > BRIDGE_PING_INTERVAL_US) {
            bridge_link_packet_t ping = { 0 };
            ping.msg_type = BRIDGE_MSG_PING;
            bridge_link_seal(&ping);
            i2c_master_transmit(s_dev, (const uint8_t *)&ping, sizeof(ping), BRIDGE_I2C_TIMEOUT_MS);
            last_ping_us = now_us;
        }
        if (now_us - last_activity_us > BRIDGE_UNRESPONSIVE_US) {
            ESP_LOGE(TAG, "브릿지 무응답(5초+) — 재시작 명령 시도");
            bridge_link_packet_t reset = { 0 };
            reset.msg_type = BRIDGE_MSG_RESET;
            bridge_link_seal(&reset);
            i2c_master_transmit(s_dev, (const uint8_t *)&reset, sizeof(reset), BRIDGE_I2C_TIMEOUT_MS);
            last_activity_us = now_us;  /* 브릿지 재부팅 여유시간 확보 — 반복 스팸 방지 */
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void i2c_bridge_init(esp_now_recv_cb_t recv_cb)
{
    s_recv_cb = recv_cb;
    s_pending_mutex = xSemaphoreCreateMutex();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BRIDGE_I2C_PORT,
        .sda_io_num = BRIDGE_I2C_SDA_PIN,
        .scl_io_num = BRIDGE_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BRIDGE_I2C_ADDR,
        .scl_speed_hz = 100000,  /* 표준모드 — isolated 배선 특성 확인 전까지 보수적으로 */
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));

    /* 통신 계층 우선순위 17(project_cntl_task_priority_scheme과 동일 원칙) — 예전 ESP-NOW
     * 드라이버 수신 콜백을 대신해서 이 태스크가 들어옴 */
    xTaskCreate(i2c_bridge_poll_task, "i2c_bridge_poll", 4096, NULL, 17, NULL);

    ESP_LOGI(TAG, "I2C 브릿지 마스터 시작됨 (SDA=%d SCL=%d addr=0x%02x)",
             BRIDGE_I2C_SDA_PIN, BRIDGE_I2C_SCL_PIN, BRIDGE_I2C_ADDR);
}

void i2c_bridge_set_channel(uint8_t channel)
{
    bridge_link_packet_t pkt = { 0 };
    pkt.msg_type = BRIDGE_MSG_SET_CHANNEL;
    pkt.mac[0] = channel;
    bridge_link_seal(&pkt);
    esp_err_t err = i2c_master_transmit(s_dev, (const uint8_t *)&pkt, sizeof(pkt), BRIDGE_I2C_TIMEOUT_MS);
    ESP_LOGI(TAG, "SET_CHANNEL(%u) -> 브릿지: %s", channel, esp_err_to_name(err));
}
