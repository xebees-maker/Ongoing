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

/* 2026-09-26(사진 전송 CAN 개선 3단계 — 사용자 설계대로 "드롭 금지, 다 차기 전에 CAN 전송 시작") —
 * 예전엔 고정 8칸 FreeRTOS 큐라 CAN이 ESP-NOW보다 느린 사진 청크 구간에 가득 차서 드롭했음(40초에
 * 152~273회 실측). 이제 CAN 쪽과 같은 Common 큐(can_bridge_queue: PSRAM에서 필요한 만큼 늘어남,
 * 드롭 없음, 락, push 때 태스크 알림)를 씀. 항목 내용은 콘으로 보낼 CAN 메시지 그대로
 * (앱 헤더 8B[RELAY, mac, flags=rssi] + ESP-NOW 프레임) — 릴레이 태스크는 꺼내서 바로 보냄 */
static can_bridge_queue_t s_incoming_q;
static uint32_t s_incoming_hwm_logged = 0;

/* recv_cb 전용 조립 버퍼(PSRAM, 1개) — recv_cb는 Wi-Fi 태스크에서만 순차 호출되고, push가
 * 복사하므로 1개면 충분(스택에 큰 버퍼를 두지 않음, feedback_never_put_large_data_on_stack) */
static uint8_t *s_rx_buf;

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || len <= 0 || len > BRIDGE_ESPNOW_MAX_FRAME) return;
    if (!s_rx_buf) return;

    /* 2026-09-23(1단계) — 콘의 esp_now_reliable_request()를 브가 대행(can_link.c의
     * CAN_DATA_RELIABLE_SEND 처리 참고)하므로, 이 recv_cb가 그 대기 매칭도 콘 대신 해줘야 함
     * (예전 I2C Bridge/main.c의 recv_cb와 동일 패턴 — msg_type은 data[1], esp_now_link.h의
     * "version, msg_type" 공통 앞부분 구조). 대기 중인 요청이 없으면 조용히 무시하고 리턴 */
    if (len >= 2) {
        esp_now_reliable_on_recv(data[1], info->src_addr, data, len);
    }

    /* recv_cb는 ESP-NOW 내부 태스크 컨텍스트 — can_bridge_send()처럼 블로킹 가능한 호출을
     * 여기서 직접 하면 안 됨. 큐에 복사만 하고 즉시 반환(push가 릴레이 태스크를 깨움) */
    int8_t rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    can_bridge_app_header_t hdr = { .msg_type = CAN_DATA_RELAY, .flags = (uint8_t)rssi };
    memcpy(hdr.mac, info->src_addr, 6);
    memcpy(s_rx_buf, &hdr, CAN_BRIDGE_APP_HEADER_LEN);
    memcpy(s_rx_buf + CAN_BRIDGE_APP_HEADER_LEN, data, len);
    can_bridge_queue_push(&s_incoming_q, s_rx_buf, CAN_BRIDGE_APP_HEADER_LEN + (size_t)len);

    /* 적체가 새 최고치를 16개 단위로 넘을 때만 로그(드롭은 없지만 쌓이는 정도는 보이게) */
    uint32_t count = 0, hwm = 0;
    can_bridge_queue_get_stats(&s_incoming_q, &count, &hwm);
    if (hwm >= s_incoming_hwm_logged + 16) {
        s_incoming_hwm_logged = hwm - (hwm % 16);
        ESP_LOGW(TAG, "수신 큐 적체 최고치 %u개(현재 %u개)", (unsigned)hwm, (unsigned)count);
    }
}

/* 2026-09-26 — 이벤트 방식: 큐에 항목이 들어오면(push) 알림으로 깨어나서 빌 때까지 CAN으로 보냄 */
static void relay_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t *msg; size_t len;
        while (can_bridge_queue_pop(&s_incoming_q, &msg, &len)) {
            if (len > CAN_BRIDGE_APP_HEADER_LEN) {
                can_bridge_app_header_t hdr;
                memcpy(&hdr, msg, sizeof(hdr));
                const uint8_t *frame = msg + CAN_BRIDGE_APP_HEADER_LEN;
                size_t frame_len = len - CAN_BRIDGE_APP_HEADER_LEN;

                char m6[7]; ui_screen_mac6(hdr.mac, m6);
                const char *type_name = frame_len >= 2 ? ui_screen_msg_type_name(frame[1]) : "?";
                ui_screen_log_wireless("RX(%d/%s/%u) %s", (int8_t)hdr.flags, m6, (unsigned)frame_len, type_name);

                /* 매번 다시 가져옴(태스크 시작 시점에 캡처해서 NULL로 굳어버리는 순서 버그를 실기에서
                 * 겪음 — can_link_init()이 나중에 불려도 이러면 안전) */
                can_bridge_ctx_t *data_ctx = can_link_get_data_ctx();
                if (!data_ctx) {
                    ui_screen_log_can("Relay dropped: not ready");
                } else {
                    esp_err_t err = can_bridge_send(data_ctx, msg, len);
                    if (err != ESP_OK) {
                        ui_screen_log_can("Relay fail: %s", ui_screen_err_short(err));
                    }
                }
            }
            can_bridge_queue_pop_free(msg);
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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

    s_rx_buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + BRIDGE_ESPNOW_MAX_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx_buf) s_rx_buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + BRIDGE_ESPNOW_MAX_FRAME, MALLOC_CAP_8BIT);
    if (!s_rx_buf) {
        ESP_LOGE(TAG, "수신 조립 버퍼 할당 실패 — ESP-NOW 수신 릴레이 안 함");
    }

    /* 순서 주의 — 큐 초기화 + 릴레이 태스크 생성/알림 등록을 recv_cb 등록보다 먼저 해야 함
     * (초기화 전 큐에 push하면 드롭 금지 정책상 abort) */
    can_bridge_queue_init(&s_incoming_q);

    static StaticTask_t s_relay_tcb;
    StackType_t *relay_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    /* 2026-09-25(사용자 설계 — 코어 분리) — ESP-NOW는 코어 0(Wi-Fi 태스크와 같은 쪽), CAN은
     * 코어 1. 통신 등급 17(project_cntl_task_priority_scheme, 콘과 동일) */
    TaskHandle_t relay = xTaskCreateStaticPinnedToCore(relay_task, "esp_now_relay", 4096 / sizeof(StackType_t), NULL, 17, relay_stack, &s_relay_tcb, 0);
    can_bridge_queue_set_notify_task(&s_incoming_q, relay);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    ESP_LOGI(TAG, "브 ESP-NOW 라디오 소유 시작됨");
}
