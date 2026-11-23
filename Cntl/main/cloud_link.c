#include "cloud_link.h"
#include "fs.h"
#include "web_auth.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"

static const char *TAG = "CLOUD";

#define CLOUD_CFG_PATH  FS_MOUNT_POINT "/cloud.cfg"
#define CLOUD_LINE_LEN  200
#define CLOUD_TOPIC_LEN 96

typedef struct {
    char uri[128];
    char user[64];
    char pass[64];
    char site[32];
    /* 2026-11-23(사용자 결정 — 원격 접속 QR) — 웹앱(GitHub Pages)이 쓸 값. 없으면 기본값(아래 cloud_link_web_qr_url).
     * 현장별 웹 계정(출시 전 할 일)이 생기면 web_user/web_pass를 cloud.cfg에 넣음 */
    char web_uri[128];
    char web_user[64];
    char web_pass[64];
    char web_app[96];
    char topic_status[CLOUD_TOPIC_LEN];
    char topic_req_sub[CLOUD_TOPIC_LEN];   /* ff/<site>/req/+ */
} cloud_cfg_t;

/* 설정·줄 버퍼는 PSRAM(feedback_prefer_psram_for_buffers). mqtt 클라이언트는 문자열을 복사하므로 init 뒤에도 그대로 둠 */
static cloud_cfg_t *s_cfg = NULL;
static char *s_line = NULL;
static SemaphoreHandle_t s_net_ready = NULL;
static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static volatile bool s_first_connect_logged = false;

