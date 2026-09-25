/**
 * @file    storage_mgr.c
 * @brief   storage_mgr.h 구현 — 파일처리 태스크 1개, 태스크 알림 비트로만 깨어남(이벤트 방식)
 */
#include "storage_mgr.h"
#include "sd_storage.h"
#include "photo_storage.h"
#include "stats_store.h"
#include "ui_log.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "storage_mgr";

#define EVT_RESCAN  (1u << 0)
#define EVT_CHECK   (1u << 1)

/* 예산(사용자 설계 2026-09-10) — 전체 용량의 사진 9 : 측정값 1, 각자 자기 예산의 90% 이상이면
 * 80%까지 오래된 것부터 정리 */
#define TRIM_START_PCT  90
#define TRIM_TARGET_PCT 80

static TaskHandle_t s_task = NULL;
static portMUX_TYPE s_snap_lock = portMUX_INITIALIZER_UNLOCKED;
static storage_mgr_snapshot_t s_snap;
static uint32_t s_pending_pic_deleted = 0;
static uint32_t s_pending_stats_deleted = 0;

static void publish_snapshot(bool valid, uint64_t total, uint64_t free_bytes, uint32_t bad_entries)
{
    uint64_t pic = photo_storage_get_used_bytes();
    uint64_t st = stats_store_get_used_bytes();
    taskENTER_CRITICAL(&s_snap_lock);
    s_snap.valid = valid;
    s_snap.sd_total = total;
    s_snap.sd_free = free_bytes;
    s_snap.pic_used = pic;
    s_snap.stats_used = st;
    s_snap.bad_entries = bad_entries;
    taskEXIT_CRITICAL(&s_snap_lock);
}

static void storage_task(void *arg)
{
    (void)arg;
    bool indexes_ready = false;
    uint32_t bad_entries = 0;

    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);

        if (!sd_storage_is_mounted()) {
            indexes_ready = false;
            publish_snapshot(false, 0, 0, bad_entries);
            continue;
        }

        uint64_t total = 0, free_bytes = 0;
        if (!sd_storage_get_capacity(&total, &free_bytes) || total == 0) {
            publish_snapshot(false, 0, 0, bad_entries);
            continue;
        }

        if ((bits & EVT_RESCAN) || !indexes_ready) {
            uint32_t pic_bad = 0, stats_bad = 0;
            photo_storage_rescan(total, &pic_bad);
            stats_store_rescan(total, &stats_bad);
            bad_entries = pic_bad + stats_bad;
            indexes_ready = true;
            ESP_LOGI(TAG, "재스캔 완료: 사진 %lluKB, 측정값 %lluKB, 손상의심 %u개",
                     (unsigned long long)(photo_storage_get_used_bytes() / 1024),
                     (unsigned long long)(stats_store_get_used_bytes() / 1024), (unsigned)bad_entries);
            if (bad_entries > 0) {
                ui_log_add_warn(UI_WARN_SD_BAD_ENTRY, "SD: %u damaged entries skipped", (unsigned)bad_entries);
            }
        }

        /* 예산 판단 — 사용량이 바뀐 직후(또는 재스캔 직후)에만 여기 옴 */
        uint64_t pic_budget = total * 9 / 10;
        uint64_t stats_budget = total / 10;
        uint64_t pic_used = photo_storage_get_used_bytes();
        uint64_t stats_used = stats_store_get_used_bytes();

        if (stats_budget > 0 && stats_used * 100 / stats_budget >= TRIM_START_PCT) {
            uint32_t deleted = stats_store_trim_to(stats_budget * TRIM_TARGET_PCT / 100);
            if (deleted > 0) {
                taskENTER_CRITICAL(&s_snap_lock);
                s_pending_stats_deleted += deleted;
                taskEXIT_CRITICAL(&s_snap_lock);
            }
        }
        if (pic_budget > 0 && pic_used * 100 / pic_budget >= TRIM_START_PCT) {
            uint32_t deleted = photo_storage_trim_to(pic_budget * TRIM_TARGET_PCT / 100);
            if (deleted > 0) {
                taskENTER_CRITICAL(&s_snap_lock);
                s_pending_pic_deleted += deleted;
                taskEXIT_CRITICAL(&s_snap_lock);
            }
        }

        /* 정리로 빈 공간이 바뀌었을 수 있어 한 번 더 읽음(FAT FSINFO — 빠름) */
        sd_storage_get_capacity(&total, &free_bytes);
        publish_snapshot(true, total, free_bytes, bad_entries);
    }
}

void storage_mgr_start(void)
{
    if (s_task) return;
    /* 스택은 PSRAM(feedback_prefer_psram_for_buffers) — 폴더 스캔/파일 I/O만 하고 LVGL은 안 건드림 */
    static StaticTask_t s_tcb;
    const uint32_t stack_size = 6144;
    StackType_t *stack = (StackType_t *)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
    if (!stack) {
        ESP_LOGE(TAG, "파일처리 태스크 스택 할당 실패 — SD 사용량 관리/정리 안 함");
        return;
    }
    /* 파일처리 등급 10(project_cntl_task_priority_scheme), UI와 무관하니 코어 1 */
    s_task = xTaskCreateStaticPinnedToCore(storage_task, "storage_mgr", stack_size / sizeof(StackType_t),
                                           NULL, 10, stack, &s_tcb, 1);
    xTaskNotify(s_task, EVT_RESCAN | EVT_CHECK, eSetBits);
}

void storage_mgr_request_rescan(void)
{
    if (s_task) xTaskNotify(s_task, EVT_RESCAN | EVT_CHECK, eSetBits);
}

void storage_mgr_notify_changed(void)
{
    if (s_task) xTaskNotify(s_task, EVT_CHECK, eSetBits);
}

void storage_mgr_get_snapshot(storage_mgr_snapshot_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_snap_lock);
    *out = s_snap;
    taskEXIT_CRITICAL(&s_snap_lock);
}

bool storage_mgr_take_cleanup(uint32_t *out_pic_deleted, uint32_t *out_stats_deleted)
{
    taskENTER_CRITICAL(&s_snap_lock);
    uint32_t p = s_pending_pic_deleted, s = s_pending_stats_deleted;
    s_pending_pic_deleted = 0;
    s_pending_stats_deleted = 0;
    taskEXIT_CRITICAL(&s_snap_lock);
    if (out_pic_deleted) *out_pic_deleted = p;
    if (out_stats_deleted) *out_stats_deleted = s;
    return (p > 0) || (s > 0);
}
