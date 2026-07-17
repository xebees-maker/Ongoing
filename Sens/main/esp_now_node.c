#include "esp_now_node.h"

#include <string.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "status_led.h"
#include "esp_now_node.h"

static const char *TAG = "esp_now_node";

/* 개발 단계 광고/SENSOR_DATA 주기. 양산 단계에서는 늘어남(history_log 틱 컨벤션과 동일) */
#define ADVERTISE_PERIOD_US    (1000 * 1000)
#define SENSOR_DATA_PERIOD_US  (1000 * 1000)

/* paired 중 연속 이만큼 전송 실패하면 페어링을 끊긴 것으로 보고 advertising으로 복귀.
 * Cntl 허브의 ESP_NOW_HUB_NODE_TIMEOUT_MS(3초=3주기)와 같은 리듬 */
#define SEND_FAIL_THRESHOLD    3

/* 부팅 후(또는 페어링 끊긴 뒤 재광고 시작한 후) 이 시간 안에 못 붙으면 Green LED를
 * "advertising 중"(빠른 깜박임)에서 "failed"(느린 깜박임)로 바꿔서 사용자에게 알림.
 * advertising 자체는 계속된다 — 표시만 바뀜 */
#define UNPAIRED_FAILED_TIMEOUT_US  (30 * 1000 * 1000)

static char s_name[ESP_NOW_LINK_NAME_LEN] = "";
static uint8_t s_mac[6] = { 0 };
static esp_timer_handle_t s_advertise_timer = NULL;
static esp_timer_handle_t s_sensor_data_timer = NULL;
static esp_timer_handle_t s_unpaired_failed_timer = NULL;  /* one-shot */

static bool    s_paired = false;
static uint8_t s_hub_mac[6] = { 0 };
static int     s_send_fail_count = 0;

static gpio_num_t s_led_pin = GPIO_NUM_NC;

static const uint8_t s_broadcast_addr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* app이 esp_now_node_set_readings()로 넣어준 최신 스냅샷 — sensor_data_timer_cb가 그대로 전송 */
static esp_now_sensor_data_t s_pending_msg = { 0 };

static void set_led(led_pattern_t pattern)
{
    if (s_led_pin == GPIO_NUM_NC) return;
    status_led_set_pattern(s_led_pin, pattern);
}

static void enter_advertising(bool immediately_failed)
{
    s_paired = false;
    s_send_fail_count = 0;
    memset(&s_pending_msg, 0, sizeof(s_pending_msg));

    if (s_sensor_data_timer) {
        esp_timer_stop(s_sensor_data_timer);  /* 실행 중 아니면 무해하게 실패 */
    }
    esp_timer_start_periodic(s_advertise_timer, ADVERTISE_PERIOD_US);

    if (immediately_failed) {
        /* 방금까지 paired였다가 끊긴 경우 — 굳이 "막 시작한 advertising"처럼
         * 빠른 깜박임을 다시 보여줄 필요 없이 바로 failed 표시 */
        set_led(LED_PATTERN_BLINK_SLOW);
        if (s_unpaired_failed_timer) esp_timer_stop(s_unpaired_failed_timer);
    } else {
        set_led(LED_PATTERN_BLINK_FAST);
        if (s_unpaired_failed_timer) {
            esp_timer_stop(s_unpaired_failed_timer);
            esp_timer_start_once(s_unpaired_failed_timer, UNPAIRED_FAILED_TIMEOUT_US);
        }
    }
}

static void unpaired_failed_timer_cb(void *arg)
{
    (void)arg;
    if (s_paired) return;  /* 그 사이 페어링됐으면 무시 */
    ESP_LOGW(TAG, "장시간 미페어링 — failed 표시로 전환 (advertising은 계속)");
    set_led(LED_PATTERN_BLINK_SLOW);
}

