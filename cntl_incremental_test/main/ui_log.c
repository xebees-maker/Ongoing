#include "ui_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* 부팅 시 한 번만 잡고 계속 재사용하는 고정 버퍼(다른 모듈들과 동일 원칙) — 꽉 차면
 * 오래된 앞부분을 memmove로 밀어내고 뒤에 이어붙임(단순 append 버퍼, 진짜 링버퍼는
 * 아님 — 화면에 그대로 이어붙여 보여주기엔 이 편이 더 단순함) */
#define UI_LOG_BUF_CAP 6144
static char s_buf[UI_LOG_BUF_CAP + 1];
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

/* (코드, 짧은 설명) 표 — ui_log.h의 UI_ERR_* 주석과 1:1로 맞춰둠. 코드만 보고 뭔지
 * 바로 알아보려는 용도라 설명은 짧게(2026-08-01, 사용자 지시) */
typedef struct {
    int         code;
    const char *desc;
} ui_err_entry_t;

static const ui_err_entry_t s_err_table[] = {
    { UI_ERR_CACHE_TOO_BIG,       "사진이 캐시 용량보다 큼" },
    { UI_ERR_CACHE_NO_BUF,        "캐시 슬롯 버퍼 없음" },
    { UI_ERR_RECV_BUF_ALLOC,      "수신 버퍼 할당 실패" },
    { UI_ERR_CACHE_SLOT_ALLOC,    "캐시 슬롯 할당 실패" },
    { UI_ERR_PANEL_BUF_ALLOC,     "판넬 버퍼 할당 실패" },
    { UI_ERR_SEND_PHOTO_REQ,      "사진 요청 전송 실패" },
    { UI_ERR_SEND_CAPTURE_REQ,    "지금촬영 요청 전송 실패" },
    { UI_ERR_SEND_LIST_REQ,       "목록 요청 전송 실패" },
    { UI_ERR_SEND_DELETE_REQ,     "삭제 요청 전송 실패" },
    { UI_ERR_SEND_DELETE_ALL_REQ, "전체삭제 요청 전송 실패" },
    { UI_ERR_REQUEST_BUSY,        "요청 무시됨(이미 수신중)" },
    { UI_ERR_META_TOO_BIG,        "사진이 수신 버퍼보다 큼" },
    { UI_ERR_CHUNK_MISSING,       "청크 누락" },
    { UI_ERR_CRC_MISMATCH,        "CRC 불일치" },
    { UI_ERR_DECODE_FAIL,         "JPEG 디코드 실패" },
    { UI_ERR_LIST_COUNT_MISMATCH, "목록 항목 유실" },
    { UI_ERR_FETCH_NORESPONSE,    "사진 가져오기 무응답" },
    { UI_ERR_LIST_NORESPONSE,     "목록 갱신 무응답" },
    { UI_ERR_DELETE_FAILED,       "사진 삭제 실패" },
    { UI_ERR_DELETE_ALL_FAILED,   "전체삭제 실패" },
    { UI_ERR_CAPTURE_FAILED,      "촬영 실패" },
    { UI_ERR_CAPTURE_NORESPONSE,  "지금촬영 무응답" },
    { UI_ERR_FONT_FILE_MISSING,   "폰트 파일 없음" },
    { UI_ERR_FONT_BUF_ALLOC,      "폰트 버퍼 할당 실패" },
    { UI_ERR_FONT_FILE_OPEN,      "폰트 파일 열기 실패" },
    { UI_ERR_FONT_CREATE,         "폰트 생성 실패" },
    { UI_ERR_HTTPD_START,         "웹서버 시작 실패" },
    { UI_ERR_TEST_FORCED,         "시험 버튼 강제 에러" },
};

void ui_log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
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

void ui_log_add(const char *fmt, ...)
{
    if (!s_mutex) return;

    char line[160];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n <= 0) return;

    size_t add_len = ((size_t)n < sizeof(line) - 1) ? (size_t)n : sizeof(line) - 2;
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
    if (!s_mutex) return;

    char msg[144];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (n <= 0) return;

    char line[160];
    int total = snprintf(line, sizeof(line), "[%04d] %s", code, msg);
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

const char *ui_log_err_desc(int code)
{
    for (size_t i = 0; i < sizeof(s_err_table) / sizeof(s_err_table[0]); i++) {
        if (s_err_table[i].code == code) return s_err_table[i].desc;
    }
    return "알 수 없는 에러";
}

void ui_log_get_snapshot(char *out, size_t out_cap)
{
    if (!s_mutex || out_cap == 0) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = (s_len < out_cap - 1) ? s_len : out_cap - 1;
    size_t start = s_len - n;
    memcpy(out, s_buf + start, n);
    out[n] = '\0';
    xSemaphoreGive(s_mutex);
}
