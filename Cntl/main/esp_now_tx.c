#include "esp_now_tx.h"
#include "esp_now_reliable.h"
#include "ui_log.h"
#include "device_config.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "esp_now_tx";

/* 2026-08-10 — "응답성"(response_interval_sec)은 CAM의 딥슬립 사이클 길이이자, 사용자
 * 온디맨드 명령의 최악 대기시간으로 문서화된 값(계획서 참고)인데, 정작 재시도 로직은
 * 호출부가 넘긴 고정 timeout_ms*max_attempts(예: 500ms*3=1.5초)만 기다리고 포기해서,
 * CAM이 정상적으로(버그 아님) 자고 있을 때 도착한 명령은 그 약속과 무관하게 거의 항상
 * 실패했음(실사용 중 3006 반복으로 발견 — 사용자 지적: "잠들었어도 반응성 시간 내에
 * 명령을 주고 결과를 받아왔어야 하는데"). 여기서 호출부의 max_attempts를 "최소값"으로
 * 남겨두고, 실제 시도 횟수는 response_interval_sec 기준으로 늘려서 CAM의 다음 자연스러운
 * 웨이크까지는 기다리게 함. 최대절전(30분) 티어는 블로킹 재시도 대상에서 제외 — 계획서가
 * 이미 그 티어는 인터랙티브 조작 자체를 포기하는 별도 시나리오/UI가 필요하다고 명시해뒀고,
 * 30분을 그대로 재시도 예산으로 쓰면 명령 하나가 UI에서 30분간 "진행 중"으로 보이게 됨 */
#define TX_RESPONSE_BUDGET_CAP_SEC 30U
#define TX_WAKE_MARGIN_MS 3000U  /* CAM 웨이크 후 페어링 핸드셰이크(SET_TIME/CONFIG_SET) 여유 */

static int effective_max_attempts(uint32_t timeout_ms, int caller_min_attempts)
{
    uint32_t sec = device_config_get_response_interval_sec();
    if (sec > TX_RESPONSE_BUDGET_CAP_SEC) sec = TX_RESPONSE_BUDGET_CAP_SEC;
    uint32_t budget_ms = sec * 1000U + TX_WAKE_MARGIN_MS;
    int attempts = (int)((budget_ms + timeout_ms - 1) / timeout_ms);
    return attempts > caller_min_attempts ? attempts : caller_min_attempts;
}

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

        int attempts = effective_max_attempts(item.timeout_ms, item.max_attempts);
        esp_err_t err = esp_now_reliable_request(item.mac, item.req, item.req_len,
                                                  item.accept_reply_types, item.accept_reply_types_count,
                                                  item.timeout_ms, attempts,
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
            ESP_LOGW(TAG, "%s 무응답(%d회 시도, 응답성예산 반영)", item.what, attempts);
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
