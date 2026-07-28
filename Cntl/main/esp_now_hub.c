#include "esp_now_hub.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "web_dashboard.h"

static const char *TAG = "esp_now_hub";

static esp_now_hub_node_t s_nodes[ESP_NOW_HUB_MAX_NODES];
static int s_node_count = 0;

/* CAM 사진 청크 재조립 — 최종 보관/열람 방법(SD? 웹 스트리밍?)은 아직 미정이라 일단
 * 힙 버퍼에 재조립만 하고 완료 로그 남긴 뒤 해제. 한 번에 한 파일만 재조립(CAM이
 * 순차로 META→CHUNK*→다음 META 순서로 보내는 걸 전제 — esp_now_cam.c의 send_one_photo
 * 참고, 파일 여러 개를 동시에 인터리브해서 보내지 않음). */
static uint32_t s_photo_file_id       = 0;
static uint32_t s_photo_total_size    = 0;
static uint16_t s_photo_total_chunks  = 0;
static uint16_t s_photo_chunks_recv   = 0;
static uint32_t s_photo_expected_crc32 = 0;
static uint8_t *s_photo_buffer        = NULL;

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
    if (len < 2) return;
    uint8_t msg_type = data[1];
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (msg_type == ESP_NOW_MSG_ADVERTISE) {
        if (len < (int)sizeof(esp_now_advertise_t)) return;
        const esp_now_advertise_t *msg = (const esp_now_advertise_t *)data;
        if (msg->version != ESP_NOW_LINK_VERSION) return;

        esp_now_hub_node_t *n = find_or_add_node(info->src_addr);
        if (!n) {
            ESP_LOGW(TAG, "노드 테이블 가득 — %s 무시", msg->name);
            return;
        }
        memcpy(n->name, msg->name, sizeof(n->name));
        n->name[ESP_NOW_LINK_NAME_LEN - 1] = '\0';
        n->last_seen_ms = now_ms;

        /* 사람이 페어링 버튼을 누르기 전이라도 즉시 응답 — 채널 스캔 중인 노드가 "Cntl을
         * 찾았다"를 알고 지금 채널에 고정하기 위한 용도(정식 페어링과는 별개). Cntl의 채널이
         * 더 이상 고정 상수가 아니게 되면서(외부망 STA 등) 노드가 채널을 스캔해서 찾아야
         * 하는데, 응답이 늦으면(사람이 페어링 누를 때까지 기다리면) 그 사이 노드가 이미
         * 다른 채널로 넘어가버려서 응답이 전달 안 됨. */
        if (!esp_now_is_peer_exist(info->src_addr)) {
            esp_now_peer_info_t peer = { 0 };
            memcpy(peer.peer_addr, info->src_addr, 6);
#if CONFIG_CNTL_WIFI_MODE_STA
            peer.ifidx = WIFI_IF_STA;
#else
            peer.ifidx = WIFI_IF_AP;
#endif
            peer.channel = 0;
            peer.encrypt = false;
            if (esp_now_add_peer(&peer) != ESP_OK) {
                ESP_LOGW(TAG, "%s ADVERTISE_ACK용 피어 등록 실패", msg->name);
                return;
            }
        }
        esp_now_advertise_ack_t ack = {
            .version  = ESP_NOW_LINK_VERSION,
            .msg_type = ESP_NOW_MSG_ADVERTISE_ACK,
        };
#if CONFIG_CNTL_WIFI_MODE_STA
        esp_wifi_get_mac(WIFI_IF_STA, ack.hub_mac);
#else
        esp_wifi_get_mac(WIFI_IF_AP, ack.hub_mac);
#endif
        esp_now_send(info->src_addr, (const uint8_t *)&ack, sizeof(ack));

    } else if (msg_type == ESP_NOW_MSG_PAIR_ACK) {
        if (len < (int)sizeof(esp_now_pair_ack_t)) return;
        const esp_now_pair_ack_t *msg = (const esp_now_pair_ack_t *)data;
        if (msg->version != ESP_NOW_LINK_VERSION) return;

        esp_now_hub_node_t *n = find_or_add_node(info->src_addr);
        if (!n) return;
        n->paired = true;
        n->last_data_ms = now_ms;
        ESP_LOGI(TAG, "페어링 완료: %s", n->name);

    } else if (msg_type == ESP_NOW_MSG_SENSOR_DATA) {
        if (len < (int)sizeof(esp_now_sensor_data_t)) return;
        const esp_now_sensor_data_t *msg = (const esp_now_sensor_data_t *)data;
        if (msg->version != ESP_NOW_LINK_VERSION) return;

        esp_now_hub_node_t *n = find_or_add_node(info->src_addr);
        if (!n) return;
        n->paired       = true;
        n->last_data_ms = now_ms;

        n->sensor_kind = msg->sensor_kind;
        n->chan_count  = (msg->chan_count > ESP_NOW_MAX_CHANNELS) ? ESP_NOW_MAX_CHANNELS : msg->chan_count;
        memcpy(n->chan_type, msg->chan_type, n->chan_count * sizeof(n->chan_type[0]));
        memcpy(n->chan_ok,   msg->chan_ok,   n->chan_count * sizeof(n->chan_ok[0]));
        memcpy(n->chan_val,  msg->chan_val,  n->chan_count * sizeof(n->chan_val[0]));

        n->batt_ok  = msg->batt_ok;
        n->batt_pct = msg->batt_pct;
        n->powered  = msg->powered;

    } else if (msg_type == ESP_NOW_MSG_PHOTO_META) {
        if (len < (int)sizeof(esp_now_photo_meta_t)) return;
        const esp_now_photo_meta_t *msg = (const esp_now_photo_meta_t *)data;
        if (msg->version != ESP_NOW_LINK_VERSION) return;

        if (s_photo_buffer) {
            ESP_LOGW(TAG, "이전 사진(id=%u) DONE 없이 새 META 도착 — 이전 것 버림",
                     (unsigned)s_photo_file_id);
            free(s_photo_buffer);
            s_photo_buffer = NULL;
        }
        s_photo_buffer = malloc(msg->total_size);
        if (!s_photo_buffer) {
            ESP_LOGE(TAG, "사진 재조립 버퍼 할당 실패 (%u bytes)", (unsigned)msg->total_size);
            return;
        }
        s_photo_file_id        = msg->file_id;
        s_photo_total_size     = msg->total_size;
        s_photo_total_chunks   = msg->total_chunks;
        s_photo_chunks_recv    = 0;
        s_photo_expected_crc32 = msg->crc32;
        ESP_LOGI(TAG, "PHOTO_META id=%u size=%u chunks=%u", (unsigned)msg->file_id,
                 (unsigned)msg->total_size, msg->total_chunks);

    } else if (msg_type == ESP_NOW_MSG_PHOTO_CHUNK) {
        if (len < (int)sizeof(esp_now_photo_chunk_t)) return;
        const esp_now_photo_chunk_t *msg = (const esp_now_photo_chunk_t *)data;
        if (msg->version != ESP_NOW_LINK_VERSION) return;
        if (!s_photo_buffer || msg->file_id != s_photo_file_id) {
            ESP_LOGW(TAG, "META 없이(또는 다른 file_id) CHUNK 도착 — 무시");
            return;
        }
        size_t offset = (size_t)msg->chunk_idx * ESP_NOW_PHOTO_CHUNK_DATA_LEN;
        if (offset + msg->chunk_len > s_photo_total_size) {
            ESP_LOGW(TAG, "CHUNK %u가 버퍼 범위 초과 — 무시", msg->chunk_idx);
            return;
        }
        memcpy(s_photo_buffer + offset, msg->data, msg->chunk_len);
        s_photo_chunks_recv++;

    } else if (msg_type == ESP_NOW_MSG_PHOTO_DONE) {
        if (len < (int)sizeof(esp_now_photo_done_t)) return;
        if (s_photo_buffer) {
            /* 최종 보관/열람 방법(SD? 웹 스트리밍?)은 아직 미정 — 지금은 완료 여부만
             * 로그로 확인하고 버퍼는 해제. 청크 개수가 맞다고 내용까지 맞다고 보지 않고
             * CAM이 보내온 CRC32와 재조립 결과를 직접 비교한다 — "크기/개수 일치 ≠ 내용
             * 일치"는 이번 CAM 프로젝트에서 실제로 겪은 버그라 이 경로에도 똑같이 적용
             * (project_cam_dvp_corruption_investigation 메모리 참고). */
            if (s_photo_chunks_recv == s_photo_total_chunks) {
                uint32_t crc = esp_rom_crc32_le(0, s_photo_buffer, s_photo_total_size);
                if (crc == s_photo_expected_crc32) {
                    ESP_LOGI(TAG, "사진 재조립 완료+CRC 일치: id=%u size=%u (청크 %u/%u)",
                             (unsigned)s_photo_file_id, (unsigned)s_photo_total_size,
                             s_photo_chunks_recv, s_photo_total_chunks);
                    /* TODO: 최종 저장/웹 대시보드 전달 — 아직 미정, 지금은 무결성 확인까지만 */
                } else {
                    ESP_LOGW(TAG, "사진 재조립 CRC 불일치: id=%u 기대=0x%08x 실제=0x%08x — 손상, 버림",
                             (unsigned)s_photo_file_id, (unsigned)s_photo_expected_crc32, (unsigned)crc);
                }
            } else {
                ESP_LOGW(TAG, "사진 재조립 불완전: id=%u (청크 %u/%u만 도착)",
                         (unsigned)s_photo_file_id, s_photo_chunks_recv, s_photo_total_chunks);
            }
            free(s_photo_buffer);
            s_photo_buffer = NULL;
        }
        ESP_LOGI(TAG, "PHOTO_DONE 수신 (요청 전체 종료)");
    }
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
        uint32_t freshness_ms = s_nodes[i].paired ? s_nodes[i].last_data_ms : s_nodes[i].last_seen_ms;
        if (now_ms - freshness_ms <= ESP_NOW_HUB_NODE_TIMEOUT_MS) {
            out[count++] = s_nodes[i];
        }
    }
    return count;
}