static size_t internal_free(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

static bool load_cfg(void)
{
    memset(s_cfg, 0, sizeof(*s_cfg));
    FILE *f = fopen(CLOUD_CFG_PATH, "r");
    if (!f) return false;
    while (fgets(s_line, CLOUD_LINE_LEN, f)) {
        size_t n = strlen(s_line);
        while (n > 0 && (s_line[n - 1] == '\n' || s_line[n - 1] == '\r' || s_line[n - 1] == ' ')) s_line[--n] = '\0';
        if (strncmp(s_line, "uri=", 4) == 0) snprintf(s_cfg->uri, sizeof(s_cfg->uri), "%s", s_line + 4);
        else if (strncmp(s_line, "user=", 5) == 0) snprintf(s_cfg->user, sizeof(s_cfg->user), "%s", s_line + 5);
        else if (strncmp(s_line, "pass=", 5) == 0) snprintf(s_cfg->pass, sizeof(s_cfg->pass), "%s", s_line + 5);
        else if (strncmp(s_line, "site=", 5) == 0) snprintf(s_cfg->site, sizeof(s_cfg->site), "%s", s_line + 5);
        else if (strncmp(s_line, "web_uri=", 8) == 0) snprintf(s_cfg->web_uri, sizeof(s_cfg->web_uri), "%s", s_line + 8);
        else if (strncmp(s_line, "web_user=", 9) == 0) snprintf(s_cfg->web_user, sizeof(s_cfg->web_user), "%s", s_line + 9);
        else if (strncmp(s_line, "web_pass=", 9) == 0) snprintf(s_cfg->web_pass, sizeof(s_cfg->web_pass), "%s", s_line + 9);
        else if (strncmp(s_line, "web_app=", 8) == 0) snprintf(s_cfg->web_app, sizeof(s_cfg->web_app), "%s", s_line + 8);
    }
    fclose(f);
    if (!s_cfg->site[0]) snprintf(s_cfg->site, sizeof(s_cfg->site), "unnamed");
    snprintf(s_cfg->topic_status, sizeof(s_cfg->topic_status), "ff/%s/status", s_cfg->site);
    snprintf(s_cfg->topic_req_sub, sizeof(s_cfg->topic_req_sub), "ff/%s/req/+", s_cfg->site);
    return s_cfg->uri[0] != '\0';
}

/* ════════════════════════════════════════════════════════════
 * 2026-11-22(사용자 결정 — 3단계 읽기 터널) — 밖의 웹앱(정적 호스팅)이 MQTT로 보낸 웹 요청을 콘의 웹 서버에 그대로 넘기고
 * 응답을 MQTT로 돌려줌. 웹 API 핸들러는 손대지 않음: 이 콘 안에서 127.0.0.1:80으로 같은 HTTP 요청을 보냄(인증·세션·
 * 409/503 등 웹앱이 아는 동작이 그대로). 접속 기록에는 127.0.0.1로 남음.
 *  - 요청 토픽 ff/<site>/req/<cid>, 응답 토픽 ff/<site>/res/<cid>(cid = 웹앱이 정한 임의 값). QoS 1.
 *  - 요청 = 줄 4개 + 본문: "<id>\n<METHOD> <경로>\n<Cookie 값>\n<Content-Type>\n<본문>"
 *  - 응답 = 조각마다 JSON 머리 한 줄 + "\n" + 본문 조각(최대 TUN_CHUNK):
 *      {"id":<id>,"i":<조각 번호>,"n":<조각 수>,"s":<HTTP 상태>,"ct":"...","sc":"<Set-Cookie>","rs":<X-Cntl-Restoring 있으면 1>}
 *  - 넘기는 요청은 tunnel_allowed(): 웹 비밀번호가 있으면 전부, 없으면 읽기와 로그인·세션 되찾기·비밀번호 정하기만.
 *    막힌 요청은 403 {"error":"remote_readonly"}. /admin·/ws는 늘 막음.
 *  - MQTT 이벤트 처리기는 PSRAM 스택 MQTT 태스크에서 불리므로 복사해서 큐에 넣기만 함 — 실제 요청은 tunnel 태스크.
 * ════════════════════════════════════════════════════════════ */
#define TUN_REQ_MAX     (16 * 1024)   /* 요청(머리+본문) 최대 — 3단계는 로그인 정도 */
#define TUN_RESP_MAX    (512 * 1024)  /* 응답 최대 — 사진(지금 최대 약 170KB)·통계 여유 */
#define TUN_CHUNK       (8 * 1024)
#define TUN_QUEUE_LEN   4
#define TUN_CID_LEN     24
#define TUN_TIMEOUT_S   130           /* SD 포맷(최대 120초)보다 길게 */

typedef struct {
    char cid[TUN_CID_LEN];
    size_t len;
    uint8_t data[];                   /* 요청 원문 */
} tun_job_t;

typedef struct {
    int status;
    char ctype[64];
    char set_cookie[160];
    bool restoring;
    uint8_t *buf;                     /* 받은 원문 전체(머리+본문) → 본문은 body부터 */
    size_t cap, len;
    uint8_t *body;
    size_t body_len;
} tun_resp_t;

static QueueHandle_t s_tun_q = NULL;
static tun_job_t *s_rx = NULL;        /* 조각으로 오는 MQTT 메시지를 모으는 중인 것 */
static size_t s_rx_want = 0;
static char *s_tun_hdr = NULL;        /* 응답 머리·조각 버퍼(PSRAM) */
static char *s_tun_http = NULL;       /* 보낼 HTTP 요청 머리 */

static void *psram_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

/* MQTT 태스크 — 요청 토픽 메시지를 모아서 큐로. 큐가 차면 버림(웹앱이 시간 초과로 다시 물음) */
static void tunnel_on_data(esp_mqtt_event_handle_t ev)
{
    if (ev->current_data_offset == 0) {
        if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
        const char *slash = NULL;
        for (int i = ev->topic_len - 1; i >= 0; i--) if (ev->topic[i] == '/') { slash = ev->topic + i; break; }
        int cid_len = slash ? (int)(ev->topic + ev->topic_len - slash - 1) : 0;
        if (cid_len <= 0 || cid_len >= TUN_CID_LEN || ev->total_data_len <= 0 || ev->total_data_len > TUN_REQ_MAX) {
            ESP_LOGW(TAG, "Tunnel: bad request (cid %d, %d bytes)", cid_len, ev->total_data_len);
            return;
        }
        s_rx = psram_alloc(sizeof(tun_job_t) + ev->total_data_len + 1);
        if (!s_rx) return;
        memcpy(s_rx->cid, slash + 1, cid_len);
        s_rx->cid[cid_len] = '\0';
        s_rx->len = 0;
        s_rx_want = ev->total_data_len;
    }
    if (!s_rx) return;
    if (ev->current_data_offset + ev->data_len > (int)s_rx_want) { heap_caps_free(s_rx); s_rx = NULL; return; }
    memcpy(s_rx->data + ev->current_data_offset, ev->data, ev->data_len);
    s_rx->len = ev->current_data_offset + ev->data_len;
    if (s_rx->len < s_rx_want) return;
    s_rx->data[s_rx->len] = '\0';
    tun_job_t *job = s_rx;
    s_rx = NULL;
    if (xQueueSend(s_tun_q, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Tunnel: queue full - request dropped");
        heap_caps_free(job);
    }
}

/* 요청 원문에서 n번째 줄(0부터)의 시작·길이. 줄 끝은 \n */
static bool line_at(const uint8_t *d, size_t len, int n, const char **out, size_t *out_len, size_t *next)
{
    size_t p = 0;
    for (int i = 0; i < n; i++) {
        while (p < len && d[p] != '\n') p++;
        if (p >= len) return false;
        p++;
    }
    size_t e = p;
    while (e < len && d[e] != '\n') e++;
    if (e >= len) return false;
    *out = (const char *)d + p;
    *out_len = e - p;
    *next = e + 1;
    return true;
}

static bool resp_grow(tun_resp_t *r, size_t need)
{
    if (need <= r->cap) return true;
    if (need > TUN_RESP_MAX) return false;
    size_t nc = r->cap ? r->cap : 4096;
    while (nc < need) nc *= 2;
    if (nc > TUN_RESP_MAX) nc = TUN_RESP_MAX;
    uint8_t *nb = heap_caps_realloc(r->buf, nc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nb) return false;
    r->buf = nb;
    r->cap = nc;
    return true;
}

/* 머리 한 줄 "Name: value"에서 name이 맞으면 값 복사(대소문자 무시) */
static bool hdr_val(const char *line, size_t ll, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    if (ll <= nl || strncasecmp(line, name, nl) != 0 || line[nl] != ':') return false;
    const char *v = line + nl + 1;
    size_t vl = ll - nl - 1;
    while (vl && *v == ' ') { v++; vl--; }
    if (out && cap) { size_t c = vl < cap - 1 ? vl : cap - 1; memcpy(out, v, c); out[c] = '\0'; }
    return true;
}

/* 받은 chunked 본문을 그 자리에서 풂. 끝(0 조각)까지 왔으면 true */
static bool dechunk(uint8_t *b, size_t len, size_t *out_len)
{
    size_t rp = 0, wp = 0;
    while (rp < len) {
        size_t e = rp;
        while (e + 1 < len && !(b[e] == '\r' && b[e + 1] == '\n')) e++;
        if (e + 1 >= len) return false;
        unsigned long n = strtoul((const char *)b + rp, NULL, 16);
        rp = e + 2;
        if (n == 0) { *out_len = wp; return true; }
        if (rp + n + 2 > len) return false;
        memmove(b + wp, b + rp, n);
        wp += n;
        rp += n + 2;
    }
    return false;
}

/* 127.0.0.1:80으로 요청 하나. 응답 상태(실패면 0) */
static int loopback_http(const char *method, size_t ml, const char *path, size_t pl, const char *cookie, size_t cl,
                         const char *ctype, size_t tl, const uint8_t *body, size_t bl, tun_resp_t *r)
{
    int hl = snprintf(s_tun_http, 1024, "%.*s %.*s HTTP/1.1\r\nHost: 127.0.0.1\r\n%s%.*s%s%s%.*s%sContent-Length: %u\r\n\r\n",
                      (int)ml, method, (int)pl, path, cl ? "Cookie: " : "", (int)cl, cookie, cl ? "\r\n" : "",
                      tl ? "Content-Type: " : "", (int)tl, ctype, tl ? "\r\n" : "", (unsigned)bl);
    if (hl <= 0 || hl >= 1024) return 0;
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) { ESP_LOGW(TAG, "Tunnel: socket failed (%d)", errno); return 0; }
    struct timeval tv = { .tv_sec = TUN_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 10;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(80) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int status = 0;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { ESP_LOGW(TAG, "Tunnel: connect failed (%d)", errno); goto done; }
    if (send(s, s_tun_http, hl, 0) != hl) goto done;
    for (size_t off = 0; off < bl;) {
        int n = send(s, body + off, bl - off, 0);
        if (n <= 0) goto done;
        off += n;
    }
    /* 머리 끝까지 받고, 본문은 Content-Length / chunked / 닫힘까지 */
    r->len = 0;
    size_t hdr_end = 0;
    long clen = -1;
    bool chunked = false;
    for (;;) {
        if (!resp_grow(r, r->len + 2048)) { ESP_LOGW(TAG, "Tunnel: response too large"); goto done; }
        int n = recv(s, r->buf + r->len, r->cap - r->len, 0);
        if (n <= 0) break;
        r->len += n;
        if (!hdr_end) {
            for (size_t i = 3; i < r->len; i++) {
                if (r->buf[i - 3] == '\r' && r->buf[i - 2] == '\n' && r->buf[i - 1] == '\r' && r->buf[i] == '\n') { hdr_end = i + 1; break; }
            }
            if (!hdr_end) continue;
            /* 상태 줄과 필요한 머리 */
            const char *h = (const char *)r->buf;
            const char *sp = memchr(h, ' ', hdr_end);
            r->status = sp ? atoi(sp + 1) : 0;
            size_t p = 0;
            while (p < hdr_end) {
                size_t e = p;
                while (e + 1 < hdr_end && !(h[e] == '\r' && h[e + 1] == '\n')) e++;
                const char *ln = h + p;
                size_t ll = e - p;
                char tmp[24];
                if (hdr_val(ln, ll, "Content-Length", tmp, sizeof(tmp))) clen = atol(tmp);
                else if (hdr_val(ln, ll, "Transfer-Encoding", tmp, sizeof(tmp))) chunked = strncasecmp(tmp, "chunked", 7) == 0;
                else if (hdr_val(ln, ll, "Content-Type", r->ctype, sizeof(r->ctype))) {}
                else if (hdr_val(ln, ll, "Set-Cookie", r->set_cookie, sizeof(r->set_cookie))) {}
                else if (hdr_val(ln, ll, "X-Cntl-Restoring", NULL, 0)) r->restoring = true;
                p = e + 2;
            }
        }
        size_t got = r->len - hdr_end;
        if (clen >= 0 && !chunked && got >= (size_t)clen) break;
        if (chunked && got >= 5 && memcmp(r->buf + r->len - 5, "0\r\n\r\n", 5) == 0) break;
    }
    if (!hdr_end || !r->status) goto done;
    r->body = r->buf + hdr_end;
    r->body_len = r->len - hdr_end;
    if (chunked && !dechunk(r->body, r->body_len, &r->body_len)) { ESP_LOGW(TAG, "Tunnel: bad chunked body"); goto done; }
    if (clen >= 0 && !chunked && r->body_len > (size_t)clen) r->body_len = clen;
    status = r->status;
done:
    close(s);
    return status;
}

/* JSON 문자열 안에 넣을 수 있게(따옴표·역슬래시·제어문자 빼기) */
static void json_safe(char *s)
{
    for (; *s; s++) if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20) *s = ' ';
}

static void tunnel_reply(const char *cid, const char *id, size_t idl, int status, const tun_resp_t *r)
{
    char topic[CLOUD_TOPIC_LEN];
    snprintf(topic, sizeof(topic), "ff/%s/res/%s", s_cfg->site, cid);
    const uint8_t *body = r ? r->body : NULL;
    size_t bl = r ? r->body_len : 0;
    if (!r) { static const char ro[] = "{\"error\":\"remote_readonly\"}"; body = (const uint8_t *)ro; bl = sizeof(ro) - 1; }
    unsigned n = bl ? (unsigned)((bl + TUN_CHUNK - 1) / TUN_CHUNK) : 1;
    for (unsigned i = 0; i < n; i++) {
        int hl;
        if (i == 0) {
            hl = snprintf(s_tun_hdr, 512, "{\"id\":%.*s,\"i\":0,\"n\":%u,\"s\":%d,\"ct\":\"%s\",\"sc\":\"%s\",\"rs\":%d}\n", (int)idl, id, n, status,
                          r ? r->ctype : "application/json", r ? r->set_cookie : "", (r && r->restoring) ? 1 : 0);
        } else {
            hl = snprintf(s_tun_hdr, 512, "{\"id\":%.*s,\"i\":%u,\"n\":%u}\n", (int)idl, id, i, n);
        }
        size_t off = (size_t)i * TUN_CHUNK, cl = bl - off < TUN_CHUNK ? bl - off : TUN_CHUNK;
        if (bl) memcpy(s_tun_hdr + hl, body + off, cl);
        else cl = 0;
        if (esp_mqtt_client_publish(s_client, topic, s_tun_hdr, hl + (int)cl, 1, 0) < 0) {
            ESP_LOGW(TAG, "Tunnel: publish failed (%u/%u)", i, n);
            return;
        }
    }
}

/* 2026-11-22(사용자 결정 — 4단계) — 콘에 PIN이 있으면 모든 요청을 넘김(쓰기도 콘의 로그인 확인을 그대로 거침).
 * 2026-11-23(사용자 결정) — PIN은 콘 화면에서만 처음 정함. 없으면 읽기(로그인 페이지 — API는 콘이 401) + 로그인(콘이 403 nopin)만.
 * /admin(파일 올리기)·/ws는 늘 막음 */
static bool tunnel_allowed(const char *m, size_t ml, const char *p, size_t pl)
{
    if (pl < 1 || p[0] != '/') return false;
    if ((pl >= 6 && strncmp(p, "/admin", 6) == 0) || (pl >= 3 && strncmp(p, "/ws", 3) == 0 && (pl == 3 || p[3] == '?'))) return false;
    if (ml == 3 && strncmp(m, "GET", 3) == 0) return true;
    if (web_auth_enabled()) return true;
    if (ml == 4 && strncmp(m, "POST", 4) == 0) {
        static const char *ok[] = { "/api/login" };
        for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
            size_t l = strlen(ok[i]);
            if (pl >= l && strncmp(p, ok[i], l) == 0 && (pl == l || p[l] == '?')) return true;
        }
    }
    return false;
}

