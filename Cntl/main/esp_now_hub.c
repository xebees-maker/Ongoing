#include "esp_now_hub.h"

#include <string.h>
#include <stdio.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "web_dashboard.h"

static const char *TAG = "esp_now_hub";

static esp_now_hub_node_t s_nodes[ESP_NOW_HUB_MAX_NODES];
static int s_node_count = 0;

static const uint8_t s_broadcast_addr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool          s_got_ip   = false;
static esp_netif_ip_info_t s_ip_info = { 0 };

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

/* dst는 호출 전에 0으로 초기화돼 있어야 함(wifi_config_t = {0}) — 남는 바이트는 그대로 0 유지 */
static void copy_str(uint8_t *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(src);
    if (len > dst_size - 1) len = dst_size - 1;
    memcpy(dst, src, len);
}

#if CONFIG_CNTL_WIFI_MODE_STA

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        ESP_LOGW(TAG, "WiFi 연결 끊김 — 재시도");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        s_ip_info = evt->ip_info;
        s_got_ip  = true;
        ESP_LOGI(TAG, "IP 받음: " IPSTR, IP2STR(&evt->ip_info.ip));
        web_dashboard_start();
    }
}

/* STA(개발 단계): 실제 AP에 접속 — 채널은 AP가 정하므로 직접 set_channel 안 함.
 * Sens도 같은 AP에 STA로 붙어있다는 전제(같은 AP ⇒같은 채널)로 ESP-NOW가 동작함. */
static void wifi_bringup(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = { 0 };
    copy_str(wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), CONFIG_CNTL_WIFI_SSID);
    copy_str(wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), CONFIG_CNTL_WIFI_PASSWORD);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

#else  /* CNTL_WIFI_MODE_AP — 양산 단계: 독자 SoftAP, 고정 채널 */

static void wifi_bringup(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t wifi_cfg = { 0 };
    const char *ssid = CONFIG_CNTL_WIFI_SSID;
    char auto_ssid[32];
    if (strlen(ssid) == 0) {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_AP, mac);
        snprintf(auto_ssid, sizeof(auto_ssid), "Cntl-%02X%02X", mac[4], mac[5]);
        ssid = auto_ssid;
    }
    copy_str(wifi_cfg.ap.ssid, sizeof(wifi_cfg.ap.ssid), ssid);
    wifi_cfg.ap.ssid_len = strlen(ssid);
    copy_str(wifi_cfg.ap.password, sizeof(wifi_cfg.ap.password), CONFIG_CNTL_WIFI_PASSWORD);
    wifi_cfg.ap.max_connection = 4;
    wifi_cfg.ap.authmode = (strlen(CONFIG_CNTL_WIFI_PASSWORD) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_cfg.ap.channel = ESP_NOW_LINK_CHANNEL;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif && esp_netif_get_ip_info(ap_netif, &s_ip_info) == ESP_OK) {
        s_got_ip = true;
    }
    web_dashboard_start();  /* AP는 GOT_IP 이벤트 없이 시작 시점에 이미 자체 IP가 있음 */
}

#endif

void esp_now_hub_init(void)
{
    wifi_bringup();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_broadcast_addr, sizeof(peer.peer_addr));
#if CONFIG_CNTL_WIFI_MODE_STA
    peer.ifidx = WIFI_IF_STA;
#else
    peer.ifidx = WIFI_IF_AP;
#endif
    peer.channel = 0;  /* 0 = 인터페이스가 지금 쓰는 채널을 그대로 따름 */
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

#if CONFIG_CNTL_WIFI_MODE_STA
    ESP_LOGI(TAG, "ESP-NOW 허브 시작됨 (STA)");
#else
    ESP_LOGI(TAG, "ESP-NOW 허브 시작됨 (AP)");
#endif
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

void esp_now_hub_get_net_status(char *buf, size_t buflen)
{
#if CONFIG_CNTL_WIFI_MODE_STA
    if (!s_got_ip) {
        snprintf(buf, buflen, "STA 연결 중...");
        return;
    }
    wifi_ap_record_t ap_info;
    int channel = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ? ap_info.primary : 0;
    snprintf(buf, buflen, "STA CH%d " IPSTR, channel, IP2STR(&s_ip_info.ip));
#else
    snprintf(buf, buflen, "AP CH%d " IPSTR, ESP_NOW_LINK_CHANNEL, IP2STR(&s_ip_info.ip));
#endif
}
