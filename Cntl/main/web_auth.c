#include "web_auth.h"
#include "fs.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "WEBAUTH";

#define WEB_AUTH_PATH        FS_MOUNT_POINT "/web_auth.bin"
#define WEB_AUTH_MAGIC       0x57415554u  /* "WAUT" */
#define WEB_AUTH_VERSION     1u           /* 구조체를 바꾸면 반드시 올림(power_relay 패딩 사고 교훈) */
#define WEB_AUTH_TOKENS      8
#define WEB_AUTH_TOKEN_LEN   16
#define WEB_AUTH_COOKIE      "cntl_t"
#define WEB_AUTH_PW_MIN      4
#define WEB_AUTH_PW_MAX      32
#define WEB_AUTH_BODY_MAX    (WEB_AUTH_PW_MAX * 2 + 2)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t  has_password;
    uint8_t  next_slot;                                   /* 다음 토큰을 쓸 자리(링) */
    uint8_t  reserved[2];
    uint8_t  salt[16];
    uint8_t  hash[32];
    uint8_t  tokens[WEB_AUTH_TOKENS][WEB_AUTH_TOKEN_LEN]; /* 전부 0이면 빈 자리 */
} web_auth_file_t;

static web_auth_file_t *s_auth = NULL;    /* 모델(PSRAM) — s_mutex로 보호 */
static SemaphoreHandle_t s_mutex = NULL;
/* 요청 처리용 작업 버퍼(PSRAM) — httpd는 태스크 하나라 요청이 겹치지 않지만, 모델과 같이 s_mutex 안에서만 씀 */
typedef struct {
    char body[WEB_AUTH_BODY_MAX + 1];
    char cookie[WEB_AUTH_TOKEN_LEN * 2 + 1];
    char set_cookie[96];
    uint8_t buf[16 + WEB_AUTH_PW_MAX];
    uint8_t digest[32];
    uint8_t tok[WEB_AUTH_TOKEN_LEN];
    uint8_t salt[16];
} web_auth_work_t;
static web_auth_work_t *s_work = NULL;

static void lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }

static bool save_locked(void)
{
    FILE *f = fopen(WEB_AUTH_PATH, "wb");
    if (!f) return false;
    bool ok = fwrite(s_auth, sizeof(*s_auth), 1, f) == 1;
    fclose(f);
    return ok;
}

/* SHA-256(salt || pw) → s_work->digest */
static bool hash_password_locked(const uint8_t salt[16], const char *pw, size_t pw_len)
{
    memcpy(s_work->buf, salt, 16);
    memcpy(s_work->buf + 16, pw, pw_len);
    size_t out_len = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, s_work->buf, 16 + pw_len,
                                       s_work->digest, sizeof(s_work->digest), &out_len);
    memset(s_work->buf, 0, sizeof(s_work->buf));
    return st == PSA_SUCCESS && out_len == 32;
}

static bool hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) return false;
    for (size_t i = 0; i < out_len; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

void web_auth_init(void)
{
    if (s_auth) return;
    s_auth = heap_caps_calloc(1, sizeof(web_auth_file_t), MALLOC_CAP_SPIRAM);
    s_work = heap_caps_calloc(1, sizeof(web_auth_work_t), MALLOC_CAP_SPIRAM);
    s_mutex = xSemaphoreCreateMutex();
    if (!s_auth || !s_work || !s_mutex) {
        ESP_LOGE(TAG, "alloc failed - web login unavailable");
        return;
    }
    psa_crypto_init();
    FILE *f = fopen(WEB_AUTH_PATH, "rb");
    if (f) {
        bool ok = fread(s_auth, sizeof(*s_auth), 1, f) == 1;
        fclose(f);
        if (!ok || s_auth->magic != WEB_AUTH_MAGIC || s_auth->version != WEB_AUTH_VERSION) {
            ESP_LOGW(TAG, "web_auth.bin format mismatch - ignored (web open until a password is set)");
            memset(s_auth, 0, sizeof(*s_auth));
        }
    }
    ESP_LOGI(TAG, "Web login %s", s_auth->has_password ? "enabled" : "not set (web open)");
}

bool web_auth_enabled(void)
{
    return s_auth && s_auth->has_password;
}

bool web_auth_check(httpd_req_t *req)
{
    if (!s_auth) return false;
    lock();
    bool ok = !s_auth->has_password;
    if (!ok) {
        size_t len = sizeof(s_work->cookie);
        uint8_t *tok = s_work->tok;
        if (httpd_req_get_cookie_val(req, WEB_AUTH_COOKIE, s_work->cookie, &len) == ESP_OK &&
            hex_decode(s_work->cookie, tok, WEB_AUTH_TOKEN_LEN)) {
            static const uint8_t zero[WEB_AUTH_TOKEN_LEN] = { 0 };
            for (int i = 0; i < WEB_AUTH_TOKENS && !ok; i++) {
                ok = memcmp(s_auth->tokens[i], zero, WEB_AUTH_TOKEN_LEN) != 0 &&
                     memcmp(s_auth->tokens[i], tok, WEB_AUTH_TOKEN_LEN) == 0;
            }
        }
    }
    unlock();
    return ok;
}

esp_err_t web_auth_reject(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"error\":\"auth\"}");
}

/* 요청 본문을 s_work->body로(잠금 안에서). 길이 초과·실패면 false */
static bool read_body_locked(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > WEB_AUTH_BODY_MAX) return false;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, s_work->body + got, req->content_len - got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    s_work->body[got] = '\0';
    return true;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* 새 토큰을 링에 넣고 Set-Cookie 헤더를 붙임(잠금 안에서, 응답 보내기 전까지 s_work->set_cookie가 살아 있어야 함) */