static void tunnel_task(void *arg)
{
    (void)arg;
    tun_resp_t *r = psram_alloc(sizeof(tun_resp_t));
    if (!r) { vTaskDelete(NULL); return; }
    memset(r, 0, sizeof(*r));
    for (;;) {
        tun_job_t *job;
        if (xQueueReceive(s_tun_q, &job, portMAX_DELAY) != pdTRUE) continue;
        const char *id, *ml, *ck, *ct;
        size_t idl, mll, ckl, ctl, nx;
        if (!line_at(job->data, job->len, 0, &id, &idl, &nx) || !line_at(job->data, job->len, 1, &ml, &mll, &nx) ||
            !line_at(job->data, job->len, 2, &ck, &ckl, &nx) || !line_at(job->data, job->len, 3, &ct, &ctl, &nx) || idl == 0 || idl > 12) {
            ESP_LOGW(TAG, "Tunnel: malformed request");
            heap_caps_free(job);
            continue;
        }
        bool id_ok = true;
        for (size_t i = 0; i < idl; i++) if (id[i] < '0' || id[i] > '9') id_ok = false;
        const char *sp = memchr(ml, ' ', mll);
        if (!id_ok || !sp) { ESP_LOGW(TAG, "Tunnel: malformed request line"); heap_caps_free(job); continue; }
        size_t m_len = sp - ml;
        const char *path = sp + 1;
        size_t p_len = mll - m_len - 1;
        if (!tunnel_allowed(ml, m_len, path, p_len)) {
            tunnel_reply(job->cid, id, idl, 403, NULL);
            heap_caps_free(job);
            continue;
        }
        memset(r->ctype, 0, sizeof(r->ctype));
        r->set_cookie[0] = '\0';
        r->restoring = false;
        r->status = 0;
        r->body = NULL;
        r->body_len = 0;
        int st = loopback_http(ml, m_len, path, p_len, ck, ckl, ct, ctl, job->data + nx, job->len - nx, r);
        if (st == 0) {   /* 콘 웹 서버에 못 넘김 → 502 */
            static const char e502[] = "{\"error\":\"tunnel\"}";
            strcpy(r->ctype, "application/json");
            r->set_cookie[0] = 0;
            r->restoring = false;
            r->body = (uint8_t *)e502;
            r->body_len = sizeof(e502) - 1;
            tunnel_reply(job->cid, id, idl, 502, r);
        } else {
            json_safe(r->ctype);
            json_safe(r->set_cookie);
            tunnel_reply(job->cid, id, idl, st, r);
        }
        heap_caps_free(job);
        /* 큰 응답(사진) 뒤엔 버퍼를 줄여 PSRAM을 돌려줌 */
        if (r->cap > 64 * 1024) { heap_caps_free(r->buf); r->buf = NULL; r->cap = 0; }
    }
}

