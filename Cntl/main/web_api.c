#include "web_api.h"
#include "web_auth.h"
#include "web_session.h"
#include "node_hub.h"
#include "device_config.h"
#include "power_relay.h"
#include "storage_mgr.h"
#include "sd_storage.h"
#include "rtc_sync.h"
#include "wifi_sta.h"
#include "ui_main.h"
#include "photo_storage.h"
#include "stats_store.h"
#include "notify.h"
#include "alarm.h"
#include "romanize.h"
#include "dev_log.h"
#include "ui_log.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

/* 2026-10-03(할 일 AD — SPA 설계) — 웹 화면 API. 콘 = 모델 + JSON, 문구는 SPA(코드만 보냄).
 * 화면 API는 WEB_SCREEN_API_BEGIN(인증 + 웹 세션 — 콘 잠금)으로 시작. 응답 본문·노드 배열은 PSRAM(스택에 안 둠) */

static const char *TAG = "WEBAPI";

/* ── 작은 JSON 버퍼 ── */
typedef struct {
    char  *p;
    size_t cap;
    size_t len;
    bool   overflow;
} jbuf_t;

static void jb_printf(jbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_printf(jbuf_t *b, const char *fmt, ...)
{
    if (b->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) { b->overflow = true; return; }
    b->len += (size_t)n;
}

/* 문자열을 따옴표 포함 JSON 문자열로(", \, 제어문자 이스케이프) */
static void jb_str(jbuf_t *b, const char *s)
{
    jb_printf(b, "\"");
    for (; s && *s && !b->overflow; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') jb_printf(b, "\\%c", c);
        else if (c < 0x20) jb_printf(b, "\\u%04x", c);
        else jb_printf(b, "%c", c);
    }
    jb_printf(b, "\"");
}

/* 2026-10-05 — ,"<key>_en":"<로마자>" — 웹이 영어 화면일 때 씀(콘 화면과 같은 romanize_hangul). httpd 태스크 하나라 버퍼 하나 */
static void jb_en(jbuf_t *b, const char *key, const char *s)
{
    static char *buf = NULL;
    if (!buf) buf = heap_caps_malloc(DEVICE_CONFIG_ALIAS_DISP_LEN * 2, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    romanize_hangul(s, buf, DEVICE_CONFIG_ALIAS_DISP_LEN * 2);
    jb_printf(b, ",\"%s_en\":", key);
    jb_str(b, buf);
}

static void jb_mac(jbuf_t *b, const uint8_t *m)
{
    jb_printf(b, "\"%02x%02x%02x%02x%02x%02x\"", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* float → JSON 숫자(소수 2자리, NaN/무한은 null) */
static void jb_float(jbuf_t *b, float v)
{
    if (v != v || v > 1e9f || v < -1e9f) { jb_printf(b, "null"); return; }
    jb_printf(b, "%.2f", (double)v);
}

static const char *conn_code(hub_conn_state_t cs)
{
    return (cs == HUB_CONN_STATE_WAITING) ? "waiting" : (cs == HUB_CONN_STATE_ACTIVE) ? "active" : "paired";
}

static esp_err_t send_jbuf(httpd_req_t *req, jbuf_t *b)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (b->overflow) {
        ESP_LOGE(TAG, "JSON buffer overflow (%u)", (unsigned)b->cap);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"overflow\"}");
    }
    return httpd_resp_send(req, b->p, b->len);
}

/* 콘 주화면 한 장분: 상단바(네트워크·상태), Summary(메모리·저장 공간), Power Control(릴레이), Sensor·Camera 목록, 알림.
 * GET /api/dashboard와 WebSocket 밀어주기(ws_push_work)가 같이 씀 — httpd 태스크에서만. mem 구간(자주 바뀜)의 위치를
 * 돌려줌(WebSocket이 바뀐 게 있는지 비교할 때 뺌). 2026-10-05 — 시각은 뺌(웹 상단 시계 없앰, 사용자 결정) */
static bool build_dashboard(jbuf_t *bp, size_t *mem_from, size_t *mem_to)
{
    jbuf_t b = *bp;
    node_hub_node_t *nodes = heap_caps_malloc(sizeof(node_hub_node_t) * NODE_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    if (!nodes) return false;

    /* 상단바 */
    bool err = false, warn = false, sd_fail = false;
    ui_main_get_status_flags(&err, &warn, &sd_fail);
    bool ap = device_config_get_wifi_ap_mode();
    const char *ssid = ap ? wifi_sta_get_ap_ssid() : wifi_sta_get_active_ssid();
    jb_printf(&b, "{\"net\":{\"mode\":\"%s\",\"ssid\":", ap ? "ap" : "sta");
    jb_str(&b, ssid ? ssid : "");
    jb_printf(&b, ",\"ip\":%s},\"status\":\"%s\"", wifi_sta_get_own_ip_str()[0] ? "true" : "false",
              (err || sd_fail) ? "error" : warn ? "warning" : "normal");
    jb_printf(&b, ",\"rec\":%lu", (unsigned long)stats_store_get_append_seq());  /* 2026-10-09 — 웹 기록 표 자동 갱신 */

    /* Summary — 메모리(바이트), 저장 공간(영역별 사용%·여유MB, 콘 Summary와 같은 계산) */
    *mem_from = b.len;
    jb_printf(&b, ",\"mem\":{\"i\":%u,\"p\":%u}", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    *mem_to = b.len;
    if (sd_fail) {
        jb_printf(&b, ",\"sd\":{\"state\":\"io_error\"}");
    } else if (!sd_storage_is_mounted()) {
        jb_printf(&b, ",\"sd\":{\"state\":\"unmounted\"}");
    } else {
        storage_mgr_snapshot_t *sp = heap_caps_malloc(sizeof(*sp), MALLOC_CAP_SPIRAM);  /* 스택에 안 둠 */
        if (sp) storage_mgr_get_snapshot(sp);
        if (!sp || !sp->valid || sp->sd_total == 0) {
            jb_printf(&b, ",\"sd\":{\"state\":\"scanning\"}");
        } else {
            jb_printf(&b, ",\"sd\":{\"state\":\"ok\",\"areas\":[");
            for (int a = 0; a <= STORAGE_AREA_COUNT; a++) {
                const storage_area_usage_t *u = (a < STORAGE_AREA_COUNT) ? &sp->area[a] : &sp->total;
                static const char *names[] = { "picture", "measure", "total" };
                unsigned pct = u->budget ? (unsigned)(u->used * 100 / u->budget) : 0;
                jb_printf(&b, "%s{\"id\":\"%s\",\"pct\":%u,\"remain_mb\":%u}", a ? "," : "", names[a], pct,
                          (unsigned)(u->remain / (1024 * 1024)));
            }
            jb_printf(&b, "]}");
        }
        heap_caps_free(sp);
    }

    /* 저장 공간 정리 안내 — 콘 화면이 띄우는 "오래된 파일 정리" 팝업과 같은 내용. 번호가 바뀌면 SPA가 안내 */
    {
        uint32_t last[STORAGE_AREA_COUNT];
        uint32_t seq = storage_mgr_get_cleanup_event(last);
        jb_printf(&b, ",\"cleanup\":{\"seq\":%lu,\"deleted\":[", (unsigned long)seq);
        for (int a = 0; a < STORAGE_AREA_COUNT; a++) jb_printf(&b, "%s%lu", a ? "," : "", (unsigned long)last[a]);
        jb_printf(&b, "]}");
    }

    /* Power Control — 요약 문구는 SPA가 조각 문구(Turn/if/…)로 만듦(콘 refresh_power_control_panel과 같은 규칙) */
    jb_printf(&b, ",\"relays\":[");
    for (int i = 0; i < POWER_RELAY_COUNT; i++) {
        const power_relay_config_t *c = power_relay_get_config(i);
        jb_printf(&b, "%s{\"idx\":%d,\"alias\":", i ? "," : "", i);
        jb_str(&b, c ? c->alias : "");
        jb_en(&b, "alias", c ? c->alias : "");
        jb_printf(&b, ",\"on\":%s", power_relay_get_commanded_on(i) ? "true" : "false");
        if (c) {
            jb_printf(&b, ",\"configured\":%s,\"override\":%s,\"override_on\":%s,\"turns_on\":%s,\"rises\":%s,\"chan\":%u,\"center\":",
                      c->configured ? "true" : "false", c->manual_override ? "true" : "false",
                      c->manual_override_on ? "true" : "false", c->z_turns_on ? "true" : "false",
                      c->y_rises ? "true" : "false", (unsigned)c->chan_type);
            jb_float(&b, (c->on_threshold + c->off_threshold) / 2.0f);
            const power_relay_timer_t *t = power_relay_get_timer(i);   /* 2026-10-09 — 타이머(행 요약용) */
            if (t && t->timer_mode) {
                jb_printf(&b, ",\"timer\":{\"on\":%lu,\"off\":%lu,\"dn\":%s,\"non\":%lu,\"noff\":%lu}",
                          (unsigned long)t->on_sec, (unsigned long)t->off_sec, t->day_night ? "true" : "false",
                          (unsigned long)t->night_on_sec, (unsigned long)t->night_off_sec);
            }
        }
        jb_printf(&b, "}");
    }
    jb_printf(&b, "]");

    /* Sensor·Camera — 콘 주화면처럼 연결 대기(waiting) 노드도 보내되 SPA가 거름 */
    int n = node_hub_get_nodes(HUB_NODE_KIND_UNKNOWN, nodes, NODE_HUB_MAX_NODES);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    jb_printf(&b, ",\"nodes\":[");
    for (int i = 0; i < n; i++) {
        const node_hub_node_t *nd = &nodes[i];
        hub_conn_state_t cs = node_hub_get_conn_state(nd->mac);
        bool cam = (nd->kind == HUB_NODE_KIND_CAM);
        jb_printf(&b, "%s{\"mac\":", i ? "," : "");
        jb_mac(&b, nd->mac);
        jb_printf(&b, ",\"kind\":\"%s\",\"name\":", cam ? "cam" : (nd->kind == HUB_NODE_KIND_SENS) ? "sens" : "unknown");
        jb_str(&b, nd->name);
        jb_printf(&b, ",\"alias\":");
        jb_str(&b, device_config_get_alias(nd->mac));
        jb_en(&b, "alias", device_config_get_alias(nd->mac));
        jb_printf(&b, ",\"status\":\"%s\"", conn_code(cs));
        uint32_t timeout_ms = node_hub_node_timeout_ms(nd);
        bool near_orphan = timeout_ms > 0 && (uint64_t)(now_ms - nd->last_seen_ms) * 10 >= (uint64_t)timeout_ms * 8;
        jb_printf(&b, ",\"near_orphan\":%s", near_orphan ? "true" : "false");
        if (nd->has_rssi) jb_printf(&b, ",\"rssi\":%d", nd->rssi);
        if (nd->has_deepsleep_stats) jb_printf(&b, ",\"battery\":%u", (unsigned)nd->battery_pct);
        if (cam) {
            jb_printf(&b, ",\"agc\":%s,\"aec\":%s,\"capture_s\":%lu",
                      device_config_get_agc_enable(nd->mac) ? "true" : "false",
                      device_config_get_aec_enable(nd->mac) ? "true" : "false",
                      (unsigned long)device_config_get_cam_capture_interval_sec(nd->mac));
        } else {
            jb_printf(&b, ",\"interval_s\":%lu", (unsigned long)device_config_get_sens_sample_interval_sec(nd->mac));
            if (nd->has_sensor_data) {
                jb_printf(&b, ",\"chans\":[");
                for (int c = 0; c < nd->chan_count && c < ESP_NOW_MAX_CHANNELS; c++) {
                    jb_printf(&b, "%s{\"type\":%u,\"ok\":%s,\"invalid\":%s,\"val\":", c ? "," : "", (unsigned)nd->chan_type[c],
                              nd->chan_ok[c] ? "true" : "false", nd->chan_invalid[c] ? "true" : "false");
                    jb_float(&b, nd->chan_val[c]);
                    jb_printf(&b, "}");
                }
                jb_printf(&b, "]");
            }
        }
        jb_printf(&b, "}");
    }
    jb_printf(&b, "]");
    heap_caps_free(nodes);

    /* 2026-10-05(할 일 AD 5단계) — 알림: 마지막 번호(안 본 개수 계산용) + 웹앱에 아직 안 간 보낼 대상(웹앱이 띄운 뒤
     * POST /api/alarms/ack로 알려 오면 빠짐) */
    alarm_rec_t *al = heap_caps_malloc(sizeof(alarm_rec_t) * 8, MALLOC_CAP_SPIRAM);
    int an = al ? alarm_peek_web_new(al, 8) : 0;
    jb_printf(&b, ",\"alarm\":{\"last\":%lu,\"new\":[", (unsigned long)alarm_last_id());
    for (int i = 0; i < an; i++) {
        jb_printf(&b, "%s{\"id\":%lu,\"t\":%lu,\"type\":%u,\"msg\":", i ? "," : "", (unsigned long)al[i].id, (unsigned long)al[i].t, (unsigned)al[i].type);
        jb_str(&b, al[i].msg);
        jb_printf(&b, "}");
    }
    jb_printf(&b, "]}}");
    heap_caps_free(al);
    *bp = b;
    return true;
}

/* GET /api/dashboard — 처음 열 때와 WebSocket이 안 될 때(5초 조회로 대신) */
static esp_err_t api_dashboard_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    const size_t cap = 8192;
    jbuf_t b = { .p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), .cap = cap };
    size_t mf = 0, mt = 0;
    if (!b.p || !build_dashboard(&b, &mf, &mt)) {
        heap_caps_free(b.p);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    esp_err_t ret = send_jbuf(req, &b);
    heap_caps_free(b.p);
    return ret;
}

/* ── WebSocket /ws(2026-10-05 — 5초 조회 대신, 사용자 지시) ──
 * - 웹앱 하나(단일 접속 전제) — 새로 붙으면 이전 연결은 닫음.
 * - 콘이 1초마다 주화면 내용을 만들어 바뀐 게 있으면 보냄(메모리 수치만 바뀌면 5초마다). 첫 연결은 바로 보냄.
 * - 웹앱은 5초마다 "hb"를 보냄 → 웹 세션 유지(web_session_touch). 휴대폰이 잠기면 hb가 끊겨 기존 20초 규칙으로 세션이
 *   끝나고, 그때 이 연결도 닫음(알림은 ntfy로 넘어감). 콘이 넘겨받으면 {"error":"local"}을 보내고 닫음 */
#define WS_PUSH_MS        1000
#define WS_MEM_ONLY_MS    5000
static httpd_handle_t s_server = NULL;
static int s_ws_fd = -1;
static uint32_t s_ws_hash = 0;
static int64_t s_ws_sent_us = 0;
static jbuf_t s_ws_buf = { 0 };
static esp_timer_handle_t s_ws_timer = NULL;

static uint32_t fnv1a(const char *p, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)p[i]; h *= 16777619u; }
    return h;
}

/* 연결이 이미 닫혔으면(닫기 프레임은 httpd가 직접 처리) 같은 소켓 번호가 다른 HTTP 연결에 다시 쓰였을 수 있음 —
 * 보내거나 닫기 전에 아직 WebSocket인지 확인 */
static bool ws_fd_alive(int fd)
{
    return fd >= 0 && httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

static void ws_close_current(void)
{
    if (ws_fd_alive(s_ws_fd)) httpd_sess_trigger_close(s_server, s_ws_fd);
    s_ws_fd = -1;
}

static esp_err_t ws_send_text(int fd, const char *p, size_t len)
{
    httpd_ws_frame_t f = { .final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)p, .len = len };
    return httpd_ws_send_frame_async(s_server, fd, &f);
}

/* httpd 태스크에서(httpd_queue_work) — 다른 핸들러와 겹치지 않음 */
static void ws_push_work(void *arg)
{
    (void)arg;
    int fd = s_ws_fd;
    if (fd < 0) return;
    if (!ws_fd_alive(fd)) {
        s_ws_fd = -1;
        return;
    }
    if (web_session_local_active()) {
        static const char local_msg[] = "{\"error\":\"local\"}";
        ws_send_text(fd, local_msg, sizeof(local_msg) - 1);
        ws_close_current();
        return;
    }
    if (!web_session_web_active()) {  /* hb가 끊겨 세션이 끝남 */
        ws_close_current();
        return;
    }
    if (!s_ws_buf.p) {
        s_ws_buf.cap = 8192;
        s_ws_buf.p = heap_caps_malloc(s_ws_buf.cap, MALLOC_CAP_SPIRAM);
        if (!s_ws_buf.p) return;
    }
    s_ws_buf.len = 0;
    s_ws_buf.overflow = false;
    size_t mf = 0, mt = 0;
    if (!build_dashboard(&s_ws_buf, &mf, &mt) || s_ws_buf.overflow) return;
    uint32_t h = fnv1a(s_ws_buf.p, mf, 2166136261u);
    h = fnv1a(s_ws_buf.p + mt, s_ws_buf.len - mt, h);
    int64_t now = esp_timer_get_time();
    if (h == s_ws_hash && now - s_ws_sent_us < (int64_t)WS_MEM_ONLY_MS * 1000) return;
    if (ws_send_text(fd, s_ws_buf.p, s_ws_buf.len) != ESP_OK) {
        ESP_LOGW(TAG, "WS send failed - closing");
        ws_close_current();
        return;
    }
    s_ws_hash = h;
    s_ws_sent_us = now;
}

static void ws_timer_cb(void *arg)
{
    (void)arg;
    if (s_ws_fd >= 0 && s_server) httpd_queue_work(s_server, ws_push_work, NULL);
}

/* 연결을 맺기 전(IDF v6는 연결 맺을 때 URI 핸들러를 부르지 않음 — 실기 확인) — 로그인 + 웹 세션. 실패하면 연결 안 맺음
 * (콘이 넘겨받은 상태면 web_session_gate가 409를 보냄 → 웹앱은 HTTP 조회로 돌아가 넘겨받음 화면) */
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_FAIL;
    if (!web_session_gate(req)) return ESP_FAIL;
    return ESP_OK;
}

