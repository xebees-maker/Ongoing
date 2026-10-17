/**
 * @file    web_access_log.c
 * @brief   web_access_log.h 구현 — httpd는 태스크 하나라 요청이 겹치지 않음(그래서 "지금 응답 중인 요청" 하나만 따라감)
 */
#include "web_access_log.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

static const char *TAG = "ACCESS";

#define ACC_SLOTS     8
#define ACC_QUIET_S   60

typedef struct {
    char     ip[40];
    char     path[48];
    char     status[4];
    int64_t  logged_s;     /* 마지막으로 줄을 남긴 시각(부팅 후 초) */
    uint32_t skipped;      /* 그 뒤 줄 없이 센 요청 수 */
} acc_slot_t;

typedef struct {
    acc_slot_t slot[ACC_SLOTS];
    char ip[48];
    char path[48];
    httpd_req_t *cur_req;  /* 응답 중인 요청 — 바뀌면 크기·상태를 새로 셈 */
    uint32_t cur_bytes;
    char cur_status[4];
} acc_ctx_t;

static acc_ctx_t *s_acc;   /* PSRAM */

static void begin_req(httpd_req_t *req)
{
    if (s_acc->cur_req == req) return;
    s_acc->cur_req = req;
    s_acc->cur_bytes = 0;
    strcpy(s_acc->cur_status, "200");
}

static void log_req(httpd_req_t *req, const char *status, uint32_t bytes)
{
    struct sockaddr_in6 a = { 0 };
    socklen_t alen = sizeof(a);
    s_acc->ip[0] = '\0';
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &alen) == 0) {
        inet6_ntoa_r(a.sin6_addr, s_acc->ip, sizeof(s_acc->ip));
    }
    const char *ip = strncmp(s_acc->ip, "::FFFF:", 7) == 0 ? s_acc->ip + 7 : s_acc->ip;
    size_t plen = strcspn(req->uri, "?");
    if (plen >= sizeof(s_acc->path)) plen = sizeof(s_acc->path) - 1;
    memcpy(s_acc->path, req->uri, plen);
    s_acc->path[plen] = '\0';

    int64_t now = esp_timer_get_time() / 1000000;
    acc_slot_t *sl = NULL, *oldest = &s_acc->slot[0];
    for (int i = 0; i < ACC_SLOTS; i++) {
        acc_slot_t *s = &s_acc->slot[i];
        if (strcmp(s->ip, ip) == 0 && strcmp(s->path, s_acc->path) == 0 && strncmp(s->status, status, 3) == 0) { sl = s; break; }
        if (s->logged_s < oldest->logged_s) oldest = s;
    }
    if (sl && now - sl->logged_s < ACC_QUIET_S) {
        sl->skipped++;
        return;
    }
    uint32_t skipped = 0;
    if (sl) {
        skipped = sl->skipped;
    } else {
        sl = oldest;
        strlcpy(sl->ip, ip, sizeof(sl->ip));
        strlcpy(sl->path, s_acc->path, sizeof(sl->path));
        strlcpy(sl->status, status, sizeof(sl->status));
    }
    sl->logged_s = now;
    sl->skipped = 0;
    if (skipped) {
        ESP_LOGI(TAG, "%s %s %.64s %s %uB (+%u since last line)", ip, http_method_str(req->method), req->uri, status,
                 (unsigned)bytes, (unsigned)skipped);
    } else {
        ESP_LOGI(TAG, "%s %s %.64s %s %uB", ip, http_method_str(req->method), req->uri, status, (unsigned)bytes);
    }
}

esp_err_t __real_httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t __real_httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t __real_httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t __wrap_httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t __wrap_httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t __wrap_httpd_resp_set_status(httpd_req_t *r, const char *status);

esp_err_t __wrap_httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    if (s_acc && r && status) {
        begin_req(r);
        strlcpy(s_acc->cur_status, status, sizeof(s_acc->cur_status));
    }
    return __real_httpd_resp_set_status(r, status);
}

esp_err_t __wrap_httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len)
{
    esp_err_t ret = __real_httpd_resp_send(r, buf, buf_len);
    if (s_acc && r) {
        begin_req(r);
        uint32_t n = !buf ? 0 : buf_len == HTTPD_RESP_USE_STRLEN ? (uint32_t)strlen(buf) : (uint32_t)buf_len;
        log_req(r, s_acc->cur_status, n);
        s_acc->cur_req = NULL;
    }
    return ret;
}

esp_err_t __wrap_httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len)
{
    esp_err_t ret = __real_httpd_resp_send_chunk(r, buf, buf_len);
    if (s_acc && r) {
        begin_req(r);
        if (!buf || buf_len == 0) {   /* 마지막 조각 — 응답 끝 */
            log_req(r, s_acc->cur_status, s_acc->cur_bytes);
            s_acc->cur_req = NULL;
        } else {
            s_acc->cur_bytes += buf_len == HTTPD_RESP_USE_STRLEN ? (uint32_t)strlen(buf) : (uint32_t)buf_len;
        }
    }
    return ret;
}

static esp_err_t not_found_cb(httpd_req_t *req, httpd_err_code_t err)
{
    if (s_acc) log_req(req, "404", 0);
    httpd_resp_send_err(req, err, NULL);
    return ESP_FAIL;   /* 기본 처리와 같이 연결을 닫음 */
}

void web_access_log_register(httpd_handle_t server)
{
    if (!s_acc) s_acc = heap_caps_calloc(1, sizeof(*s_acc), MALLOC_CAP_SPIRAM);
    if (!s_acc) {
        ESP_LOGE(TAG, "alloc failed - access log off");
        return;
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_cb);
}