static bool tunnel_start(void)
{
    s_tun_q = xQueueCreate(TUN_QUEUE_LEN, sizeof(tun_job_t *));
    s_tun_hdr = psram_alloc(512 + TUN_CHUNK);
    s_tun_http = psram_alloc(1024);
    if (!s_tun_q || !s_tun_hdr || !s_tun_http) return false;
    /* 스택 PSRAM — 소켓·MQTT 발행만 하고 플래시는 안 씀(핸들러는 웹 서버 태스크에서 돔) */
    return xTaskCreatePinnedToCoreWithCaps(tunnel_task, "tunnel", 6144, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM) == pdPASS;
}

/* MQTT 태스크(스택 PSRAM)에서 불림 — 플래시 쓰기 금지(sdkconfig.defaults 주석). 지금은 상태 기록·발행만 */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t ev = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGI(TAG, "MEM connecting: internal free=%u", (unsigned)internal_free());
            break;
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            esp_mqtt_client_publish(ev->client, s_cfg->topic_status, "online", 0, 1, 1);
            if (s_tun_q) esp_mqtt_client_subscribe(ev->client, s_cfg->topic_req_sub, 1);   /* 세션이 새로 시작되므로 접속마다 */
            ESP_LOGI(TAG, "Connected - internal free=%u", (unsigned)internal_free());
            if (!s_first_connect_logged) {
                s_first_connect_logged = true;
                /* 연결 과정(TLS 핸드셰이크 포함)의 최저치 — init 직전에 시작한 감시 구간 */
                size_t min_i = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
                heap_caps_monitor_local_minimum_free_size_stop();
                ESP_LOGI(TAG, "MEM handshake min internal free=%u", (unsigned)min_i);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "Disconnected - internal free=%u", (unsigned)internal_free());
            break;
        case MQTT_EVENT_DATA:
            if (s_tun_q) tunnel_on_data(ev);
            break;
        case MQTT_EVENT_ERROR:
            if (ev->error_handle) {
                ESP_LOGW(TAG, "Error type=%d tls=0x%x sock=%d conn_rc=%d", ev->error_handle->error_type,
                         (unsigned)ev->error_handle->esp_tls_last_esp_err, ev->error_handle->esp_transport_sock_errno,
                         ev->error_handle->connect_return_code);
            }
            break;
        default:
            break;
    }
}

