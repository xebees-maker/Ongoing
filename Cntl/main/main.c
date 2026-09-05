#include <assert.h>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "ui_main.h"
#include "ui_strings.h"
#include "waveshare_rgb_lcd_port.h"
#include "nvs_flash.h"
#include "fs.h"
#include "esp_heap_caps.h"
#include "draw/lv_draw_buf_private.h"
#include "ui_font.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now_hub.h"
#include "esp_now_photo.h"
#include "esp_now_tx.h"
#include "esp_lv_decoder.h"
#include "ui_log.h"
#include "rtc_sync.h"
#include "device_config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lvgl9_demo";

/* 2026-08-10 — 적응형 반응시간(esp_now_hub.h)의 "마지막 사용자 조작" 시각을 통신 관련
 * 5개 함수뿐 아니라 화면 터치 전체로 넓힘(보류했다가 재활성화 — 통신 경로에 남아있던 버그를
 * 먼저 잡은 뒤 진행하기로 사용자와 합의). LV_EVENT_PRESSED만 걸어도 충분 — 터치가 시작될
 * 때마다 한 번씩만 갱신되면 되고, 드래그 중 계속 오는 LV_EVENT_PRESSING까지 볼 필요 없음 */
static void touch_activity_event_cb(lv_event_t *e)
{
    (void)e;
    esp_now_hub_note_user_action();
}

/* 2026-08-30(사용자 지시: "첫 페이지가, 장치목록/사진목록 보여주는 페이지가 의도한 거지?") —
 * IP만 입력해도 바로 SPA(/app)로 가도록 리다이렉트. 예전엔 여기가 더미 테스트 페이지+레거시
 * /photos 링크였음(Cntl 통합 테스트 5단계 때 흔적, "esp_http_server 자체가 RGB 패널을
 * 깨는지"만 격리 확인하려던 용도 — 이제 그 역할 끝남) */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/app");
    return httpd_resp_send(req, NULL, 0);
}

/* 2026-08-30(사용자 지시: "웹페이지에 사진 목록 보기(목록 가져오기 포함) 기능만 넣어볼래?",
 * 이후 "목록 다 가져와서 브라우저가 다 보여줄 수 있을 때 연결이 완료되는 거야" — 요청 즉시
 * placeholder를 리턴하지 않고, 핸들러 안에서 READY/ERROR가 되거나 타임아웃될 때까지
 * 붙잡고 있다가 최종 결과 하나만 리턴. httpd 기본 워커가 하나뿐이라 그동안 다른 요청은
 * 대기하지만, 개인용 대시보드라 수용 가능한 트레이드오프로 판단(사용자 설계 확인)) —
 * 페어링된 첫 CAM 대상. esp_now_photo.c가 뮤텍스로 보호돼있어 httpd 워커 태스크에서
 * 직접 불러도 안전(esp_now_photo_list_request 구현 확인함). 큰 지역버퍼는 오늘 하루종일
 * 겪은 스택오버플로우 패턴을 피하려고 PSRAM에서 할당 */
/* 2026-08-30(사용자 지시: "대기중인 장치 목록(CNTL과 동일한 형태)을 보여주고, 여기서 장치
 * 연결을 할 수 있게 해") — "AA11BB22CC33" 형식(콜론 없는 12자리 hex, URL 파라미터용)을
 * mac[6]으로 디코딩. 실패 시 false */
static bool decode_mac_hex(const char *hex, uint8_t mac[6])
{
    if (strlen(hex) != 12) return false;
    for (int i = 0; i < 6; i++) {
        unsigned byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return false;
        mac[i] = (uint8_t)byte;
    }
    return true;
}

/* 2026-08-30 — ui_main.c의 encode_file_seq_base36()과 동일 인코딩(CAM 실제 파일명 표기)을
 * 웹 목록에도 그대로 미러링 — 웹이 file_id를 화면에 별도 번호로 보여주면서 겪은 혼선(카운트
 * 기반 표시 vs 실제 file_id) 재발 방지, 사용자 지시로 CNTL과 동일한 표기로 통일 */