/* 프레임 — 웹앱은 연결되자마자, 그 뒤 5초마다 "hb"를 보냄. 이 연결을 처음 보면 여기서 등록하고 첫 내용을 바로 보냄 */
static esp_err_t ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t f = { 0 };
    uint8_t buf[8];  /* 받는 것은 "hb" 하나뿐 — 큰 프레임은 읽지 않고 버림 */
    f.payload = buf;
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
    if (f.len > sizeof(buf)) return ESP_FAIL;  /* 읽지 않고 두면 다음 프레임이 어긋남 — 연결을 닫음 */
    if (f.len > 0) {
        f.payload = buf;
        if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) return ESP_FAIL;
    }
    if (f.type == HTTPD_WS_TYPE_CLOSE) {
        if (httpd_req_to_sockfd(req) == s_ws_fd) s_ws_fd = -1;
        return ESP_OK;
    }
    if (f.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;
    int fd = httpd_req_to_sockfd(req);
    if (fd != s_ws_fd) {
        if (s_ws_fd >= 0) ws_close_current();  /* 단일 접속 — 이전 연결은 닫음 */
        s_ws_fd = fd;
        s_ws_hash = 0;
        s_ws_sent_us = 0;
        ESP_LOGI(TAG, "WS connected (fd %d)", fd);
    }
    web_session_touch();   /* 넘겨받은 상태면 false — 아래 밀어주기가 local을 알리고 닫음 */
    if (s_ws_hash == 0) httpd_queue_work(s_server, ws_push_work, NULL);  /* 첫 내용 바로 */
    return ESP_OK;
}


