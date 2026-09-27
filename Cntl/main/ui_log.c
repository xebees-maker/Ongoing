#include "ui_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <time.h>
#include "esp_timer.h"

/* 부팅 시 한 번만 잡고 계속 재사용하는 고정 버퍼(다른 모듈들과 동일 원칙) — 꽉 차면
 * 오래된 앞부분을 memmove로 밀어내고 뒤에 이어붙임(단순 append 버퍼, 진짜 링버퍼는
 * 아님 — 화면에 그대로 이어붙여 보여주기엔 이 편이 더 단순함).
 * 2026-08-21 — 내부(비-PSRAM) DRAM이 httpd_start 실패(5005)를 겪을 만큼 빠듯했던 걸 실기로
 * 확인(free internal=1111~1419B) — 텍스트 버퍼라 빠른 접근이 필수가 아니어서 PSRAM으로
 * 옮김(6.1KB, 이 파일 하나가 내부RAM 사용량 2위였음) */
#define UI_LOG_BUF_CAP 6144
static char *s_buf = NULL;
static size_t s_len = 0;
static SemaphoreHandle_t s_mutex;

#define UI_LOG_ERR_CAP 128
static char s_last_err[UI_LOG_ERR_CAP];
static bool s_err_pending = false;

/* 확인 안 한 에러 코드 누적 목록 — 로고 탭하면 이걸 전부 팝업으로 보여줌(2026-08-01,
 * 사용자 지시). 같은 코드가 반복 발생해도 한 번만 남김(안 그러면 계속 도돌이표로 찍히는
 * 코드 하나가 목록을 다 채워버림). 용량은 ui_log.h의 UI_ERR_HISTORY_CAP(공개 상수 —
 * 호출부가 ui_log_get_error_history()에 넘길 배열 크기를 알아야 해서 헤더로 옮김) */
static int s_err_history[UI_ERR_HISTORY_CAP];
static int s_err_history_count = 0;

void ui_log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_buf = heap_caps_malloc(UI_LOG_BUF_CAP + 1, MALLOC_CAP_SPIRAM);
}

/* 내부용 — 호출부가 이미 뮤텍스를 잡고 있어야 함 */
static void append_locked(const char *line, size_t add_len)
{
    if (s_len + add_len > UI_LOG_BUF_CAP) {
        size_t drop = (s_len + add_len) - UI_LOG_BUF_CAP + (UI_LOG_BUF_CAP / 4);
        if (drop > s_len) drop = s_len;
        memmove(s_buf, s_buf + drop, s_len - drop);
        s_len -= drop;
    }
    memcpy(s_buf + s_len, line, add_len);
    s_len += add_len;
    s_buf[s_len] = '\0';
}

/* 2026-08-22, 사용자 지시 — 일반/전력 로그 포맷 통일용 공용 타임스탬프 함수. lv_tick_get()
 * 기반이라 ui_log_add류(폰트/화면 초기화 이전에도 호출될 수 있음)에서 써도 안전함 —
 * LVGL 틱 카운터 자체는 화면 위젯 존재 여부와 무관하게 계속 흐름 */
void ui_log_format_timestamp(char *buf, size_t cap)
{
    /* 2026-09-27(로그 정리, 사용자 확정 형식 "시각 레벨 내용") — RTC 벽시계 HH:MM:SS, RTC가 아직 안 맞았으면
     * 부팅 후 경과(+MM:SS). 개발 로그(dev_log.c make_ts)와 같은 규칙 */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    if (tm_buf.tm_year + 1900 >= 2025) {
        snprintf(buf, cap, "%02d:%02d:%02d ", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    } else {
        uint32_t total_sec = (uint32_t)(esp_timer_get_time() / 1000000);
        snprintf(buf, cap, "+%02lu:%02lu ", (unsigned long)(total_sec / 60), (unsigned long)(total_sec % 60));
    }
}

void ui_log_add(const char *fmt, ...)
{
    if (!s_mutex || !s_buf) return;

    char ts[16];
    ui_log_format_timestamp(ts, sizeof(ts));

    char msg[144];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (n <= 0) return;

    char line[160];
    int total = snprintf(line, sizeof(line), "%sI %s", ts, msg);
    if (total <= 0) return;

    size_t add_len = ((size_t)total < sizeof(line) - 1) ? (size_t)total : sizeof(line) - 2;
    line[add_len] = '\n';
    add_len++;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    append_locked(line, add_len);
    xSemaphoreGive(s_mutex);
}

/* 호출부가 이미 뮤텍스를 잡고 있어야 함 — 같은 코드가 이미 있으면 무시(중복 방지) */
static void push_history_locked(int code)
{
    for (int i = 0; i < s_err_history_count; i++) {
        if (s_err_history[i] == code) return;
    }
    if (s_err_history_count < UI_ERR_HISTORY_CAP) {
        s_err_history[s_err_history_count++] = code;
    }
}

void ui_log_add_err(int code, const char *fmt, ...)
{
    if (!s_mutex || !s_buf) return;

    char msg[144];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (n <= 0) return;

    char ts[16];
    ui_log_format_timestamp(ts, sizeof(ts));

    char line[160];
    int total = snprintf(line, sizeof(line), "%sE [%04d] %s", ts, code, msg);
    if (total <= 0) return;
    size_t add_len = ((size_t)total < sizeof(line) - 1) ? (size_t)total : sizeof(line) - 2;
    line[add_len] = '\0';

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_last_err, line, UI_LOG_ERR_CAP - 1);
    s_last_err[UI_LOG_ERR_CAP - 1] = '\0';
    s_err_pending = true;
    push_history_locked(code);

    line[add_len] = '\n';
    append_locked(line, add_len + 1);
    xSemaphoreGive(s_mutex);
}