static void format_file_tag(char kind, uint32_t file_id, char *out, size_t out_size)
{
    static const char digits[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint32_t seq = file_id % 1679616u;  /* 36^4 */
    char seq_str[5];
    for (int i = 3; i >= 0; i--) {
        seq_str[i] = digits[seq % 36];
        seq /= 36;
    }
    seq_str[4] = '\0';
    snprintf(out, out_size, "%c%s", kind, seq_str);
}

/* 원본 해상도 그대로 원격에서 보기 — Cntl은 캐시된 압축 JPEG 바이트를 그대로 던져줄 뿐,
 * 디코드는 요청한 브라우저가 함(PC/폰은 메모리 여유가 있어서 원본을 그대로 풀 수 있음,
 * Cntl 자체 화면은 PSRAM이 부족해서 못 함 — 2026-08-01). 캐시 전용, 캐시에 없으면 404 */
static esp_err_t photo_get_handler(httpd_req_t *req)
{
    char query[32] = { 0 };
    char id_str[16] = { 0 };
    uint32_t file_id = 0;
    bool has_id = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK) {
        file_id = (uint32_t)strtoul(id_str, NULL, 10);
        has_id = true;
    }

    /* 2026-08-30 — file_id==0은 실제 유효한 사진 ID(가장 오래된 파일)일 수 있어서, "쿼리에
     * id가 아예 없었는지"는 값이 아니라 has_id로 따로 판단해야 함(버그: 예전엔 file_id==0을
     * "id 없음"으로 오판해 그 사진만 항상 즉시 실패) */
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!has_id || !esp_now_photo_cache_get(file_id, &data, &len)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "photo not cached");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    return httpd_resp_send(req, (const char *)data, len);
}

/* 2026-08-30(사용자 지시: "콘은 순수 JSON API만 제공하고... 정적 프론트엔드가 이 API를
 * 호출") — assets에 업로드될 정적 HTML/JS(app.html)가 이 API들을 fetch()로 호출해서 화면을 그림 */
static esp_err_t api_devices_get_handler(httpd_req_t *req)
{
    esp_now_hub_node_t cams[ESP_NOW_HUB_MAX_NODES];
    int n = esp_now_hub_get_nodes(HUB_NODE_KIND_CAM, cams, ESP_NOW_HUB_MAX_NODES);

    char *body = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    int len = snprintf(body, 2048, "[");
    for (int i = 0; i < n && len < 2048 - 150; i++) {
        hub_conn_state_t cs = esp_now_hub_get_conn_state(cams[i].mac);
        const char *status = (cs == HUB_CONN_STATE_WAITING) ? "waiting"
                            : (cs == HUB_CONN_STATE_ACTIVE)  ? "active" : "paired";
        /* 2026-09-04(사용자 설계: "앱의 문구들을 그대로 웹에서 써야한다") — status는 JS의
         * 로직 분기용 코드로 남기고, 표시용 문구는 콘 자신이 카메라판넬에 쓰는 ui_str()을
         * 그대로 별도 필드로 실어보냄 */
        const char *status_msg = (cs == HUB_CONN_STATE_WAITING) ? ui_str(STR_STATUS_CONNECTING)
                                : (cs == HUB_CONN_STATE_ACTIVE)  ? ui_str(STR_STATUS_ACTIVE)
                                                                  : ui_str(STR_STATUS_PAIRED);
        len += snprintf(body + len, 2048 - len,
                         "%s{\"mac\":\"%02x%02x%02x%02x%02x%02x\",\"name\":\"%s\",\"status\":\"%s\","
                         "\"status_msg\":\"%s\",\"paired\":%s}",
                         i == 0 ? "" : ",",
                         cams[i].mac[0], cams[i].mac[1], cams[i].mac[2],
                         cams[i].mac[3], cams[i].mac[4], cams[i].mac[5],
                         cams[i].name, status, status_msg,
                         cams[i].conn_state == NODE_CONN_PAIRED ? "true" : "false");
    }
    len += snprintf(body + len, 2048 - len, "]");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, body, len);
    heap_caps_free(body);
    return ret;
}

