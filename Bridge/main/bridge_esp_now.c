#include "bridge_esp_now.h"
#include "can_link.h"
#include "can_bridge_link.h"
#include "ui_screen.h"

#include "esp_now.h"
#include "esp_now_reliable.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "bridge_esp_now";

/* ESP-NOW v2 최대 프레임 — can_link.c의 DATA 재조립 버퍼와 맞춤(1536 = 8+1470 여유) */
#define BRIDGE_ESPNOW_MAX_FRAME 1470

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;
    uint16_t len;
    uint8_t  data[BRIDGE_ESPNOW_MAX_FRAME];
} incoming_t;

static QueueHandle_t s_incoming_q;

/* 2026-09-25(실기 — "stack overflow in task esp_now_relay") — incoming_t(약 1480B)를
 * recv_cb/relay_task 양쪽에서 스택 지역변수로 잡고 있었음(feedback_never_put_large_data_on_stack
 * 위반). 초기화 때 PSRAM에 한 번씩만 잡아 두고 포인터로만 씀. recv_cb는 Wi-Fi 태스크에서만
 * 순차 호출되고 relay_task는 자기 것만 쓰므로 각각 버퍼 1개면 충분(xQueueSend/Receive가 복사) */
static incoming_t *s_rx_item;     /* recv_cb 전용 */
static incoming_t *s_relay_item;  /* relay_task 전용 */

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || len <= 0 || len > BRIDGE_ESPNOW_MAX_FRAME) return;
    if (!s_rx_item || !s_incoming_q) return;

    /* 2026-09-23(1단계) — 콘의 esp_now_reliable_request()를 브가 대행(can_link.c의
     * CAN_DATA_RELIABLE_SEND 처리 참고)하므로, 이 recv_cb가 그 대기 매칭도 콘 대신 해줘야 함
     * (예전 I2C Bridge/main.c의 recv_cb와 동일 패턴 — msg_type은 data[1], esp_now_link.h의
     * "version, msg_type" 공통 앞부분 구조). 대기 중인 요청이 없으면 조용히 무시하고 리턴 */
    if (len >= 2) {
        esp_now_reliable_on_recv(data[1], info->src_addr, data, len);
    }

    /* recv_cb는 ESP-NOW 내부 태스크 컨텍스트 — can_bridge_send()처럼 블로킹 가능한 호출을
     * 여기서 직접 하면 안 됨. 큐에 복사만 하고 즉시 반환 */
    incoming_t *item = s_rx_item;
    memcpy(item->mac, info->src_addr, 6);
    item->rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    item->len = (uint16_t)len;
    memcpy(item->data, data, len);

    if (xQueueSend(s_incoming_q, item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "수신 큐 가득참 — 드롭(mac=%02X%02X%02X%02X%02X%02X)",
                 item->mac[0], item->mac[1], item->mac[2], item->mac[3], item->mac[4], item->mac[5]);
    }
}

static void relay_task(void *arg)
{
    (void)arg;
    incoming_t *item = s_relay_item;

    uint8_t *msg = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + BRIDGE_ESPNOW_MAX_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!msg) msg = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + BRIDGE_ESPNOW_MAX_FRAME, MALLOC_CAP_8BIT);
    if (!msg || !item) {
        ESP_LOGE(TAG, "relay_task 버퍼 할당 실패(msg=%p item=%p) — 릴레이 중단", (void *)msg, (void *)item);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        if (xQueueReceive(s_incoming_q, item, portMAX_DELAY) != pdTRUE) continue;

        char m6[7]; ui_screen_mac6(item->mac, m6);
        const char *type_name = item->len >= 2 ? ui_screen_msg_type_name(item->data[1]) : "?";
        ui_screen_log_wireless("RX(%d/%s/%u) %s", item->rssi, m6, item->len, type_name);

        can_bridge_app_header_t hdr = { .msg_type = CAN_DATA_RELAY, .flags = (uint8_t)item->rssi };
        memcpy(hdr.mac, item->mac, 6);
        memcpy(msg, &hdr, CAN_BRIDGE_APP_HEADER_LEN);
        memcpy(msg + CAN_BRIDGE_APP_HEADER_LEN, item->data, item->len);

        /* 매번 다시 가져옴(태스크 시작 시점에 캡처해서 NULL로 굳어버리는 순서 버그를 실기에서
         * 겪음 — can_link_init()이 나중에 불려도 이러면 안전) */
        can_bridge_ctx_t *data_ctx = can_link_get_data_ctx();
        if (!data_ctx) {
            ui_screen_log_can("Relay dropped: not ready");
            continue;
        }
        esp_err_t err = can_bridge_send(data_ctx, msg, CAN_BRIDGE_APP_HEADER_LEN + item->len);
        if (err != ESP_OK) {
            ui_screen_log_can("Relay fail: %s", ui_screen_err_short(err));
        }
    }
}

