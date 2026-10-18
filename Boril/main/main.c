/**
 * @file    main.c
 * @brief   보릴(보일러 릴레이 장치) — 2026-10-18(Docs/설계_보브_보릴_2026-10-18.md 4단계: 광고·연결)
 *
 * XIAO ESP32-C6, 상시 전원(딥슬립 없음). 콘과는 보브(브릿지 2번)를 거쳐 ESP-NOW로 통신 — 콘은 광고 이름 "B..."를 보고 보브를 통해서만
 * 응답함(can_bridge.c). 연결 절차는 센스·캠과 같음: 1~13 채널을 돌며 광고 → 콘의 ADVERTISE_ACK로 채널 고정 → PAIR_REQUEST에 PAIR_ACK.
 * 릴레이 제어·상태 보고·통신 끊김 타이머는 다음 단계(5단계).
 *
 * 안테나(회로도 XIAO-ESP32-C6_v1.0): RF 스위치 전원 GPIO3(0 = 켜짐, 기본 풀업이라 꺼짐), 안테나 선택 GPIO14(0 = 내장 세라믹,
 * 1 = U.FL 외장, 기본 풀다운). 외장 안테나를 씀(사용자 — 상황에 따라 다르지만 지금은 외장).
 * 국가 설정 KR(채널 1~13) — 브릿지 기본("01", 1~11 보고)과의 애매함을 없앰(사용자 10-18).
 */
#include <string.h>
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_now_link.h"

static const char *TAG = "BORIL";

#define RF_SWITCH_POWER_GPIO   GPIO_NUM_3    /* 0 = RF 스위치 켜짐 */
#define RF_ANT_SELECT_GPIO     GPIO_NUM_14   /* 1 = 외장(U.FL) */

#define SCAN_CHANNEL_MIN    1
#define SCAN_CHANNEL_MAX    13
#define SCAN_DWELL_US       (300 * 1000)       /* 채널마다 광고하고 머무는 시간 */
#define ADVERTISE_PERIOD_US (1000 * 1000)      /* 채널을 찾은 뒤(연결 전) 광고 주기 */

static char    s_name[ESP_NOW_LINK_NAME_LEN];
static uint8_t s_mac[6];
static uint8_t s_hub_mac[6];
static bool    s_paired;
static bool    s_channel_locked;
static uint8_t s_scan_channel = SCAN_CHANNEL_MIN;
static bool    s_first_tick;      /* 광고를 막 시작함 — 첫 틱은 채널을 옮기지 않음 */
static esp_timer_handle_t s_adv_timer;
static const uint8_t s_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void antenna_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << RF_SWITCH_POWER_GPIO) | (1ULL << RF_ANT_SELECT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(RF_SWITCH_POWER_GPIO, 0);
    gpio_set_level(RF_ANT_SELECT_GPIO, 1);
    ESP_LOGI(TAG, "Antenna: external (U.FL)");
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_country_code("KR", false));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));   /* AP에 접속하지 않음 — ESP-NOW 전용 */
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    wifi_country_t c;
    if (esp_wifi_get_country(&c) == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi country %.2s: channels %u-%u", c.cc, (unsigned)c.schan, (unsigned)(c.schan + c.nchan - 1));
    }
}

static void enter_advertising(void)
{
    s_paired = false;
    s_channel_locked = false;
    s_scan_channel = SCAN_CHANNEL_MIN;
    s_first_tick = true;
    esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);
    esp_timer_stop(s_adv_timer);
    esp_timer_start_periodic(s_adv_timer, SCAN_DWELL_US);
}

/* 채널을 모르면 먼저 다음 채널로 옮기고 거기서 광고 — 응답(ADVERTISE_ACK)은 브릿지·CAN·콘을 왕복해 늦게 오므로 광고한 채널에 다음
 * 틱까지 머물러야 받음(실기 — 광고 직후 채널을 옮기던 순서로는 응답을 매번 놓침). 맨 처음 틱은 enter_advertising이 맞춘 채널에서 */
