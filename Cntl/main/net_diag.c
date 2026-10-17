/**
 * @file    net_diag.c
 * @brief   net_diag.h 구현 — 2026-10-17(진단, 임시)
 */
#include "net_diag.h"
#include "stats_store.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/tcpip.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/ip_addr.h"
#include "sdkconfig.h"
#include "freertos/task.h"
#include "freertos/freertos_debug.h"
#include "esp_memory_utils.h"
#if CONFIG_HEAP_TASK_TRACKING
#include "esp_heap_task_info.h"
#endif

static const char *TAG = "NETDIAG";

static const char *const s_state[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTAB", "FIN_W1", "FIN_W2",
                                       "CLOSE_W", "CLOSING", "LAST_ACK", "TIME_W" };

/* 한 번 읽는 데 쓰는 작업 공간 — PSRAM(스택에 두지 않음) */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    SemaphoreHandle_t done;
    char ip[48];
    multi_heap_info_t hi;
} dump_ctx_t;

static void put(dump_ctx_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(dump_ctx_t *c, const char *fmt, ...)
{
    if (c->len + 1 >= c->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(c->buf + c->len, c->cap - c->len, fmt, ap);
    va_end(ap);
    if (n > 0) c->len += ((size_t)n < c->cap - c->len) ? (size_t)n : c->cap - c->len - 1;
}

static uint32_t seg_bytes(const struct tcp_seg *s)
{
    uint32_t n = 0;
    for (; s; s = s->next) n += s->len;
    return n;
}

static void dump_list(dump_ctx_t *c, const char *name, struct tcp_pcb *list)
{
    for (struct tcp_pcb *p = list; p; p = p->next) {
        ipaddr_ntoa_r(&p->remote_ip, c->ip, sizeof(c->ip));
        put(c, "tcp %s %s :%u <- %s:%u unsent %u unacked %u qlen %u sndbuf %u peerwnd %u nrtx %u rto %ums persist %u idle %us\n",
            name, p->state < sizeof(s_state) / sizeof(s_state[0]) ? s_state[p->state] : "?", (unsigned)p->local_port, c->ip,
            (unsigned)p->remote_port, (unsigned)seg_bytes(p->unsent), (unsigned)seg_bytes(p->unacked), (unsigned)p->snd_queuelen,
            (unsigned)p->snd_buf, (unsigned)p->snd_wnd, (unsigned)p->nrtx, (unsigned)(p->rto * TCP_SLOW_INTERVAL),
            (unsigned)p->persist_backoff, (unsigned)((tcp_ticks - p->tmr) * TCP_SLOW_INTERVAL / 1000));
    }
}

/* lwIP 스레드에서 — 연결 목록은 이 스레드만 바꾸므로 여기서 읽음 */
static void dump_in_tcpip(void *arg)
{
    dump_ctx_t *c = arg;
    dump_list(c, "active", tcp_active_pcbs);
    dump_list(c, "tw", tcp_tw_pcbs);
    xSemaphoreGive(c->done);
}

/* 2026-10-17(측정용) — 내부 RAM 사용 블록 분포. 1K 이상은 하나씩, 작은 것은 구간별 개수·합계.
 * 각 태스크의 스택·TCB가 어느 블록에 있는지 맞춰서 이름을 붙임 */
#define WALK_BIG_MAX 256
#define WALK_TASK_MAX 48
typedef struct { uintptr_t p; uint32_t sz; } wblk_t;
typedef struct {
    wblk_t big[WALK_BIG_MAX];
    uint32_t nbig, big_over;
    uint32_t cnt[3], sum[3];   /* <64, <256, <1024 */
    uint32_t used_n, used_sum;
    TaskSnapshot_t snap[WALK_TASK_MAX];
    uintptr_t stk[WALK_TASK_MAX];
    char name[WALK_TASK_MAX][configMAX_TASK_NAME_LEN];
    UBaseType_t ntask;
} walk_ctx_t;

static bool walk_cb(walker_heap_into_t hi, walker_block_info_t bi, void *ud)
{
    (void)hi;
    walk_ctx_t *w = ud;
    if (!bi.used) return true;
    w->used_n++;
    w->used_sum += bi.size;
    if (bi.size >= 1024) {
        if (w->nbig < WALK_BIG_MAX) w->big[w->nbig++] = (wblk_t){ (uintptr_t)bi.ptr, (uint32_t)bi.size };
        else w->big_over++;
    } else {
        int k = bi.size < 64 ? 0 : bi.size < 256 ? 1 : 2;
        w->cnt[k]++;
        w->sum[k] += bi.size;
    }
    return true;
}

static void dump_blocks(dump_ctx_t *c)
{
    walk_ctx_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM);
    if (!w) return;
    vTaskSuspendAll();
    w->ntask = uxTaskGetSnapshotAll(w->snap, WALK_TASK_MAX, NULL);
    xTaskResumeAll();
    for (UBaseType_t i = 0; i < w->ntask; i++) {
        TaskHandle_t h = (TaskHandle_t)w->snap[i].pxTCB;
        strlcpy(w->name[i], pcTaskGetName(h), sizeof(w->name[i]));
        w->stk[i] = (uintptr_t)xTaskGetStackStart(h);
    }
    heap_caps_walk(MALLOC_CAP_INTERNAL, walk_cb, w);

    for (uint32_t i = 1; i < w->nbig; i++) {   /* 큰 것부터 */
        wblk_t t = w->big[i];
        uint32_t j = i;
        for (; j > 0 && w->big[j - 1].sz < t.sz; j--) w->big[j] = w->big[j - 1];
        w->big[j] = t;
    }
    put(c, "internal used blocks %u sum %u | <64: %u/%u  <256: %u/%u  <1K: %u/%u (count/bytes)\n", (unsigned)w->used_n,
        (unsigned)w->used_sum, (unsigned)w->cnt[0], (unsigned)w->sum[0], (unsigned)w->cnt[1], (unsigned)w->sum[1],
        (unsigned)w->cnt[2], (unsigned)w->sum[2]);
    uint32_t stk_sum = 0, big_sum = 0;
    put(c, "internal blocks >=1K (%u%s):\n", (unsigned)w->nbig, w->big_over ? " +more" : "");
    for (uint32_t i = 0; i < w->nbig; i++) {
        const char *lab = "";
        const char *kind = "";
        for (UBaseType_t t = 0; t < w->ntask; t++) {
            if (w->stk[t] >= w->big[i].p && w->stk[t] < w->big[i].p + w->big[i].sz) { lab = w->name[t]; kind = "stack "; break; }
            uintptr_t tcb = (uintptr_t)w->snap[t].pxTCB;
            if (tcb >= w->big[i].p && tcb < w->big[i].p + w->big[i].sz) { lab = w->name[t]; kind = "tcb "; }
        }
        if (kind[0] == 's') stk_sum += w->big[i].sz;
        big_sum += w->big[i].sz;
        put(c, "  %08x %6u %s%s\n", (unsigned)w->big[i].p, (unsigned)w->big[i].sz, kind, lab);
    }
    put(c, "  big sum %u, of which task stacks %u\n", (unsigned)big_sum, (unsigned)stk_sum);
    put(c, "task stacks not in internal heap:");
    for (UBaseType_t t = 0; t < w->ntask; t++) {
        bool in_heap = false;
        for (uint32_t i = 0; i < w->nbig && !in_heap; i++)
            in_heap = w->stk[t] >= w->big[i].p && w->stk[t] < w->big[i].p + w->big[i].sz;
        if (!in_heap) put(c, " %s(%s)", w->name[t], esp_ptr_external_ram((void *)w->stk[t]) ? "psram" : "static");
    }
    put(c, "\n");
    heap_caps_free(w);
}

size_t net_diag_dump(char *buf, size_t cap)
{
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    dump_ctx_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM);
    if (!c) return 0;
    c->buf = buf;
    c->cap = cap;
    c->done = xSemaphoreCreateBinary();
    heap_caps_get_info(&c->hi, MALLOC_CAP_INTERNAL);
    put(c, "heap internal free %u largest %u min_ever %u blocks %u\n", (unsigned)c->hi.total_free_bytes,
        (unsigned)c->hi.largest_free_block, (unsigned)c->hi.minimum_free_bytes, (unsigned)c->hi.allocated_blocks);
    if (c->done && tcpip_callback(dump_in_tcpip, c) == ERR_OK) {
        if (xSemaphoreTake(c->done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            xSemaphoreTake(c->done, portMAX_DELAY);   /* c를 lwIP가 쓰는 중 — 돌아올 때까지는 풀 수 없음 */
            put(c, "(tcpip slow)\n");
        }
    }
#if CONFIG_HEAP_TASK_TRACKING
    /* 2026-10-17(측정용) — 태스크별 내부 RAM 사용(현재·최대). 요약 출력(heap_caps_print_all_task_stat_overview)은 내부·PSRAM을
     * 합쳐서 보여 줘서, 구조체로 받아 내부 RAM 힙 몫만 더함. 배열은 PSRAM, 할당 목록은 받지 않음(alloc_count 0) */
    {
        const size_t NT = 48, NH = 48 * 6;
        heap_all_tasks_stat_t *st = heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_SPIRAM);
        if (st) {
            st->task_count = NT;
            st->stat_arr = heap_caps_calloc(NT, sizeof(task_stat_t), MALLOC_CAP_SPIRAM);
            st->heap_count = NH;
            st->heap_stat_start = heap_caps_calloc(NH, sizeof(heap_stat_t), MALLOC_CAP_SPIRAM);
            if (st->stat_arr && st->heap_stat_start && heap_caps_get_all_task_stat(st) == ESP_OK) {
                size_t tot = 0, totp = 0;
                put(c, "task internal RAM (current / peak):\n");
                for (size_t i = 0; i < NT; i++) {
                    task_stat_t *t = &st->stat_arr[i];
                    if (!t->handle && !t->name[0]) continue;
                    size_t cur = 0, peak = 0;
                    for (size_t h = 0; t->heap_stat && h < t->heap_count; h++) {
                        const heap_stat_t *hs = &t->heap_stat[h];
                        if ((hs->caps & MALLOC_CAP_INTERNAL) && !(hs->caps & MALLOC_CAP_SPIRAM)) { cur += hs->current_usage; peak += hs->peak_usage; }
                    }
                    if (!cur && !peak) continue;
                    tot += cur; totp += peak;
                    put(c, "  %-16s %7u / %7u\n", t->name, (unsigned)cur, (unsigned)peak);
                }
                put(c, "  %-16s %7u / %7u\n", "TOTAL", (unsigned)tot, (unsigned)totp);
            }
            heap_caps_free(st->stat_arr);
            heap_caps_free(st->heap_stat_start);
            heap_caps_free(st);
        }
    }
#endif
    dump_blocks(c);
    size_t n = c->len;
    if (c->done) vSemaphoreDelete(c->done);
    heap_caps_free(c);
    return n;
}

/* 기록 태스크에서(화면 태스크를 막지 않게) */
static void log_job(const uint8_t *arg)
{
    const char *why;
    memcpy(&why, arg, sizeof(why));   /* 부르는 쪽이 넘긴 문자열 상수의 주소 */
    const size_t cap = 2048;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    size_t n = net_diag_dump(buf, cap);
    ESP_LOGW(TAG, "%s", why);
    for (char *line = buf, *end; line < buf + n && *line; line = end + 1) {
        end = strchr(line, '\n');
        if (!end) end = buf + n;
        *end = '\0';
        ESP_LOGW(TAG, "%s", line);
    }
    heap_caps_free(buf);
}

void net_diag_log(const char *why)
{
    if (!why) why = "dump";
    stats_store_post_job(log_job, &why, sizeof(why));   /* 문자열 상수만 넘길 것(주소를 넘김) */
}