/* ── 요청 인자 ── */
static bool hex_to_mac(const char *hex, uint8_t mac[6])
{
    if (strlen(hex) != 12) return false;
    for (int i = 0; i < 6; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
        mac[i] = (uint8_t)v;
    }
    return true;
}

/* 요청 작업 버퍼(PSRAM) — httpd는 태스크 하나라 요청이 겹치지 않음 */
typedef struct {
    char query[256];   /* 쿼리, 또는 릴레이 Apply 본문 */
    char tmp[24];      /* 값 하나 꺼내기용 */
    char mac_hex[16];
    char key[16];
    char val[16];
    char body[64];
    ui_web_op_t op;
    node_hub_node_t node;
} web_api_work_t;
static web_api_work_t *s_w = NULL;

static bool work_ready(httpd_req_t *req)
{
    if (!s_w) s_w = heap_caps_calloc(1, sizeof(*s_w), MALLOC_CAP_SPIRAM);
    if (!s_w) { httpd_resp_send_500(req); return false; }
    memset(s_w->query, 0, sizeof(s_w->query));
    s_w->mac_hex[0] = s_w->key[0] = s_w->val[0] = s_w->body[0] = '\0';
    if (httpd_req_get_url_query_str(req, s_w->query, sizeof(s_w->query)) == ESP_OK) {
        httpd_query_key_value(s_w->query, "mac", s_w->mac_hex, sizeof(s_w->mac_hex));
        httpd_query_key_value(s_w->query, "key", s_w->key, sizeof(s_w->key));
        httpd_query_key_value(s_w->query, "val", s_w->val, sizeof(s_w->val));
    }
    return true;
}

static esp_err_t send_simple(httpd_req_t *req, const char *status, const char *json)
{
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* 노드 하나 찾기(s_w->node에) */
static bool find_node(const uint8_t mac[6])
{
    node_hub_node_t *nodes = heap_caps_malloc(sizeof(node_hub_node_t) * NODE_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    if (!nodes) return false;
    int n = node_hub_get_nodes(HUB_NODE_KIND_UNKNOWN, nodes, NODE_HUB_MAX_NODES);
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        if (memcmp(nodes[i].mac, mac, 6) == 0) { s_w->node = nodes[i]; found = true; }
    }
    heap_caps_free(nodes);
    return found;
}

static void jb_values(jbuf_t *b, int which)
{
    const uint32_t *v = NULL;
    int n = ui_main_get_option_values(which, &v);
    jb_printf(b, "[");
    for (int i = 0; i < n; i++) jb_printf(b, "%s%lu", i ? "," : "", (unsigned long)v[i]);
    jb_printf(b, "]");
}

/* GET /api/device?mac=<12hex> — 장치 팝업 한 장분(콘 build_device_popup과 같은 항목) + 선택지 값 */
static esp_err_t api_device_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    uint8_t mac[6];
    if (!hex_to_mac(s_w->mac_hex, mac)) return send_simple(req, "400 Bad Request", "{\"error\":\"mac\"}");
    if (!find_node(mac)) return send_simple(req, "404 Not Found", "{\"error\":\"node\"}");
    const node_hub_node_t *nd = &s_w->node;
    const size_t cap = 1024;
    jbuf_t b = { .p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), .cap = cap };
    if (!b.p) { httpd_resp_send_500(req); return ESP_FAIL; }
    bool cam = (nd->kind == HUB_NODE_KIND_CAM);
    jb_printf(&b, "{\"mac\":");
    jb_mac(&b, nd->mac);
    jb_printf(&b, ",\"kind\":\"%s\",\"name\":", cam ? "cam" : "sens");
    jb_str(&b, nd->name);
    jb_printf(&b, ",\"alias\":");
    jb_str(&b, device_config_get_alias(nd->mac));
    jb_printf(&b, ",\"alias_max\":%d,\"status\":\"%s\"", DEVICE_CONFIG_ALIAS_MAX_LEN - 1, conn_code(node_hub_get_conn_state(nd->mac)));
    if (cam) {
        jb_printf(&b, ",\"capture_s\":%lu,\"agc\":%s,\"aec\":%s,\"xclk\":%u,\"opt_capture\":",
                  (unsigned long)device_config_get_cam_capture_interval_sec(nd->mac),
                  device_config_get_agc_enable(nd->mac) ? "true" : "false",
                  device_config_get_aec_enable(nd->mac) ? "true" : "false",
                  (unsigned)device_config_get_xclk_mhz(nd->mac));
        jb_values(&b, 1);
        jb_printf(&b, ",\"opt_xclk\":");
        jb_values(&b, 2);
    } else {
        jb_printf(&b, ",\"interval_s\":%lu,\"opt_interval\":", (unsigned long)device_config_get_sens_sample_interval_sec(nd->mac));
        jb_values(&b, 0);
    }
    jb_printf(&b, "}");
    esp_err_t ret = send_jbuf(req, &b);
    heap_caps_free(b.p);
    return ret;
}

/* POST /api/device/set?mac=&key=(alias|interval|capture|agc|aec|xclk|light)&val= — alias는 본문이 새 별명.
 * 콘 화면 콜백과 같은 모델 함수를 LVGL 태스크에서 실행(ui_main_run_web_op) */
static esp_err_t api_device_set_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    ui_web_op_t *op = &s_w->op;
    memset(op, 0, sizeof(*op));
    if (!hex_to_mac(s_w->mac_hex, op->mac)) return send_simple(req, "400 Bad Request", "{\"ok\":false,\"error\":\"mac\"}");
    static const struct { const char *key; ui_web_op_type_t type; } keys[] = {
        { "alias", UI_WEB_OP_ALIAS }, { "interval", UI_WEB_OP_SENS_INTERVAL }, { "capture", UI_WEB_OP_CAM_CAPTURE },
        { "agc", UI_WEB_OP_CAM_AGC }, { "aec", UI_WEB_OP_CAM_AEC }, { "xclk", UI_WEB_OP_CAM_XCLK }, { "light", UI_WEB_OP_CAM_LIGHT },
    };
    op->type = 0;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) if (strcmp(s_w->key, keys[i].key) == 0) op->type = keys[i].type;
    if (!op->type) return send_simple(req, "400 Bad Request", "{\"ok\":false,\"error\":\"key\"}");
    if (op->type == UI_WEB_OP_ALIAS) {
        if (req->content_len > DEVICE_CONFIG_ALIAS_MAX_LEN - 1) return send_simple(req, "400 Bad Request", "{\"ok\":false,\"error\":\"len\"}");
        int got = 0;
        while (got < (int)req->content_len) {
            int r = httpd_req_recv(req, s_w->body + got, req->content_len - got);
            if (r <= 0) return send_simple(req, "400 Bad Request", "{\"ok\":false}");
            got += r;
        }
        s_w->body[got] = '\0';
        memcpy(op->text, s_w->body, (size_t)got + 1);  /* got <= ALIAS_MAX_LEN-1 < sizeof(op->text) */
    } else {
        op->value = (uint32_t)strtoul(s_w->val, NULL, 10);
    }
    bool ok = ui_main_run_web_op(op, 2000);
    return send_simple(req, ok ? NULL : "400 Bad Request", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* POST /api/device/unpair?mac= — 연결 해제(콘 Disconnect와 같음) */
static esp_err_t api_device_unpair_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    ui_web_op_t *op = &s_w->op;
    memset(op, 0, sizeof(*op));
    if (!hex_to_mac(s_w->mac_hex, op->mac)) return send_simple(req, "400 Bad Request", "{\"ok\":false,\"error\":\"mac\"}");
    op->type = UI_WEB_OP_UNPAIR;
    bool ok = ui_main_run_web_op(op, 2000);
    return send_simple(req, ok ? NULL : "500 Internal Server Error", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* ── 릴레이 팝업 ── */
static int query_int(const char *q, const char *key, int def)
{
    char *v = s_w->tmp;   /* 스택에 안 둠 */
    if (httpd_query_key_value(q, key, v, sizeof(s_w->tmp)) != ESP_OK) return def;
    return (int)strtol(v, NULL, 10);
}

static float query_float(const char *q, const char *key, float def)
{
    char *v = s_w->tmp;
    if (httpd_query_key_value(q, key, v, sizeof(s_w->tmp)) != ESP_OK) return def;
    return strtof(v, NULL);
}

static bool relay_idx_from_query(int *idx)
{
    *idx = query_int(s_w->query, "idx", -1);
    return *idx >= 0 && *idx < POWER_RELAY_COUNT;
}

/* 요청 본문을 buf(cap)로 — 길이 초과·실패면 false */
static bool read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len >= cap) return false;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    buf[got] = '\0';
    return true;
}

