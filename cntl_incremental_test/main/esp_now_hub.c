#include "esp_now_hub.h"
#include "esp_now_photo.h"
#include "rtc_sync.h"
#include "ui_log.h"

#include <string.h>
#include <assert.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "esp_now_hub";

/* Cntl 실제 sdkconfig 기준 STA 모드+SSID/PW — 4단계 테스트 때와 동일 */
#define WIFI_SSID     "iptime2.4"
#define WIFI_PASSWORD "sk1234!@#"

static const uint8_t s_broadcast_addr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static esp_now_hub_node_t s_nodes[ESP_NOW_HUB_MAX_NODES];
static int                s_node_count = 0;

/* recv_cb()는 ESP-NOW/WiFi 드라이버 태스크에서 호출되고, esp_now_hub_get_nodes()/pair()/
 * unpair()는 LVGL 워커 태스크(esp_lv_adapter)에서 호출됨 — 서로 다른 태스크가 락 없이
 * s_nodes[]/s_node_count를 동시에 건드리던 레이스가 있었음(연결 리스트가 간헐적으로 깨지고
 * 반응 없던 문제의 원인으로 추정). 아래 뮤텍스로 s_nodes[]/s_node_count 접근 전체를 보호. */
static SemaphoreHandle_t s_nodes_mutex = NULL;

static hub_node_kind_t classify_name(const char *name)
{
    if (strncmp(name, "Cam-", 4) == 0)  return HUB_NODE_KIND_CAM;
    if (strncmp(name, "Sens-", 5) == 0) return HUB_NODE_KIND_SENS;
    return HUB_NODE_KIND_UNKNOWN;
}

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

