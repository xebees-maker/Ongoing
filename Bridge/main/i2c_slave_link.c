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
#include "status_led.h"

static const char *TAG = "i2c_slave_link";

/* 2026-09-21 — XIAO ESP32-C6 공식 핀아웃(사용자 확인, wiki.seeedstudio.com/xiao_esp32c6_getting_started):
 * SDA=D4=GPIO22, SCL=D5=GPIO23 */
#define BRIDGE_I2C_PORT    0
#define BRIDGE_I2C_SDA_PIN 22
#define BRIDGE_I2C_SCL_PIN 23
#define BRIDGE_I2C_ADDR    0x42   /* TODO — 임의값, 충돌 확인 필요(7비트 주소) */

/* 2026-09-21(사용자 지시 — "받을 때/보낼 때 구분되게, 너무 짧으면 안 구분되니까") — XIAO
 * ESP32-C6 유일한 사용자제어 LED(GPIO15, 나머지 하나는 충전표시 하드웨어 전용, 소프트웨어로
 * 못 건드림). Common/components/status_led(Sens의 Green/Blue LED와 동일 공용 드라이버)를
 * 그대로 씀 — 평소엔 HEARTBEAT(살아있음), 수신 시 ON(고정 점등)으로 500ms, 송신 시
 * BLINK_FAST(빠른 점멸)로 1000ms 바꿨다가 HEARTBEAT로 복귀. 두 이벤트가 겹쳐도 순서대로
 * 처리되게 전용 태스크+큐로 분리(I2C rx/tx 처리 자체를 지연시키지 않기 위함) */
#define BRIDGE_LED_PIN GPIO_NUM_15
typedef enum { LED_EVT_RX, LED_EVT_TX } led_evt_t;
static QueueHandle_t s_led_evt_queue = NULL;