/* GET /api/relay?idx= — 릴레이 팝업 한 장분(콘 build_relay_popup과 같은 복원 규칙) + 기본값·범위·선택지·기준 장치 후보 */
static esp_err_t api_relay_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    int idx;
    if (!relay_idx_from_query(&idx)) return send_simple(req, "400 Bad Request", "{\"error\":\"idx\"}");
    const power_relay_config_t *c = power_relay_get_config(idx);
    if (!c) return send_simple(req, "404 Not Found", "{\"error\":\"idx\"}");
    const size_t cap = 4096;
    jbuf_t b = { .p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), .cap = cap };
    node_hub_node_t *nodes = heap_caps_malloc(sizeof(node_hub_node_t) * NODE_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    if (!b.p || !nodes) { heap_caps_free(b.p); heap_caps_free(nodes); httpd_resp_send_500(req); return ESP_FAIL; }

    /* 값·오차: 설정했으면 임계값에서, 아니면 채널 기본값(콘 팝업과 같음) */
    float center, margin;
    if (c->configured) {
        center = (c->on_threshold + c->off_threshold) / 2.0f;
        margin = (c->on_threshold > c->off_threshold ? c->on_threshold - c->off_threshold : c->off_threshold - c->on_threshold) / 2.0f;
    } else {
        ui_main_relay_defaults((uint8_t)c->chan_type, &center, &margin);
    }
    int gc = (c->group == POWER_GROUP_AGAR) ? 2 : (c->precise ? 1 : 0);
    jb_printf(&b, "{\"idx\":%d,\"alias\":", idx);
    jb_str(&b, c->alias);
    jb_printf(&b, ",\"alias_max\":%d,\"configured\":%s,\"ai\":%s,\"manual\":%s,\"manual_on\":%s,\"chan\":%u,"
                  "\"y\":%s,\"z\":%s,\"center\":", POWER_RELAY_ALIAS_MAX_LEN - 1,
              c->configured ? "true" : "false", c->ai_mode ? "true" : "false", c->manual_override ? "true" : "false",
              c->manual_override_on ? "true" : "false", (unsigned)c->chan_type, c->y_rises ? "true" : "false",
              c->z_turns_on ? "true" : "false");
    jb_float(&b, center);
    jb_printf(&b, ",\"margin\":");
    jb_float(&b, margin);
    jb_printf(&b, ",\"src\":%u,\"gc\":%d,\"dev\":", (unsigned)c->source_kind, gc);
    jb_mac(&b, c->device_mac);
    jb_printf(&b, ",\"stat\":%u,\"trend\":%s,\"ts\":%lu,\"hold\":%lu,\"opt_ts\":", (unsigned)c->stat,
              c->trend_enable ? "true" : "false", (unsigned long)c->trend_sample_count, (unsigned long)c->min_hold_sec);
    jb_values(&b, 3);
    jb_printf(&b, ",\"opt_hold\":");
    jb_values(&b, 4);
    {   /* 2026-10-09(할 일 AG) — 타이머 칸, 수동 자동 해제 선택지 */
        const power_relay_timer_t *t = power_relay_get_timer(idx);
        jb_printf(&b, ",\"timer\":{\"mode\":%s,\"on\":%lu,\"off\":%lu,\"dn\":%s,\"ds\":%u,\"ns\":%u,\"non\":%lu,\"noff\":%lu}",
                  t->timer_mode ? "true" : "false", (unsigned long)t->on_sec, (unsigned long)t->off_sec,
                  t->day_night ? "true" : "false", (unsigned)t->day_start_min, (unsigned)t->night_start_min,
                  (unsigned long)t->night_on_sec, (unsigned long)t->night_off_sec);
        jb_printf(&b, ",\"rel\":%lu,\"opt_rel\":", (unsigned long)t->manual_release_sec);
        jb_values(&b, 7);
    }

    /* 채널별: 기본값, 범위(기본/정밀), 기준 장치 후보(그 채널을 보고하는 센서 — 콘 relay_rebuild_device_dropdown과 같은 기준) */
    uint8_t chans[4];
    int nch = ui_main_relay_chan_list(chans, 4);
    int nn = node_hub_get_nodes(HUB_NODE_KIND_SENS, nodes, NODE_HUB_MAX_NODES);
    jb_printf(&b, ",\"chans\":[");
    for (int k = 0; k < nch; k++) {
        float dc, dm, mn, mx, st, mm;
        int dec;
        ui_main_relay_defaults(chans[k], &dc, &dm);
        jb_printf(&b, "%s{\"type\":%u,\"def\":[", k ? "," : "", (unsigned)chans[k]);
        jb_float(&b, dc); jb_printf(&b, ","); jb_float(&b, dm);
        jb_printf(&b, "],\"spec\":[");
        for (int pr = 0; pr < 2; pr++) {
            ui_main_relay_spec(chans[k], pr == 1, &mn, &mx, &st, &dec, &mm);
            jb_printf(&b, "%s[", pr ? "," : "");   /* [최소, 최대, 단위, 소수 자리, 오차 최대] */
            jb_float(&b, mn); jb_printf(&b, ","); jb_float(&b, mx); jb_printf(&b, ","); jb_float(&b, st);
            jb_printf(&b, ",%d,", dec);
            jb_float(&b, mm);
            jb_printf(&b, "]");
        }
        jb_printf(&b, "],\"devices\":[");
        int nd = 0;
        for (int i = 0; i < nn; i++) {
            bool has = false;
            for (int ch = 0; ch < nodes[i].chan_count && ch < ESP_NOW_MAX_CHANNELS; ch++) if (nodes[i].chan_type[ch] == chans[k]) has = true;
            if (!has) continue;
            const char *alias = device_config_get_alias(nodes[i].mac);
            jb_printf(&b, "%s{\"mac\":", nd++ ? "," : "");
            jb_mac(&b, nodes[i].mac);
            jb_printf(&b, ",\"name\":");
            jb_str(&b, alias[0] ? alias : nodes[i].name);
            jb_en(&b, "name", alias[0] ? alias : nodes[i].name);
            jb_printf(&b, "}");
        }
        jb_printf(&b, "]}");
    }
    jb_printf(&b, "]}");
    heap_caps_free(nodes);
    esp_err_t ret = send_jbuf(req, &b);
    heap_caps_free(b.p);
    return ret;
}

/* POST /api/relay/apply?idx= — 본문 "chan=&y=&z=&center=&margin=&ai=&manual=&mon=&rel=&src=&gc=&dev=&stat=&trend=&ts=&hold="
 * (화면 선택값 그대로). 설정 계산은 콘 화면과 같은 함수(relay_apply_choices) */
