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
