#include "diary_api.h"
#include "web_auth.h"
#include "web_session.h"
#include "sd_storage.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

/* 2026-11-26(사용자 설계 — 영농일지 1단계) — diary_api.h 설명 참고.
 * 저장 위치: SD의 /sdcard/diary(storage_mgr가 정리하는 사진·측정 영역 밖이라 공간 정리로 지워지지 않음).
 * 쓰기는 임시 파일에 다 쓴 뒤 바꿔 끼움(중간에 전원이 꺼져도 예전 것이나 새것 중 하나는 남게):
 *   <이름>.tmp 쓰기·fsync → 기존 <이름>.json을 <이름>.bak으로 → .tmp를 .json으로 → .bak 지움.
 *   읽을 때 .json이 없고 .bak이 있으면 .bak(바꿔 끼우는 도중 꺼진 경우).
 * 버퍼는 요청마다 PSRAM(스택에 큰 데이터 안 둠). httpd 태스크 하나라 동시 접근 없음(원격 터널도 같은 웹 서버로 들어옴). */

static const char *TAG = "DIARY";

#define DIARY_DIR       SD_STORAGE_MOUNT_POINT "/diary"
#define DIARY_NAME_MAX  32
#define DIARY_FILE_MAX  (60 * 1024)   /* 파일 하나 최대 — 작기 하나(할 일 약 40개)는 10KB 안팎. 원격 터널 요청 한도(cloud_link TUN_REQ_MAX)보다 작게 */
#define DIARY_PATH_MAX  (sizeof(DIARY_DIR) + DIARY_NAME_MAX + 8)

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json, size_t len)
{
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, (ssize_t)len);
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *json)
{
    return send_json(req, status, json, strlen(json));
}

/* ?f=<이름> → name(DIARY_NAME_MAX+1). 영문 소문자·숫자·_·-만 */
static bool get_name(httpd_req_t *req, char *name)
{
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, "f", name, DIARY_NAME_MAX + 1) != ESP_OK) return false;
    size_t n = strlen(name);
    if (n == 0 || n > DIARY_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static void make_path(char *out, const char *name, const char *ext)
{
    snprintf(out, DIARY_PATH_MAX, DIARY_DIR "/%s.%s", name, ext);
}

/* 파일 전체를 PSRAM으로 — 없으면 NULL, *len = 0 */
static char *read_file(const char *path, size_t *len)
{
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0 && sz <= DIARY_FILE_MAX && fseek(f, 0, SEEK_SET) == 0) {
            buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) *len = (size_t)sz;
            else { heap_caps_free(buf); buf = NULL; }
        }
    }
    fclose(f);
    return buf;
}

/* GET /api/diary?f=<이름> */
static esp_err_t diary_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    char name[DIARY_NAME_MAX + 1];
    if (!get_name(req, name)) return send_err(req, "400 Bad Request", "{\"error\":\"name\"}");
    if (!sd_storage_is_mounted()) return send_err(req, "503 Service Unavailable", "{\"error\":\"sd\"}");
    char path[DIARY_PATH_MAX];
    make_path(path, name, "json");
    size_t len;
    char *buf = read_file(path, &len);
    if (!buf) {
        make_path(path, name, "bak");
        buf = read_file(path, &len);
        if (buf) ESP_LOGW(TAG, "%s: using .bak", name);
    }
    if (!buf) return send_err(req, NULL, "null");
    esp_err_t ret = send_json(req, NULL, buf, len);
    heap_caps_free(buf);
    return ret;
}

/* POST /api/diary?f=<이름> — 본문(JSON)으로 통째로 바꿈 */
static esp_err_t diary_post_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);
    char name[DIARY_NAME_MAX + 1];
    if (!get_name(req, name)) return send_err(req, "400 Bad Request", "{\"ok\":false,\"error\":\"name\"}");
    if (req->content_len == 0 || req->content_len > DIARY_FILE_MAX) return send_err(req, "400 Bad Request", "{\"ok\":false,\"error\":\"len\"}");
    if (!sd_storage_is_mounted()) return send_err(req, "503 Service Unavailable", "{\"ok\":false,\"error\":\"sd\"}");

    size_t len = req->content_len;
    char *body = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!body) return send_err(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"mem\"}");
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) { heap_caps_free(body); return send_err(req, "400 Bad Request", "{\"ok\":false,\"error\":\"recv\"}"); }
        got += (size_t)r;
    }
    if (body[0] != '{' && body[0] != '[') { heap_caps_free(body); return send_err(req, "400 Bad Request", "{\"ok\":false,\"error\":\"json\"}"); }

    mkdir(DIARY_DIR, 0775);   /* 이미 있으면 실패 — 무시 */
    char tmp[DIARY_PATH_MAX], dst[DIARY_PATH_MAX], bak[DIARY_PATH_MAX];
    make_path(tmp, name, "tmp");
    make_path(dst, name, "json");
    make_path(bak, name, "bak");
    bool ok = false;
    FILE *f = fopen(tmp, "wb");
    if (f) {
        ok = fwrite(body, 1, len, f) == len;
        ok = (fflush(f) == 0) && ok;
        ok = (fsync(fileno(f)) == 0) && ok;
        fclose(f);
    }
    heap_caps_free(body);
    if (ok) {
        struct stat st;
        bool had = stat(dst, &st) == 0;
        if (had) {
            unlink(bak);
            if (rename(dst, bak) != 0) ok = false;
        }
        if (ok && rename(tmp, dst) != 0) {
            ok = false;
            if (had) rename(bak, dst);   /* 되돌림 */
        }
        if (ok && had) unlink(bak);
    }
    if (!ok) {
        unlink(tmp);
        ESP_LOGE(TAG, "%s: write failed (%u bytes)", name, (unsigned)len);
        return send_err(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"write\"}");
    }
    ESP_LOGI(TAG, "%s: saved %u bytes", name, (unsigned)len);
    return send_err(req, NULL, "{\"ok\":true}");
}

void diary_api_register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t get_uri = { .uri = "/api/diary", .method = HTTP_GET, .handler = diary_get_handler };
    static const httpd_uri_t post_uri = { .uri = "/api/diary", .method = HTTP_POST, .handler = diary_post_handler };
    httpd_register_uri_handler(server, &get_uri);
    httpd_register_uri_handler(server, &post_uri);
}