static esp_err_t api_relay_apply_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    ui_web_op_t *op = &s_w->op;
    memset(op, 0, sizeof(*op));
    if (!relay_idx_from_query(&op->idx)) return send_simple(req, "400 Bad Request", "{\"ok\":false,\"error\":\"idx\"}");
    char *body = s_w->query;   /* 쿼리는 이미 읽었으니 같은 버퍼를 본문용으로 */
    if (!read_body(req, body, sizeof(s_w->query))) {
        return send_simple(req, "400 Bad Request", "{\"ok\":false,\"error\":\"body\"}");
    }
    ui_web_relay_choices_t *c = &op->relay;
    c->chan_type = (uint8_t)query_int(body, "chan", 0);
    c->y_rises = query_int(body, "y", 1) != 0;
    c->z_turns_on = query_int(body, "z", 1) != 0;
    c->center = query_float(body, "center", 0);
    c->margin = query_float(body, "margin", 0);
    c->ai_mode = query_int(body, "ai", 0) != 0;
    c->manual_override = query_int(body, "manual", 0) != 0;
    c->manual_on = query_int(body, "mon", 0) != 0;                        /* 2026-10-09 — 방식 = 강제 */
    c->manual_release_sec = (uint32_t)query_int(body, "rel", 0);
    c->source_kind = (uint8_t)query_int(body, "src", 0);
    c->group_choice = (uint8_t)query_int(body, "gc", 0);
    c->device_valid = httpd_query_key_value(body, "dev", s_w->tmp, sizeof(s_w->tmp)) == ESP_OK && hex_to_mac(s_w->tmp, c->device_mac);
    c->stat = (uint8_t)query_int(body, "stat", 0);
    c->trend_enable = query_int(body, "trend", 0) != 0;
    c->trend_samples = (uint32_t)query_int(body, "ts", 0);
    c->min_hold_sec = (uint32_t)query_int(body, "hold", 0);
    /* 2026-10-09(할 일 AG) — 타이머: tm=1이면 ton/toff(초), tdn, tds/tns(분), tnon/tnoff(초) */
    c->timer_mode = query_int(body, "tm", 0) != 0;
    c->timer.on_sec = (uint32_t)query_int(body, "ton", 900);
    c->timer.off_sec = (uint32_t)query_int(body, "toff", 2700);
    c->timer.day_night = query_int(body, "tdn", 0) != 0;
    c->timer.day_start_min = (uint16_t)query_int(body, "tds", 360);
    c->timer.night_start_min = (uint16_t)query_int(body, "tns", 1080);
    c->timer.night_on_sec = (uint32_t)query_int(body, "tnon", 900);
    c->timer.night_off_sec = (uint32_t)query_int(body, "tnoff", 2700);
    op->type = UI_WEB_OP_RELAY_APPLY;
    bool ok = ui_main_run_web_op(op, 2000);
    return send_simple(req, ok ? NULL : "400 Bad Request", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* POST /api/relay/alias?idx= — 본문 = 새 별명 */
static esp_err_t api_relay_alias_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    ui_web_op_t *op = &s_w->op;
    memset(op, 0, sizeof(*op));
    if (!relay_idx_from_query(&op->idx)) return send_simple(req, "400 Bad Request", "{\"ok\":false}");
    if (!read_body(req, s_w->body, POWER_RELAY_ALIAS_MAX_LEN)) return send_simple(req, "400 Bad Request", "{\"ok\":false,\"error\":\"len\"}");
    memcpy(op->text, s_w->body, strlen(s_w->body) + 1);
    op->type = UI_WEB_OP_RELAY_ALIAS;
    bool ok = ui_main_run_web_op(op, 2000);
    return send_simple(req, ok ? NULL : "400 Bad Request", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* POST /api/relay/override?idx=&on=0|1 — 수동 조작(콘 전원 아이콘 확인 팝업 Yes와 같음) */
static esp_err_t api_relay_override_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    ui_web_op_t *op = &s_w->op;
    memset(op, 0, sizeof(*op));
    if (!relay_idx_from_query(&op->idx)) return send_simple(req, "400 Bad Request", "{\"ok\":false}");
    op->value = (uint32_t)(query_int(s_w->query, "on", 0) != 0);
    op->value2 = (uint32_t)query_int(s_w->query, "rel", 0);   /* 2026-10-09 — 자동 해제(초) */
    op->type = UI_WEB_OP_RELAY_OVERRIDE;
    bool ok = ui_main_run_web_op(op, 2000);
    return send_simple(req, ok ? NULL : "400 Bad Request", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* ── 카메라 팝업 ── */
#define WEB_PHOTO_PAGE_SIZE 20            /* 콘 PHOTO_LIST_PAGE_SIZE와 같음 */
#define WEB_PHOTO_BUF_CAP   (1024 * 1024) /* 콘 PHOTO_RAW_BUF_CAP과 같음 */

/* GET /api/cam/list — 카메라 선택지: 지금 연결된 캠 + 사진 폴더가 있는 캠(콘 카메라 팝업 드롭다운과 같은 기준) */
static esp_err_t api_cam_list_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    const size_t cap = 2048;
    jbuf_t b = { .p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), .cap = cap };
    node_hub_node_t *nodes = heap_caps_malloc(sizeof(node_hub_node_t) * NODE_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    uint8_t (*known)[6] = heap_caps_malloc(6 * NODE_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    if (!b.p || !nodes || !known) { heap_caps_free(b.p); heap_caps_free(nodes); heap_caps_free(known); httpd_resp_send_500(req); return ESP_FAIL; }
    int n = node_hub_get_nodes(HUB_NODE_KIND_CAM, nodes, NODE_HUB_MAX_NODES);
    int nk = (int)photo_storage_list_camera_macs(known, NODE_HUB_MAX_NODES);
    /* 2026-10-05 — wake_s: 잠든 캠이 깨는 주기(응답성) — 웹 "깨어나면 촬영(최대 약 N초)" 안내와 사진 기다리는 시간 */
    jb_printf(&b, "{\"wake_s\":%lu,\"cams\":[", (unsigned long)device_config_get_response_interval_sec());
    int out = 0;
    for (int i = 0; i < n; i++) {   /* 연결된 캠(연결 대기 제외 — 콘 주화면과 같음) */
        hub_conn_state_t cs = node_hub_get_conn_state(nodes[i].mac);
        if (cs == HUB_CONN_STATE_WAITING) continue;
        jb_printf(&b, "%s{\"mac\":", out++ ? "," : "");
        jb_mac(&b, nodes[i].mac);
        jb_printf(&b, ",\"name\":");
        const char *alias = device_config_get_alias(nodes[i].mac);
        jb_str(&b, alias[0] ? alias : nodes[i].name);
        jb_en(&b, "name", alias[0] ? alias : nodes[i].name);
        jb_printf(&b, ",\"status\":\"%s\",\"count\":%lu}", conn_code(cs), (unsigned long)photo_storage_get_count(nodes[i].mac));
    }
    for (int k = 0; k < nk; k++) {  /* 사진 이력만 있는 캠 */
        bool listed = false;
        for (int i = 0; i < n && !listed; i++) {
            listed = memcmp(nodes[i].mac, known[k], 6) == 0 && node_hub_get_conn_state(nodes[i].mac) != HUB_CONN_STATE_WAITING;
        }
        if (listed) continue;
        jb_printf(&b, "%s{\"mac\":", out++ ? "," : "");
        jb_mac(&b, known[k]);
        const char *alias = device_config_get_alias(known[k]);
        jb_printf(&b, ",\"name\":");
        if (alias[0]) { jb_str(&b, alias); jb_en(&b, "name", alias); }
        else jb_printf(&b, "\"C%02X%02X%02X\"", known[k][3], known[k][4], known[k][5]);
        jb_printf(&b, ",\"status\":\"offline\",\"count\":%lu}", (unsigned long)photo_storage_get_count(known[k]));
    }
    jb_printf(&b, "]}");
    heap_caps_free(nodes);
    heap_caps_free(known);
    esp_err_t ret = send_jbuf(req, &b);
    heap_caps_free(b.p);
    return ret;
}

/* GET /api/cam/photos?mac=&page= — 사진 목록 한 페이지(0 = 최신, 콘과 같은 20장) */
static esp_err_t api_cam_photos_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    uint8_t mac[6];
    if (!hex_to_mac(s_w->mac_hex, mac)) return send_simple(req, "400 Bad Request", "{\"error\":\"mac\"}");
    int page = query_int(s_w->query, "page", 0);
    if (page < 0) page = 0;
    photo_storage_item_t *items = heap_caps_malloc(sizeof(photo_storage_item_t) * WEB_PHOTO_PAGE_SIZE, MALLOC_CAP_SPIRAM);
    const size_t cap = 3072;
    jbuf_t b = { .p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), .cap = cap };
    if (!items || !b.p) { heap_caps_free(items); heap_caps_free(b.p); httpd_resp_send_500(req); return ESP_FAIL; }
    uint32_t total = photo_storage_get_count(mac);
    uint32_t n = photo_storage_read_page(mac, (uint32_t)page, WEB_PHOTO_PAGE_SIZE, items, WEB_PHOTO_PAGE_SIZE);
    jb_printf(&b, "{\"count\":%lu,\"page\":%d,\"page_size\":%d,\"items\":[", (unsigned long)total, page, WEB_PHOTO_PAGE_SIZE);
    for (uint32_t i = 0; i < n; i++) {
        jb_printf(&b, "%s{\"kind\":\"%c\",\"seq\":%lu,\"t\":%lld,\"size\":%u}", i ? "," : "", items[i].kind,
                  (unsigned long)items[i].seq, (long long)items[i].mtime, (unsigned)items[i].file_size);
    }
    jb_printf(&b, "]}");
    heap_caps_free(items);
    esp_err_t ret = send_jbuf(req, &b);
    heap_caps_free(b.p);
    return ret;
}

/* GET /api/cam/photo?mac=&kind=&seq= — 원본 JPEG(브라우저가 줄여서 보여 줌). 사진 번호는 바뀌지 않으므로 하루 캐시 */
static esp_err_t api_cam_photo_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    uint8_t mac[6];
    if (!hex_to_mac(s_w->mac_hex, mac)) return send_simple(req, "400 Bad Request", "{\"error\":\"mac\"}");
    char kind = (char)query_int(s_w->query, "kind", 'M');
    uint32_t seq = (uint32_t)query_int(s_w->query, "seq", -1);
    uint8_t *buf = heap_caps_malloc(WEB_PHOTO_BUF_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    size_t len = 0;
    if (!photo_storage_read_file(mac, (uint8_t)kind, seq, buf, WEB_PHOTO_BUF_CAP, &len)) {
        heap_caps_free(buf);
        return send_simple(req, "404 Not Found", "{\"error\":\"photo\"}");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=86400");
    esp_err_t ret = httpd_resp_send(req, (const char *)buf, len);
    heap_caps_free(buf);
    return ret;
}

/* POST /api/cam/capture?mac=, /api/cam/delete?mac=&kind=&seq=, /api/cam/delete_all?mac= — 콘 카메라 팝업 버튼과 같은 모델 함수 */
static esp_err_t cam_op(httpd_req_t *req, ui_web_op_type_t type)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    ui_web_op_t *op = &s_w->op;
    memset(op, 0, sizeof(*op));
    if (!hex_to_mac(s_w->mac_hex, op->mac)) return send_simple(req, "400 Bad Request", "{\"ok\":false,\"error\":\"mac\"}");
    op->type = type;
    op->kind = (uint8_t)query_int(s_w->query, "kind", 'M');
    op->seq = (uint32_t)query_int(s_w->query, "seq", -1);
    bool ok = ui_main_run_web_op(op, 3000);
    return send_simple(req, ok ? NULL : "409 Conflict", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}
static esp_err_t api_cam_capture_post_handler(httpd_req_t *req)    { return cam_op(req, UI_WEB_OP_CAPTURE); }
static esp_err_t api_cam_delete_post_handler(httpd_req_t *req)     { return cam_op(req, UI_WEB_OP_PHOTO_DELETE); }
static esp_err_t api_cam_delete_all_post_handler(httpd_req_t *req) { return cam_op(req, UI_WEB_OP_PHOTO_DELETE_ALL); }

/* ── 통계(콘 통계 팝업·Record 팝업과 같은 계산은 ui_main_stats_*에서) ── */

/* SD I/O 에러 중엔 콘처럼 통계를 열지 않음(cb_stats_btn_tap) — SPA가 STR_MSG_STATS_BLOCKED_SD_FAIL을 띄움 */
static bool stats_sd_blocked(httpd_req_t *req)
{
    bool sd_fail = false;
    ui_main_get_status_flags(NULL, NULL, &sd_fail);
    if (sd_fail) send_simple(req, "503 Service Unavailable", "{\"error\":\"sd\"}");
    return sd_fail;
}

static void jb_mma(jbuf_t *b, const ui_stats_mma_t *m)
{
    if (!m->have) { jb_printf(b, "null"); return; }
    jb_printf(b, "[");
    jb_float(b, m->mx);
    jb_printf(b, ",");
    jb_float(b, m->mn);
    jb_printf(b, ",");
    jb_float(b, m->avg);
    jb_printf(b, "]");
}

/* GET /api/stats?scale=0..4&offset=N&group=0..2&tp=0|1&hp=0|1 — 개괄(지금까지 scale) + 그래프 한 창.
 * 개괄 줄 = [max,min,avg] 또는 null. 슬롯: v = 실측 칸(없으면 null), tr = 추세값(선), lo = 저신뢰 칸 "0/1" 문자열 */
static esp_err_t api_stats_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    if (stats_sd_blocked(req)) return ESP_OK;
    int scale = query_int(s_w->query, "scale", 0);
    int offset = query_int(s_w->query, "offset", 0);
    int group = query_int(s_w->query, "group", 0);
    if (scale < 0 || scale >= STATS_SCALE_COUNT || offset < 0 || offset > 100000 || group < 0 || group > 2) {
        return send_simple(req, "400 Bad Request", "{\"error\":\"arg\"}");
    }
    bool tp = query_int(s_w->query, "tp", 1) != 0;
    bool hp = query_int(s_w->query, "hp", 1) != 0;

    ui_stats_graph_t *g = heap_caps_malloc(sizeof(*g), MALLOC_CAP_SPIRAM);
    ui_stats_overview_t *ov = heap_caps_malloc(sizeof(*ov), MALLOC_CAP_SPIRAM);
    const size_t cap = 16384;
    jbuf_t b = { .p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), .cap = cap };
    if (!g || !ov || !b.p) { heap_caps_free(g); heap_caps_free(ov); heap_caps_free(b.p); httpd_resp_send_500(req); return ESP_FAIL; }
    if (!ui_main_stats_overview((uint8_t)scale, ov) ||
        !ui_main_stats_graph((uint8_t)scale, (uint32_t)offset, (uint8_t)group, tp, hp, g)) {
        heap_caps_free(g); heap_caps_free(ov); heap_caps_free(b.p);
        return send_simple(req, "503 Service Unavailable", "{\"error\":\"sd\"}");
    }

    jb_printf(&b, "{\"scale\":%d,\"offset\":%d,\"group\":%d,\"tp\":%d,\"hp\":%d,\"end\":%lu,\"ov\":{\"air_t\":[",
              scale, offset, group, tp, hp, (unsigned long)g->window_end);
    jb_mma(&b, &ov->air_t[0]); jb_printf(&b, ","); jb_mma(&b, &ov->air_t[1]);
    jb_printf(&b, "],\"air_h\":[");
    jb_mma(&b, &ov->air_h[0]); jb_printf(&b, ","); jb_mma(&b, &ov->air_h[1]);
    jb_printf(&b, "],\"agar_all\":");
    jb_mma(&b, &ov->agar_all);
    jb_printf(&b, ",\"agar\":[");
    for (int i = 0; i < 3; i++) {
        jb_printf(&b, "%s{\"name\":", i ? "," : "");
        jb_str(&b, ov->agar_name[i]);
        jb_en(&b, "name", ov->agar_name[i]);
        jb_printf(&b, ",\"v\":");
        jb_mma(&b, &ov->agar[i]);
        jb_printf(&b, "}");
    }
    jb_printf(&b, "],\"co2\":");
    jb_mma(&b, &ov->co2);
    jb_printf(&b, ",\"nh3\":");
    jb_mma(&b, &ov->nh3);
    jb_printf(&b, "},\"slots\":[");
    for (int sidx = 0; sidx < g->slot_count; sidx++) {
        const ui_stats_slot_t *sl = &g->slot[sidx];
        const ui_stats_series_t *d = &sl->d;
        jb_printf(&b, "%s{\"chan\":%u,\"name\":", sidx ? "," : "", (unsigned)sl->chan_type);
        jb_str(&b, sl->name);
        jb_en(&b, "name", sl->name);
        if (d->have_range) {
            jb_printf(&b, ",\"min\":");
            jb_float(&b, d->mn);
            jb_printf(&b, ",\"max\":");
            jb_float(&b, d->mx);
        }
        jb_printf(&b, ",\"v\":[");
        for (int i = 0; i < UI_STATS_POINTS; i++) {
            if (i) jb_printf(&b, ",");
            if (d->has[i]) jb_float(&b, d->vals[i]); else jb_printf(&b, "null");
        }
        jb_printf(&b, "],\"tr\":[");
        for (int i = 0; i < UI_STATS_POINTS; i++) {
            if (i) jb_printf(&b, ",");
            if (d->trend_has[i]) jb_float(&b, d->trend[i]); else jb_printf(&b, "null");
        }
        jb_printf(&b, "],\"lo\":\"");
        for (int i = 0; i < UI_STATS_POINTS; i++) jb_printf(&b, "%c", d->low[i] ? '1' : '0');
        jb_printf(&b, "\"}");
    }
    jb_printf(&b, "]}");
    heap_caps_free(g);
    heap_caps_free(ov);
    esp_err_t ret = send_jbuf(req, &b);
    heap_caps_free(b.p);
    return ret;
}