static esp_now_hub_node_t *find_node(const uint8_t *mac)
{
    for (int i = 0; i < s_node_count; i++) {
        if (memcmp(s_nodes[i].mac, mac, 6) == 0) return &s_nodes[i];
    }
    return NULL;
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
    }
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

        xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
        esp_now_hub_node_t *n = find_or_add_node(info->src_addr);
        if (!n) {
            xSemaphoreGive(s_nodes_mutex);
            ESP_LOGW(TAG, "노드 테이블 가득 — %s 무시", msg->name);
            return;
        }
        memcpy(n->name, msg->name, sizeof(n->name));
        n->name[ESP_NOW_LINK_NAME_LEN - 1] = '\0';
        n->kind = classify_name(n->name);
        n->last_seen_ms = now_ms;
        xSemaphoreGive(s_nodes_mutex);

        /* 사람이 페어링을 누르기 전이라도 즉시 응답 — 채널 스캔 중인 노드가 Cntl을
         * 찾았다는 걸 알고 이 채널에 고정하기 위한 용도(정식 페어링과 별개) */
        add_peer_if_needed(info->src_addr);
        esp_now_advertise_ack_t ack = {
            .version  = ESP_NOW_LINK_VERSION,
            .msg_type = ESP_NOW_MSG_ADVERTISE_ACK,
        };
        esp_wifi_get_mac(WIFI_IF_STA, ack.hub_mac);
        esp_now_send(info->src_addr, (const uint8_t *)&ack, sizeof(ack));

    } else if (msg_type == ESP_NOW_MSG_PAIR_ACK) {
        if (len < (int)sizeof(esp_now_pair_ack_t)) return;

        xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
        esp_now_hub_node_t *n = find_node(info->src_addr);
        bool became_paired = false;
        char name_copy[ESP_NOW_LINK_NAME_LEN] = { 0 };
        if (n) {
            /* 생존 신호(last_seen_ms)는 항상 갱신 — 페어링 후엔 CAM/SENS가 ADVERTISE를
             * 끊고 이 PAIR_ACK(keepalive)로만 살아있음을 알리는 것으로 보이는데, 이걸
             * user_unpaired로 걸러버리면(예전 버그) 연결 해제 직후부터 생존 신호 자체가
             * 안 갱신돼서 5초 타임아웃 뒤 리스트에서 완전히 사라져버림 */
            n->last_seen_ms = now_ms;
            /* 재연결(paired=true로 되돌리는 것)만 user_unpaired로 막음 — 사용자가
             * esp_now_hub_pair()를 다시 불러야(리스트에서 다시 연결 허용) 풀림.
             * became_paired는 "방금 막 페어링됨(false->true 전환)"일 때만 true여야 함 —
             * 예전엔 n->paired가 이미 true였어도 PAIR_ACK(1초 keepalive)가 올 때마다 매번
             * true로 잡혀서, 페어링된 CAM/Sens에 SET_TIME을 1초마다 무한정 계속 보내고
             * 있었음(2026-08-03, CAM 시리얼 로그로 발견 — 8초 사이에 SET_TIME이 9번 옴) */
            bool was_paired = n->paired;
            if (!n->user_unpaired) {
                n->paired = true;
                became_paired = !was_paired;
                strncpy(name_copy, n->name, sizeof(name_copy) - 1);
            }
        }
        xSemaphoreGive(s_nodes_mutex);
        if (became_paired) {
            ESP_LOGI(TAG, "페어링 완료: %s", name_copy);
            /* CAM/Sens는 자체 RTC가 없어서 페어링될 때마다 Cntl 시각을 알려줌 — 이게
             * 없으면 CAM의 시계가 부팅 시각(1970-01-01 근처)에 멈춰있어서 사진 파일명
             * (촬영시각 유닉스 타임스탬프)이 전부 1월 1일로 찍힘(2026-08-01 실기에서 확인) */
            esp_now_set_time_t set_time = {
                .version   = ESP_NOW_LINK_VERSION,
                .msg_type  = ESP_NOW_MSG_SET_TIME,
                .unix_time = rtc_sync_get_unix_time(),
            };
            esp_err_t err = esp_now_send(info->src_addr, (const uint8_t *)&set_time, sizeof(set_time));
            ESP_LOGI(TAG, "SET_TIME(%u) 전송: %s", (unsigned)set_time.unix_time, esp_err_to_name(err));
        }

    } else if (msg_type == ESP_NOW_MSG_PHOTO_META || msg_type == ESP_NOW_MSG_PHOTO_CHUNK ||
               msg_type == ESP_NOW_MSG_PHOTO_DONE || msg_type == ESP_NOW_MSG_CAPTURE_STATUS ||
               msg_type == ESP_NOW_MSG_PHOTO_LIST_ENTRY || msg_type == ESP_NOW_MSG_PHOTO_LIST_DONE ||
               msg_type == ESP_NOW_MSG_PHOTO_DELETE_ACK || msg_type == ESP_NOW_MSG_PHOTO_DELETE_ALL_ACK) {
        /* ESP-NOW는 recv_cb를 하나만 등록할 수 있어서, 사진 관련 프로토콜(전송/목록/삭제/
         * 지금촬영 진행상태) 처리는 전부 esp_now_photo.c로 넘김 */
        esp_now_photo_on_recv(msg_type, data, len);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 연결 끊김 — 재시도");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP 받음: " IPSTR, IP2STR(&evt->ip_info.ip));
    }
}

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
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* WiFi 절전(모뎀 슬립) 끔(2026-08-02) — 부팅 로그에 "wifi:pm start, type: 1"이 찍혀서
     * 기본값(절전 켜짐)으로 동작 중이었음을 확인. 절전 중엔 라디오가 공유기 비콘 주기에
     * 맞춰서만 깨어나는데, CAM이 보내는 ESP-NOW 청크는 그 주기와 무관하게 아무 때나
     * 도착해서 라디오가 자는 타이밍에 온 청크를 놓침 — 사진 가져오기 청크 누락(3002)의
     * 실제 원인으로 추정(실기에서 확인된 증상: 느리고 ETA가 들쭉날쭉하다 결국 실패).
     * Cntl은 상시전원(배터리 아님)이라 절전을 꺼도 손해가 없음 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void esp_now_hub_init(void)
{
    /* recv_cb가 등록되기 전에 먼저 만들어둬야 함 — 등록 직후부터 다른 태스크에서
     * s_nodes[]를 건드릴 수 있음 */
    s_nodes_mutex = xSemaphoreCreateMutex();
    assert(s_nodes_mutex != NULL);

    wifi_bringup();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_broadcast_addr, sizeof(peer.peer_addr));
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /* Cntl 재부팅 신호 브로드캐스트(2026-08-02, 타이밍 재수정 2026-08-03) — CAM/Sens가
     * 소프트리셋 전에 이미 페어링돼 있었다면 Cntl의 노드 테이블은 방금 비워졌는데도 그쪽은
     * 여전히 자기가 페어링된 줄 알고 ADVERTISE 없이 keepalive만 보냄(esp_now_link.h의
     * ESP_NOW_MSG_HUB_RESET 주석 참고) — 그 상태론 재광고를 스스로 트리거할 방법이 없어서
     * Cntl이 먼저 알려줘야 함.
     * 원래 IP_EVENT_STA_GOT_IP(공유기 완전 연결 시점)에서 보냈는데, 이게 사용자 실제
     * 워크플로우("항상 소프트리셋 -> 연결 -> 목록 -> 사진")와 충돌하는 레이스가 있었음
     * (2026-08-03 실기에서 확인): 공유기 재연결이 느릴 때(인증 backoff 등, 실기에서
     * "Association refused... comeback time" 확인된 적 있음) 사용자가 CAM과 방금 새로
     * 페어링을 끝낸 뒤에야 이 브로드캐스트가 뒤늦게 나가서, 이번 부팅에서 막 정상적으로
     * 맺어진 페어링을 "예전 거"로 오인하고 걷어차버림(사용자 지적: "연결됨으로 상태가
     * 바뀌고... 목록을 가져왔잖아, 연결 됐던 거 아냐?"). 지금은 여기, ESP-NOW 자체가 막
     * 켜진 시점(공유기 연결 여부와 무관, 부팅 극초반)에 보내서 사람이 화면을 터치해서
     * 페어링할 수 있는 어떤 시점보다도 반드시 먼저 나가도록 함 — 802.11 채널 고정 자체는
     * 공유기 인증 절차 초반(esp_wifi_connect 시작 시점)에 이미 이뤄지므로 채널 문제도
     * 없음 */
    esp_now_hub_reset_t reset_msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_HUB_RESET,
    };
    esp_err_t reset_err = esp_now_send(s_broadcast_addr, (const uint8_t *)&reset_msg, sizeof(reset_msg));
    ESP_LOGI(TAG, "HUB_RESET 브로드캐스트: %s", esp_err_to_name(reset_err));

    ESP_LOGI(TAG, "ESP-NOW 허브 시작됨 (STA)");
}