static esp_err_t api_connect_get_handler(httpd_req_t *req)
{
    char query[32] = { 0 };
    char mac_hex[16] = { 0 };
    uint8_t mac[6];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "mac", mac_hex, sizeof(mac_hex)) != ESP_OK ||
        !decode_mac_hex(mac_hex, mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
        return ESP_FAIL;
    }

    /* 2026-09-04(사용자 설계: "PC 원격제어처럼") — esp_now_hub_request_pair()를 직접 안 부르고
     * 실제 카메라 행 탭+확인 팝업까지 합성. 합성 자체가 실패하면(지금 목록에 없음 등) 그
     * 자리에서 바로 실패 — 성공했으면 이벤트 기반 블로킹 대기("연결실패"는 이 대기가
     * 타임아웃에 도달하는 것 자체가 신호) */
    bool paired = false;
    if (ui_main_inject_connect(mac)) {
        paired = esp_now_hub_wait_paired(mac, 25000);
    }

    /* 2026-09-04(사용자 설계: "앱의 문구들을 그대로 웹에서 써야한다") — JS가 따로 문구를
     * 갖지 않고, 콘 자신이 쓰는 ui_str() 문구를 그대로 실어보냄 */
    char body[96];
    int len = snprintf(body, sizeof(body), "{\"ok\":%s,\"msg\":\"%s\"}",
                        paired ? "true" : "false",
                        paired ? ui_str(STR_STATUS_PAIRED) : ui_str(STR_CONNECT_FAILED));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, len);
}

static esp_err_t api_disconnect_get_handler(httpd_req_t *req)
{
    char query[32] = { 0 };
    char mac_hex[16] = { 0 };
    uint8_t mac[6];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "mac", mac_hex, sizeof(mac_hex)) != ESP_OK ||
        !decode_mac_hex(mac_hex, mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
        return ESP_FAIL;
    }
    /* 2026-09-04("PC 원격제어처럼") — 카메라 행 탭+끊기 확인 팝업까지 합성. 로컬 상태변경이라
     * 합성이 성공하면 그 안에서 이미 완료된 것(esp_now_hub_unpair()가 동기적) */
    bool ok = ui_main_inject_disconnect(mac);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char body[96];
    int len = snprintf(body, sizeof(body), "{\"ok\":%s,\"msg\":\"%s\"}",
                        ok ? "true" : "false",
                        ok ? ui_str(STR_DISCONNECT_SUCCESS) : "");
    return httpd_resp_send(req, body, len);
}