/* GET /api/stats/records?page= — 콘 Record 표 한 페이지(0 = 최신, 28개, 최신이 앞) */
static esp_err_t api_stats_records_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    if (stats_sd_blocked(req)) return ESP_OK;
    int page = query_int(s_w->query, "page", 0);
    if (page < 0) page = 0;
    stats_record_t *recs = heap_caps_malloc(sizeof(stats_record_t) * STATS_STORE_PAGE_SIZE, MALLOC_CAP_SPIRAM);
    const size_t cap = 4096;
    jbuf_t b = { .p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), .cap = cap };
    char *name = heap_caps_malloc(48, MALLOC_CAP_SPIRAM);
    if (!recs || !b.p || !name) { heap_caps_free(recs); heap_caps_free(b.p); heap_caps_free(name); httpd_resp_send_500(req); return ESP_FAIL; }
    uint32_t got = stats_store_read_page((uint32_t)page, STATS_STORE_PAGE_SIZE, recs, STATS_STORE_PAGE_SIZE);
    uint32_t total = stats_store_get_count();
    if ((got == 0 || total == 0) && stats_store_had_io_error()) {
        heap_caps_free(recs); heap_caps_free(b.p); heap_caps_free(name);
        return send_simple(req, "503 Service Unavailable", "{\"error\":\"sd\"}");
    }
    jb_printf(&b, "{\"count\":%lu,\"page\":%d,\"page_size\":%d,\"items\":[", (unsigned long)total, page, STATS_STORE_PAGE_SIZE);
    for (uint32_t i = 0; i < got; i++) {
        const stats_record_t *r = &recs[got - 1 - i];   /* 파일 순서(오래된 것부터) → 최신이 앞(콘 표와 같음) */
        ui_main_stats_device_name(r->mac, name, 48);
        jb_printf(&b, "%s{\"name\":", i ? "," : "");
        jb_str(&b, name);
        jb_en(&b, "name", name);
        jb_printf(&b, ",\"chan\":%u,\"v\":", (unsigned)r->chan_type);
        jb_float(&b, r->value);
        jb_printf(&b, ",\"t\":%lu}", (unsigned long)r->unix_time);
    }
    jb_printf(&b, "]}");
    heap_caps_free(recs);
    heap_caps_free(name);
    esp_err_t ret = send_jbuf(req, &b);
    heap_caps_free(b.p);
    return ret;
}

/* ── 설정(콘 설정 팝업·로그 탭과 같은 항목 + 웹 전용 알림 설정) ── */

/* GET /api/settings — 설정 팝업 한 장분 */
static esp_err_t api_settings_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    const size_t cap = 2048;
    jbuf_t b = { .p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), .cap = cap };
    notify_settings_t *ns = heap_caps_malloc(sizeof(*ns), MALLOC_CAP_SPIRAM);
    if (!b.p || !ns) { heap_caps_free(b.p); heap_caps_free(ns); httpd_resp_send_500(req); return ESP_FAIL; }
    notify_get_settings(ns);
    jb_printf(&b, "{\"auto_new\":%s,\"auto_known\":%s,\"resp\":%lu,\"resp_opts\":",
              device_config_get_auto_connect_new() ? "true" : "false", device_config_get_auto_connect_known() ? "true" : "false",
              (unsigned long)device_config_get_response_interval_sec());
    jb_values(&b, 5);
    jb_printf(&b, ",\"adapt\":%lu,\"adapt_opts\":", (unsigned long)device_config_get_adaptive_response_sec());
    jb_values(&b, 6);
    jb_printf(&b, ",\"time\":%lu,\"dev\":{\"save\":%u,\"mask\":%u,\"tag\":%u,\"tags\":[", (unsigned long)rtc_sync_get_unix_time(),
              (unsigned)dev_log_get_save_level(), (unsigned)dev_log_get_view_mask(), (unsigned)dev_log_get_tag_filter());
    for (int i = 0; i < DEV_LOG_TAG_COUNT; i++) {
        if (i) jb_printf(&b, ",");
        jb_str(&b, DEV_LOG_TAGS[i]);
    }
    jb_printf(&b, "]},\"notify\":{\"topic\":");
    jb_str(&b, ns->topic);
    jb_printf(&b, ",\"click\":");
    jb_str(&b, ns->click);
    jb_printf(&b, ",\"types\":%lu,\"count\":%d}}", (unsigned long)ns->types, NOTIFY_TYPE_COUNT);
    heap_caps_free(ns);
    esp_err_t ret = send_jbuf(req, &b);
    heap_caps_free(b.p);
    return ret;
}