void esp_now_node_set_readings(sensor_kind_t sensor_kind, uint8_t chan_count,
                                const uint8_t *chan_type, const uint8_t *chan_ok,
                                const float *chan_val,
                                bool batt_ok, int batt_pct, bool powered)
{
    if (chan_count > ESP_NOW_MAX_CHANNELS) chan_count = ESP_NOW_MAX_CHANNELS;

    s_pending_msg.version     = ESP_NOW_LINK_VERSION;
    s_pending_msg.msg_type    = ESP_NOW_MSG_SENSOR_DATA;
    memcpy(s_pending_msg.mac, s_mac, sizeof(s_pending_msg.mac));
    s_pending_msg.sensor_kind = (uint8_t)sensor_kind;
    s_pending_msg.chan_count  = chan_count;
    memcpy(s_pending_msg.chan_type, chan_type, chan_count * sizeof(chan_type[0]));
    memcpy(s_pending_msg.chan_ok,   chan_ok,   chan_count * sizeof(chan_ok[0]));
    memcpy(s_pending_msg.chan_val,  chan_val,  chan_count * sizeof(chan_val[0]));
    s_pending_msg.batt_ok  = batt_ok;
    s_pending_msg.batt_pct = batt_pct;
    s_pending_msg.powered  = powered;
}

void esp_now_node_set_status_led(gpio_num_t pin)
{
    s_led_pin = pin;
    status_led_init(pin);
}

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    if (!s_paired) return;  /* advertise 브로드캐스트 결과는 관심 없음 */

    if (status == ESP_NOW_SEND_SUCCESS) {
        s_send_fail_count = 0;
        set_led(LED_PATTERN_HEARTBEAT);
        return;
    }

    s_send_fail_count++;
    ESP_LOGW(TAG, "SENSOR_DATA 전송 실패 (연속 %d회)", s_send_fail_count);
    if (s_send_fail_count >= SEND_FAIL_THRESHOLD) {
        ESP_LOGW(TAG, "허브 응답 끊김으로 판단 — 재광고 시작");
        enter_advertising(true);
    }
}

static void sensor_data_timer_cb(void *arg)
{
    (void)arg;
    esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&s_pending_msg, sizeof(s_pending_msg));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SENSOR_DATA 전송 실패: %s", esp_err_to_name(err));
    }
}

static void start_sensor_data_timer(void)
{
    if (!s_sensor_data_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = sensor_data_timer_cb,
            .name     = "espnow_data",
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_sensor_data_timer));
    }
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_sensor_data_timer, SENSOR_DATA_PERIOD_US));
}

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
    if (s_paired || len < (int)sizeof(esp_now_pair_request_t)) {
        return;
    }
    const esp_now_pair_request_t *req = (const esp_now_pair_request_t *)data;
    if (req->msg_type != ESP_NOW_MSG_PAIR_REQUEST) {
        return;
    }

    memcpy(s_hub_mac, req->hub_mac, sizeof(s_hub_mac));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_hub_mac, sizeof(peer.peer_addr));
#if CONFIG_SENS_WIFI_MODE_STA
    peer.ifidx = WIFI_IF_STA;
#else
    peer.ifidx = WIFI_IF_AP;
#endif
    peer.channel = 0;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(s_hub_mac)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    }

    s_paired = true;
    s_send_fail_count = 0;
    ESP_ERROR_CHECK(esp_timer_stop(s_advertise_timer));
    if (s_unpaired_failed_timer) esp_timer_stop(s_unpaired_failed_timer);
    start_sensor_data_timer();
    set_led(LED_PATTERN_HEARTBEAT);

    esp_now_pair_ack_t ack = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PAIR_ACK,
    };
    memcpy(ack.node_mac, s_mac, sizeof(ack.node_mac));
    esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "페어링됨: hub " MACSTR ", PAIR_ACK %s", MAC2STR(s_hub_mac), esp_err_to_name(err));
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
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

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

    const esp_timer_create_args_t adv_timer_args = {
        .callback = advertise_timer_cb,
        .name     = "espnow_adv",
    };
    ESP_ERROR_CHECK(esp_timer_create(&adv_timer_args, &s_advertise_timer));

    const esp_timer_create_args_t unpaired_timer_args = {
        .callback = unpaired_failed_timer_cb,
        .name     = "espnow_unpaired",
    };
    ESP_ERROR_CHECK(esp_timer_create(&unpaired_timer_args, &s_unpaired_failed_timer));

    enter_advertising(false);
}

const char *esp_now_node_get_name(void)
{
    return s_name;
}

bool esp_now_node_is_paired(void)
{
    return s_paired;
}