uint8_t esp_now_hub_get_wifi_channel(void)
{
    uint8_t channel = 0;
    wifi_second_chan_t second_chan;
    esp_wifi_get_channel(&channel, &second_chan);
    return channel;
}

int esp_now_hub_get_nodes(hub_node_kind_t kind, esp_now_hub_node_t *out, int max)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    int count = 0;
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < s_node_count && count < max; i++) {
        if (kind != HUB_NODE_KIND_UNKNOWN && s_nodes[i].kind != kind) continue;
        if (now_ms - s_nodes[i].last_seen_ms > ESP_NOW_HUB_NODE_TIMEOUT_MS) continue;
        out[count++] = s_nodes[i];
    }
    xSemaphoreGive(s_nodes_mutex);
    return count;
}

void esp_now_hub_pair(const uint8_t *mac)
{
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    esp_now_hub_node_t *n = find_node(mac);
    bool ok = (n && !n->paired);
    char name_copy[ESP_NOW_LINK_NAME_LEN] = { 0 };
    if (ok) {
        n->user_unpaired = false;  /* 사용자가 다시 연결을 시도하는 것 — keepalive 무시 플래그 해제 */
        strncpy(name_copy, n->name, sizeof(name_copy) - 1);
    }
    xSemaphoreGive(s_nodes_mutex);
    if (!ok) return;

    add_peer_if_needed(mac);

    uint8_t hub_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, hub_mac);

    esp_now_pair_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PAIR_REQUEST,
    };
    memcpy(req.hub_mac, hub_mac, sizeof(req.hub_mac));
    esp_err_t err = esp_now_send(mac, (const uint8_t *)&req, sizeof(req));
    ESP_LOGI(TAG, "PAIR_REQUEST -> %s: %s", name_copy, esp_err_to_name(err));
}

void esp_now_hub_unpair(const uint8_t *mac)
{
    xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
    esp_now_hub_node_t *n = find_node(mac);
    char name_copy[ESP_NOW_LINK_NAME_LEN] = { 0 };
    if (n) {
        strncpy(name_copy, n->name, sizeof(name_copy) - 1);
        n->paired = false;
        n->user_unpaired = true;  /* CAM/SENS의 keepalive(PAIR_ACK 재전송)로 조용히 재연결되는 것 방지 */
    }
    xSemaphoreGive(s_nodes_mutex);
    if (!n) return;

    /* 피어를 지우기 전에 먼저 UNPAIR을 보내야 함(피어 삭제 후엔 유니캐스트가 안 나감) —
     * 이게 없으면 노드가 자기 상태를 몰라서 계속 keepalive를 보냄(실기로 확인된 문제) */
    esp_now_unpair_t msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_UNPAIR,
    };
    esp_err_t err = esp_now_send(mac, (const uint8_t *)&msg, sizeof(msg));
    ESP_LOGI(TAG, "UNPAIR -> %s: %s", name_copy, esp_err_to_name(err));

    if (esp_now_is_peer_exist(mac)) {
        esp_now_del_peer(mac);
    }
    ESP_LOGI(TAG, "연결 해제: %s", name_copy);
}
