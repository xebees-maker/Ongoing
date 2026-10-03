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

/* GET /api/dashboard — 콘 주화면 한 장분: 상단바(시각·네트워크·상태), Summary(메모리·저장 공간), Power Control(릴레이),
 * Sensor·Camera 목록. SPA가 5초마다 부름(웹 세션 유지 신호를 겸함) */
static esp_err_t api_dashboard_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    const size_t cap = 8192;
    jbuf_t b = { .p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), .cap = cap };
    node_hub_node_t *nodes = heap_caps_malloc(sizeof(node_hub_node_t) * NODE_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    if (!b.p || !nodes) {
        heap_caps_free(b.p);
        heap_caps_free(nodes);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* 상단바 */
    bool err = false, warn = false, sd_fail = false;
    ui_main_get_status_flags(&err, &warn, &sd_fail);
    bool ap = device_config_get_wifi_ap_mode();
    const char *ssid = ap ? wifi_sta_get_ap_ssid() : wifi_sta_get_active_ssid();
    jb_printf(&b, "{\"time\":%lu,\"net\":{\"mode\":\"%s\",\"ssid\":", (unsigned long)rtc_sync_get_unix_time(), ap ? "ap" : "sta");
    jb_str(&b, ssid ? ssid : "");
    jb_printf(&b, ",\"ip\":%s},\"status\":\"%s\"", wifi_sta_get_own_ip_str()[0] ? "true" : "false",
              (err || sd_fail) ? "error" : warn ? "warning" : "normal");

    /* Summary — 메모리(바이트), 저장 공간(영역별 사용%·여유MB, 콘 Summary와 같은 계산) */
    jb_printf(&b, ",\"mem\":{\"i\":%u,\"p\":%u}", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
        jb_printf(&b, ",\"on\":%s", power_relay_get_commanded_on(i) ? "true" : "false");
        if (c) {
            jb_printf(&b, ",\"configured\":%s,\"override\":%s,\"override_on\":%s,\"turns_on\":%s,\"rises\":%s,\"chan\":%u,\"center\":",
                      c->configured ? "true" : "false", c->manual_override ? "true" : "false",
                      c->manual_override_on ? "true" : "false", c->z_turns_on ? "true" : "false",
                      c->y_rises ? "true" : "false", (unsigned)c->chan_type);
            jb_float(&b, (c->on_threshold + c->off_threshold) / 2.0f);
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
    jb_printf(&b, "]}");
    heap_caps_free(nodes);

    esp_err_t ret = send_jbuf(req, &b);
    heap_caps_free(b.p);
    return ret;
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

/* POST /api/relay/apply?idx= — 본문 "chan=&y=&z=&center=&margin=&ai=&manual=&src=&gc=&dev=&stat=&trend=&ts=&hold="
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
    c->source_kind = (uint8_t)query_int(body, "src", 0);
    c->group_choice = (uint8_t)query_int(body, "gc", 0);
    c->device_valid = httpd_query_key_value(body, "dev", s_w->tmp, sizeof(s_w->tmp)) == ESP_OK && hex_to_mac(s_w->tmp, c->device_mac);
    c->stat = (uint8_t)query_int(body, "stat", 0);
    c->trend_enable = query_int(body, "trend", 0) != 0;
    c->trend_samples = (uint32_t)query_int(body, "ts", 0);
    c->min_hold_sec = (uint32_t)query_int(body, "hold", 0);
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
    jb_printf(&b, "{\"cams\":[");
    int out = 0;
    for (int i = 0; i < n; i++) {   /* 연결된 캠(연결 대기 제외 — 콘 주화면과 같음) */
        hub_conn_state_t cs = node_hub_get_conn_state(nodes[i].mac);
        if (cs == HUB_CONN_STATE_WAITING) continue;
        jb_printf(&b, "%s{\"mac\":", out++ ? "," : "");
        jb_mac(&b, nodes[i].mac);
        jb_printf(&b, ",\"name\":");
        const char *alias = device_config_get_alias(nodes[i].mac);
        jb_str(&b, alias[0] ? alias : nodes[i].name);
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
        if (alias[0]) jb_str(&b, alias);
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

void web_api_register_handlers(httpd_handle_t server)
{
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