/* 2026-09-25 — 송신 1건 완료 = 드라이버 송신 큐에 자리 생김. NO_MEM으로 대기 중인
 * esp_now_reliable_request()(can_link.c의 RELIABLE_SEND 대행)를 깨움 — 브는 그 외 send_cb
 * 용도가 없음 */
static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    (void)status;
    esp_now_reliable_on_send_done();
}

static void add_peer_if_needed(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;   /* 현재 라디오 채널 그대로 */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add_peer 실패(mac=%02X%02X%02X%02X%02X%02X): %s",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], esp_err_to_name(err));
        return;
    }
    /* 2026-09-23(1단계) — 콘의 esp_now_hub.c에 있던 것 그대로 이식(사용자 지시: 콘엔
     * ESP-NOW 관련 기능이 전혀 없어야 함 — 피어관리/레이트설정도 브 몫). CAM 쪽과 짝맞춤
     * (esp_now_cam.c 동일 설정) */
    esp_now_rate_config_t rate_cfg = { .phymode = WIFI_PHY_MODE_HT20, .rate = WIFI_PHY_RATE_MCS0_LGI, .ersu = false, .dcm = false };
    esp_err_t rate_err = esp_now_set_peer_rate_config(mac, &rate_cfg);
    ESP_LOGI(TAG, "피어 레이트 설정(MCS0/HT20) -> %s", esp_err_to_name(rate_err));
}

void bridge_esp_now_ensure_peer(const uint8_t mac[6])
{
    add_peer_if_needed(mac);
}

void bridge_esp_now_send_raw(const uint8_t mac[6], const uint8_t *data, uint16_t len)
{
    add_peer_if_needed(mac);
    esp_now_send(mac, data, len);
    char m6[7]; ui_screen_mac6(mac, m6);
    const char *type_name = len >= 2 ? ui_screen_msg_type_name(data[1]) : "?";
    ui_screen_log_wireless("TX(%s/%u) %s", m6, len, type_name);
}

static void wifi_bringup(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* AP에 접속하지 않는 순수 STA(ESP-NOW 전용 라디오). 캠이 실제로 이 보드를 바라보게 하려면
     * 캠과 같은 채널이어야 하는데, 지금은 캠이 아직 콘(COM19)에 페어링되어 있어 이 보드로는
     * 안 옴(유니캐스트라 안 보임, ADVERTISE 브로드캐스트로 재광고해야 관측 가능) — 채널은
     * 우선 기본값(1)에 둠, 실제 캠 연결 단계에서 맞출 것 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void bridge_esp_now_init(void)
{
    wifi_bringup();

    s_rx_item = (incoming_t *)heap_caps_malloc(sizeof(incoming_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx_item) s_rx_item = (incoming_t *)heap_caps_malloc(sizeof(incoming_t), MALLOC_CAP_8BIT);
    s_relay_item = (incoming_t *)heap_caps_malloc(sizeof(incoming_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_relay_item) s_relay_item = (incoming_t *)heap_caps_malloc(sizeof(incoming_t), MALLOC_CAP_8BIT);
    if (!s_rx_item || !s_relay_item) {
        ESP_LOGE(TAG, "incoming 버퍼 할당 실패(rx=%p relay=%p)", (void *)s_rx_item, (void *)s_relay_item);
    }

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    /* 2026-09-22(사용자 지적 — "버퍼 또 인터널로 잡았냐?") — incoming_t가 1479B씩(ESP-NOW
     * 최대 프레임 포함) 8개라 ~11.8KB나 내부RAM에 잡고 있었음. PSRAM으로 옮김. 태스크 스택도
     * 동일 원칙 적용(esp_now_tx.c의 tx_worker 패턴) */
    static StaticQueue_t s_incoming_q_struct;
    uint8_t *incoming_q_storage = (uint8_t *)heap_caps_malloc(8 * sizeof(incoming_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_incoming_q = xQueueCreateStatic(8, sizeof(incoming_t), incoming_q_storage, &s_incoming_q_struct);

    static StaticTask_t s_relay_tcb;
    StackType_t *relay_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    /* 2026-09-25(사용자 설계 — 코어 분리) — ESP-NOW는 코어 0(Wi-Fi 태스크와 같은 쪽), CAN은
     * 코어 1. 통신 등급 17(project_cntl_task_priority_scheme, 콘과 동일) */
    xTaskCreateStaticPinnedToCore(relay_task, "esp_now_relay", 4096 / sizeof(StackType_t), NULL, 17, relay_stack, &s_relay_tcb, 0);

    ESP_LOGI(TAG, "브 ESP-NOW 라디오 소유 시작됨");
}
