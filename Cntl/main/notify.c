#include "notify.h"
#include "fs.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "NOTIFY";

#define NOTIFY_CFG_PATH   FS_MOUNT_POINT "/notify.cfg"
#define NOTIFY_SERVER     "http://ntfy.sh/"
#define NOTIFY_Q_DEPTH    4
#define NOTIFY_TIMEOUT_MS 10000

typedef struct {
    char title[64];
    char msg[192];
} notify_item_t;

typedef struct {
    char topic[64];
    char click[160];
} notify_cfg_t;

static QueueHandle_t s_q = NULL;
static StaticQueue_t s_q_struct;
/* 큐 저장소·수신 버퍼·설정·URL은 PSRAM(feedback_prefer_psram_for_buffers), 태스크 스택에 두지 않음 */
static notify_item_t *s_item = NULL;      /* 보내기 태스크 수신 버퍼 */
static notify_item_t *s_staging = NULL;   /* notify_send 공용 조립 버퍼(s_send_mutex로 보호) */
static SemaphoreHandle_t s_send_mutex = NULL;
static notify_cfg_t *s_cfg = NULL;        /* 파일에서 읽은 값(s_cfg_mutex로 보호) */
static notify_cfg_t *s_send_cfg = NULL;   /* 보내기 태스크가 보낼 동안 쓰는 복사본(잠금 없이) */
/* s_cfg·s_line 보호 — 보내기 태스크·웹서버·LVGL 태스크가 같이 씀. 잡는 구간은 파일 읽기·복사뿐(네트워크 대기 없음) */
static SemaphoreHandle_t s_cfg_mutex = NULL;
static char *s_url = NULL;
static char *s_line = NULL;               /* notify.cfg 한 줄(보내기 태스크 전용) */
#define NOTIFY_LINE_LEN 200
#define NOTIFY_URL_LEN (sizeof(NOTIFY_SERVER) + sizeof(((notify_cfg_t *)0)->topic))

/* notify.cfg를 읽어 s_cfg에 채움(s_cfg_mutex 안에서) — topic이 없으면 false. 줄 끝 공백·CR 제거 */
static bool load_cfg_locked(void)
{
    memset(s_cfg, 0, sizeof(*s_cfg));
    FILE *f = fopen(NOTIFY_CFG_PATH, "r");
    if (!f) return false;
    char *line = s_line;
    while (fgets(line, NOTIFY_LINE_LEN, f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
        if (strncmp(line, "topic=", 6) == 0) {
            snprintf(s_cfg->topic, sizeof(s_cfg->topic), "%s", line + 6);
        } else if (strncmp(line, "click=", 6) == 0) {
            snprintf(s_cfg->click, sizeof(s_cfg->click), "%s", line + 6);
        }
    }
    fclose(f);
    return s_cfg->topic[0] != '\0';
}

static void send_one(const notify_item_t *it)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    bool have_topic = load_cfg_locked();
    *s_send_cfg = *s_cfg;
    xSemaphoreGive(s_cfg_mutex);
    if (!have_topic) {
        ESP_LOGW(TAG, "No topic in notify.cfg - not sent: %s", it->title);
        return;
    }
    snprintf(s_url, NOTIFY_URL_LEN, NOTIFY_SERVER "%s", s_send_cfg->topic);
    esp_http_client_config_t hc = {
        .url = s_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = NOTIFY_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "text/plain");
    if (it->title[0]) esp_http_client_set_header(client, "Title", it->title);
    if (s_send_cfg->click[0]) esp_http_client_set_header(client, "Click", s_send_cfg->click);
    esp_http_client_set_post_field(client, it->msg, (int)strlen(it->msg));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Send failed: %s (HTTP %d) - %s", esp_err_to_name(err), status, it->title);
    } else {
        ESP_LOGI(TAG, "Sent: %s", it->title);
    }
}

static void notify_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xQueueReceive(s_q, s_item, portMAX_DELAY) == pdTRUE) send_one(s_item);
    }
}

void notify_init(void)
{
    if (s_q) return;
    uint8_t *q_storage = heap_caps_malloc(NOTIFY_Q_DEPTH * sizeof(notify_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_item = heap_caps_malloc(sizeof(notify_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_staging = heap_caps_malloc(sizeof(notify_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_send_mutex = xSemaphoreCreateMutex();
    s_cfg = heap_caps_calloc(1, sizeof(notify_cfg_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_send_cfg = heap_caps_calloc(1, sizeof(notify_cfg_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_cfg_mutex = xSemaphoreCreateMutex();
    s_url = heap_caps_malloc(NOTIFY_URL_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_line = heap_caps_malloc(NOTIFY_LINE_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!q_storage || !s_item || !s_staging || !s_send_mutex || !s_cfg || !s_send_cfg || !s_cfg_mutex || !s_url || !s_line) {
        ESP_LOGE(TAG, "alloc failed - notifications disabled");
        return;
    }
    notify_reload_cfg();  /* 콘 화면 Web 주소·QR용 바깥 주소를 처음 한 번 읽어 둠 */
    s_q = xQueueCreateStatic(NOTIFY_Q_DEPTH, sizeof(notify_item_t), q_storage, &s_q_struct);
    /* 우선순위 5(통신 17·SR 15·파일 10보다 낮음 — 알림은 늦어도 됨), 스택 PSRAM */
    if (xTaskCreatePinnedToCoreWithCaps(notify_task, "notify", 6144, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "task create failed - notifications disabled");
        s_q = NULL;
    }
}

bool notify_send(const char *title, const char *msg)
{
    if (!s_q) return false;
    /* 항목(256B)을 호출 태스크 스택에 두지 않음(feedback_never_put_large_data_on_stack) — PSRAM 버퍼 하나를 뮤텍스로 공유,
     * 큐에 넣을 때 복사되므로 잡는 구간은 짧음 */
    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    snprintf(s_staging->title, sizeof(s_staging->title), "%s", title ? title : "");
    snprintf(s_staging->msg, sizeof(s_staging->msg), "%s", msg ? msg : "");
    bool ok = (xQueueSend(s_q, s_staging, 0) == pdTRUE);
    if (!ok) ESP_LOGW(TAG, "Queue full - dropped: %s", s_staging->title);
    xSemaphoreGive(s_send_mutex);
    return ok;
}

void notify_reload_cfg(void)
{
    if (!s_cfg_mutex) return;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    load_cfg_locked();
    xSemaphoreGive(s_cfg_mutex);
}

bool notify_copy_public_url(char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!s_cfg_mutex) return false;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    snprintf(out, cap, "%s", s_cfg->click);
    xSemaphoreGive(s_cfg_mutex);
    return out[0] != '\0';
}