bool ui_log_get_pending_error(char *out, size_t out_cap)
{
    if (!s_mutex || out_cap == 0) return false;

    bool had = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_err_pending) {
        strncpy(out, s_last_err, out_cap - 1);
        out[out_cap - 1] = '\0';
        s_err_pending = false;
        had = true;
    }
    xSemaphoreGive(s_mutex);
    return had;
}

int ui_log_get_error_history(int *out_codes, int max)
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = (s_err_history_count < max) ? s_err_history_count : max;
    for (int i = 0; i < n; i++) out_codes[i] = s_err_history[i];
    xSemaphoreGive(s_mutex);
    return n;
}

/* 2026-09-11 — 에러목록 팝업의 행별 "지우기" 버튼용. 찾으면 그 자리를 뒤 항목들로 당겨서
 * 채움(순서는 중요하지 않음, 단순 목록) */
void ui_log_clear_one_error(int code)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_err_history_count; i++) {
        if (s_err_history[i] == code) {
            for (int j = i; j < s_err_history_count - 1; j++) {
                s_err_history[j] = s_err_history[j + 1];
            }
            s_err_history_count--;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}

/* 2026-08-11 — 워닝 레벨. 에러(위)와 완전히 별개 상태를 가지는 이유는 ui_log.h 주석
 * 참고(확인하면 지워짐 vs 안 지워짐) — 그 차이 자체가 두 이력을 하나로 합쳐서 플래그로
 * 구분하는 것보다 완전히 분리된 구조가 더 단순함 */
#define UI_LOG_WARN_CAP 128
static char s_last_warn[UI_LOG_WARN_CAP];
static bool s_warn_pending = false;
static int  s_warn_history[UI_WARN_HISTORY_CAP];
static int  s_warn_history_count = 0;

/* 워닝 설명 문구는 화면 표시용이라 ui_main.c의 warn_code_to_desc_str()(ui_strings)에 둠 —
 * 여기에 한글 문자열을 두면 비트맵 폰트에서 깨짐 */

static void push_warn_history_locked(int code)
{
    for (int i = 0; i < s_warn_history_count; i++) {
        if (s_warn_history[i] == code) return;
    }
    if (s_warn_history_count < UI_WARN_HISTORY_CAP) {
        s_warn_history[s_warn_history_count++] = code;
    }
}

void ui_log_add_warn(int code, const char *fmt, ...)
{
    if (!s_mutex || !s_buf) return;

    char msg[144];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (n <= 0) return;

    char ts[16];
    ui_log_format_timestamp(ts, sizeof(ts));

    char line[160];
    int total = snprintf(line, sizeof(line), "%sW [%04d] %s", ts, code, msg);
    if (total <= 0) return;
    size_t add_len = ((size_t)total < sizeof(line) - 1) ? (size_t)total : sizeof(line) - 2;
    line[add_len] = '\0';

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_last_warn, line, UI_LOG_WARN_CAP - 1);
    s_last_warn[UI_LOG_WARN_CAP - 1] = '\0';
    s_warn_pending = true;
    push_warn_history_locked(code);

    line[add_len] = '\n';
    append_locked(line, add_len + 1);
    xSemaphoreGive(s_mutex);
}

bool ui_log_get_pending_warn(char *out, size_t out_cap)
{
    if (!s_mutex || out_cap == 0) return false;

    bool had = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_warn_pending) {
        strncpy(out, s_last_warn, out_cap - 1);
        out[out_cap - 1] = '\0';
        s_warn_pending = false;
        had = true;
    }
    xSemaphoreGive(s_mutex);
    return had;
}

int ui_log_get_warn_history(int *out_codes, int max)
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = (s_warn_history_count < max) ? s_warn_history_count : max;
    for (int i = 0; i < n; i++) out_codes[i] = s_warn_history[i];
    xSemaphoreGive(s_mutex);
    return n;
}

/* 2026-09-11 — 위 ui_log_clear_one_error()와 동일 패턴(워닝용) */
void ui_log_clear_one_warn(int code)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_warn_history_count; i++) {
        if (s_warn_history[i] == code) {
            for (int j = i; j < s_warn_history_count - 1; j++) {
                s_warn_history[j] = s_warn_history[j + 1];
            }
            s_warn_history_count--;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}

void ui_log_get_snapshot(char *out, size_t out_cap)
{
    if (!s_mutex || !s_buf || out_cap == 0) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = (s_len < out_cap - 1) ? s_len : out_cap - 1;
    size_t start = s_len - n;
    memcpy(out, s_buf + start, n);
    out[n] = '\0';
    xSemaphoreGive(s_mutex);
}
