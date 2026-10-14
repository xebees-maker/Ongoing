/**
 * @file    flash_usage.c
 * @brief   flash_usage.h 구현 — 2026-10-15
 */
#include "flash_usage.h"
#include "fs.h"
#include "stats_store.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_littlefs.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "SYS";

#define FLASH_USAGE_PERIOD_SEC 60
#define STATS_FS_DIR           FS_MOUNT_POINT "/stats"

static flash_usage_t s_snap;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_timer = NULL;

static uint32_t dir_bytes(const char *dir)
{
    const size_t cap = 300;   /* 폴더 + 긴 파일 이름(d_name 최대 255) — 스택에 두지 않음 */
    char *path = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!path) return 0;
    DIR *d = opendir(dir);
    if (!d) { heap_caps_free(path); return 0; }
    uint32_t sum = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        snprintf(path, cap, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) sum += (uint32_t)st.st_size;
    }
    closedir(d);
    heap_caps_free(path);
    return sum;
}

static void compute(void)
{
    flash_usage_t n = { 0 };
    /* 펌웨어 크기는 실행 중에 안 바뀜 — 처음 한 번만 읽음(esp_image_get_metadata가 부를 때마다 구간 로그를 여러 줄 찍음) */
    static uint32_t s_app_used = 0, s_app_size = 0;
    if (s_app_size == 0) {
        const esp_partition_t *app = esp_ota_get_running_partition();
        if (app) {
            s_app_size = app->size;
            esp_partition_pos_t pos = { .offset = app->address, .size = app->size };
            esp_image_metadata_t *md = heap_caps_malloc(sizeof(*md), MALLOC_CAP_SPIRAM);   /* 스택에 두지 않음 */
            if (md && esp_image_get_metadata(&pos, md) == ESP_OK) s_app_used = md->image_len;
            heap_caps_free(md);
        }
    }
    n.app_size = s_app_size;
    n.app_used = s_app_used;
    const esp_partition_t *fs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, FS_PARTITION_LABEL);
    if (fs) n.fs_size = fs->size;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(FS_PARTITION_LABEL, &total, &used) == ESP_OK) n.fs_used = (uint32_t)used;
    n.fs_stats = dir_bytes(STATS_FS_DIR);
    nvs_stats_t ns;
    if (nvs_get_stats(NULL, &ns) == ESP_OK) { n.nvs_used = (uint32_t)ns.used_entries; n.nvs_total = (uint32_t)ns.total_entries; }
    const esp_partition_t *phy = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY, NULL);
    if (phy) n.phy_size = phy->size;
    n.valid = true;
    taskENTER_CRITICAL(&s_lock);
    n.seq = s_snap.seq + 1;
    s_snap = n;
    taskEXIT_CRITICAL(&s_lock);
}

static void job(const uint8_t *arg)
{
    (void)arg;
    compute();
}

/* 타이머 문맥에선 파일을 안 건드리고 기록 태스크로 넘김 */
static void timer_cb(void *arg)
{
    (void)arg;
    stats_store_post_job(job, NULL, 0);
}

void flash_usage_init(void)
{
    compute();
    ESP_LOGI(TAG, "Flash usage: app %u/%u, littlefs %u/%u (stats %u), nvs %u/%u entries, phy %u",
             (unsigned)s_snap.app_used, (unsigned)s_snap.app_size, (unsigned)s_snap.fs_used, (unsigned)s_snap.fs_size,
             (unsigned)s_snap.fs_stats, (unsigned)s_snap.nvs_used, (unsigned)s_snap.nvs_total, (unsigned)s_snap.phy_size);
    const esp_timer_create_args_t ta = { .callback = timer_cb, .name = "flash_usage" };
    if (esp_timer_create(&ta, &s_timer) == ESP_OK) esp_timer_start_periodic(s_timer, (uint64_t)FLASH_USAGE_PERIOD_SEC * 1000000ULL);
}

void flash_usage_get(flash_usage_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_snap;
    taskEXIT_CRITICAL(&s_lock);
}
