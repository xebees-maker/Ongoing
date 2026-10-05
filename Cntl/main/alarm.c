#include "alarm.h"
#include "sd_storage.h"
#include "rtc_sync.h"
#include "web_session.h"
#include "ui_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "ALARM";

#define ALARM_FILE    SD_STORAGE_MOUNT_POINT "/alarm.bin"
#define ALARM_Q_DEPTH 16

typedef struct {
    uint8_t type;
    char    title[32];
    char    msg[86];
} alarm_item_t;

static QueueHandle_t s_q = NULL;
static StaticQueue_t s_q_struct;
/* 버퍼는 전부 PSRAM(feedback_prefer_psram_for_buffers), 태스크 스택에 두지 않음 */
static alarm_item_t *s_item = NULL;      /* 알림 태스크 수신 버퍼 */
static alarm_item_t *s_staging = NULL;   /* alarm_post 조립 버퍼(s_post_mutex) */
static SemaphoreHandle_t s_post_mutex = NULL;
static alarm_rec_t *s_ring = NULL;       /* ALARM_HISTORY_MAX칸 — 파일과 같은 배치 */
static alarm_rec_t *s_wr = NULL;         /* 파일에 쓸 한 칸 복사본(알림 태스크 전용) */
static notify_settings_t *s_ns = NULL;   /* 알림 태스크 전용 */
static SemaphoreHandle_t s_mutex = NULL; /* s_ring·s_last_id·s_pending_from — 알림 태스크·웹서버 */
static SemaphoreHandle_t s_start = NULL;
static uint32_t s_last_id = 0;
static uint32_t s_pending_from = 1;      /* 이보다 앞 번호엔 보낼 것이 남아 있지 않음 */

static alarm_rec_t *slot(uint32_t id) { return &s_ring[id % ALARM_HISTORY_MAX]; }

static bool is_pending(const alarm_rec_t *r, uint32_t id)
{
    return r->id == id && (r->flags & ALARM_F_PUSH) && !(r->flags & (ALARM_F_SEEN | ALARM_F_SENT));
}

/* 기록 파일 → s_ring. 없으면 빈 파일(전체 칸)을 만들어 둠 — 이후엔 칸 자리에 덮어쓰기만 */
static void load_file(void)
{
    if (!sd_storage_is_mounted()) {
        ESP_LOGW(TAG, "SD not mounted - alarm history kept in RAM only");
        return;
    }
    FILE *f = fopen(ALARM_FILE, "rb");
    if (!f) {
        f = fopen(ALARM_FILE, "wb");
        if (f) {
            fwrite(s_ring, sizeof(alarm_rec_t), ALARM_HISTORY_MAX, f);
            fclose(f);
        }
        return;
    }
    size_t n = fread(s_ring, sizeof(alarm_rec_t), ALARM_HISTORY_MAX, f);
    fclose(f);
    for (size_t i = 0; i < ALARM_HISTORY_MAX; i++) {
        alarm_rec_t *r = &s_ring[i];
        if (i >= n || r->id % ALARM_HISTORY_MAX != i) { memset(r, 0, sizeof(*r)); continue; }
        if (r->id > s_last_id) s_last_id = r->id;
        r->flags |= ALARM_F_SEEN;  /* 지난 부팅 것은 다시 보내지 않음 */
    }
    s_pending_from = s_last_id + 1;
    ESP_LOGI(TAG, "History loaded: last #%lu", (unsigned long)s_last_id);
}

static void write_rec(const alarm_rec_t *r)
{
    if (!sd_storage_is_mounted()) return;
    FILE *f = fopen(ALARM_FILE, "r+b");
    if (!f) {
        ESP_LOGW(TAG, "%s open failed", ALARM_FILE);
        return;
    }
    if (fseek(f, (long)(r->id % ALARM_HISTORY_MAX) * (long)sizeof(*r), SEEK_SET) == 0) fwrite(r, sizeof(*r), 1, f);
    fclose(f);
}

static void handle(const alarm_item_t *it)
{
    notify_get_settings(s_ns);
    bool push = (s_ns->types >> it->type) & 1u;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t id = ++s_last_id;
    alarm_rec_t *r = slot(id);
    memset(r, 0, sizeof(*r));
    r->id = id;
    r->t = rtc_sync_get_unix_time();
    r->type = it->type;
    r->flags = push ? ALARM_F_PUSH : 0;
    memcpy(r->title, it->title, sizeof(r->title));
    memcpy(r->msg, it->msg, sizeof(r->msg));
    *s_wr = *r;
    xSemaphoreGive(s_mutex);
    write_rec(s_wr);
    ESP_LOGI(TAG, "#%lu type %u%s: %s - %s", (unsigned long)id, (unsigned)it->type, push ? "" : " (off)", it->title, it->msg);
}

/* 웹 세션이 없으면 남은 보낼 대상을 ntfy로. 큐가 차면 멈췄다가 다음 차례에 이어서 */
static void flush_pending(void)
{
    if (web_session_web_active()) return;  /* 웹앱이 주화면 조회로 받아 감 */
    for (;;) {
        bool found = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        while (s_pending_from <= s_last_id && !is_pending(slot(s_pending_from), s_pending_from)) s_pending_from++;
        if (s_pending_from <= s_last_id) { *s_wr = *slot(s_pending_from); found = true; }
        xSemaphoreGive(s_mutex);
        if (!found) return;
        if (!notify_send(s_wr->title, s_wr->msg)) return;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        alarm_rec_t *r = slot(s_wr->id);
        if (r->id == s_wr->id) r->flags |= ALARM_F_SENT;
        xSemaphoreGive(s_mutex);
    }
}

