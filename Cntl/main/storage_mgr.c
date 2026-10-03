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

static const char *TAG = "SYS";

#define EVT_RESCAN  (1u << 0)
#define EVT_CHECK   (1u << 1)

/* 예산(사용자 설계 2026-09-10) — 전체 용량의 사진 9 : 측정값 1, 각자 자기 예산의 90% 이상이면
 * 80%까지 오래된 것부터 정리 */
#define TRIM_START_PCT  90
#define TRIM_TARGET_PCT 80

static TaskHandle_t s_task = NULL;
static portMUX_TYPE s_snap_lock = portMUX_INITIALIZER_UNLOCKED;
static storage_mgr_snapshot_t s_snap;
static uint32_t s_pending_deleted[STORAGE_AREA_COUNT];
/* 2026-10-03(할 일 AD — 웹 주화면) — 웹은 콘 화면처럼 "가져가며 지우기"를 못 함(콘 화면이 먼저 가져감). 정리 때마다 번호를
 * 올리고 마지막 정리의 영역별 개수를 남겨 둠 — 웹은 번호가 바뀌면 안내를 띄움 */
static uint32_t s_cleanup_seq = 0;
static uint32_t s_last_deleted[STORAGE_AREA_COUNT];

/* 2026-10-01(사용자 설계 — 할 일 T "관리 폴더 표") — 영역별 예산 비율과 그 영역의 사용량/정리/재스캔 함수. 예산 판단·정리·
 * 재스캔·화면 표시가 모두 이 표를 따라 돎(예전엔 사진 9 : 측정 1이 여기와 ui_main.c에 따로 적혀 있었음). 비율 합 = 100 */
typedef struct {
    uint8_t  budget_pct;
    uint64_t (*used_bytes)(void);
    uint32_t (*trim_to)(uint64_t target_bytes);
    void     (*rescan)(uint64_t sd_total, uint32_t *out_bad_entries);
} area_def_t;

static const area_def_t s_areas[STORAGE_AREA_COUNT] = {
    [STORAGE_AREA_PICTURE] = { 90, photo_storage_get_used_bytes, photo_storage_trim_to, photo_storage_rescan },
    [STORAGE_AREA_MEASURE] = { 10, stats_store_get_used_bytes,   stats_store_trim_to,   stats_store_rescan   },
};

static void publish_snapshot(bool valid, uint64_t total, uint64_t free_bytes, uint32_t bad_entries)
{
    storage_mgr_snapshot_t snap = { .valid = valid, .sd_total = total, .sd_free = free_bytes, .bad_entries = bad_entries };
    for (int a = 0; a < STORAGE_AREA_COUNT; a++) {
        storage_area_usage_t *u = &snap.area[a];
        u->used = s_areas[a].used_bytes();
        u->budget = total * s_areas[a].budget_pct / 100;
        u->remain = (u->used < u->budget) ? (u->budget - u->used) : 0;
        if (u->remain > free_bytes) u->remain = free_bytes;  /* 영역 밖 파일이 빈 공간을 먹었을 수 있음 */
        snap.total.used += u->used;
        snap.total.budget += u->budget;
        snap.total.remain += u->remain;
    }
    if (snap.total.remain > free_bytes) snap.total.remain = free_bytes;
    taskENTER_CRITICAL(&s_snap_lock);
    s_snap = snap;
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
            bad_entries = 0;
            for (int a = 0; a < STORAGE_AREA_COUNT; a++) {
                uint32_t bad = 0;
                s_areas[a].rescan(total, &bad);
                bad_entries += bad;
            }
            indexes_ready = true;
            ESP_LOGI(TAG, "Rescan done: photos %lluKB, measurements %lluKB, suspect %u",
                     (unsigned long long)(s_areas[STORAGE_AREA_PICTURE].used_bytes() / 1024),
                     (unsigned long long)(s_areas[STORAGE_AREA_MEASURE].used_bytes() / 1024), (unsigned)bad_entries);
            if (bad_entries > 0) {
                ui_log_add_warn(UI_WARN_SD_BAD_ENTRY, "SD: %u damaged entries skipped", (unsigned)bad_entries);
            }
        }

        /* 예산 판단 — 사용량이 바뀐 직후(또는 재스캔 직후)에만 여기 옴. 영역마다 자기 예산의 90% 이상이면 80%까지 정리 */
        for (int a = 0; a < STORAGE_AREA_COUNT; a++) {
            uint64_t budget = total * s_areas[a].budget_pct / 100;
            if (budget == 0 || s_areas[a].used_bytes() * 100 / budget < TRIM_START_PCT) continue;
            uint32_t deleted = s_areas[a].trim_to(budget * TRIM_TARGET_PCT / 100);
            if (deleted > 0) {
                taskENTER_CRITICAL(&s_snap_lock);
                s_pending_deleted[a] += deleted;
                for (int k = 0; k < STORAGE_AREA_COUNT; k++) s_last_deleted[k] = (k == a) ? deleted : 0;
                s_cleanup_seq++;
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
        ESP_LOGE(TAG, "File task stack alloc failed - SD usage management/trim disabled");
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

uint32_t storage_mgr_get_cleanup_event(uint32_t out_last_deleted[STORAGE_AREA_COUNT])
{
    taskENTER_CRITICAL(&s_snap_lock);
    uint32_t seq = s_cleanup_seq;
    for (int a = 0; a < STORAGE_AREA_COUNT; a++) out_last_deleted[a] = s_last_deleted[a];
    taskEXIT_CRITICAL(&s_snap_lock);
    return seq;
}

bool storage_mgr_take_cleanup(uint32_t out_deleted[STORAGE_AREA_COUNT])
{
    bool any = false;
    taskENTER_CRITICAL(&s_snap_lock);
    for (int a = 0; a < STORAGE_AREA_COUNT; a++) {
        out_deleted[a] = s_pending_deleted[a];
        s_pending_deleted[a] = 0;
        if (out_deleted[a] > 0) any = true;
    }
    taskEXIT_CRITICAL(&s_snap_lock);
    return any;
}