static esp_err_t api_photos_get_handler(httpd_req_t *req)
{
    char query[32] = { 0 };
    bool force_refresh = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && strstr(query, "refresh=1")) {
        force_refresh = true;
    }

    esp_now_hub_node_t cams[ESP_NOW_HUB_MAX_NODES];
    int n = esp_now_hub_get_nodes(HUB_NODE_KIND_CAM, cams, ESP_NOW_HUB_MAX_NODES);
    int cam_idx = -1;
    for (int i = 0; i < n; i++) {
        if (cams[i].conn_state == NODE_CONN_PAIRED) { cam_idx = i; break; }
    }

    char *body = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }

    if (cam_idx < 0) {
        int len = snprintf(body, 4096, "{\"cam\":false,\"state\":\"no_cam\",\"items\":[],\"msg\":\"%s\"}",
                            ui_str(STR_PANEL_NO_PAIRED_DEVICE));
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        esp_err_t ret = httpd_resp_send(req, body, len);
        heap_caps_free(body);
        return ret;
    }

    /* 2026-08-30("웹기생" 설계) — 이미 결과가 있으면(다른 리더가 먼저 ack해도 버퍼는
     * 안 지워짐) 그대로 읽고, 없으면 새로 요청해서 세대번호로 ack와 무관하게 완료를 확인 */
    esp_now_photo_list_state_t cur_state = esp_now_photo_list_get_state();
    bool have_result = false;
    bool result_ok = false;
    if (!force_refresh && (cur_state == ESP_NOW_PHOTO_LIST_STATE_READY || cur_state == ESP_NOW_PHOTO_LIST_STATE_ERROR)) {
        have_result = true;
        result_ok = (cur_state == ESP_NOW_PHOTO_LIST_STATE_READY);
    } else if (force_refresh || cur_state == ESP_NOW_PHOTO_LIST_STATE_IDLE) {
        /* 2026-09-04(사용자 설계: "PC 원격제어처럼") — esp_now_photo_list_request()를 직접
         * 안 부르고 실제 "다시 가져오기" 버튼 탭을 합성(2026-08-30 버그수정 — REQUESTING
         * 중엔 새로 요청 안 함, 그건 아래 else 분기가 담당 — 중복요청이 나가면 ESP-NOW
         * 전송 큐가 밀려 WAKE_HELLO_ACK까지 실패해서 캠이 재광고하는 현상으로 실기에서
         * 확인됨). 합성 자체가 실패하면(카메라 선택 안 됨 등) 대기 없이 바로 실패 */
        bool inject_ok = false;
        uint32_t generation = ui_main_inject_list_refresh(&inject_ok);
        if (inject_ok) {
            have_result = esp_now_photo_list_wait_result(generation, 25000, &result_ok);
        } else {
            have_result = true;
            result_ok = false;
        }
    } else {
        /* 이미 REQUESTING 중(다른 곳에서 시작된 요청) — 새로 걸지 않고 그 세대를 그대로 기다림 */
        uint32_t generation = esp_now_photo_list_get_current_generation();
        have_result = esp_now_photo_list_wait_result(generation, 25000, &result_ok);
    }

    const char *state_str = !have_result ? "timeout" : (result_ok ? "ready" : "error");
    int len = snprintf(body, 4096, "{\"cam\":true,\"state\":\"%s\",\"items\":[", state_str);
    if (have_result && result_ok) {
        esp_now_photo_list_view_item_t items[ESP_NOW_PHOTO_LIST_MAX];
        int cnt = esp_now_photo_list_get_items(items, ESP_NOW_PHOTO_LIST_MAX);
        for (int i = 0; i < cnt && len < 4096 - 150; i++) {
            char tag[8];
            format_file_tag(items[i].kind, items[i].file_id, tag, sizeof(tag));
            len += snprintf(body + len, 4096 - len,
                             "%s{\"file_id\":%u,\"tag\":\"%s\",\"capture_time\":%u,\"file_size\":%u}",
                             i == 0 ? "" : ",",
                             (unsigned)items[i].file_id, tag,
                             (unsigned)items[i].capture_time, (unsigned)items[i].file_size);
        }
    }
    /* 2026-09-04(사용자 설계: "앱의 문구들을 그대로 웹에서 써야한다") */
    const char *msg = (have_result && result_ok) ? ui_str(STR_LIST_FETCH_SUCCESS) : ui_str(STR_LIST_FETCH_FAILED);
    len += snprintf(body + len, 4096 - len, "],\"msg\":\"%s\"}", msg);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, body, len);
    heap_caps_free(body);
    return ret;
}