static void alarm_task(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_start, portMAX_DELAY);
    load_file();
    for (;;) {
        if (xQueueReceive(s_q, s_item, pdMS_TO_TICKS(1000)) == pdTRUE) handle(s_item);
        flush_pending();
    }
}

/* ui_log 에러·경고가 상태 목록에 새로 올라갈 때 — SD·CAN 코드는 그 종류로 한 번만(에러·경고로 또 보내지 않음) */
static void on_log_event(bool is_err, int code, const char *msg)
{
    notify_type_t type;
    char title[32];
    if (code == UI_ERR_SD_MOUNT_FAILED || code == UI_ERR_SD_IO_FAIL || code == UI_WARN_SD_BAD_ENTRY) {
        type = NOTIFY_TYPE_SD;
        snprintf(title, sizeof(title), "SD error");
    } else if (code == UI_ERR_CAN_BUS_OFF || code == UI_ERR_CAN_TX_STUCK) {
        type = NOTIFY_TYPE_CAN;
        snprintf(title, sizeof(title), "CAN bus fault");
    } else {
        type = is_err ? NOTIFY_TYPE_ERROR : NOTIFY_TYPE_WARN;
        snprintf(title, sizeof(title), "%s %04d", is_err ? "Error" : "Warning", code);
    }
    alarm_post(type, title, "[%04d] %s", code, msg);
}

void alarm_init(void)
{
    if (s_q) return;
    uint8_t *q_storage = heap_caps_malloc(ALARM_Q_DEPTH * sizeof(alarm_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_item = heap_caps_malloc(sizeof(alarm_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_staging = heap_caps_malloc(sizeof(alarm_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ring = heap_caps_calloc(ALARM_HISTORY_MAX, sizeof(alarm_rec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_wr = heap_caps_calloc(1, sizeof(alarm_rec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ns = heap_caps_calloc(1, sizeof(notify_settings_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_post_mutex = xSemaphoreCreateMutex();
    s_mutex = xSemaphoreCreateMutex();
    s_start = xSemaphoreCreateBinary();
    if (!q_storage || !s_item || !s_staging || !s_ring || !s_wr || !s_ns || !s_post_mutex || !s_mutex || !s_start) {
        ESP_LOGE(TAG, "alloc failed - alarms disabled");
        return;
    }
    s_q = xQueueCreateStatic(ALARM_Q_DEPTH, sizeof(alarm_item_t), q_storage, &s_q_struct);
    /* 우선순위 5(알림 보내기와 같음 — 통신 17·SR 15·파일 10보다 낮음), 스택 PSRAM */
    if (xTaskCreatePinnedToCoreWithCaps(alarm_task, "alarm", 4096, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "task create failed - alarms disabled");
        s_q = NULL;
        return;
    }
    ui_log_set_event_cb(on_log_event);
}

void alarm_start(void)
{
    if (s_start) xSemaphoreGive(s_start);
}

void alarm_post(notify_type_t type, const char *title, const char *fmt, ...)
{
    if (!s_q || type >= NOTIFY_TYPE_COUNT) return;
    xSemaphoreTake(s_post_mutex, portMAX_DELAY);
    memset(s_staging, 0, sizeof(*s_staging));
    s_staging->type = (uint8_t)type;
    snprintf(s_staging->title, sizeof(s_staging->title), "%s", title ? title : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_staging->msg, sizeof(s_staging->msg), fmt, ap);
    va_end(ap);
    if (xQueueSend(s_q, s_staging, 0) != pdTRUE) ESP_LOGW(TAG, "Queue full - dropped: %s", s_staging->title);
    xSemaphoreGive(s_post_mutex);
}

uint32_t alarm_last_id(void)
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t id = s_last_id;
    xSemaphoreGive(s_mutex);
    return id;
}

int alarm_read_page(uint32_t page, uint32_t page_size, alarm_rec_t *out)
{
    if (!s_mutex || page_size == 0) return 0;
    int n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint64_t skip = (uint64_t)page * page_size;
    if (skip < s_last_id) {
        uint32_t id = s_last_id - (uint32_t)skip;
        for (; id > 0 && (uint32_t)n < page_size && s_last_id - id < ALARM_HISTORY_MAX; id--) {
            const alarm_rec_t *r = slot(id);
            if (r->id == id) out[n++] = *r;
        }
    }
    xSemaphoreGive(s_mutex);
    return n;
}

int alarm_take_web_new(alarm_rec_t *out, int cap)
{
    if (!s_mutex) return 0;
    int n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (uint32_t id = s_pending_from; id <= s_last_id && n < cap; id++) {
        alarm_rec_t *r = slot(id);
        if (!is_pending(r, id)) continue;
        r->flags |= ALARM_F_SEEN;
        out[n++] = *r;
    }
    xSemaphoreGive(s_mutex);
    return n;
}