static void cloud_task(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_net_ready, portMAX_DELAY);
    if (!load_cfg()) {
        ESP_LOGI(TAG, "No uri in cloud.cfg - cloud link off");
        vTaskDelete(NULL);
        return;
    }
    /* Wi-Fi가 IP를 받고 부팅 직후 할당이 가라앉을 시간 — 측정 기준선을 부팅 소란과 섞지 않으려고 */
    vTaskDelay(pdMS_TO_TICKS(30000));
    size_t before = internal_free();
    heap_caps_monitor_local_minimum_free_size_start();
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = s_cfg->uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = s_cfg->user[0] ? s_cfg->user : NULL,
        .credentials.client_id = s_cfg->site,   /* 기본값(칩 MAC 뒷자리)을 쓰지 않음 */
        .credentials.authentication.password = s_cfg->pass[0] ? s_cfg->pass : NULL,
        .session.last_will = { .topic = s_cfg->topic_status, .msg = "offline", .qos = 1, .retain = 1 },
        /* 11-22 실측 — 120이면 첫 PING(절반 = 60초) 전에 브로커가 60초 무통신으로 닫았음(test.mosquitto.org). 60 → 30초마다 PING */
        .session.keepalive = 60,
        .task.stack_size = 6144,
        .task.priority = 5,          /* 알림과 같은 수준(통신 17·SR 15·파일 10보다 낮음) */
        /* 터널 — 받는 요청은 작고(2KB), 보내는 응답은 TUN_CHUNK 조각(+머리). 두 버퍼 다 PSRAM(sdkconfig.defaults) */
        .buffer.size = 2048,
        .buffer.out_size = TUN_CHUNK + 1024,
    };
    if (!tunnel_start()) ESP_LOGE(TAG, "Tunnel start failed - remote web off");
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        heap_caps_monitor_local_minimum_free_size_stop();
        ESP_LOGE(TAG, "mqtt client init failed");
        vTaskDelete(NULL);
        return;
    }
    size_t after_init = internal_free();
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MEM before=%u after_init=%u after_start=%u (uri %s)", (unsigned)before, (unsigned)after_init,
             (unsigned)internal_free(), s_cfg->uri);
    /* 연결 뒤 1분 동안 10초마다 내부 RAM — 안정 상태 확인 */
    for (int i = 0; i < 6; i++) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "MEM steady %ds: internal free=%u connected=%d", (i + 1) * 10, (unsigned)internal_free(), (int)s_connected);
    }
    vTaskDelete(NULL);
}

