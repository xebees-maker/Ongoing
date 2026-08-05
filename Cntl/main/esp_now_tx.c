#include "esp_now_tx.h"
#include "esp_now_reliable.h"
#include "ui_log.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "esp_now_tx";

#define TX_REQ_MAX_LEN 32   /* PAIR_REQUEST/PHOTO_REQUEST/LIST_REQUEST/DELETE_*_REQUEST 모두
                              * 20바이트 이하 — 여유있게 32 */
#define TX_QUEUE_LEN   8

typedef struct {
    uint8_t  mac[6];
    uint8_t  req[TX_REQ_MAX_LEN];
    size_t   req_len;
    const uint8_t *accept_reply_types;
    size_t   accept_reply_types_count;
    uint32_t timeout_ms;
    int      max_attempts;
    const char *what;
} tx_item_t;

static QueueHandle_t s_tx_queue = NULL;

static void tx_task(void *arg)
{
    (void)arg;
    tx_item_t item;
    for (;;) {
        if (xQueueReceive(s_tx_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        esp_err_t err = esp_now_reliable_request(item.mac, item.req, item.req_len,
                                                  item.accept_reply_types, item.accept_reply_types_count,
                                                  item.timeout_ms, item.max_attempts,
                                                  NULL, 0, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s 무응답(%d회 시도)", item.what, item.max_attempts);
            ui_log_add_err(UI_ERR_NOT_PAIRED, "%s 실패(무응답)", item.what);
        } else {
            ESP_LOGI(TAG, "%s 완료", item.what);
        }
    }
}

void esp_now_tx_init(void)
{
    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(tx_item_t));
    xTaskCreate(tx_task, "esp_now_tx", 4096, NULL, 5, NULL);
}

void esp_now_tx_enqueue(const uint8_t *mac, const void *req, size_t req_len,
                         const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                         uint32_t timeout_ms, int max_attempts, const char *what)
{
    if (req_len > TX_REQ_MAX_LEN) {
        ESP_LOGE(TAG, "%s: req_len(%u)이 TX_REQ_MAX_LEN(%d) 초과 — 무시", what, (unsigned)req_len, TX_REQ_MAX_LEN);
        return;
    }
    tx_item_t item = {
        .req_len               = req_len,
        .accept_reply_types    = accept_reply_types,
        .accept_reply_types_count = accept_reply_types_count,
        .timeout_ms             = timeout_ms,
        .max_attempts           = max_attempts,
        .what                   = what,
    };
    memcpy(item.mac, mac, sizeof(item.mac));
    memcpy(item.req, req, req_len);
    if (xQueueSend(s_tx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "%s: 큐 가득 — 무시", what);
    }
}
