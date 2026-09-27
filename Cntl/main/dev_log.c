#include "dev_log.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "fs.h"

static const char *TAG = "SYS";

const char *const DEV_LOG_TAGS[DEV_LOG_TAG_COUNT] = {
    "All", "CAN", "LINK", "CASK", "PHOTO", "CAM", "SENS", "RELAY", "UI", "SYS", "Other",
};

#define DEV_LOG_CFG_PATH  FS_MOUNT_POINT "/devlog.cfg"
#define DEV_LOG_ENTRIES   400     /* 줄 수 — 400 × 약 140B ≈ 56KB(PSRAM) */
#define DEV_LOG_TEXT_LEN  120
#define DEV_LOG_TAG_LEN   8

typedef struct {
    char    ts[10];               /* "HH:MM:SS" 또는 "+MMM:SS" */
    char    lvl;                  /* 'E' 'W' 'I' 'D' 'V', 형식 밖 줄은 '?' */
    char    tag[DEV_LOG_TAG_LEN];
    char    text[DEV_LOG_TEXT_LEN];
} dev_log_entry_t;

static dev_log_entry_t *s_ring = NULL;
static uint32_t s_head = 0;       /* 다음에 쓸 자리(누적 번호 — % DEV_LOG_ENTRIES) */
static volatile uint32_t s_seq = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf = NULL;

static uint8_t s_save_level = DEV_LOG_LVL_I;
static uint8_t s_view_mask  = DEV_LOG_MASK(DEV_LOG_LVL_E) | DEV_LOG_MASK(DEV_LOG_LVL_W) | DEV_LOG_MASK(DEV_LOG_LVL_I) |
                              DEV_LOG_MASK(DEV_LOG_LVL_D);
static uint8_t s_tag_filter = DEV_LOG_TAG_ALL;

static int lvl_of_char(char c)
{
    switch (c) {
        case 'E': return DEV_LOG_LVL_E;
        case 'W': return DEV_LOG_LVL_W;
        case 'I': return DEV_LOG_LVL_I;
        case 'D': return DEV_LOG_LVL_D;
        case 'V': return 5;
        default:  return 0;
    }
}

static void make_ts(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    if (tm_buf.tm_year + 1900 >= 2025) {
        snprintf(out, cap, "%02d:%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    } else {
        /* RTC가 아직 안 맞았으면 부팅 후 경과 */
        uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
        snprintf(out, cap, "+%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
    }
}

/* ESP_LOG v1 형식: "L (tick) TAG: 내용\n" — 한 줄이 vprintf 한 번으로 옴. 로그를 찍은 태스크 안에서 돌므로 짧게:
 * 스택 버퍼에 포맷 → 파싱 → 링에 복사(잠금은 복사하는 동안만). 화면 갱신은 UI 타이머가 따로 함 */
static int dev_log_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int ret = s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);

    char line[176];
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n <= 0 || !s_ring) return ret;
    size_t len = strnlen(line, sizeof(line));
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    if (len == 0) return ret;

    dev_log_entry_t e;
    make_ts(e.ts, sizeof(e.ts));
    const char *tag_p = NULL, *msg_p = line;
    size_t tag_len = 0;
    if (len > 4 && lvl_of_char(line[0]) && line[1] == ' ' && line[2] == '(') {
        const char *rp = strstr(line, ") ");
        const char *cp = rp ? strstr(rp + 2, ": ") : NULL;
        if (rp && cp) {
            tag_p = rp + 2;
            tag_len = (size_t)(cp - tag_p);
            msg_p = cp + 2;
        }
    }
    e.lvl = tag_p ? line[0] : '?';
    if (tag_len >= DEV_LOG_TAG_LEN) tag_len = DEV_LOG_TAG_LEN - 1;
    if (tag_p) memcpy(e.tag, tag_p, tag_len);
    e.tag[tag_len] = '\0';
    strncpy(e.text, msg_p, DEV_LOG_TEXT_LEN - 1);
    e.text[DEV_LOG_TEXT_LEN - 1] = '\0';

    taskENTER_CRITICAL(&s_lock);
    s_ring[s_head % DEV_LOG_ENTRIES] = e;
    s_head++;
    s_seq++;
    taskEXIT_CRITICAL(&s_lock);
    return ret;
}

/* 우리 기능 태그는 저장 문턱대로, 그 밖(ESP-IDF 내부 wifi 등)은 I를 넘지 않게 — 내부 D 로그 폭주 방지 */
static void apply_levels(void)
{
    esp_log_level_t lvl = (esp_log_level_t)s_save_level;
    esp_log_level_set("*", lvl > ESP_LOG_INFO ? ESP_LOG_INFO : lvl);
    for (int i = 1; i < DEV_LOG_TAG_OTHER; i++) esp_log_level_set(DEV_LOG_TAGS[i], lvl);
}

static void save_cfg(void)
{
    FILE *f = fopen(DEV_LOG_CFG_PATH, "w");
    if (!f) {
        ESP_LOGW(TAG, "devlog.cfg save failed");
        return;
    }
    fprintf(f, "save=%u\nview=%u\ntag=%u\n", (unsigned)s_save_level, (unsigned)s_view_mask, (unsigned)s_tag_filter);
    fclose(f);
}

