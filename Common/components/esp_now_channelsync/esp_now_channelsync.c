#include "esp_now_channelsync.h"
#include "esp_now_link.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "esp_now_chsync";

#define SCAN_DWELL_US        (300 * 1000)
#define SCAN_CHANNEL_MIN     1
#define SCAN_CHANNEL_MAX     13

/* 500ms마다 PING 왕복 확인, 연속 3회(약 1.5초) 무응답이면 동기화 끊김으로 판정 — 오늘의
 * "몇 분"과 대비되는 목표치(수 초 이내). 매 라운드는 "직전 PING의 PONG이 왔는가"만 검사하고
 * 바로 다음 PING을 보내는 단순한 구조라 별도의 응답 타임아웃 타이머가 필요 없음(위 헤더의
 * 설계 원칙 참고 — 이 루프는 다른 무엇으로도 멈추지 않음). */
#define PING_INTERVAL_US     (500 * 1000)
#define PING_FAIL_THRESHOLD  3

static const uint8_t s_broadcast_addr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static char    s_node_name[ESP_NOW_LINK_NAME_LEN] = "";
static uint8_t s_node_mac[6] = { 0 };

static esp_now_channelsync_on_synced_cb_t    s_on_synced    = NULL;
static esp_now_channelsync_on_lost_sync_cb_t s_on_lost_sync = NULL;

static volatile bool s_synced       = false;
static uint8_t        s_scan_channel = SCAN_CHANNEL_MIN;
static uint8_t         s_hub_mac[6]  = { 0 };

static esp_timer_handle_t s_scan_timer = NULL;  /* UNSYNCED에서만 동작 */
static esp_timer_handle_t s_ping_timer = NULL;  /* SYNCED에서만 동작 */
static volatile bool s_pong_pending    = false; /* 직전 PING에 대한 PONG을 아직 못 받음 */
static int            s_ping_fail_count = 0;

static void add_peer_if_needed(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

static void enter_unsynced(void)
{
    s_synced = false;
    if (s_ping_timer) esp_timer_stop(s_ping_timer);
    s_pong_pending    = false;
    s_ping_fail_count = 0;

    s_scan_channel = SCAN_CHANNEL_MIN;
    esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);

    if (s_scan_timer) {
        esp_timer_stop(s_scan_timer);
        esp_timer_start_periodic(s_scan_timer, SCAN_DWELL_US);
    }
}

static void scan_timer_cb(void *arg)
{
    (void)arg;
    /* 채널 이동을 먼저 하고 그 채널에서 광고를 보냄(2026-08-04 — 순서를 반대로 하면, 광고를
     * 보낸 그 채널에서 상대의 응답을 들을 시간이 없어서 매번 다음 채널로 잘못 락되는 레이스가
     * 있었음, 실기로 재현/수정 검증됨). 이 채널에 다음 타이머까지 계속 머무르므로 응답이
     * 도착할 시간이 충분함 */
    s_scan_channel++;
    if (s_scan_channel > SCAN_CHANNEL_MAX) s_scan_channel = SCAN_CHANNEL_MIN;
    esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);

    esp_now_advertise_t msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_ADVERTISE,
    };
    memcpy(msg.name, s_node_name, sizeof(msg.name));
    memcpy(msg.mac, s_node_mac, sizeof(msg.mac));
    esp_now_send(s_broadcast_addr, (const uint8_t *)&msg, sizeof(msg));
}

static void ping_timer_cb(void *arg)
{
    (void)arg;
    if (s_pong_pending) {
        s_ping_fail_count++;
        ESP_LOGW(TAG, "PING 무응답(연속 %d회)", s_ping_fail_count);
        if (s_ping_fail_count >= PING_FAIL_THRESHOLD) {
            ESP_LOGW(TAG, "채널 동기화 끊김 — 재스캔 시작");
            enter_unsynced();
            if (s_on_lost_sync) s_on_lost_sync();
            return;  /* enter_unsynced()가 이미 ping_timer를 멈춤 */
        }
    }
    esp_now_channel_ping_t ping = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_CHANNEL_PING,
    };
    esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&ping, sizeof(ping));
    if (err == ESP_ERR_ESPNOW_NO_MEM) {
        /* 로컬 송신큐 포화 — PING이 아예 안 나간 것뿐, 채널/상대 문제와 무관함(2026-08-05,
         * 실기에서 발견: 사진 청크를 한꺼번에 많이 보내는 중에 이 로컬 포화가 PING까지
         * 밀어내서 "동기화 끊김"으로 오판 — 실제로 CAM이 전송 중간에 채널을 이탈해서
         * Cntl의 DONE_ACK가 도달 못 하고 3006으로 이어짐). "무응답"과 다른 원인이므로
         * 실패로 안 세고 이번 사이클은 그냥 건너뜀(pending도 안 켬 — 다음 틱에 새로 시도) */
        s_pong_pending = false;
        return;
    }
    s_pong_pending = true;
}