static esp_err_t api_photo_fetch_get_handler(httpd_req_t *req)
{
    char query[32] = { 0 };
    char id_str[16] = { 0 };
    uint32_t file_id = 0;
    bool has_id = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK) {
        file_id = (uint32_t)strtoul(id_str, NULL, 10);
        has_id = true;
    }
    bool ok = false;
    if (has_id) {
        const uint8_t *data = NULL;
        size_t len0 = 0;
        if (esp_now_photo_cache_get(file_id, &data, &len0)) {
            ok = true;
        } else {
            esp_now_hub_node_t cams[ESP_NOW_HUB_MAX_NODES];
            int n = esp_now_hub_get_nodes(HUB_NODE_KIND_CAM, cams, ESP_NOW_HUB_MAX_NODES);
            int cam_idx = -1;
            for (int i = 0; i < n; i++) {
                if (cams[i].conn_state == NODE_CONN_PAIRED) { cam_idx = i; break; }
            }
            if (cam_idx >= 0) {
                /* 2026-09-04(사용자 설계: "PC 원격제어처럼") — 모델만 세팅하는 대신 실제
                 * 사진목록 행 탭을 합성(지금 화면/목록에 그 file_id가 없으면 합성 자체가
                 * 실패 — 대기 없이 바로 실패). 결과 대기는 이벤트 기반(비파괴적 캐시 확인) */
                ok = ui_main_inject_photo_select(file_id) &&
                     esp_now_photo_wait_cached(file_id, 25000, &data, &len0);
            }
        }
    }
    /* 2026-09-04(사용자 설계: "앱의 문구들을 그대로 웹에서 써야한다") — 사진 성공/실패는
     * 콘 자신의 진행팝업이 이미 쓰는 STR_FETCH_DONE/STR_FETCH_FAILED를 그대로 재사용 */
    char body[96];
    int len = snprintf(body, sizeof(body), "{\"ok\":%s,\"msg\":\"%s\"}",
                        ok ? "true" : "false",
                        ok ? ui_str(STR_FETCH_DONE) : ui_str(STR_FETCH_FAILED));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, len);
}

/* 2026-08-30 — 정적 프론트엔드(assets에 업로드된 app.html)를 깔끔한 URL로 서빙. 파일이
 * 아직 없으면(최초 배포 전) 404 — /admin/upload?file=app.html로 올리면 그때부터 동작 */
static esp_err_t app_get_handler(httpd_req_t *req)
{
    FILE *f = fopen(FS_MOUNT_POINT "/app.html", "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "app.html not uploaded yet");
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, buf, rd);
    heap_caps_free(buf);
    return ret;
}

/* 2026-08-30(사용자 지시: "PC쪽에 파일을 나누고, 각 파일을 복사하는 개념으로 콘의 assets
 * 파티션에 저장" — assets 파티션 전체를 재빌드/재플래시하지 않고 파일 하나만 개별적으로
 * 갱신/조회하기 위한 범용 엔드포인트. device_config.bin처럼 assets에 이미 있는 파일도
 * 이걸로 백업/복원 가능. 인증 없음(같은 네트워크 내 신뢰 전제, 사용자 확인:
 * "복사해 가도 상관 없어") — 경로 조작만 방어 */
static bool is_safe_asset_filename(const char *name)
{
    if (name[0] == '\0') return false;
    if (strstr(name, "..") != NULL) return false;
    if (strchr(name, '/') != NULL) return false;
    return true;
}

static esp_err_t admin_upload_post_handler(httpd_req_t *req)
{
    char query[64] = { 0 };
    char filename[64] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", filename, sizeof(filename)) != ESP_OK ||
        !is_safe_asset_filename(filename)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid file param");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 200 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid content length");
        return ESP_FAIL;
    }

    /* 2026-08-30(사용자 지시: "메모리 특히 신경써야") — 업로드 버퍼는 PSRAM, 요청 끝나면 즉시 해제 */
    char *buf = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            heap_caps_free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += (size_t)r;
    }

    char path[80];
    snprintf(path, sizeof(path), "%s/%s", FS_MOUNT_POINT, filename);
    FILE *f = fopen(path, "wb");
    if (!f) {
        heap_caps_free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    fwrite(buf, 1, received, f);
    fclose(f);
    heap_caps_free(buf);

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t admin_download_get_handler(httpd_req_t *req)
{
    char query[64] = { 0 };
    char filename[64] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", filename, sizeof(filename)) != ESP_OK ||
        !is_safe_asset_filename(filename)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid file param");
        return ESP_FAIL;
    }

    char path[80];
    snprintf(path, sizeof(path), "%s/%s", FS_MOUNT_POINT, filename);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 300 * 1024) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "bad file size");
        return ESP_FAIL;
    }

    char *buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);

    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t ret = httpd_resp_send(req, buf, rd);
    heap_caps_free(buf);
    return ret;
}