void esp_now_hub_pair(const uint8_t *mac)
{
    esp_now_hub_node_t *n = find_or_add_node(mac);
    if (!n || n->paired) return;

    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peer = { 0 };
        memcpy(peer.peer_addr, mac, 6);
#if CONFIG_CNTL_WIFI_MODE_STA
        peer.ifidx = WIFI_IF_STA;
#else
        peer.ifidx = WIFI_IF_AP;
#endif
        peer.channel = 0;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            ESP_LOGW(TAG, "%s 피어 등록 실패", n->name);
            return;
        }
    }

    uint8_t hub_mac[6];
#if CONFIG_CNTL_WIFI_MODE_STA
    esp_wifi_get_mac(WIFI_IF_STA, hub_mac);
#else
    esp_wifi_get_mac(WIFI_IF_AP, hub_mac);
#endif

    esp_now_pair_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PAIR_REQUEST,
    };
    memcpy(req.hub_mac, hub_mac, sizeof(req.hub_mac));
    esp_err_t err = esp_now_send(mac, (const uint8_t *)&req, sizeof(req));
    ESP_LOGI(TAG, "PAIR_REQUEST -> %s: %s", n->name, esp_err_to_name(err));
}

static void accumulate_f(bool ok, float val, float *min, float *max, float *sum, int *cnt)
{
    if (!ok) return;
    if (*cnt == 0 || val < *min) *min = val;
    if (*cnt == 0 || val > *max) *max = val;
    *sum += val;
    (*cnt)++;
}