static bool issue_token_locked(httpd_req_t *req)
{
    uint8_t *slot = s_auth->tokens[s_auth->next_slot % WEB_AUTH_TOKENS];
    esp_fill_random(slot, WEB_AUTH_TOKEN_LEN);
    s_auth->next_slot = (uint8_t)((s_auth->next_slot + 1) % WEB_AUTH_TOKENS);
    if (!save_locked()) return false;
    int n = snprintf(s_work->set_cookie, sizeof(s_work->set_cookie), WEB_AUTH_COOKIE "=");
    for (int i = 0; i < WEB_AUTH_TOKEN_LEN; i++) n += snprintf(s_work->set_cookie + n, sizeof(s_work->set_cookie) - n, "%02x", slot[i]);
    snprintf(s_work->set_cookie + n, sizeof(s_work->set_cookie) - n, "; Max-Age=31536000; Path=/; HttpOnly; SameSite=Lax");
    httpd_resp_set_hdr(req, "Set-Cookie", s_work->set_cookie);
    return true;
}

/* POST /api/login — 본문 = 비밀번호 그대로. 성공하면 토큰 쿠키 */
static esp_err_t api_login_post_handler(httpd_req_t *req)
{
    if (!s_auth) return send_json(req, "500 Internal Server Error", "{\"ok\":false}");
    lock();
    bool ok = s_auth->has_password && read_body_locked(req);
    if (ok) {
        size_t pw_len = strlen(s_work->body);
        ok = pw_len >= WEB_AUTH_PW_MIN && pw_len <= WEB_AUTH_PW_MAX &&
             hash_password_locked(s_auth->salt, s_work->body, pw_len) &&
             memcmp(s_work->digest, s_auth->hash, 32) == 0;
    }
    memset(s_work->body, 0, sizeof(s_work->body));
    if (ok) ok = issue_token_locked(req);
    esp_err_t ret;
    if (ok) {
        ret = send_json(req, NULL, "{\"ok\":true}");
        ESP_LOGI(TAG, "Login OK");
    } else {
        unlock();
        vTaskDelay(pdMS_TO_TICKS(1000));  /* 대입 공격 늦추기 */
        ESP_LOGW(TAG, "Login failed");
        return send_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"password\"}");
    }
    unlock();
    return ret;
}

/* POST /api/password — 처음(비밀번호 없음): 본문 = 새 비밀번호, 바로 로그인 토큰도 줌.
 * 이미 있으면: 인증 필요 + 본문 = "기존\n새것" */
static esp_err_t api_password_post_handler(httpd_req_t *req)
{
    if (!s_auth) return send_json(req, "500 Internal Server Error", "{\"ok\":false}");
    if (web_auth_enabled() && !web_auth_check(req)) return web_auth_reject(req);
    lock();
    bool ok = read_body_locked(req);
    const char *new_pw = s_work->body;
    if (ok && s_auth->has_password) {
        char *nl = strchr(s_work->body, '\n');
        ok = nl != NULL;
        if (ok) {
            *nl = '\0';
            new_pw = nl + 1;
            size_t old_len = strlen(s_work->body);
            ok = hash_password_locked(s_auth->salt, s_work->body, old_len) && memcmp(s_work->digest, s_auth->hash, 32) == 0;
        }
    }
    size_t new_len = ok ? strlen(new_pw) : 0;
    ok = ok && new_len >= WEB_AUTH_PW_MIN && new_len <= WEB_AUTH_PW_MAX;
    if (ok) {
        uint8_t *salt = s_work->salt;
        esp_fill_random(salt, 16);
        ok = hash_password_locked(salt, new_pw, new_len);
        if (ok) {
            s_auth->magic = WEB_AUTH_MAGIC;
            s_auth->version = WEB_AUTH_VERSION;
            memcpy(s_auth->salt, salt, 16);
            memcpy(s_auth->hash, s_work->digest, 32);
            memset(s_auth->tokens, 0, sizeof(s_auth->tokens));  /* 비밀번호가 바뀌면 다른 기기는 다시 로그인 */
            s_auth->next_slot = 0;
            s_auth->has_password = 1;
            ok = issue_token_locked(req);  /* save_locked 포함 */
        }
    }
    memset(s_work->body, 0, sizeof(s_work->body));
    esp_err_t ret = ok ? send_json(req, NULL, "{\"ok\":true}")
                       : send_json(req, "400 Bad Request", "{\"ok\":false}");
    unlock();
    if (ok) ESP_LOGI(TAG, "Web password set");
    return ret;
}

/* POST /api/logout_all — 모든 기기 로그아웃(토큰 전부 지움). 인증 필요 */
static esp_err_t api_logout_all_post_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);
    lock();
    memset(s_auth->tokens, 0, sizeof(s_auth->tokens));
    s_auth->next_slot = 0;
    bool ok = save_locked();
    unlock();
    return send_json(req, NULL, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void web_auth_register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t login_uri = { .uri = "/api/login", .method = HTTP_POST, .handler = api_login_post_handler };
    static const httpd_uri_t password_uri = { .uri = "/api/password", .method = HTTP_POST, .handler = api_password_post_handler };
    static const httpd_uri_t logout_all_uri = { .uri = "/api/logout_all", .method = HTTP_POST, .handler = api_logout_all_post_handler };
    httpd_register_uri_handler(server, &login_uri);
    httpd_register_uri_handler(server, &password_uri);
    httpd_register_uri_handler(server, &logout_all_uri);
}