void web_dashboard_start(void)
{
    /* 2026-08-30 — STA(IP_EVENT_STA_GOT_IP, 재연결로 여러 번 올 수 있음)와 AP(부팅 시 1회
     * 직접 호출) 두 경로에서 부를 수 있게 돼서, 호출부별 로컬 플래그 대신 여기서 직접 방어 */
    static bool s_started = false;
    if (s_started) return;
    s_started = true;

    ui_log_add("Web: entered web_dashboard_start()");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* 2026-08-21 — 5005(httpd_start 실패) 원인 확인됨: 부팅 이 시점엔 내부(비-PSRAM) DRAM이
     * 거의 바닥남(실기 확인: free internal=1419B) — HTTPD_DEFAULT_CONFIG()의 task_caps
     * 기본값이 MALLOC_CAP_INTERNAL이라 태스크 스택을 내부 RAM에서만 찾다가 실패함. PSRAM은
     * 넉넉하니(같은 시점 free heap=263660B) 여기로 돌림(esp_lv_adapter의 stack_in_psram,
     * 폰트 버퍼의 font_buf_malloc과 동일 원칙) */
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    /* 2026-08-30 — 핸들러들이 지역 노드 배열(esp_now_hub_node_t[8])을 스택에 두는데,
     * 기본 스택 크기가 빠듯할 수 있어 확대(PSRAM이라 비용 낮음) */
    config.stack_size = 8192;
    /* 2026-08-30 — URI 핸들러가 계속 늘어나서(root/photo/admin 2개 + API) 기본
     * max_uri_handlers(8)를 넘을 수 있어 여유있게 확대 */
    config.max_uri_handlers = 16;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s (free heap=%u, free internal=%u)",
                 esp_err_to_name(err), (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        /* 2026-08-21 — 시리얼 캡처 도구가 계속 불안정해서 원인 코드를 못 잡았음. 화면
         * 로그(통계 탭)에도 원인+여유메모리를 바로 보이게 해서 시리얼 없이도 확인 가능하게 함 */
        ui_log_add_err(UI_ERR_HTTPD_START, "Web server start failed: %s (heap=%uB, internal=%uB)",
                       esp_err_to_name(err), (unsigned)esp_get_free_heap_size(),
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return;
    }
    ui_log_add("Web: httpd_start SUCC");
    static const httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    esp_err_t root_err = httpd_register_uri_handler(server, &root_uri);
    ui_log_add("Web: '/' register %s", root_err == ESP_OK ? "SUCC" : "FAIL");
    static const httpd_uri_t photo_uri = { .uri = "/photo", .method = HTTP_GET, .handler = photo_get_handler };
    esp_err_t photo_err = httpd_register_uri_handler(server, &photo_uri);
    ui_log_add("Web: '/photo' register %s", photo_err == ESP_OK ? "SUCC" : "FAIL");
    static const httpd_uri_t admin_upload_uri = { .uri = "/admin/upload", .method = HTTP_POST,
                                                    .handler = admin_upload_post_handler };
    esp_err_t admin_upload_err = httpd_register_uri_handler(server, &admin_upload_uri);
    ui_log_add("Web: '/admin/upload' register %s", admin_upload_err == ESP_OK ? "SUCC" : "FAIL");
    static const httpd_uri_t admin_download_uri = { .uri = "/admin/download", .method = HTTP_GET,
                                                      .handler = admin_download_get_handler };
    esp_err_t admin_download_err = httpd_register_uri_handler(server, &admin_download_uri);
    ui_log_add("Web: '/admin/download' register %s", admin_download_err == ESP_OK ? "SUCC" : "FAIL");
    static const httpd_uri_t api_devices_uri = { .uri = "/api/devices", .method = HTTP_GET,
                                                   .handler = api_devices_get_handler };
    httpd_register_uri_handler(server, &api_devices_uri);
    static const httpd_uri_t api_connect_uri = { .uri = "/api/connect", .method = HTTP_GET,
                                                   .handler = api_connect_get_handler };
    httpd_register_uri_handler(server, &api_connect_uri);
    static const httpd_uri_t api_disconnect_uri = { .uri = "/api/disconnect", .method = HTTP_GET,
                                                      .handler = api_disconnect_get_handler };
    httpd_register_uri_handler(server, &api_disconnect_uri);
    static const httpd_uri_t api_photos_uri = { .uri = "/api/photos", .method = HTTP_GET,
                                                  .handler = api_photos_get_handler };
    httpd_register_uri_handler(server, &api_photos_uri);
    static const httpd_uri_t api_photo_fetch_uri = { .uri = "/api/photo_fetch", .method = HTTP_GET,
                                                       .handler = api_photo_fetch_get_handler };
    httpd_register_uri_handler(server, &api_photo_fetch_uri);
    ui_log_add("Web: API endpoints registered");
    static const httpd_uri_t app_uri = { .uri = "/app", .method = HTTP_GET, .handler = app_get_handler };
    httpd_register_uri_handler(server, &app_uri);
    /* 2026-08-21 — 성공할 때도 같은 여유메모리를 남김(사용자 지시) — 실패할 때만 찍으면
     * "언제부터 빠듯해지기 시작했는지" 추세를 못 봄. 5005는 이 시점 내부RAM이 간당간당할
     * 때만 뜨는 경계선 증상이라, 성공한 부팅들의 수치도 같이 쌓여야 나중에 진짜 임계점을
     * 추적할 수 있음 */
    unsigned free_heap = (unsigned)esp_get_free_heap_size();
    unsigned free_internal = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Web dashboard (stub) started (free heap=%u, free internal=%u)", free_heap, free_internal);
    ui_log_add("Web server started (heap=%uB, internal=%uB)", free_heap, free_internal);
}

/* 2026-08-21 — 예전엔 app_main() 맨 끝에서 esp_now_hub_init() 직후 곧바로 불렀는데, 그
 * 시점엔 WiFi가 아직 인증/연결 단계라 IP를 받기도 전이었음(실기 로그로 확인: httpd_start가
 * IP_EVENT_STA_GOT_IP보다 1초 이상 먼저 실행됨) — 이게 5005(httpd_start 실패)가 항상 뜨던
 * 원인. IP를 실제로 받은 뒤에 시작하도록 이벤트로 미룸. 재연결로 GOT_IP가 여러 번 올 수
 * 있는데, 그 중복 방어는 web_dashboard_start() 내부로 옮김(AP 모드 직접 호출 경로와
 * 공유해야 해서, 2026-08-30) */
static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg; (void)event_base; (void)event_data;
    ui_log_add("IP_EVENT(id=%ld) received", (long)event_id);
    if (event_id == IP_EVENT_STA_GOT_IP) {
        web_dashboard_start();
    }
}