void cloud_link_network_ready(void)
{
    if (s_net_ready) xSemaphoreGive(s_net_ready);
}

bool cloud_link_is_connected(void)
{
    return s_connected;
}

/* 주소에 넣을 값 — 영숫자와 -_.~ 말고는 %XX */
static size_t url_enc(char *out, size_t n, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = '\0';
    return o;
}

/* 2026-11-23(사용자 결정 — 원격 접속 QR) — 웹앱 주소 + #b=<브로커 wss>&s=<현장>&u=<계정>&p=<비밀번호>(웹앱이 받아 저장하고 주소에서 지움).
 * 기본값: 웹앱 = GitHub Pages(flexfarm-web), 브로커 = 콘 주소의 호스트로 wss://<호스트>:8084/mqtt(EMQX 웹소켓 TLS), 계정 = 콘과 같음.
 * 사용자에게 보이는 곳은 PIN으로 잠긴 콘 화면 QR뿐. cloud.cfg를 아직 못 읽었으면 false */
bool cloud_link_web_qr_url(char *out, size_t n, size_t *base_len)
{
    if (!s_cfg || !s_cfg->uri[0] || !out || n < 64) return false;
    char wss[160];
    if (s_cfg->web_uri[0]) snprintf(wss, sizeof(wss), "%s", s_cfg->web_uri);
    else {
        const char *h = strstr(s_cfg->uri, "://");
        h = h ? h + 3 : s_cfg->uri;
        size_t hl = strcspn(h, ":/");
        snprintf(wss, sizeof(wss), "wss://%.*s:8084/mqtt", (int)hl, h);
    }
    const char *app = s_cfg->web_app[0] ? s_cfg->web_app : "https://xebees-maker.github.io/flexfarm-web/";
    const char *user = s_cfg->web_user[0] ? s_cfg->web_user : s_cfg->user;
    const char *pass = s_cfg->web_user[0] ? s_cfg->web_pass : s_cfg->pass;
    size_t o = (size_t)snprintf(out, n, "%s", app);
    if (base_len) *base_len = o;
    if (o + 4 >= n) return false;
    o += (size_t)snprintf(out + o, n - o, "#b=");
    o += url_enc(out + o, n - o, wss);
    o += (size_t)snprintf(out + o, n - o, "&s=");
    o += url_enc(out + o, n - o, s_cfg->site);
    o += (size_t)snprintf(out + o, n - o, "&u=");
    o += url_enc(out + o, n - o, user);
    o += (size_t)snprintf(out + o, n - o, "&p=");
    o += url_enc(out + o, n - o, pass);
    return o + 1 < n;
}

void cloud_link_init(void)
{
    if (s_cfg) return;
    s_cfg =heap_caps_calloc(1, sizeof(cloud_cfg_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_line = heap_caps_malloc(CLOUD_LINE_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_net_ready = xSemaphoreCreateBinary();
    if (!s_cfg || !s_line || !s_net_ready) {
        ESP_LOGE(TAG, "alloc failed - cloud link off");
        return;
    }
    /* 스택 PSRAM(notify와 같음) — 파일 읽기는 이 태스크에서 연결 전에 한 번만 */
    if (xTaskCreatePinnedToCoreWithCaps(cloud_task, "cloud", 4096, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "task create failed - cloud link off");
    }
}