static void adv_timer_cb(void *arg)
{
    (void)arg;
    if (!s_channel_locked && !s_first_tick) {
        s_scan_channel = s_scan_channel >= SCAN_CHANNEL_MAX ? SCAN_CHANNEL_MIN : s_scan_channel + 1;
        esp_err_t ce = esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);
        if (ce != ESP_OK) ESP_LOGW(TAG, "Channel CH%u failed: %s", (unsigned)s_scan_channel, esp_err_to_name(ce));
    }
    s_first_tick = false;
    esp_now_advertise_t msg = {
        .version = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_ADVERTISE,
        .channel = s_scan_channel,
    };
    memcpy(msg.name, s_name, sizeof(msg.name));
    memcpy(msg.mac, s_mac, sizeof(msg.mac));
    esp_err_t err = esp_now_send(s_broadcast, (const uint8_t *)&msg, sizeof(msg));
    if (err != ESP_OK) ESP_LOGW(TAG, "Advertise send failed: %s", esp_err_to_name(err));
}

static void add_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = { .ifidx = WIFI_IF_STA, .channel = 0, .encrypt = false };
    memcpy(peer.peer_addr, mac, 6);
    esp_now_add_peer(&peer);
}

/* ESP-NOW 수신(Wi-Fi 태스크) — 짧게 처리 */
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 2) return;
    uint8_t type = data[1];
    if (type == ESP_NOW_MSG_ADVERTISE_ACK && !s_paired && !s_channel_locked) {
        s_channel_locked = true;
        esp_timer_stop(s_adv_timer);
        esp_timer_start_periodic(s_adv_timer, ADVERTISE_PERIOD_US);
        ESP_LOGI(TAG, "Cntl found on CH%u (via bridge " MACSTR ") - waiting for pairing", (unsigned)s_scan_channel,
                 MAC2STR(info->src_addr));
    } else if (type == ESP_NOW_MSG_PAIR_REQUEST && len >= (int)sizeof(esp_now_pair_request_t)) {
        /* 콘의 MAC은 쓰지 않음 — 응답 상대는 프레임의 실제 발신지(브릿지) */
        memcpy(s_hub_mac, info->src_addr, 6);
        add_peer(s_hub_mac);
        bool was_paired = s_paired;
        s_paired = true;
        esp_timer_stop(s_adv_timer);
        esp_now_pair_ack_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PAIR_ACK };
        memcpy(ack.node_mac, s_mac, 6);
        esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
        if (!was_paired) {
            uint8_t ch = 0;
            wifi_second_chan_t sec;
            esp_wifi_get_channel(&ch, &sec);
            ESP_LOGI(TAG, "Paired on CH%u via bridge " MACSTR " (PAIR_ACK %s)", (unsigned)ch, MAC2STR(s_hub_mac), esp_err_to_name(err));
        }
    } else if (type == ESP_NOW_MSG_UNPAIR && s_paired) {
        esp_now_unpair_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_UNPAIR_ACK };
        esp_now_send(info->src_addr, (const uint8_t *)&ack, sizeof(ack));
        ESP_LOGI(TAG, "Unpaired by Cntl - advertising again");
        enter_advertising();
    }
}

void app_main(void)
{
    antenna_init();

    esp_err_t ret = nvs_flash_init();   /* Wi-Fi 드라이버가 씀(설정 저장 아님 — 저장은 RAM) */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();
    esp_wifi_get_mac(WIFI_IF_STA, s_mac);
    snprintf(s_name, sizeof(s_name), "B%02X%02X%02X", s_mac[3], s_mac[4], s_mac[5]);   /* 캠 "C…", 센스 "S…"와 같은 형식 */
    ESP_LOGI(TAG, "Boril %s (MAC " MACSTR ")", s_name, MAC2STR(s_mac));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    add_peer(s_broadcast);
    const esp_timer_create_args_t a = { .callback = adv_timer_cb, .name = "adv" };
    ESP_ERROR_CHECK(esp_timer_create(&a, &s_adv_timer));
    enter_advertising();
}
