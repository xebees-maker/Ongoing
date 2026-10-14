/**
 * @file    fs_bench.c
 * @brief   2026-10-14(시험, 임시 — 할 일 AF에서 제거) SD와 내부 플래시(LittleFS) 쓰기 속도·다른 태스크 지연 측정.
 *
 * 사용자 지시: 2단계(통계 이중 보관) 설계 전에 내부 플래시 속도·수명을 자료와 보드 실측으로 확인, SD와 비교.
 * POST /api/fsbench 로 시작(기록 태스크에서 실행), GET /api/fsbench 로 결과 문자열.
 * 크기별로 "열기(ab) → 쓰기 → fflush+fsync → 닫기"를 20번, 걸린 시간 최소/평균/최대(us).
 * 그동안 코어 0·1에 우선순위 높은 감시 태스크가 1틱씩 잠들며 깨어나는 간격의 최대를 잼(다른 태스크가 멈추는지).
 */
#include "fs_bench.h"
#include "fs.h"
#include "sd_storage.h"
#include "sd_health.h"
#include "stats_store.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BENCH";

#define BENCH_REPEAT 20

static char *s_result = NULL;          /* 결과(PSRAM) */
static volatile bool s_running = false;
static volatile bool s_probe_run = false;
static volatile int64_t s_probe_max[2];

static void probe_task(void *arg)
{
    int core = (int)(intptr_t)arg;
    int64_t last = esp_timer_get_time();
    while (s_probe_run) {
        vTaskDelay(1);
        int64_t now = esp_timer_get_time();
        if (now - last > s_probe_max[core]) s_probe_max[core] = now - last;
        last = now;
    }
    vTaskDelete(NULL);
}

static int bench_one(char *out, size_t cap, const char *dir, const char *label, size_t size, uint8_t *data)
{
    char path[48];
    snprintf(path, sizeof(path), "%s/fsbench.bin", dir);
    unlink(path);
    int64_t mn = INT64_MAX, mx = 0, sum = 0;
    int ok = 0;
    for (int i = 0; i < BENCH_REPEAT; i++) {
        int64_t t0 = esp_timer_get_time();
        FILE *f = fopen(path, "ab");
        if (!f) break;
        size_t w = fwrite(data, 1, size, f);
        bool synced = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
        fclose(f);
        int64_t dt = esp_timer_get_time() - t0;
        if (w != size || !synced) break;
        ok++;
        sum += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }
    unlink(path);
    if (ok == 0) return snprintf(out, cap, "%s %6uB: failed\n", label, (unsigned)size);
    return snprintf(out, cap, "%s %6uB: min %7lld avg %7lld max %7lld us (%d ok)\n", label, (unsigned)size,
                    (long long)mn, (long long)(sum / ok), (long long)mx, ok);
}

static void bench_job(const uint8_t *arg)
{
    (void)arg;
    const size_t cap = 2048;
    if (!s_result) s_result = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    uint8_t *data = heap_caps_malloc(17 * 1000, MALLOC_CAP_SPIRAM);
    if (!s_result || !data) { heap_caps_free(data); s_running = false; return; }
    for (int i = 0; i < 17 * 1000; i++) data[i] = (uint8_t)(i * 31 + 7);
    static const size_t sizes[] = { 51, 1024, 4096, 17000 };

    s_probe_max[0] = s_probe_max[1] = 0;
    s_probe_run = true;
    xTaskCreatePinnedToCore(probe_task, "bp0", 2048, (void *)0, 22, NULL, 0);
    xTaskCreatePinnedToCore(probe_task, "bp1", 2048, (void *)1, 22, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    int64_t base0 = s_probe_max[0], base1 = s_probe_max[1];

    int n = snprintf(s_result, cap, "tick %u ms, idle probe max: core0 %lld core1 %lld us\n", (unsigned)portTICK_PERIOD_MS,
                     (long long)base0, (long long)base1);
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        s_probe_max[0] = s_probe_max[1] = 0;
        n += bench_one(s_result + n, cap - n, FS_MOUNT_POINT, "flash", sizes[k], data);
        n += snprintf(s_result + n, cap - n, "        probe max during: core0 %lld core1 %lld us\n",
                      (long long)s_probe_max[0], (long long)s_probe_max[1]);
    }
    if (sd_storage_is_mounted() && sd_health_writes_allowed()) {
        stats_store_io_suspend();   /* 측정 동안 통계 기록과 겹치지 않게(재연결과 같은 잠금) */
        for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            s_probe_max[0] = s_probe_max[1] = 0;
            n += bench_one(s_result + n, cap - n, SD_STORAGE_MOUNT_POINT, "sd   ", sizes[k], data);
            n += snprintf(s_result + n, cap - n, "        probe max during: core0 %lld core1 %lld us\n",
                          (long long)s_probe_max[0], (long long)s_probe_max[1]);
        }
        stats_store_io_resume();
    } else {
        n += snprintf(s_result + n, cap - n, "sd: skipped (not mounted or faulted)\n");
    }
    s_probe_run = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    heap_caps_free(data);
    ESP_LOGW(TAG, "fsbench done:\n%s", s_result);
    s_running = false;
}

bool fs_bench_start(void)
{
    if (s_running) return false;
    s_running = true;
    if (!stats_store_post_job(bench_job, NULL, 0)) { s_running = false; return false; }
    return true;
}

const char *fs_bench_result(void)
{
    if (s_running) return "running";
    return s_result ? s_result : "not run";
}