/* POST /api/settings/set?key=auto_new|auto_known|resp|adapt|dev_save|dev_mask|dev_tag&val= — 고르는 즉시 저장(콘과 같음) */
static esp_err_t api_settings_set_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    static const struct { const char *key; ui_web_op_type_t type; } map[] = {
        { "auto_new", UI_WEB_OP_AUTO_NEW }, { "auto_known", UI_WEB_OP_AUTO_KNOWN }, { "resp", UI_WEB_OP_RESP_INTERVAL },
        { "adapt", UI_WEB_OP_ADAPTIVE }, { "dev_save", UI_WEB_OP_DEV_SAVE }, { "dev_mask", UI_WEB_OP_DEV_VIEW_MASK },
        { "dev_tag", UI_WEB_OP_DEV_TAG },
    };
    ui_web_op_t *op = &s_w->op;
    memset(op, 0, sizeof(*op));
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(s_w->key, map[i].key) == 0) op->type = map[i].type;
    }
    if (!op->type || s_w->val[0] == '\0') return send_simple(req, "400 Bad Request", "{\"ok\":false}");
    op->value = (uint32_t)strtoul(s_w->val, NULL, 10);
    bool ok = ui_main_run_web_op(op, 3000);
    return send_simple(req, ok ? NULL : "409 Conflict", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* POST /api/time?t=YYYY-MM-DD%20HH:MM:SS — 콘 시각 설정(콘 현지 시각) */
static esp_err_t api_time_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    ui_web_op_t *op = &s_w->op;
    memset(op, 0, sizeof(*op));
    op->type = UI_WEB_OP_SET_TIME;
    if (httpd_query_key_value(s_w->query, "t", op->text, sizeof(op->text)) != ESP_OK) return send_simple(req, "400 Bad Request", "{\"ok\":false}");
    for (char *p = op->text; *p; p++) if (*p == '+') *p = ' ';   /* 쿼리의 공백 */
    char *pc = strstr(op->text, "%20");
    if (pc) { *pc = ' '; memmove(pc + 1, pc + 3, strlen(pc + 3) + 1); }
    bool ok = ui_main_run_web_op(op, 3000);
    return send_simple(req, ok ? NULL : "400 Bad Request", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* POST /api/restart — 콘 장치 재시작(확인은 웹에서) */
static esp_err_t api_restart_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    ui_web_op_t *op = &s_w->op;
    memset(op, 0, sizeof(*op));
    op->type = UI_WEB_OP_RESTART;
    bool ok = ui_main_run_web_op(op, 3000);
    return send_simple(req, ok ? NULL : "409 Conflict", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* GET /api/logs?kind=gen|dev — 일반 로그 / 개발 로그(콘 화면과 같은 보기 설정으로 거른 줄), 줄바꿈 텍스트 */
#define WEB_LOG_CAP 8192
static esp_err_t api_logs_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    char *buf = heap_caps_malloc(WEB_LOG_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    buf[0] = '\0';
    httpd_query_key_value(s_w->query, "kind", s_w->key, sizeof(s_w->key));
    size_t len;
    if (strcmp(s_w->key, "dev") == 0) len = dev_log_render(buf, WEB_LOG_CAP);
    else { ui_log_get_snapshot(buf, WEB_LOG_CAP); len = strlen(buf); }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, buf, len);
    heap_caps_free(buf);
    return ret;
}

/* POST /api/notify/set — 본문 "topic\nclick\ntypes"(웹 전용 알림 설정, notify.cfg) */
static esp_err_t api_notify_set_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    if (!read_body(req, s_w->query, sizeof(s_w->query))) return send_simple(req, "400 Bad Request", "{\"ok\":false}");
    notify_settings_t *ns = heap_caps_calloc(1, sizeof(*ns), MALLOC_CAP_SPIRAM);
    if (!ns) { httpd_resp_send_500(req); return ESP_FAIL; }
    char *topic = s_w->query, *click = strchr(topic, '\n'), *types = NULL;
    bool ok = click != NULL;
    if (ok) { *click++ = '\0'; types = strchr(click, '\n'); ok = types != NULL; }
    if (ok) {
        *types++ = '\0';
        ok = strlen(topic) < sizeof(ns->topic) && strlen(click) < sizeof(ns->click);
    }
    if (ok) {
        strcpy(ns->topic, topic);
        strcpy(ns->click, click);
        ns->types = (uint32_t)strtoul(types, NULL, 10);
        ok = notify_set_settings(ns);
    }
    heap_caps_free(ns);
    return send_simple(req, ok ? NULL : "400 Bad Request", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* POST /api/notify/test — 시험 알림 하나(제목·내용은 ASCII) */
static esp_err_t api_notify_test_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    bool ok = notify_send("FlexFarm test", "Test notification from Cntl");
    return send_simple(req, ok ? NULL : "503 Service Unavailable", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* GET /api/alarms?page= — 알림 기록 한 페이지(0 = 최신, 50개, 최신이 앞) */
#define WEB_ALARM_PAGE 50
static esp_err_t api_alarms_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    int page = query_int(s_w->query, "page", 0);
    if (page < 0) page = 0;
    alarm_rec_t *recs = heap_caps_malloc(sizeof(alarm_rec_t) * WEB_ALARM_PAGE, MALLOC_CAP_SPIRAM);
    const size_t cap = 12288;
    jbuf_t b = { .p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), .cap = cap };
    if (!recs || !b.p) { heap_caps_free(recs); heap_caps_free(b.p); httpd_resp_send_500(req); return ESP_FAIL; }
    uint32_t last = alarm_last_id();
    int n = alarm_read_page((uint32_t)page, WEB_ALARM_PAGE, recs);
    uint32_t total = last < ALARM_HISTORY_MAX ? last : ALARM_HISTORY_MAX;
    jb_printf(&b, "{\"last\":%lu,\"count\":%lu,\"page\":%d,\"page_size\":%d,\"items\":[", (unsigned long)last, (unsigned long)total, page, WEB_ALARM_PAGE);
    for (int i = 0; i < n; i++) {
        jb_printf(&b, "%s{\"id\":%lu,\"t\":%lu,\"type\":%u,\"push\":%s,\"msg\":", i ? "," : "", (unsigned long)recs[i].id, (unsigned long)recs[i].t,
                  (unsigned)recs[i].type, (recs[i].flags & ALARM_F_PUSH) ? "true" : "false");
        jb_str(&b, recs[i].msg);
        jb_printf(&b, "}");
    }
    jb_printf(&b, "]}");
    heap_caps_free(recs);
    esp_err_t ret = send_jbuf(req, &b);
    heap_caps_free(b.p);
    return ret;
}

/* POST /api/alarms/ack?id= — 웹앱이 띄운 알림 번호까지 "받아 감"(그 뒤로는 ntfy로 안 보냄) */
static esp_err_t api_alarms_ack_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    int id = query_int(s_w->query, "id", 0);
    if (id > 0) alarm_ack_web((uint32_t)id);
    return send_simple(req, NULL, "{\"ok\":true}");
}

/* GET /api/errors — 콘 상태 아이콘 목록과 같은 에러·경고 코드(설명 문구는 SPA가 코드로 찾음 — build_web.py가 콘 표에서 뽑음) */
static esp_err_t api_errors_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    int *codes = heap_caps_malloc(sizeof(int) * (UI_ERR_HISTORY_CAP + UI_WARN_HISTORY_CAP), MALLOC_CAP_SPIRAM);
    const size_t cap = 512;
    jbuf_t b = { .p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), .cap = cap };
    if (!codes || !b.p) { heap_caps_free(codes); heap_caps_free(b.p); httpd_resp_send_500(req); return ESP_FAIL; }
    int ne = ui_main_get_error_codes(codes, UI_ERR_HISTORY_CAP);
    int nw = ui_main_get_warn_codes(codes + UI_ERR_HISTORY_CAP, UI_WARN_HISTORY_CAP);
    jb_printf(&b, "{\"errors\":[");
    for (int i = 0; i < ne; i++) jb_printf(&b, "%s%d", i ? "," : "", codes[i]);
    jb_printf(&b, "],\"warns\":[");
    for (int i = 0; i < nw; i++) jb_printf(&b, "%s%d", i ? "," : "", codes[UI_ERR_HISTORY_CAP + i]);
    jb_printf(&b, "]}");
    heap_caps_free(codes);
    esp_err_t ret = send_jbuf(req, &b);
    heap_caps_free(b.p);
    return ret;
}

/* POST /api/errors/clear?kind=e|w&code= , /api/sd/reconnect , /api/sd/format — 콘 목록 팝업의 지우기·해결(재마운트/포맷)과 같음 */
static esp_err_t err_op(httpd_req_t *req, ui_web_op_type_t type, uint32_t timeout_ms)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    ui_web_op_t *op = &s_w->op;
    memset(op, 0, sizeof(*op));
    op->type = type;
    httpd_query_key_value(s_w->query, "kind", s_w->tmp, sizeof(s_w->tmp));
    op->kind = (uint8_t)(s_w->tmp[0] == 'w' ? 'w' : 'e');
    op->value = (uint32_t)query_int(s_w->query, "code", 0);
    bool ok = ui_main_run_web_op(op, timeout_ms);
    return send_simple(req, ok ? NULL : "409 Conflict", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}