static void *font_buf_malloc(size_t size, lv_color_format_t cf)
{
    (void)cf;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static void font_buf_free(void *buf)
{
    heap_caps_free(buf);
}

void app_main(void)
{
    ui_log_init();  /* 통계 탭 로그박스용 — 최대한 먼저(이후 관심 지점들이 여기 씀) */

    /* Cntl 통합 테스트 1단계: NVS init (Cntl main.c와 동일) */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* LittleFS "assets" 파티션 마운트 — LCD/I2C와 무관해서 최대한 먼저: 언어 설정
     * (/assets/settings.bin)과 RTC 시드값(/assets/time_sync.txt) 둘 다 이 안에 있음 */
    ESP_ERROR_CHECK(fs_init());

    /* 영구 저장 설정값(언어 등) 복원 — UI 생성(ui_init) 전에 해야 라벨이 처음부터
     * 올바른 언어로 뜸 */
    ui_lang_load();
    /* CAM/SENS 원격 설정값(Cntl이 주인, 2026-08-08 설계) — UI 생성 전에 로드해야 설정탭
     * 드롭다운이 처음부터 저장된 값을 보여줌(부팅 시 "값 미리 로드" 요구사항) */
    device_config_load();

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
    const uint8_t frame_buffer_count = esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation);

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_touch_handle_t touch_handle = NULL;

    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init(
        frame_buffer_count,
        &panel_handle,
        &touch_handle));
    ESP_ERROR_CHECK(waveshare_rgb_lcd_backlight_on());

    /* 보드 실장 PCF85063A RTC — I2C 버스가 막 만들어진 직후, UI가 뜨기 전에 시각을
     * 읽어와야 로고 부제(시계)가 처음부터 맞는 값으로 뜸 */
    esp_err_t rtc_ret = rtc_sync_init();
    ESP_LOGI(TAG, "rtc_sync_init: %s", rtc_ret == ESP_OK ? "OK" : "FAILED");

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.stack_in_psram = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t disp_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        panel_handle,
        NULL,
        EXAMPLE_LCD_H_RES,
        EXAMPLE_LCD_V_RES,
        rotation);
    disp_config.profile.use_psram = true;

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_config);
    assert(disp != NULL);

    if (touch_handle != NULL) {
        esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
        assert(touch != NULL);
        lv_indev_add_event_cb(touch, touch_activity_event_cb, LV_EVENT_PRESSED, NULL);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    /* Cntl 통합 테스트 3단계: 폰트 글리프 버퍼 PSRAM 할당 설정 + NanumGothic TTF 로드
     * (아직 실제 UI에서 쓰지는 않음 — 로드 자체가 문제인지만 확인, 4단계에서 실제 사용) */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_draw_buf_handlers_t *font_handlers = lv_draw_buf_get_font_handlers();
        font_handlers->buf_malloc_cb = font_buf_malloc;
        font_handlers->buf_free_cb   = font_buf_free;
        esp_err_t font_ret = ui_font_init();
        ESP_LOGI(TAG, "ui_font_init: %s", font_ret == ESP_OK ? "OK" : "FAILED");

        /* CAM에서 받은 JPEG을 lv_image로 바로 표시하기 위한 디코더 등록 — LVGL 호출이라
         * 다른 lv_* 초기화와 마찬가지로 락 안에서 해야 함 */
        esp_lv_decoder_handle_t decoder_handle = NULL;
        esp_err_t decoder_ret = esp_lv_decoder_init(&decoder_handle);
        ESP_LOGI(TAG, "esp_lv_decoder_init: %s", decoder_ret == ESP_OK ? "OK" : "FAILED");

        esp_lv_adapter_unlock();
    }

    ESP_LOGI(TAG, "Starting Cntl UI");
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        ui_init();
        esp_lv_adapter_unlock();
    }

    /* Cntl 통합 테스트 4단계 → CAM 연결 기능: WiFi+ESP-NOW 허브(페어링/노드테이블 포함) —
     * Cntl main.c와 동일하게 UI 뜬 뒤 마지막에 켬 */
    esp_now_photo_init();
    esp_now_hub_init();  /* 내부에서 esp_netif_init()+esp_event_loop_create_default() 호출 —
                             아래 이벤트 등록은 반드시 그 다음이어야 함 */
    esp_now_tx_init();

    /* 웹 대시보드는 실제로 IP를 받은 뒤에 시작(위 ip_event_handler 참고, 5005 버그 수정) */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &ip_event_handler, NULL, NULL));

    ui_main_register_wifi_events();  /* 같은 이유로 여기서(esp_now_hub_init() 이후) 등록 */
}