static void led_task(void *arg)
{
    (void)arg;
    status_led_init(BRIDGE_LED_PIN);
    status_led_set_pattern(BRIDGE_LED_PIN, LED_PATTERN_HEARTBEAT);
    for (;;) {
        led_evt_t evt;
        if (xQueueReceive(s_led_evt_queue, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (evt == LED_EVT_RX) {
            status_led_set_pattern(BRIDGE_LED_PIN, LED_PATTERN_ON);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            status_led_set_pattern(BRIDGE_LED_PIN, LED_PATTERN_BLINK_FAST);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        status_led_set_pattern(BRIDGE_LED_PIN, LED_PATTERN_HEARTBEAT);
    }
}

static void led_notify(led_evt_t evt)
{
    xQueueSend(s_led_evt_queue, &evt, 0);  /* 가득 차면 그냥 버림 — 표시용이라 무해 */
}

/* 마스터(CNTL)가 쓴 걸 받아서 처리할 큐, 마스터가 읽어갈 걸 올려둘 큐 — 둘 다 ISR에서
 * FromISR API로 넣고, 전용 태스크가 일반 컨텍스트에서 소비함(i2c_slave_write()가
 * IRAM_ATTR 아니라 ISR 콜백에서 직접 부르면 안 됨 — esp_driver_i2c/i2c_slave.c 확인) */
#define I2C_QUEUE_DEPTH 8
static QueueHandle_t s_rx_queue = NULL;   /* CNTL -> 브릿지 (완성된 메시지) */
static QueueHandle_t s_tx_queue = NULL;   /* 브릿지 -> CNTL (완성된 메시지, 드레인 대상) */
static TaskHandle_t  s_tx_task_handle = NULL;

static i2c_slave_dev_handle_t s_i2c_slave = NULL;

/* 2026-09-21 가변크기 재설계 — CNTL은 항상 헤더 트랜잭션 먼저, payload_len>0이면 곧바로
 * 페이로드(+CRC) 트랜잭션을 이어서 보냄(i2c_bridge.c의 transmit_msg() 참고, 그 파일이
 * 이 두 트랜잭션 사이에 다른 마스터 트랜잭션을 끼워넣지 않는다는 게 여기 상태머신의 전제).
 * on_receive는 하드웨어 FIFO 한도 이상은 여러 번 나눠서 올 수 있으므로(드라이버 링버퍼
 * 주석 참고) 각 단계가 목표 바이트수를 채울 때까지 계속 이어붙임. s_rx_staging은 ISR
 * 전용 조립 버퍼(전역 static) — sizeof가 1.5KB 가까이 되는 구조체를 스택에 두면 안 된다는
 * 규칙(2026-09-21 CNTL 스택오버플로우 크래시로 확인됨) 때문에 ISR 지역변수로 두지 않음.
 * ISR에서만 건드리므로 동시접근 걱정 없음(I2C 슬레이브 ISR 콜백은 직렬 호출) */
typedef enum { RX_PHASE_HEADER, RX_PHASE_PAYLOAD } rx_phase_t;
static rx_phase_t        s_rx_phase = RX_PHASE_HEADER;
static bridge_link_msg_t s_rx_staging;
static size_t             s_rx_accum_len = 0;

static IRAM_ATTR bool on_receive(i2c_slave_dev_handle_t i2c_slave,
                                  const i2c_slave_rx_done_event_data_t *evt_data, void *arg)
{
    BaseType_t hp_task_woken = pdFALSE;
    size_t n = evt_data->length;
    const uint8_t *src = evt_data->buffer;

    while (n > 0) {
        uint8_t *dst;
        size_t target;
        if (s_rx_phase == RX_PHASE_HEADER) {
            dst = (uint8_t *)&s_rx_staging.header;
            target = sizeof(s_rx_staging.header);
        } else {
            dst = s_rx_staging.payload;
            target = (size_t)s_rx_staging.header.payload_len + 2;
        }
        size_t space = target - s_rx_accum_len;
        size_t take = n < space ? n : space;
        memcpy(dst + s_rx_accum_len, src, take);
        s_rx_accum_len += take;
        src += take;
        n -= take;

        if (s_rx_accum_len < target) break;  /* 이 단계 아직 덜 모임 — 다음 on_receive 호출 대기 */

        if (s_rx_phase == RX_PHASE_HEADER) {
            uint16_t plen = s_rx_staging.header.payload_len;
            if (plen == 0) {
                xQueueSendFromISR(s_rx_queue, &s_rx_staging, &hp_task_woken);
            } else if (plen <= BRIDGE_LINK_MAX_PAYLOAD) {
                s_rx_accum_len = 0;
                s_rx_phase = RX_PHASE_PAYLOAD;
                continue;  /* 페이로드 단계로 전환 — 이번 호출에 남은 n바이트 있으면 계속 소비 */
            }
            /* plen이 상한을 넘으면 손상된 헤더로 보고 버림 — 아래 공통 리셋으로 다음
             * 트랜잭션부터 재동기화(HEADER 단계는 이미 그대로이므로 별도 처리 불필요) */
        } else {
            xQueueSendFromISR(s_rx_queue, &s_rx_staging, &hp_task_woken);
            s_rx_phase = RX_PHASE_HEADER;
        }
        s_rx_accum_len = 0;
    }
    return hp_task_woken == pdTRUE;
}

/* 2026-09-22(임시 진단, 사용자 지시 — espressif/esp-idf#15259와 같은 증상인지: on_request
 * ISR이 vTaskNotifyGiveFromISR로 tx 태스크를 실제로 깨우는지 확인). ISR 컨텍스트라 ESP_LOG를
 * 직접 부르면 안 되므로(IRAM/ISR 세이프 아님) 카운터만 증가시키고, diag_task(main.c)가
 * 주기적으로 읽어서 로그로 찍음. i2c_slave_link_get_diag_counters()로 노출 */
static volatile uint32_t s_on_request_count = 0;
static volatile uint32_t s_on_request_woken_count = 0;
static volatile uint32_t s_tx_task_notify_count = 0;

void i2c_slave_link_get_diag_counters(uint32_t *on_request, uint32_t *on_request_woken, uint32_t *tx_notified)
{
    if (on_request) *on_request = s_on_request_count;
    if (on_request_woken) *on_request_woken = s_on_request_woken_count;
    if (tx_notified) *tx_notified = s_tx_task_notify_count;
}

/* 마스터가 읽으려는데 FIFO가 비어있을 때(=지금 올려줄 게 없음) 호출됨 — tx 태스크를 깨워서
 * NONE 메시지를 새로 스테이징하게 함(이 콜백 자신은 i2c_slave_write()를 직접 못 부름, ISR임) */
static IRAM_ATTR bool on_request(i2c_slave_dev_handle_t i2c_slave,
                                  const i2c_slave_request_event_data_t *evt_data, void *arg)
{
    s_on_request_count++;
    BaseType_t hp_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_tx_task_handle, &hp_task_woken);
    if (hp_task_woken == pdTRUE) s_on_request_woken_count++;
    return hp_task_woken == pdTRUE;
}

/* 브릿지 -> CNTL 큐를 소비해서 실제 I2C 슬레이브 FIFO에 스테이징. 큐가 비었는데 on_request로
 * 깨워졌으면(마스터가 폴링했는데 줄 게 없음) 빈 NONE 메시지를 대신 스테이징함.
 * 2026-09-21 가변크기 재설계 — 헤더+페이로드를 한 번의 i2c_slave_write() 호출로 같이 내보냄
 * (bridge_link_msg_t가 packed라 &msg에서 wire_len만큼이 정확히 헤더+페이로드+CRC 연속 바이트임).
 * 슬레이브 송신버퍼는 링버퍼라 CNTL의 두 번(헤더/페이로드) 분리 읽기가 이 한 번의 연속 쓰기를
 * 순서대로 나눠 가져가는 방식으로 자연히 맞물림 — 별도 2단계 상태 추적 불필요 */
static void i2c_tx_task(void *arg)
{
    (void)arg;
    static bridge_link_msg_t msg;  /* 재사용 버퍼 — 이 태스크만 소유, 스택에 두지 않음(규칙) */
    for (;;) {
        if (xQueueReceive(s_tx_queue, &msg, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (ulTaskNotifyTake(pdTRUE, 0) == 0) continue;
            s_tx_task_notify_count++;
            memset(&msg.header, 0, sizeof(msg.header));
            msg.header.msg_type = BRIDGE_MSG_NONE;
        }
        bridge_link_msg_seal(&msg);
        size_t wire_len = bridge_link_msg_wire_len(&msg);
        uint32_t written = 0;
        esp_err_t err = i2c_slave_write(s_i2c_slave, (const uint8_t *)&msg, wire_len, &written, pdMS_TO_TICKS(200));
        if (err != ESP_OK || written != wire_len) {
            ESP_LOGW(TAG, "i2c_slave_write 실패(%s, %u/%u바이트)", esp_err_to_name(err),
                     (unsigned)written, (unsigned)wire_len);
        } else if (msg.header.msg_type != BRIDGE_MSG_NONE) {
            led_notify(LED_EVT_TX);  /* NONE(빈 폴링 응답)은 실제 통신이 아니라 표시 안 함 */
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

static void handle_reliable_send(const bridge_link_msg_t *req)
{
    static bridge_link_msg_t result;  /* 재사용 — 이 함수는 reliable_worker_task에서만, 직렬 호출 */
    memset(&result.header, 0, sizeof(result.header));
    result.header.msg_type = BRIDGE_MSG_RELIABLE_RESULT;
    memcpy(result.header.mac, req->header.mac, 6);

    add_peer_if_needed(req->header.mac);
    size_t reply_len = 0;
    /* reply_out_cap = payload[] 크기 - 2 : 뒤 2바이트는 tx_task에서 붙일 CRC 자리이므로
     * esp_now_reliable_request()의 응답 데이터가 그 자리를 침범하면 안 됨 */
    esp_err_t err = esp_now_reliable_request(req->header.mac, req->payload, req->header.payload_len,
                                              req->header.accept_reply_types, req->header.accept_reply_types_count,
                                              req->header.timeout_ms, req->header.max_attempts,
                                              result.payload, sizeof(result.payload) - 2, &reply_len);
    result.header.ok = (err == ESP_OK) ? 1 : 0;
    result.header.payload_len = (uint16_t)reply_len;
    if (xQueueSend(s_tx_queue, &result, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "RELIABLE_RESULT 큐잉 실패(tx 큐 포화)");
    }
}

static void handle_fire_and_forget(const bridge_link_msg_t *req)
{
    add_peer_if_needed(req->header.mac);
    esp_err_t err = esp_now_send(req->header.mac, req->payload, req->header.payload_len);
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
    static bridge_link_msg_t req;  /* 재사용 — 이 태스크만 소유, 스택에 두지 않음(규칙) */
    for (;;) {
        if (xQueueReceive(s_reliable_queue, &req, portMAX_DELAY) == pdTRUE) {
            handle_reliable_send(&req);
        }
    }
}

static void i2c_rx_task(void *arg)
{
    (void)arg;
    static bridge_link_msg_t msg;  /* 재사용 — 이 태스크만 소유, 스택에 두지 않음(규칙) */
    for (;;) {
        if (xQueueReceive(s_rx_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        if (!bridge_link_header_verify(&msg.header)) {
            ESP_LOGW(TAG, "헤더 CRC 불일치 — 프레임 버림(msg_type=%u)", msg.header.msg_type);
            continue;
        }
        if (msg.header.payload_len > 0 && !bridge_link_payload_verify(msg.payload, msg.header.payload_len)) {
            ESP_LOGW(TAG, "페이로드 CRC 불일치 — 프레임 버림(msg_type=%u)", msg.header.msg_type);
            continue;
        }
        led_notify(LED_EVT_RX);
        switch (msg.header.msg_type) {
        case BRIDGE_MSG_RELIABLE_SEND:
            if (xQueueSend(s_reliable_queue, &msg, 0) != pdTRUE) {
                ESP_LOGW(TAG, "reliable 큐 포화 — 요청 버림");
            }
            break;
        case BRIDGE_MSG_FIRE_AND_FORGET:
            handle_fire_and_forget(&msg);
            break;
        case BRIDGE_MSG_SET_CHANNEL: {
            uint8_t ch = msg.header.mac[0];
            esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            ESP_LOGI(TAG, "SET_CHANNEL(%u): %s", ch, esp_err_to_name(err));
            break;
        }
        case BRIDGE_MSG_PING: {
            static bridge_link_msg_t pong;  /* 재사용 — 스택에 두지 않음(규칙) */
            memset(&pong.header, 0, sizeof(pong.header));
            pong.header.msg_type = BRIDGE_MSG_PONG;
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
            ESP_LOGW(TAG, "알 수 없는 msg_type=%u — 무시", msg.header.msg_type);
            break;
        }
    }
}

void i2c_slave_link_queue_incoming(const uint8_t *mac, int8_t rssi, const uint8_t *data, size_t len)
{
    if (len > BRIDGE_LINK_MAX_PAYLOAD) len = BRIDGE_LINK_MAX_PAYLOAD;
    static bridge_link_msg_t msg;  /* 재사용 — ESP-NOW recv 콜백 컨텍스트에서만 직렬 호출됨,
                                     * 스택에 두지 않음(규칙) */
    memset(&msg.header, 0, sizeof(msg.header));
    msg.header.msg_type = BRIDGE_MSG_INCOMING;
    memcpy(msg.header.mac, mac, 6);
    msg.header.rssi = rssi;
    msg.header.payload_len = (uint16_t)len;
    memcpy(msg.payload, data, len);
    if (xQueueSend(s_tx_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "INCOMING_MSG 큐잉 실패(tx 큐 포화) — CNTL이 이 수신을 못 봄");
    }
}

void i2c_slave_link_init(void)
{
    s_rx_queue = xQueueCreate(I2C_QUEUE_DEPTH, sizeof(bridge_link_msg_t));
    s_tx_queue = xQueueCreate(I2C_QUEUE_DEPTH, sizeof(bridge_link_msg_t));
    s_reliable_queue = xQueueCreate(RELIABLE_QUEUE_DEPTH, sizeof(bridge_link_msg_t));
    s_led_evt_queue = xQueueCreate(8, sizeof(led_evt_t));

    /* 통신 계층 우선순위 17 — CNTL 쪽 tx_dispatcher/worker와 동일 원칙(project_cntl_task_priority_scheme).
     * LED는 표시 전용이라 낮은 우선순위(1)로 — I2C 처리 지연시키면 안 됨 */
    xTaskCreate(i2c_rx_task, "i2c_rx", 4096, NULL, 17, NULL);
    xTaskCreate(reliable_worker_task, "reliable_worker", 4096, NULL, 17, NULL);
    xTaskCreate(i2c_tx_task, "i2c_tx", 3072, NULL, 17, &s_tx_task_handle);
    xTaskCreate(led_task, "bridge_led", 2048, NULL, 1, NULL);

    i2c_slave_config_t cfg = {
        .i2c_port      = BRIDGE_I2C_PORT,
        .sda_io_num    = BRIDGE_I2C_SDA_PIN,
        .scl_io_num    = BRIDGE_I2C_SCL_PIN,
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = sizeof(bridge_link_msg_t) * 2,
        .receive_buf_depth = sizeof(bridge_link_msg_t) * 2,
        .slave_addr    = BRIDGE_I2C_ADDR,
        .addr_bit_len  = I2C_ADDR_BIT_LEN_7,
    };
    /* 2026-09-22(임시 진단 — 사용자 지적: 풀업 없는 상태의 SDA/SCL=0 측정은 "능동적으로 물고
     * 있음"과 "그냥 뜬 채로 로우"를 구별 못 함. 내장 풀업을 켜서 재측정 — 풀업 켰는데도 0이면
     * 진짜로 뭔가 물고 있는 것, 1로 올라가면 그냥 뜬 상태였던 것) */
    cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_slave_device(&cfg, &s_i2c_slave));

    i2c_slave_event_callbacks_t cbs = {
        .on_receive = on_receive,
        .on_request = on_request,
    };
    ESP_ERROR_CHECK(i2c_slave_register_event_callbacks(s_i2c_slave, &cbs, NULL));

    ESP_LOGI(TAG, "I2C 슬레이브 시작됨 (SDA=%d SCL=%d addr=0x%02x)",
             BRIDGE_I2C_SDA_PIN, BRIDGE_I2C_SCL_PIN, BRIDGE_I2C_ADDR);
}
