#include "bridge_esp_now.h"
#include "can_link.h"
#include "can_bridge_link.h"
#include "ui_screen.h"

#include "esp_now.h"
#include "esp_now_reliable.h"
#include "esp_now_link.h"
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
/* 2026-09-26(설계 §3, 2-③) — 큐와 릴레이 태스크를 경로별 2벌로(can_bridge_path_for_esp_now_msg로
 * 분류). 하나로 두면 청크 하나를 CAN으로 보내는 동안(수십 ms) 뒤에 온 WAKE_HELLO가 기다림 */
typedef struct {
    can_bridge_queue_t q;
    uint32_t           hwm_logged;
    const char        *name;
} incoming_path_t;
static incoming_path_t s_ctrl_in = { .name = "CTRL" };
static incoming_path_t s_data_in = { .name = "DATA" };

/* recv_cb 전용 조립 버퍼(PSRAM, 1개) — recv_cb는 Wi-Fi 태스크에서만 순차 호출되고, push가
 * 복사하므로 1개면 충분(스택에 큰 버퍼를 두지 않음, feedback_never_put_large_data_on_stack) */
static uint8_t *s_rx_buf;

/* 2026-09-26 — 브 라디오의 현재 채널(wifi_bringup() 후 1회 읽음 — 브는 채널을 바꾸지 않음).
 * 0이면 아직 모름 → 광고 채널 필터 안 함 */
static uint8_t  s_own_channel = 0;
static uint32_t s_adjacent_adv_dropped = 0;

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || len <= 0 || len > BRIDGE_ESPNOW_MAX_FRAME) return;
    if (!s_rx_buf) return;

    /* 2026-09-26 — 이웃 채널 광고 버림. 2.4GHz 채널은 5MHz 간격인데 신호 폭이 ~20MHz라, 가까운
     * 노드가 CH2에서 보낸 광고도 CH1의 브에 수신돼서 스윕 한 번이 광고 2~3개로 콘까지 릴레이됐음
     * (실기, 캠 1대 기준). 노드가 광고에 실은 송신 채널(esp_now_advertise_t.channel)이 브 채널과
     * 다르면 CAN으로 안 넘김 — 콘은 브 채널에서 보낸 광고에만 응답하게 되고, 그 응답이 노드가 다른
     * 채널로 넘어간 뒤 도착해서 엉뚱한 채널에 동기화될 위험도 없어짐. 필드 없는 옛 펌웨어의
     * 짧은 광고는 여기서 판단 안 하고 넘김(콘이 길이 검사로 거름) */
    if (len >= (int)sizeof(esp_now_advertise_t) && data[1] == ESP_NOW_MSG_ADVERTISE && s_own_channel != 0) {
        const esp_now_advertise_t *adv = (const esp_now_advertise_t *)data;
        if (adv->channel != s_own_channel) {
            s_adjacent_adv_dropped++;
            ESP_LOGD(TAG, "이웃 채널 광고 버림(광고 CH%u, 브 CH%u, 누적 %u)",
                     (unsigned)adv->channel, (unsigned)s_own_channel, (unsigned)s_adjacent_adv_dropped);
            return;
        }
    }

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
    incoming_path_t *path = (len >= 2 && can_bridge_path_for_esp_now_msg(data[1]) == CAN_BRIDGE_CAT_DATA)
                          ? &s_data_in : &s_ctrl_in;
    can_bridge_queue_push(&path->q, s_rx_buf, CAN_BRIDGE_APP_HEADER_LEN + (size_t)len);

    /* 적체가 새 최고치를 16개 단위로 넘을 때만 로그(드롭은 없지만 쌓이는 정도는 보이게) */
    uint32_t count = 0, hwm = 0;
    can_bridge_queue_get_stats(&path->q, &count, &hwm);
    if (hwm >= path->hwm_logged + 16) {
        path->hwm_logged = hwm - (hwm % 16);
        ESP_LOGW(TAG, "%s 수신 큐 적체 최고치 %u개(현재 %u개)", path->name, (unsigned)hwm, (unsigned)count);
    }
}

/* 2026-09-26(설계 3단계) — 무선 수신이 아닌 곳(RELIABLE_SEND 대행 완료 콜백)에서 콘으로 보낼 app 메시지를
 * 경로 분류에 맞는 송신 큐에 넣음(복사). 호출 문맥에서 CAN 송신(블로킹)을 하지 않게 하려는 것 */
void bridge_esp_now_queue_to_cntl(const uint8_t *msg, size_t len)
{
    incoming_path_t *path = (can_bridge_path_for_app_msg(msg, len) == CAN_BRIDGE_CAT_DATA) ? &s_data_in : &s_ctrl_in;
    can_bridge_queue_push(&path->q, msg, len);
}

