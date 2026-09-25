#include "esp_now_reliable.h"

#include <string.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "esp_now_reliable";

/* ESP-NOW v2 한도(1470B)까지 여유있게 — 어떤 응답 타입이 오든(DONE_ACK가 재사용하는
 * esp_now_photo_chunk_nack_t가 가장 큼, 약 810B) 이 버퍼 안에 들어옴 */
#define REPLY_BUF_CAP 1470

/* API 호출 자체를 직렬화(여러 태스크가 동시에 불러도 순서대로 처리되게) — 실제로는
 * Cntl의 esp_now_tx/CAM의 photo_tx처럼 호출부가 이미 태스크 하나로 직렬화돼 있어서
 * 경합이 거의 없을 것으로 예상되지만, 안전을 위해 둠 */
static SemaphoreHandle_t s_api_mutex = NULL;
static SemaphoreHandle_t s_done_sem  = NULL;
/* 송신 완료(=드라이버 송신 큐에 자리 생김) 신호 — esp_now_reliable_on_send_done()이 줌 */
static SemaphoreHandle_t s_tx_done_sem = NULL;
static volatile bool s_waiting_tx_slot = false;

/* NO_MEM 뒤 송신 완료 이벤트를 기다리는 최대 시간. 정상이면 다른 프레임이 끝나는 즉시(수 ms)
 * 깨어남 — 이 값은 앱이 on_send_done을 안 불러주거나 이벤트가 끝내 안 오는 경우의 상한일 뿐 */
#define TX_SLOT_WAIT_MAX_MS 100
#define TX_SLOT_MAX_TRIES   6

static volatile bool s_waiting = false;
static uint8_t        s_wait_peer_mac[6];
static const uint8_t  *s_wait_accept_types = NULL;
static size_t          s_wait_accept_count = 0;

static uint8_t s_reply_buf[REPLY_BUF_CAP];
static size_t  s_reply_len = 0;
static volatile bool s_matched = false;

static void ensure_init(void)
{
    if (s_api_mutex) return;
    s_api_mutex = xSemaphoreCreateMutex();
    s_done_sem  = xSemaphoreCreateBinary();
    s_tx_done_sem = xSemaphoreCreateBinary();
}

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

esp_err_t esp_now_reliable_request(const uint8_t *peer_mac,
                                    const void *req, size_t req_len,
                                    const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                                    uint32_t timeout_ms, int max_attempts,
                                    void *reply_out, size_t reply_out_cap, size_t *reply_out_len)
{
    ensure_init();
    add_peer_if_needed(peer_mac);

    xSemaphoreTake(s_api_mutex, portMAX_DELAY);

    memcpy(s_wait_peer_mac, peer_mac, sizeof(s_wait_peer_mac));
    s_wait_accept_types = accept_reply_types;
    s_wait_accept_count = accept_reply_types_count;
    s_matched = false;
    xSemaphoreTake(s_done_sem, 0);  /* 이전에 남아있을 수 있는 신호 비움 */
    s_waiting = true;

    esp_err_t result = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        /* 로컬 송신큐 포화(NO_MEM)는 "무응답"과 다른 원인 — 요청이 아예 안 나간 것뿐이라
         * 상대의 응답을 기다릴 이유가 없음. 짧게 재시도해서 실제로 내보낸 뒤에만
         * timeout_ms를 씀(2026-08-05, 실기에서 발견: 사진 청크 버스트로 큐가 찬 동안
         * DONE reliable_request가 매번 NO_MEM으로 못 나가면서도 매번 800ms씩 허비 —
         * 다른 곳(esp_now_cam.c 청크 전송 루프)과 같은 재시도 관례를 그대로 따름).
         * 2026-09-25(사용자 지시 — 이벤트 방식) — 예전엔 NO_MEM이면 20ms 쉬고 다시 보내는
         * 폴링이었음. 이제 송신 완료 이벤트(esp_now_reliable_on_send_done — 큐에 자리가 생김)를
         * 기다렸다가 다시 보냄. 보내기 직전에 신호를 비워 두므로, send가 NO_MEM을 돌려주기 전에
         * 끝난 송신이 있었다면 그 신호가 남아 있어 바로 재시도함(깨우기 유실 없음) */
        esp_err_t send_err = ESP_FAIL;
        for (int local_retry = 0; local_retry < TX_SLOT_MAX_TRIES; local_retry++) {
            xSemaphoreTake(s_tx_done_sem, 0);  /* 이전에 남아있을 수 있는 신호 비움 */
            s_waiting_tx_slot = true;
            send_err = esp_now_send(peer_mac, (const uint8_t *)req, req_len);
            if (send_err != ESP_ERR_ESPNOW_NO_MEM) {
                s_waiting_tx_slot = false;
                break;
            }
            xSemaphoreTake(s_tx_done_sem, pdMS_TO_TICKS(TX_SLOT_WAIT_MAX_MS));
            s_waiting_tx_slot = false;
        }
        if (send_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_now_send 실패(시도 %d/%d): %s", attempt + 1, max_attempts, esp_err_to_name(send_err));
        }

        if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE && s_matched) {
            if (reply_out && reply_out_cap > 0) {
                size_t copy_len = s_reply_len < reply_out_cap ? s_reply_len : reply_out_cap;
                memcpy(reply_out, s_reply_buf, copy_len);
                if (reply_out_len) *reply_out_len = copy_len;
            } else if (reply_out_len) {
                *reply_out_len = s_reply_len;
            }
            result = ESP_OK;
            break;
        }
    }

    s_waiting = false;
    xSemaphoreGive(s_api_mutex);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "요청 타임아웃(%d회 시도 모두 무응답)", max_attempts);
    }
    return result;
}

void esp_now_reliable_on_recv(uint8_t msg_type, const uint8_t *src_mac,
                               const uint8_t *data, int len)
{
    if (!s_waiting) return;
    if (!src_mac || memcmp(src_mac, s_wait_peer_mac, sizeof(s_wait_peer_mac)) != 0) return;

    bool type_ok = false;
    for (size_t i = 0; i < s_wait_accept_count; i++) {
        if (s_wait_accept_types[i] == msg_type) { type_ok = true; break; }
    }
    if (!type_ok) return;

    size_t copy_len = (size_t)len;
    if (copy_len > REPLY_BUF_CAP) copy_len = REPLY_BUF_CAP;
    memcpy(s_reply_buf, data, copy_len);
    s_reply_len = copy_len;
    s_matched   = true;
    xSemaphoreGive(s_done_sem);
}

void esp_now_reliable_on_send_done(void)
{
    /* send_cb는 Wi-Fi 태스크 컨텍스트(ISR 아님) — 일반 Give 사용. 초기화 전이거나 NO_MEM으로
     * 기다리는 요청이 없으면 아무 것도 안 함 */
    if (!s_tx_done_sem || !s_waiting_tx_slot) return;
    xSemaphoreGive(s_tx_done_sem);
}
