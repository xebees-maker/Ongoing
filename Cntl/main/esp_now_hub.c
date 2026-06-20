#include "esp_now_hub.h"

#include <string.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "esp_now_hub";

static esp_now_hub_node_t s_nodes[ESP_NOW_HUB_MAX_NODES];
static int s_node_count = 0;

static const uint8_t s_broadcast_addr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static esp_now_hub_node_t *find_or_add_node(const uint8_t *mac)
{
    for (int i = 0; i < s_node_count; i++) {
        if (memcmp(s_nodes[i].mac, mac, 6) == 0) return &s_nodes[i];
    }
    if (s_node_count < ESP_NOW_HUB_MAX_NODES) {
        esp_now_hub_node_t *n = &s_nodes[s_node_count++];
        memcpy(n->mac, mac, 6);
        return n;
    }
    return NULL;  /* 테이블 가득 — 새 노드 무시 */
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_advertise_t)) return;

    const esp_now_advertise_t *msg = (const esp_now_advertise_t *)data;
    if (msg->version != ESP_NOW_LINK_VERSION || msg->msg_type != ESP_NOW_MSG_ADVERTISE) return;

    esp_now_hub_node_t *n = find_or_add_node(info->src_addr);
    if (!n) {
        ESP_LOGW(TAG, "노드 테이블 가득 — %s 무시", msg->name);
        return;
    }

    memcpy(n->name, msg->name, sizeof(n->name));
    n->name[ESP_NOW_LINK_NAME_LEN - 1] = '\0';
    n->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/* AP에 붙지 않음 — ESP-NOW만 쓰려고 라디오를 켜는 용도라 연결 이벤트 처리 불필요 */
static void wifi_bringup(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Sens 쪽도 동일하게 ESP_NOW_LINK_CHANNEL로 SoftAP을 고정해서 호스팅함 —
     * 양쪽이 같은 헤더의 상수를 쓰므로 채널 어긋날 일이 없음 */
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESP_NOW_LINK_CHANNEL, WIFI_SECOND_CHAN_NONE));
}

void esp_now_hub_init(void)
{
    wifi_bringup();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_broadcast_addr, sizeof(peer.peer_addr));
    peer.ifidx = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    ESP_LOGI(TAG, "ESP-NOW 허브 시작됨 (채널 %d)", ESP_NOW_LINK_CHANNEL);
}

int esp_now_hub_get_nodes(esp_now_hub_node_t *out, int max)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    int count = 0;
    for (int i = 0; i < s_node_count && count < max; i++) {
        if (now_ms - s_nodes[i].last_seen_ms <= ESP_NOW_HUB_NODE_TIMEOUT_MS) {
            out[count++] = s_nodes[i];
        }
    }
    return count;
}