static esp_err_t api_errors_clear_post_handler(httpd_req_t *req) { return err_op(req, UI_WEB_OP_ERR_CLEAR, 3000); }
static esp_err_t api_sd_reconnect_post_handler(httpd_req_t *req) { return err_op(req, UI_WEB_OP_SD_RECONNECT, 30000); }
static esp_err_t api_sd_format_post_handler(httpd_req_t *req)    { return err_op(req, UI_WEB_OP_SD_FORMAT, 120000); }

/* 시험용 — 출시 전 제거(조건부 C9). POST /api/alarm_sim?type=0..6 — 실제 사건과 같은 alarm_post로 넣음.
 * 웹 세션은 건드리지 않음(로그인만 확인) — 세션 유무에 따른 받는 곳 나눔을 시험하려고 */
static esp_err_t api_alarm_sim_post_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return web_auth_reject(req);
    if (!work_ready(req)) return ESP_FAIL;
    /* ?err=코드 / ?warn=코드 — 실제 ui_log 에러·경고 기록 경로로(상태 아이콘·목록·알림 연결까지 시험) */
    int err_code = query_int(s_w->query, "err", 0), warn_code = query_int(s_w->query, "warn", 0);
    if (err_code > 0) { ui_log_add_err(err_code, "TEST error (simulated)"); return send_simple(req, NULL, "{\"ok\":true}"); }
    if (warn_code > 0) { ui_log_add_warn(warn_code, "TEST warning (simulated)"); return send_simple(req, NULL, "{\"ok\":true}"); }
    int type = query_int(s_w->query, "type", -1);
    if (type < 0 || type >= NOTIFY_TYPE_COUNT) return send_simple(req, "400 Bad Request", "{\"ok\":false}");
    static const char *const names[NOTIFY_TYPE_COUNT] = { "Error", "Warning", "Relay", "Device disconnected", "SD error", "Low battery", "CAN bus fault" };
    alarm_post((notify_type_t)type, names[type], "TEST %s (simulated)", names[type]);
    return send_simple(req, NULL, "{\"ok\":true}");
}

/* POST /api/stats/delete_all — 콘 Record 팝업 Delete All 확인 Yes와 같음 */
static esp_err_t api_stats_delete_all_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    if (!work_ready(req)) return ESP_FAIL;
    ui_web_op_t *op = &s_w->op;
    memset(op, 0, sizeof(*op));
    op->type = UI_WEB_OP_STATS_DELETE_ALL;
    bool ok = ui_main_run_web_op(op, 5000);
    return send_simple(req, ok ? NULL : "409 Conflict", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void web_api_register_handlers(httpd_handle_t server)
{
    s_server = server;
    s_ws_fd = -1;
    static const httpd_uri_t ws_uri = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
                                        .ws_pre_handshake_cb = ws_pre_handshake };
    static const httpd_uri_t alarms_ack_uri = { .uri = "/api/alarms/ack", .method = HTTP_POST, .handler = api_alarms_ack_post_handler };
    httpd_register_uri_handler(server, &ws_uri);
    httpd_register_uri_handler(server, &alarms_ack_uri);
    static const httpd_uri_t errors_uri = { .uri = "/api/errors", .method = HTTP_GET, .handler = api_errors_get_handler };
    static const httpd_uri_t errors_clear_uri = { .uri = "/api/errors/clear", .method = HTTP_POST, .handler = api_errors_clear_post_handler };
    static const httpd_uri_t sd_reconnect_uri = { .uri = "/api/sd/reconnect", .method = HTTP_POST, .handler = api_sd_reconnect_post_handler };
    static const httpd_uri_t sd_format_uri = { .uri = "/api/sd/format", .method = HTTP_POST, .handler = api_sd_format_post_handler };
    httpd_register_uri_handler(server, &errors_uri);
    httpd_register_uri_handler(server, &errors_clear_uri);
    httpd_register_uri_handler(server, &sd_reconnect_uri);
    httpd_register_uri_handler(server, &sd_format_uri);
    if (!s_ws_timer) {
        const esp_timer_create_args_t ta = { .callback = ws_timer_cb, .name = "ws_push" };
        if (esp_timer_create(&ta, &s_ws_timer) == ESP_OK) esp_timer_start_periodic(s_ws_timer, (uint64_t)WS_PUSH_MS * 1000);
    }
    static const httpd_uri_t stats_uri = { .uri = "/api/stats", .method = HTTP_GET, .handler = api_stats_get_handler };
    static const httpd_uri_t stats_records_uri = { .uri = "/api/stats/records", .method = HTTP_GET, .handler = api_stats_records_get_handler };
    static const httpd_uri_t stats_delete_all_uri = { .uri = "/api/stats/delete_all", .method = HTTP_POST, .handler = api_stats_delete_all_post_handler };
    httpd_register_uri_handler(server, &stats_uri);
    httpd_register_uri_handler(server, &stats_records_uri);
    httpd_register_uri_handler(server, &stats_delete_all_uri);
    static const httpd_uri_t settings_uri = { .uri = "/api/settings", .method = HTTP_GET, .handler = api_settings_get_handler };
    static const httpd_uri_t settings_set_uri = { .uri = "/api/settings/set", .method = HTTP_POST, .handler = api_settings_set_post_handler };
    static const httpd_uri_t time_uri = { .uri = "/api/time", .method = HTTP_POST, .handler = api_time_post_handler };
    static const httpd_uri_t restart_uri = { .uri = "/api/restart", .method = HTTP_POST, .handler = api_restart_post_handler };
    static const httpd_uri_t logs_uri = { .uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_get_handler };
    static const httpd_uri_t notify_set_uri = { .uri = "/api/notify/set", .method = HTTP_POST, .handler = api_notify_set_post_handler };
    static const httpd_uri_t notify_test_uri = { .uri = "/api/notify/test", .method = HTTP_POST, .handler = api_notify_test_post_handler };
    httpd_register_uri_handler(server, &settings_uri);
    httpd_register_uri_handler(server, &settings_set_uri);
    httpd_register_uri_handler(server, &time_uri);
    httpd_register_uri_handler(server, &restart_uri);
    httpd_register_uri_handler(server, &logs_uri);
    httpd_register_uri_handler(server, &notify_set_uri);
    httpd_register_uri_handler(server, &notify_test_uri);
    static const httpd_uri_t alarms_uri = { .uri = "/api/alarms", .method = HTTP_GET, .handler = api_alarms_get_handler };
    static const httpd_uri_t alarm_sim_uri = { .uri = "/api/alarm_sim", .method = HTTP_POST, .handler = api_alarm_sim_post_handler };
    httpd_register_uri_handler(server, &alarms_uri);
    httpd_register_uri_handler(server, &alarm_sim_uri);
    static const httpd_uri_t cam_list_uri = { .uri = "/api/cam/list", .method = HTTP_GET, .handler = api_cam_list_get_handler };
    static const httpd_uri_t cam_photos_uri = { .uri = "/api/cam/photos", .method = HTTP_GET, .handler = api_cam_photos_get_handler };
    static const httpd_uri_t cam_photo_uri = { .uri = "/api/cam/photo", .method = HTTP_GET, .handler = api_cam_photo_get_handler };
    static const httpd_uri_t cam_capture_uri = { .uri = "/api/cam/capture", .method = HTTP_POST, .handler = api_cam_capture_post_handler };
    static const httpd_uri_t cam_delete_uri = { .uri = "/api/cam/delete", .method = HTTP_POST, .handler = api_cam_delete_post_handler };
    static const httpd_uri_t cam_delete_all_uri = { .uri = "/api/cam/delete_all", .method = HTTP_POST, .handler = api_cam_delete_all_post_handler };
    httpd_register_uri_handler(server, &cam_list_uri);
    httpd_register_uri_handler(server, &cam_photos_uri);
    httpd_register_uri_handler(server, &cam_photo_uri);
    httpd_register_uri_handler(server, &cam_capture_uri);
    httpd_register_uri_handler(server, &cam_delete_uri);
    httpd_register_uri_handler(server, &cam_delete_all_uri);
    static const httpd_uri_t relay_uri = { .uri = "/api/relay", .method = HTTP_GET, .handler = api_relay_get_handler };
    static const httpd_uri_t relay_apply_uri = { .uri = "/api/relay/apply", .method = HTTP_POST, .handler = api_relay_apply_post_handler };
    static const httpd_uri_t relay_alias_uri = { .uri = "/api/relay/alias", .method = HTTP_POST, .handler = api_relay_alias_post_handler };
    static const httpd_uri_t relay_override_uri = { .uri = "/api/relay/override", .method = HTTP_POST, .handler = api_relay_override_post_handler };
    httpd_register_uri_handler(server, &relay_uri);
    httpd_register_uri_handler(server, &relay_apply_uri);
    httpd_register_uri_handler(server, &relay_alias_uri);
    httpd_register_uri_handler(server, &relay_override_uri);
    static const httpd_uri_t device_uri = { .uri = "/api/device", .method = HTTP_GET, .handler = api_device_get_handler };
    static const httpd_uri_t device_set_uri = { .uri = "/api/device/set", .method = HTTP_POST, .handler = api_device_set_post_handler };
    static const httpd_uri_t device_unpair_uri = { .uri = "/api/device/unpair", .method = HTTP_POST, .handler = api_device_unpair_post_handler };
    httpd_register_uri_handler(server, &device_uri);
    httpd_register_uri_handler(server, &device_set_uri);
    httpd_register_uri_handler(server, &device_unpair_uri);
    static const httpd_uri_t dashboard_uri = { .uri = "/api/dashboard", .method = HTTP_GET, .handler = api_dashboard_get_handler };
    httpd_register_uri_handler(server, &dashboard_uri);
}