/* 2026-09-26 — 이벤트 방식: 큐에 항목이 들어오면(push) 알림으로 깨어나서 빌 때까지 CAN으로 보냄.
 * 경로마다 하나씩(arg = incoming_path_t). CAN 경로는 can_link_send()가 분류로 고름(같은 결과) */
static void relay_task(void *arg)
{
    incoming_path_t *path = (incoming_path_t *)arg;
    for (;;) {
        uint8_t *msg; size_t len;
        while (can_bridge_queue_pop(&path->q, &msg, &len)) {
            if (len > CAN_BRIDGE_APP_HEADER_LEN) {
                can_bridge_app_header_t hdr;
                memcpy(&hdr, msg, sizeof(hdr));
                const uint8_t *frame = msg + CAN_BRIDGE_APP_HEADER_LEN;
                size_t frame_len = len - CAN_BRIDGE_APP_HEADER_LEN;

                /* RX 로그는 캠에서 받은 무선 프레임(RELAY)만 — 대행 결과(RELIABLE_RESULT)는 RL 로그가 이미 찍힘 */
                if (hdr.msg_type == CAN_DATA_RELAY) {
                    char m6[7]; ui_screen_mac6(hdr.mac, m6);
                    const char *type_name = frame_len >= 2 ? ui_screen_msg_type_name(frame[1]) : "?";
                    ui_screen_log_wireless("RX(%d/%s/%u) %s", (int8_t)hdr.flags, m6, (unsigned)frame_len, type_name);
                }

                /* 2026-09-26 — can_link_send()가 경로 분류로 Control/Data ctx를 고름. CAN 링크가 아직
                 * 안 섰으면 ESP_ERR_INVALID_STATE(예전의 NULL ctx 크래시 방지와 같은 역할) */
                esp_err_t err = can_link_send(msg, len);
                if (err == ESP_ERR_INVALID_STATE) {
                    ui_screen_log_can("Relay dropped: not ready");
                } else if (err != ESP_OK) {
                    ui_screen_log_can("Relay fail: %s", ui_screen_err_short(err));
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
    /* 2026-09-23(1단계) — 콘의 node_hub.c에 있던 것 그대로 이식(사용자 지시: 콘엔
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

    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
        s_own_channel = primary;
        ESP_LOGI(TAG, "브 채널 CH%u — 다른 채널에서 보낸 광고는 버림(이웃 채널 수신 필터)", (unsigned)primary);
    } else {
        ESP_LOGW(TAG, "채널 조회 실패 — 이웃 채널 광고 필터 끔");
    }

    s_rx_buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + BRIDGE_ESPNOW_MAX_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx_buf) s_rx_buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + BRIDGE_ESPNOW_MAX_FRAME, MALLOC_CAP_8BIT);
    if (!s_rx_buf) {
        ESP_LOGE(TAG, "수신 조립 버퍼 할당 실패 — ESP-NOW 수신 릴레이 안 함");
    }

    /* 순서 주의 — 큐 초기화 + 릴레이 태스크 생성/알림 등록을 recv_cb 등록보다 먼저 해야 함
     * (초기화 전 큐에 push하면 드롭 금지 정책상 abort) */
    can_bridge_queue_init(&s_ctrl_in.q);
    can_bridge_queue_init(&s_data_in.q);

    static StaticTask_t s_ctrl_relay_tcb, s_data_relay_tcb;
    StackType_t *ctrl_relay_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StackType_t *data_relay_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    /* 2026-09-25(사용자 설계 — 코어 분리) — ESP-NOW는 코어 0(Wi-Fi 태스크와 같은 쪽), CAN은
     * 코어 1. 2026-09-26(설계 §3) — Control 17 / Data(SR) 15 */
    TaskHandle_t ctrl_relay = xTaskCreateStaticPinnedToCore(relay_task, "ctrl_relay", 4096 / sizeof(StackType_t), &s_ctrl_in, 17, ctrl_relay_stack, &s_ctrl_relay_tcb, 0);
    can_bridge_queue_set_notify_task(&s_ctrl_in.q, ctrl_relay);
    TaskHandle_t data_relay = xTaskCreateStaticPinnedToCore(relay_task, "data_relay", 4096 / sizeof(StackType_t), &s_data_in, 15, data_relay_stack, &s_data_relay_tcb, 0);
    can_bridge_queue_set_notify_task(&s_data_in.q, data_relay);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    ESP_LOGI(TAG, "브 ESP-NOW 라디오 소유 시작됨");
}
