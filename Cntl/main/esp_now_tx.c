#include "esp_now_tx.h"
#include "esp_now_reliable.h"
#include "ui_log.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "esp_now_tx";

/* 2026-08-10 도입 -> 2026-08-26 삭제(사용자 지시) — 원래는 "CAM이 자고 있을 때 도착한 명령도
 * CAM의 다음 자연스러운 웨이크까지는 재시도해서 언젠가 닿게 하자"는 취지로, 응답성 설정
 * 기준으로 재시도 횟수를 시간 단위로 부풀렸었음(effective_max_attempts, 최대 30초+3초
 * 마진까지 — 즉 메시지 하나가 최대 33초까지 걸릴 수 있었음). 그런데 CASK 재설계 이후
 * esp_now_tx_enqueue()를 부르는 모든 경우(CONFIG/할일/SLEEP_NOW/PAIR_REQUEST 등)가 전부
 * 캠이 방금 먼저 연락해왔을 때(WAKE_HELLO/ADVERTISE)의 응답으로만 나가서, 부르는 그 순간
 * 캠이 깨어있다는 게 이미 보장됨 — "자고 있을지 모르니 시간을 두고 재시도"할 이유 자체가
 * 없어짐. 이 시간 기반 부풀리기가 오히려 SLEEP_NOW 하나가 못 가면 캠이 CASK_SILENCE_TIMEOUT_MS
 * 없이 무한정 기다리게 되는 버그의 실제 원인 중 하나로 드러남(실기에서 확인) — 이제 호출부가
 * 넘긴 고정 횟수를 그대로 씀(count 기반) */

#define TX_REQ_MAX_LEN 32   /* PAIR_REQUEST/PHOTO_REQUEST/LIST_REQUEST/DELETE_*_REQUEST 모두
                              * 20바이트 이하 — 여유있게 32 */
/* 2026-08-26(사용자 지시) — CASK 재설계로 WAKE_HELLO 하나당 항상 3개(CONFIG/할일/SLEEP_NOW)가
 * 들어오는데, 옛 8은 노드 3대만 겹쳐도 꽉 참. 30개 이상으로 늘림(사용자 지정) */
#define TX_QUEUE_LEN   32

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
            /* 2026-08-10 — 예전엔 여기서 무조건 UI_ERR_NOT_PAIRED(2007, "페어링 끊김")를
             * 찍었는데, 이 모듈은 어떤 요청이든 다 거쳐가는 범용 전송 스케줄러라 "페어링
             * 끊김"이 실제 원인과 안 맞는 경우가 대부분이었음(실사용 중 발견 — CAM이
             * 딥슬립 중이라 사진요청이 무응답인 것뿐인데 2007이 3006과 같이 겹쳐서 뜸).
             * 이제 호출부마다 자기 상황에 맞는 전용 에러(UI_ERR_FETCH_NORESPONSE/
             * _LIST_NORESPONSE/_CAPTURE_NORESPONSE/_CONFIG_NORESPONSE 등, 각자의 상태머신
             * 폴링에서 이미 처리)가 있으므로 여기서는 진단용 로그만 남기고 UI 에러는 안 띄움.
             * 자동 재연결(esp_now_hub_pair)처럼 애초에 사용자에게 알릴 필요 없는 백그라운드
             * 요청도 있어서, 범용 계층에서 일괄 판단하는 게 애초에 무리였음 */
            ESP_LOGW(TAG, "%s 무응답(%d회 시도)", item.what, item.max_attempts);
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
        /* 2026-08-26(사용자 지시) — "큐 풀이면 최소한 에러라도 냈어야" — 예전엔 시리얼
         * 로그(ESP_LOGW)만 남기고 화면엔 아무 표시가 없어서 시리얼 안 보고 있으면 통째로
         * 놓쳤음. 이건 재시도 여지없이 그 자리에서 완전히 버려지는 거라 워닝이 아니라
         * 에러 — ui_log_add_err()로 화면 토스트까지 뜨게 함(2xxx 통신 전송 대역) */
        ESP_LOGE(TAG, "%s: 큐 가득 — 버림", what);
        ui_log_add_err(UI_ERR_TX_QUEUE_FULL, "TX queue full, dropped: %s", what);
    }
}
