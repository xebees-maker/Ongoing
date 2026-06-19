#include "esp_now_node.h"

#include <string.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_now_link.h"

static const char *TAG = "esp_now_node";

/* 개발 단계 광고 주기. 양산 단계에서는 늘어남(history_log 틱 컨벤션과 동일) */
#define ADVERTISE_PERIOD_US (1000 * 1000)

static char s_name[ESP_NOW_LINK_NAME_LEN] = "";
static uint8_t s_mac[6] = { 0 };
static esp_timer_handle_t s_advertise_timer = NULL;

static const uint8_t s_broadcast_addr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void resolve_name(void)
{
#if CONFIG_SENS_WIFI_MODE_STA
    esp_wifi_get_mac(WIFI_IF_STA, s_mac);
#else
    esp_wifi_get_mac(WIFI_IF_AP, s_mac);
#endif

#if defined(CONFIG_SENS_NODE_NAME)
    if (strlen(CONFIG_SENS_NODE_NAME) > 0) {
        snprintf(s_name, sizeof(s_name), "%s", CONFIG_SENS_NODE_NAME);
        return;
    }
#endif
    snprintf(s_name, sizeof(s_name), "Sens-%02X%02X", s_mac[4], s_mac[5]);
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    ESP_LOGI(TAG, "수신: %d바이트 from " MACSTR, len, MAC2STR(info->src_addr));
}

static void advertise_timer_cb(void *arg)
{
    esp_now_advertise_t msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_ADVERTISE,
    };
    memcpy(msg.name, s_name, sizeof(msg.name));
    memcpy(msg.mac, s_mac, sizeof(msg.mac));

    esp_err_t err = esp_now_send(s_broadcast_addr, (const uint8_t *)&msg, sizeof(msg));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "광고 전송 실패: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "광고 전송: %s", s_name);
    }
}

void esp_now_node_init(void)
{
    resolve_name();
    ESP_LOGI(TAG, "노드 이름: %s (MAC " MACSTR ")", s_name, MAC2STR(s_mac));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_broadcast_addr, sizeof(peer.peer_addr));
#if CONFIG_SENS_WIFI_MODE_STA
    peer.ifidx = WIFI_IF_STA;
#else
    peer.ifidx = WIFI_IF_AP;
#endif
    peer.channel = 0;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    const esp_timer_create_args_t timer_args = {
        .callback = advertise_timer_cb,
        .name     = "espnow_adv",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_advertise_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_advertise_timer, ADVERTISE_PERIOD_US));
}

const char *esp_now_node_get_name(void)
{
    return s_name;
}