void esp_now_channelsync_init(const char *node_name, const uint8_t *node_mac,
                               esp_now_channelsync_on_synced_cb_t on_synced,
                               esp_now_channelsync_on_lost_sync_cb_t on_lost_sync)
{
    strncpy(s_node_name, node_name, sizeof(s_node_name) - 1);
    memcpy(s_node_mac, node_mac, sizeof(s_node_mac));
    s_on_synced    = on_synced;
    s_on_lost_sync = on_lost_sync;

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_broadcast_addr, sizeof(peer.peer_addr));
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(s_broadcast_addr)) esp_now_add_peer(&peer);

    const esp_timer_create_args_t scan_args = { .callback = scan_timer_cb, .name = "chsync_scan" };
    esp_timer_create(&scan_args, &s_scan_timer);

    const esp_timer_create_args_t ping_args = { .callback = ping_timer_cb, .name = "chsync_ping" };
    esp_timer_create(&ping_args, &s_ping_timer);

    enter_unsynced();
}

void esp_now_channelsync_on_recv(const esp_now_recv_info_t *info, uint8_t msg_type,
                                  const uint8_t *data, int len)
{
    if (!s_synced && msg_type == ESP_NOW_MSG_ADVERTISE_ACK) {
        if (len < (int)sizeof(esp_now_advertise_ack_t)) return;
        const esp_now_advertise_ack_t *ack = (const esp_now_advertise_ack_t *)data;

        /* s_scan_channel을 그대로 믿으면 안 됨 — scan_timer_cb(별도 타이머 콜백)가 이 콜백과
         * 다른 시점에 채널을 바꿀 수 있어서, 실제로 이 응답을 수신한 채널은
         * info->rx_ctrl->channel로 확인하는 게 정확함(2026-08-02, 실기에서 발견된 레이스) */
        uint8_t actual_channel = (info && info->rx_ctrl) ? info->rx_ctrl->channel : s_scan_channel;

        memcpy(s_hub_mac, ack->hub_mac, sizeof(s_hub_mac));
        add_peer_if_needed(s_hub_mac);

        s_scan_channel = actual_channel;
        esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);
        if (s_scan_timer) esp_timer_stop(s_scan_timer);

        s_synced          = true;
        s_ping_fail_count = 0;
        s_pong_pending    = false;
        if (s_ping_timer) esp_timer_start_periodic(s_ping_timer, PING_INTERVAL_US);

        ESP_LOGI(TAG, "채널 동기화됨(CH%d)", s_scan_channel);
        if (s_on_synced) s_on_synced(s_scan_channel, s_hub_mac);
        return;
    }

    if (s_synced && msg_type == ESP_NOW_MSG_CHANNEL_PONG) {
        if (!info || memcmp(info->src_addr, s_hub_mac, sizeof(s_hub_mac)) != 0) return;
        s_pong_pending    = false;
        s_ping_fail_count = 0;
        return;
    }
}

void esp_now_channelsync_notify_alive(void)
{
    if (!s_synced) return;
    /* PONG을 실제로 받은 것과 동일하게 취급 — 위 헤더 설명 참고. 지금 막 딴 왕복(예:
     * DONE_ACK)이 성공했다는 건 이 순간 채널이 멀쩡하다는 증거이므로, 마침 큐에 밀려
     * 대기 중이던 PING이 뒤늦게 실패로 잡히더라도 그건 이미 낡은 판단임 */
    s_pong_pending    = false;
    s_ping_fail_count = 0;
}

bool esp_now_channelsync_is_synced(void)
{
    return s_synced;
}
