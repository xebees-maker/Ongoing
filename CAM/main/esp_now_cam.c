#include "esp_now_cam.h"
#include "cam_storage.h"
#include "cam_node.h"

#include <string.h>
#include <stdio.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "status_led.h"

static const char *TAG = "esp_now_cam";

/* --- 광고/채널스캔/페어링 — Sens/main/esp_now_node.c와 동일 패턴, 자세한 이유는
 * 그쪽 주석 참고(이 세션에서 같이 설계함) --- */
#define ADVERTISE_PERIOD_US        (1000 * 1000)
#define KEEPALIVE_PERIOD_US        (1000 * 1000)
#define SEND_FAIL_THRESHOLD        3
#define UNPAIRED_FAILED_TIMEOUT_US (30 * 1000 * 1000)
#define SCAN_DWELL_US              (300 * 1000)
#define SCAN_CHANNEL_MIN           1
#define SCAN_CHANNEL_MAX           13

static char    s_name[ESP_NOW_LINK_NAME_LEN] = "";
static uint8_t s_mac[6] = { 0 };
static esp_timer_handle_t s_advertise_timer = NULL;
static esp_timer_handle_t s_keepalive_timer = NULL;
static esp_timer_handle_t s_unpaired_failed_timer = NULL;

static bool    s_paired = false;
static uint8_t s_hub_mac[6] = { 0 };
static int     s_send_fail_count = 0;

static bool    s_channel_locked = false;
static uint8_t s_scan_channel   = SCAN_CHANNEL_MIN;

static gpio_num_t s_led_pin = GPIO_NUM_NC;

static const uint8_t s_broadcast_addr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* --- 사진 전송 --- */
static QueueHandle_t s_photo_request_queue = NULL;
static SemaphoreHandle_t s_send_done_sem   = NULL;
static volatile bool s_awaiting_chunk_ack  = false;
static volatile esp_now_send_status_t s_last_chunk_status = ESP_NOW_SEND_SUCCESS;

static void set_led(led_pattern_t pattern)
{
    if (s_led_pin == GPIO_NUM_NC) return;
    status_led_set_pattern(s_led_pin, pattern);
}

static void enter_advertising(bool immediately_failed)
{
    s_paired = false;
    s_send_fail_count = 0;

    if (s_keepalive_timer) esp_timer_stop(s_keepalive_timer);

    s_channel_locked = false;
    s_scan_channel   = SCAN_CHANNEL_MIN;
    esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);

    esp_timer_stop(s_advertise_timer);
    esp_timer_start_periodic(s_advertise_timer, SCAN_DWELL_US);

    if (immediately_failed) {
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
    if (s_paired) return;
    set_led(LED_PATTERN_BLINK_SLOW);
}

static void keepalive_timer_cb(void *arg)
{
    (void)arg;
    /* Sens의 SENSOR_DATA와 달리 CAM은 페어링 후 딱히 매초 보낼 데이터가 없음 —
     * PAIR_ACK를 그대로 재전송해서 "아직 살아있음"을 알림. Cntl의 기존 PAIR_ACK
     * 핸들러가 이미 paired=true/last_data_ms 갱신을 해주므로 Cntl 쪽 변경 불필요. */
    esp_now_pair_ack_t ack = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PAIR_ACK,
    };
    memcpy(ack.node_mac, s_mac, sizeof(ack.node_mac));
    esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
}

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;

    if (s_awaiting_chunk_ack) {
        s_last_chunk_status = status;
        s_awaiting_chunk_ack = false;
        xSemaphoreGive(s_send_done_sem);
        return;
    }

    if (!s_paired) return;

    if (status == ESP_NOW_SEND_SUCCESS) {
        s_send_fail_count = 0;
        set_led(LED_PATTERN_HEARTBEAT);
        return;
    }

    s_send_fail_count++;
    ESP_LOGW(TAG, "전송 실패 (연속 %d회)", s_send_fail_count);
    if (s_send_fail_count >= SEND_FAIL_THRESHOLD) {
        ESP_LOGW(TAG, "허브 응답 끊김으로 판단 — 재광고 시작");
        enter_advertising(true);
    }
}

/* file_id 하나를 META + CHUNK*로 전송. 청크 전송 실패 시 최대 3회 재시도(과설계 방지 —
 * 실기 테스트 후 부족하면 Cntl 쪽 NACK/누락감지를 추가) */