void esp_now_hub_get_summary(esp_now_hub_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    float type_sum[SENSOR_CHAN_TYPE_COUNT] = { 0 };
    int   type_cnt[SENSOR_CHAN_TYPE_COUNT] = { 0 };
    float batt_pct_sum = 0;
    int   batt_cnt = 0;

    for (int i = 0; i < s_node_count; i++) {
        esp_now_hub_node_t *n = &s_nodes[i];
        if (!n->paired || now_ms - n->last_data_ms > ESP_NOW_HUB_NODE_TIMEOUT_MS) continue;
        out->count++;

        for (int c = 0; c < n->chan_count; c++) {
            uint8_t type = n->chan_type[c];
            if (type >= SENSOR_CHAN_TYPE_COUNT) continue;
            esp_now_hub_channel_agg_t *agg = &out->by_type[type];
            accumulate_f(n->chan_ok[c], n->chan_val[c],
                         &agg->val_min, &agg->val_max, &type_sum[type], &type_cnt[type]);
            if (n->chan_ok[c]) agg->has_data = true;
        }

        if (n->batt_ok) {
            if (batt_cnt == 0 || n->batt_pct < out->batt_pct_min) out->batt_pct_min = n->batt_pct;
            if (batt_cnt == 0 || n->batt_pct > out->batt_pct_max) out->batt_pct_max = n->batt_pct;
            batt_pct_sum += n->batt_pct;
            batt_cnt++;
        }
    }

    for (int t = 0; t < SENSOR_CHAN_TYPE_COUNT; t++) {
        out->by_type[t].val_avg = type_cnt[t] ? type_sum[t] / type_cnt[t] : 0;
    }
    out->batt_pct_avg = batt_cnt ? batt_pct_sum / batt_cnt : 0;
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
