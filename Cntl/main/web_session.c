#include "web_session.h"
#include "web_auth.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "WEBSESS";

/* SPA는 5초마다 조회 — 4번 빠지면 접속 끝으로 봄 */
#define WEB_SESSION_IDLE_MS   20000
#define WEB_SESSION_CHECK_MS  2000

typedef enum {
    OWNER_NONE = 0,   /* 웹 접속 없음 — 콘 평소대로 */
    OWNER_WEB,        /* 웹 사용 중 — 콘 잠금 */
    OWNER_LOCAL,      /* 콘이 넘겨받음 — 웹 화면 API는 409, 웹이 claim하면 다시 OWNER_WEB */
} owner_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static owner_t s_owner = OWNER_NONE;
static int64_t s_last_seen_us = 0;
static web_session_lock_cb_t s_cb = NULL;
static esp_timer_handle_t s_idle_timer = NULL;

static void notify(bool locked)
{
    ESP_LOGI(TAG, "Cntl screen %s", locked ? "locked (web in use)" : "unlocked");
    if (s_cb) s_cb(locked);
}

/* 주인 바꾸고, 잠금 여부가 바뀌었으면 알림 */
static void set_owner(owner_t o)
{
    taskENTER_CRITICAL(&s_lock);
    bool was_locked = (s_owner == OWNER_WEB);
    s_owner = o;
    if (o == OWNER_WEB) s_last_seen_us = esp_timer_get_time();
    bool locked = (o == OWNER_WEB);
    taskEXIT_CRITICAL(&s_lock);
    if (was_locked != locked) notify(locked);
}

static void idle_check_cb(void *arg)
{
    (void)arg;
    taskENTER_CRITICAL(&s_lock);
    bool expired = (s_owner == OWNER_WEB) &&
                   (esp_timer_get_time() - s_last_seen_us > (int64_t)WEB_SESSION_IDLE_MS * 1000);
    if (expired) s_owner = OWNER_NONE;
    taskEXIT_CRITICAL(&s_lock);
    if (expired) {
        ESP_LOGI(TAG, "Web idle %ds - session ended", WEB_SESSION_IDLE_MS / 1000);
        notify(false);
    }
}

void web_session_init(web_session_lock_cb_t cb)
{
    s_cb = cb;
    if (s_idle_timer) return;
    const esp_timer_create_args_t args = { .callback = idle_check_cb, .name = "web_sess" };
    if (esp_timer_create(&args, &s_idle_timer) == ESP_OK) {
        esp_timer_start_periodic(s_idle_timer, (uint64_t)WEB_SESSION_CHECK_MS * 1000);
    } else {
        ESP_LOGE(TAG, "idle timer create failed - Cntl lock would never release by timeout");
    }
}

bool web_session_gate(httpd_req_t *req)
{
    taskENTER_CRITICAL(&s_lock);
    owner_t o = s_owner;
    taskEXIT_CRITICAL(&s_lock);
    if (o == OWNER_LOCAL) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_sendstr(req, "{\"error\":\"local\"}");
        return false;
    }
    set_owner(OWNER_WEB);  /* 처음이면 잠금, 이미 웹이면 마지막 요청 시각만 갱신 */
    return true;
}

void web_session_takeover(void)
{
    set_owner(OWNER_LOCAL);
}

bool web_session_web_active(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool active = (s_owner == OWNER_WEB);
    taskEXIT_CRITICAL(&s_lock);
    return active;
}

/* POST /api/session/claim — "다시 연결": 콘이 넘겨받은 상태여도 바로 웹이 주인 */
static esp_err_t api_session_claim_post_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);
    set_owner(OWNER_WEB);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void web_session_register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t claim_uri = { .uri = "/api/session/claim", .method = HTTP_POST,
                                           .handler = api_session_claim_post_handler };
    httpd_register_uri_handler(server, &claim_uri);
}