static bool send_one_photo(uint32_t file_id)
{
    FILE *fp = NULL;
    uint32_t size = 0;
    if (cam_storage_open_read(file_id, &fp, &size) != ESP_OK) {
        ESP_LOGW(TAG, "파일 열기 실패: id=%u", (unsigned)file_id);
        return false;
    }

    uint16_t total_chunks = (uint16_t)((size + ESP_NOW_PHOTO_CHUNK_DATA_LEN - 1) / ESP_NOW_PHOTO_CHUNK_DATA_LEN);

    /* META를 보내기 전에 파일 전체를 한 번 스트리밍으로 훑어 CRC32를 계산해둔다(끝나면
     * rewind해서 실제 전송은 처음부터 다시 읽음) — Cntl이 청크 개수만 맞다고 안심하지 않고
     * 재조립된 내용이 실제로 같은지 확인할 수 있게. "크기/개수가 맞다 ≠ 내용이 맞다"는
     * 이번 CAM corruption 조사에서 직접 겪은 교훈(project_cam_dvp_corruption_investigation
     * 메모리 참고) — 같은 실수를 이 전송 경로에서도 반복하지 않기 위함. */
    uint32_t crc = 0;
    {
        uint8_t crc_buf[256];
        size_t n;
        while ((n = fread(crc_buf, 1, sizeof(crc_buf), fp)) > 0) {
            crc = esp_rom_crc32_le(crc, crc_buf, n);
        }
        rewind(fp);
    }

    esp_now_photo_meta_t meta = {
        .version      = ESP_NOW_LINK_VERSION,
        .msg_type     = ESP_NOW_MSG_PHOTO_META,
        .file_id      = file_id,
        .total_size   = size,
        .total_chunks = total_chunks,
        .crc32        = crc,
    };
    esp_now_send(s_hub_mac, (const uint8_t *)&meta, sizeof(meta));
    vTaskDelay(pdMS_TO_TICKS(5));

    esp_now_photo_chunk_t chunk = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_CHUNK };
    bool ok = true;

    for (uint16_t idx = 0; idx < total_chunks; idx++) {
        size_t n = fread(chunk.data, 1, ESP_NOW_PHOTO_CHUNK_DATA_LEN, fp);
        chunk.file_id   = file_id;
        chunk.chunk_idx = idx;
        chunk.chunk_len = (uint16_t)n;

        int attempt;
        for (attempt = 0; attempt < 3; attempt++) {
            s_awaiting_chunk_ack = true;
            esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&chunk, sizeof(chunk));
            if (err != ESP_OK) {
                s_awaiting_chunk_ack = false;
                ESP_LOGW(TAG, "chunk %u/%u send() 실패: %s", idx, total_chunks, esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (xSemaphoreTake(s_send_done_sem, pdMS_TO_TICKS(200)) == pdTRUE &&
                s_last_chunk_status == ESP_NOW_SEND_SUCCESS) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (attempt >= 3) {
            ESP_LOGW(TAG, "chunk %u/%u 3회 재시도 실패 — 이 파일 전송 중단", idx, total_chunks);
            ok = false;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));  /* ESP-NOW 자체 큐 과부하 방지용 최소 페이싱 */
    }

    fclose(fp);
    return ok;
}

static void photo_transfer_task(void *arg)
{
    (void)arg;
    esp_now_photo_request_t req;
    for (;;) {
        if (xQueueReceive(s_photo_request_queue, &req, portMAX_DELAY) != pdTRUE) continue;
        if (!s_paired) continue;

        /* Cntl이 기존 저장분 대신 "지금 당장 새로 찍어서" 원하는 경우 — esp_camera_fb_get()이
         * 최대 수 초 블로킹될 수 있어서 recv_cb(콜백 컨텍스트)에서 바로 처리하지 않고 여기
         * (전용 태스크)까지 큐로 넘겨서 처리한다. 찍고 나면 방금 그 1장만 LATEST로 보낸다. */
        if (req.mode == PHOTO_REQUEST_MODE_CAPTURE_NOW) {
            ESP_LOGI(TAG, "CAPTURE_NOW 요청 — 즉시 촬영");
            if (!cam_node_capture_now()) {
                ESP_LOGW(TAG, "즉시 촬영 실패 — 보낼 사진 없이 DONE만 전송");
                esp_now_photo_done_t done = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_DONE };
                esp_now_send(s_hub_mac, (const uint8_t *)&done, sizeof(done));
                continue;
            }
            req.mode = PHOTO_REQUEST_MODE_LATEST;
        }

        uint32_t ids[CAM_STORAGE_MAX_FILES];
        int count = cam_storage_list((photo_request_mode_t)req.mode, req.param, ids, CAM_STORAGE_MAX_FILES);
        ESP_LOGI(TAG, "PHOTO_REQUEST mode=%d param=%u -> %d장 전송 시작", req.mode, (unsigned)req.param, count);

        for (int i = 0; i < count && s_paired; i++) {
            send_one_photo(ids[i]);
        }

        esp_now_photo_done_t done = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_DONE };
        esp_now_send(s_hub_mac, (const uint8_t *)&done, sizeof(done));
        ESP_LOGI(TAG, "PHOTO_DONE 전송");
    }
}