static void load_cfg(void)
{
    FILE *f = fopen(DEV_LOG_CFG_PATH, "r");
    if (!f) return;  /* 처음 — 기본값(저장 I, 보기 E/W/I/D, 전체) */
    char buf[32];
    unsigned v;
    while (fgets(buf, sizeof(buf), f)) {
        if (sscanf(buf, "save=%u", &v) == 1 && v >= DEV_LOG_LVL_E && v <= DEV_LOG_LVL_D) s_save_level = (uint8_t)v;
        else if (sscanf(buf, "view=%u", &v) == 1) s_view_mask = (uint8_t)v;
        else if (sscanf(buf, "tag=%u", &v) == 1 && v < DEV_LOG_TAG_COUNT) s_tag_filter = (uint8_t)v;
    }
    fclose(f);
}

void dev_log_init(void)
{
    if (s_ring) return;
    s_ring = heap_caps_calloc(DEV_LOG_ENTRIES, sizeof(dev_log_entry_t), MALLOC_CAP_SPIRAM);
    load_cfg();
    apply_levels();
    if (!s_ring) {
        ESP_LOGE(TAG, "Dev log buffer alloc failed - dev log disabled");
        return;
    }
    s_orig_vprintf = esp_log_set_vprintf(dev_log_vprintf);
}

uint8_t dev_log_get_save_level(void) { return s_save_level; }
uint8_t dev_log_get_view_mask(void)  { return s_view_mask; }
uint8_t dev_log_get_tag_filter(void) { return s_tag_filter; }
uint32_t dev_log_seq(void)           { return s_seq; }

void dev_log_set_save_level(uint8_t lvl)
{
    if (lvl < DEV_LOG_LVL_E || lvl > DEV_LOG_LVL_D || lvl == s_save_level) return;
    s_save_level = lvl;
    apply_levels();
    save_cfg();
}

void dev_log_set_view_mask(uint8_t mask)
{
    if (mask == s_view_mask) return;
    s_view_mask = mask;
    s_seq++;  /* 화면 다시 거르게 */
    save_cfg();
}

void dev_log_set_tag_filter(uint8_t idx)
{
    if (idx >= DEV_LOG_TAG_COUNT || idx == s_tag_filter) return;
    s_tag_filter = idx;
    s_seq++;
    save_cfg();
}

static bool tag_match(const char *tag)
{
    if (s_tag_filter == DEV_LOG_TAG_ALL) return true;
    if (s_tag_filter == DEV_LOG_TAG_OTHER) {
        for (int i = 1; i < DEV_LOG_TAG_OTHER; i++) if (strcmp(tag, DEV_LOG_TAGS[i]) == 0) return false;
        return true;
    }
    return strcmp(tag, DEV_LOG_TAGS[s_tag_filter]) == 0;
}

size_t dev_log_render(char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return 0;
    out[0] = '\0';
    if (!s_ring) return 0;

    /* 최신 쪽부터 거꾸로 훑어 들어갈 만큼만 고른 뒤, 오래된 것부터 씀. 링을 잠그는 동안엔 복사만 */
    static dev_log_entry_t *snap = NULL;
    if (!snap) snap = heap_caps_malloc(DEV_LOG_ENTRIES * sizeof(dev_log_entry_t), MALLOC_CAP_SPIRAM);
    if (!snap) return 0;
    /* 한 줄씩 잠그고 복사 — 400줄을 한 번에 잠그면 인터럽트가 너무 오래 막힘(CAN). 복사 도중 새 줄이 들어와
     * 가장 오래된 몇 줄이 바뀌어도 보기용이라 무방 */
    taskENTER_CRITICAL(&s_lock);
    uint32_t head = s_head;
    taskEXIT_CRITICAL(&s_lock);
    uint32_t count = head < DEV_LOG_ENTRIES ? head : DEV_LOG_ENTRIES;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t idx = (head - count + k) % DEV_LOG_ENTRIES;
        taskENTER_CRITICAL(&s_lock);
        snap[k] = s_ring[idx];
        taskEXIT_CRITICAL(&s_lock);
    }

    size_t total = 0;
    int32_t first = (int32_t)count;
    for (int32_t k = (int32_t)count - 1; k >= 0; k--) {
        const dev_log_entry_t *e = &snap[k];
        int lvl = lvl_of_char(e->lvl);
        if (lvl && lvl <= DEV_LOG_LVL_D && !(s_view_mask & DEV_LOG_MASK(lvl))) continue;
        if (!tag_match(e->tag)) continue;
        size_t tl = strlen(e->tag);
        size_t need = strlen(e->ts) + 3 + (tl < 5 ? 5 : tl) + 1 + strlen(e->text) + 1;
        if (total + need + 1 > out_cap) break;
        total += need;
        first = k;
    }
    size_t pos = 0;
    for (int32_t k = first; k < (int32_t)count; k++) {
        const dev_log_entry_t *e = &snap[k];
        int lvl = lvl_of_char(e->lvl);
        if (lvl && lvl <= DEV_LOG_LVL_D && !(s_view_mask & DEV_LOG_MASK(lvl))) continue;
        if (!tag_match(e->tag)) continue;
        int w = snprintf(out + pos, out_cap - pos, "%s %c %-5s %s\n", e->ts, e->lvl, e->tag, e->text);
        if (w <= 0 || (size_t)w >= out_cap - pos) break;
        pos += (size_t)w;
    }
    if (pos > 0 && out[pos - 1] == '\n') out[--pos] = '\0';
    return pos;
}