static void resolve_name(void)
{
    esp_wifi_get_mac(WIFI_IF_AP, s_mac);
#if defined(CONFIG_CAM_NODE_NAME)
    if (strlen(CONFIG_CAM_NODE_NAME) > 0) {
        snprintf(s_name, sizeof(s_name), "%s", CONFIG_CAM_NODE_NAME);
        return;
    }
#endif
    snprintf(s_name, sizeof(s_name), "Cam-%02X%02X", s_mac[4], s_mac[5]);
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 2) return;
    uint8_t msg_type = data[1];

    if (!s_paired && !s_channel_locked && msg_type == ESP_NOW_MSG_ADVERTISE_ACK) {
        if (len < (int)sizeof(esp_now_advertise_ack_t)) return;
        s_channel_locked = true;
        esp_timer_stop(s_advertise_timer);
        esp_timer_start_periodic(s_advertise_timer, ADVERTISE_PERIOD_US);
        ESP_LOGI(TAG, "Cntl 채널 확인됨(CH%d) — 스캔 중지, 페어링 대기", s_scan_channel);
        return;
    }

    if (msg_type == ESP_NOW_MSG_PHOTO_REQUEST) {
        if (!s_paired || len < (int)sizeof(esp_now_photo_request_t)) return;
        esp_now_photo_request_t req;
        memcpy(&req, data, sizeof(req));
        if (xQueueSend(s_photo_request_queue, &req, 0) != pdTRUE) {
            ESP_LOGW(TAG, "PHOTO_REQUEST 큐 가득 — 이전 전송 아직 진행중, 무시");
        }
        return;
    }

    if (s_paired || msg_type != ESP_NOW_MSG_PAIR_REQUEST) return;
    if (len < (int)sizeof(esp_now_pair_request_t)) return;
    const esp_now_pair_request_t *req = (const esp_now_pair_request_t *)data;

    memcpy(s_hub_mac, req->hub_mac, sizeof(s_hub_mac));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_hub_mac, sizeof(peer.peer_addr));
    peer.ifidx   = WIFI_IF_AP;
    peer.channel = 0;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(s_hub_mac)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    }

    s_paired = true;
    s_send_fail_count = 0;
    esp_timer_stop(s_advertise_timer);
    if (s_unpaired_failed_timer) esp_timer_stop(s_unpaired_failed_timer);
    if (!s_keepalive_timer) {
        const esp_timer_create_args_t ka_args = { .callback = keepalive_timer_cb, .name = "cam_keepalive" };
        ESP_ERROR_CHECK(esp_timer_create(&ka_args, &s_keepalive_timer));
    }
    esp_timer_start_periodic(s_keepalive_timer, KEEPALIVE_PERIOD_US);
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
    (void)arg;
    esp_now_advertise_t msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_ADVERTISE,
    };
    memcpy(msg.name, s_name, sizeof(msg.name));
    memcpy(msg.mac, s_mac, sizeof(msg.mac));
    esp_now_send(s_broadcast_addr, (const uint8_t *)&msg, sizeof(msg));

    if (!s_channel_locked) {
        s_scan_channel++;
        if (s_scan_channel > SCAN_CHANNEL_MAX) s_scan_channel = SCAN_CHANNEL_MIN;
        esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);
    }
}

void esp_now_cam_set_status_led(gpio_num_t pin)
{
    s_led_pin = pin;
    status_led_init(pin);
}

void esp_now_cam_init(void)
{
    resolve_name();
    ESP_LOGI(TAG, "노드 이름: %s (MAC " MACSTR ")", s_name, MAC2STR(s_mac));

    s_photo_request_queue = xQueueCreate(2, sizeof(esp_now_photo_request_t));
    s_send_done_sem       = xSemaphoreCreateBinary();
    xTaskCreate(photo_transfer_task, "photo_tx", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_broadcast_addr, sizeof(peer.peer_addr));
    peer.ifidx   = WIFI_IF_AP;
    peer.channel = 0;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    const esp_timer_create_args_t adv_args = { .callback = advertise_timer_cb, .name = "cam_adv" };
    ESP_ERROR_CHECK(esp_timer_create(&adv_args, &s_advertise_timer));

    const esp_timer_create_args_t unpaired_args = { .callback = unpaired_failed_timer_cb, .name = "cam_unpaired" };
    ESP_ERROR_CHECK(esp_timer_create(&unpaired_args, &s_unpaired_failed_timer));

    enter_advertising(false);
}

const char *esp_now_cam_get_name(void) { return s_name; }
bool esp_now_cam_is_paired(void) { return s_paired; }
