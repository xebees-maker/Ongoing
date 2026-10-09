#include <assert.h>
#include "memdiag.h"

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
#include "node_hub.h"
#include "wifi_sta.h"
#include "photo_rx.h"
#include "node_request.h"
#include "esp_lv_decoder.h"
#include "ui_log.h"
#include "dev_log.h"
#include "rtc_sync.h"
#include "device_config.h"
#include "sd_storage.h"
#include "storage_mgr.h"
#include "stats_store.h"
#include "power_relay.h"
#include "sens_kind_store.h"
#include "notify.h"
#include "alarm.h"
#include "esp_system.h"
#include "web_auth.h"
#include "web_session.h"
#include "web_api.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "esp_timer.h"

static const char *TAG = "SYS";

/* 2026-08-10 — 적응형 반응시간(node_hub.h)의 "마지막 사용자 조작" 시각을 통신 관련
 * 5개 함수뿐 아니라 화면 터치 전체로 넓힘(보류했다가 재활성화 — 통신 경로에 남아있던 버그를
 * 먼저 잡은 뒤 진행하기로 사용자와 합의). LV_EVENT_PRESSED만 걸어도 충분 — 터치가 시작될
 * 때마다 한 번씩만 갱신되면 되고, 드래그 중 계속 오는 LV_EVENT_PRESSING까지 볼 필요 없음 */
static void touch_activity_event_cb(lv_event_t *e)
{
    (void)e;
    node_hub_note_user_action();
}

/* 2026-08-30(사용자 지시: "첫 페이지가, 장치목록/사진목록 보여주는 페이지가 의도한 거지?") —
 * IP만 입력해도 바로 SPA(/app)로 가도록 리다이렉트. 예전엔 여기가 더미 테스트 페이지+레거시
 * /photos 링크였음(Cntl 통합 테스트 5단계 때 흔적, "esp_http_server 자체가 RGB 패널을
 * 깨는지"만 격리 확인하려던 용도 — 이제 그 역할 끝남) */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    /* 2026-10-03(할 일 AD) — 상대 경로: 앞에 중계 서비스가 경로 접두어를 붙여도 그대로 동작 */
    httpd_resp_set_hdr(req, "Location", "app");
    return httpd_resp_send(req, NULL, 0);
}

/* 2026-08-30(사용자 지시: "웹페이지에 사진 목록 보기(목록 가져오기 포함) 기능만 넣어볼래?",
 * 이후 "목록 다 가져와서 브라우저가 다 보여줄 수 있을 때 연결이 완료되는 거야" — 요청 즉시
 * placeholder를 리턴하지 않고, 핸들러 안에서 READY/ERROR가 되거나 타임아웃될 때까지
 * 붙잡고 있다가 최종 결과 하나만 리턴. httpd 기본 워커가 하나뿐이라 그동안 다른 요청은
 * 대기하지만, 개인용 대시보드라 수용 가능한 트레이드오프로 판단(사용자 설계 확인)) —
 * 페어링된 첫 CAM 대상. photo_rx.c가 뮤텍스로 보호돼있어 httpd 워커 태스크에서
 * 직접 불러도 안전(photo_rx_list_request 구현 확인함). 큰 지역버퍼는 오늘 하루종일
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

/* 2026-09-19(SD 제거 재설계) — 예전엔 CAM 실제 파일명 표기(base36 4자리)를 그대로
 * 미러링해야 했지만, 이제 콘 SD의 실제 파일명 자체가 kind+8자리 십진수라 그걸 그대로 씀
 * (photo_storage.c의 파일명 규칙과 동일) */
static void format_file_tag(char kind, uint32_t seq, char *out, size_t out_size)
{
    snprintf(out, out_size, "%c%08u", kind, (unsigned)seq);
}

/* 원본 해상도 그대로 원격에서 보기 — Cntl은 지금 화면에 선택돼 표시 중인 사진의 압축 JPEG
 * 바이트를 그대로 던져줄 뿐, 디코드는 요청한 브라우저가 함(PC/폰은 메모리 여유가 있어서
 * 원본을 그대로 풀 수 있음, Cntl 자체 화면은 PSRAM이 부족해서 못 함 — 2026-08-01).
 * 2026-09-19(SD 제거 재설계 — "웹은 콘에 기생") — 예전엔 file_id로 독립된 캐시를 조회했지만,
 * 이제 콘 화면(ui_main.c)이 로컬 SD에서 읽어 들고 있는 바로 그 원본을 그대로 읽어감
 * (ui_main_get_selected_photo_raw) — kind/seq가 같이 오면 "지금 화면이 보여주는 것"과
 * 일치하는지 확인해서, 그 사이 다른 선택으로 바뀌었으면 엉뚱한 사진을 내려주지 않고 404 */
static esp_err_t photo_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    char query[32] = { 0 };
    char kind_str[4] = { 0 }, seq_str[16] = { 0 };
    bool has_kind = false, has_seq = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        has_kind = (httpd_query_key_value(query, "kind", kind_str, sizeof(kind_str)) == ESP_OK);
        has_seq  = (httpd_query_key_value(query, "seq", seq_str, sizeof(seq_str)) == ESP_OK);
    }

    const uint8_t *data = NULL;
    size_t len = 0;
    if (!ui_main_get_selected_photo_raw(&data, &len)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no photo selected");
        return ESP_FAIL;
    }
    if (has_kind && has_seq && kind_str[0] != '\0') {
        uint8_t sel_kind = 0; uint32_t sel_seq = 0;
        uint32_t seq = (uint32_t)strtoul(seq_str, NULL, 10);
        if (!ui_main_get_selected_photo_id(&sel_kind, &sel_seq) ||
            sel_kind != (uint8_t)kind_str[0] || sel_seq != seq) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "selection changed");
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, "image/jpeg");
    return httpd_resp_send(req, (const char *)data, len);
}

/* 2026-08-30(사용자 지시: "콘은 순수 JSON API만 제공하고... 정적 프론트엔드가 이 API를
 * 호출") — assets에 업로드될 정적 HTML/JS(app.html)가 이 API들을 fetch()로 호출해서 화면을 그림 */
static esp_err_t api_devices_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    node_hub_node_t cams[NODE_HUB_MAX_NODES];
    int n = node_hub_get_nodes(HUB_NODE_KIND_CAM, cams, NODE_HUB_MAX_NODES);

    char *body = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    int len = snprintf(body, 2048, "[");
    for (int i = 0; i < n && len < 2048 - 150; i++) {
        hub_conn_state_t cs = node_hub_get_conn_state(cams[i].mac);
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
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    char query[32] = { 0 };
    char mac_hex[16] = { 0 };
    uint8_t mac[6];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "mac", mac_hex, sizeof(mac_hex)) != ESP_OK ||
        !decode_mac_hex(mac_hex, mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
        return ESP_FAIL;
    }

    /* 2026-09-04(사용자 설계: "PC 원격제어처럼") — node_hub_request_pair()를 직접 안 부르고
     * 실제 카메라 행 탭+확인 팝업까지 합성. 합성 자체가 실패하면(지금 목록에 없음 등) 그
     * 자리에서 바로 실패 — 성공했으면 이벤트 기반 블로킹 대기("연결실패"는 이 대기가
     * 타임아웃에 도달하는 것 자체가 신호) */
    bool paired = false;
    if (ui_main_inject_connect(mac)) {
        paired = node_hub_wait_paired(mac, 25000);
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

/* 2026-09-06(사용자 지시 — 야간 자동 테스트용, "사용자처럼" 센스 연결+응답성 변경) —
 * api_connect_get_handler와 동일 패턴, 센스 전용 합성 함수만 다름 */
static esp_err_t api_connect_sensor_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    char query[32] = { 0 };
    char mac_hex[16] = { 0 };
    uint8_t mac[6];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "mac", mac_hex, sizeof(mac_hex)) != ESP_OK ||
        !decode_mac_hex(mac_hex, mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
        return ESP_FAIL;
    }

    bool paired = false;
    if (ui_main_inject_connect_sensor(mac)) {
        paired = node_hub_wait_paired(mac, 25000);
    }

    char body[96];
    int len = snprintf(body, sizeof(body), "{\"ok\":%s,\"msg\":\"%s\"}",
                        paired ? "true" : "false",
                        paired ? ui_str(STR_STATUS_PAIRED) : ui_str(STR_CONNECT_FAILED));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, len);
}

/* ?sec=0|3|10|30|60 — 응답성 드롭다운+Apply 합성. 실제 적용 완료(CAM/SENS 응답 대기)까지는
 * 기다리지 않고 "합성 자체가 성공했는지"만 반환 — 야간 자동 테스트 스크립트가 그 다음
 * 단계(재연결 등) 전에 device_config 값이 실제로 바뀌었는지는 별도로 확인하면 됨 */
/* 2026-09-27(3002 조사 — 사용자 지시) — 수동 촬영 합성. 결과는 합성 성공 여부만(촬영/수신 결과는 로그로 봄) */
static esp_err_t api_capture_now_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    char query[32] = { 0 };
    char mac_hex[16] = { 0 };
    uint8_t mac[6];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "mac", mac_hex, sizeof(mac_hex)) != ESP_OK ||
        !decode_mac_hex(mac_hex, mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
        return ESP_FAIL;
    }
    bool ok = ui_main_inject_capture_now(mac);
    char body[32];
    int len = snprintf(body, sizeof(body), "{\"ok\":%s}", ok ? "true" : "false");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, len);
}

/* 2026-09-27(3002 조사 — 사용자 지시 "네가 직접 바꾸고") — 개발 로그 저장 문턱. 화면 드롭다운과 같은 값
 * (dev_log_set_save_level — 파일에도 저장됨). 로그 창이 열려 있으면 드롭다운 표시는 다음에 열 때 맞춰짐 */
static esp_err_t api_devlog_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    char query[16] = { 0 };
    char v[4] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "save", v, sizeof(v)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing save");
        return ESP_FAIL;
    }
    const char *lv = "EWID";
    const char *pos = strchr(lv, v[0]);
    if (!pos || v[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "save must be E/W/I/D");
        return ESP_FAIL;
    }
    dev_log_set_save_level((uint8_t)(DEV_LOG_LVL_E + (pos - lv)));
    char body[32];
    int len = snprintf(body, sizeof(body), "{\"save\":\"%c\"}", v[0]);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, len);
}

/* 2026-09-30(사용자 지시 — "Dev Log를 네가 가져갈 수 있게 따로 만들어") — RAM 링에만 있는 Dev Log 전체(최대 400줄)를
 * 텍스트로 돌려줌. SD 에러 등이 났을 때 재부팅/플래시 전에 먼저 받아 둠(조건부 할 일 C5) */
static esp_err_t api_devlog_dump_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    /* 2026-10-09 — ?lvl=we 면 경고·에러 링(2000줄) */
    char q[24] = "", lvl[8] = "";
    bool we = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
              httpd_query_key_value(q, "lvl", lvl, sizeof(lvl)) == ESP_OK && strcmp(lvl, "we") == 0;
    const size_t cap = we ? 2000 * 150 : 400 * 150;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    size_t len = we ? dev_log_dump_we(buf, cap) : dev_log_dump(buf, cap);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, buf, len);
    heap_caps_free(buf);
    return ret;
}

/* 2026-09-28(SD 손상 재현 시험 — 사용자 지시, 할 일 24 테스트 API와 함께 나중에 제거) — 5640 사진 폴더에서만 이름이 FF로
 * 읽히는 디렉터리 손상이 조용히 생김(한일 09-28 61번). 사진 저장과 같은 순서(임시파일에 16KB씩 → fsync → 닫기 → 이름 바꾸기)로
 * /sdcard/sdtest에 파일을 계속 만들어 디렉터리를 클러스터 경계 너머로 키우고, 50개마다 폴더를 다시 읽어 깨진 항목을 셈 */
static volatile bool s_sdtest_running = false;
static int s_sdtest_files = 0, s_sdtest_kb = 0;
static bool s_sdtest_clean = false;

static void sdtest_scan(const char *dir, int *out_total, int *out_bad, int *out_ff)
{
    int total = 0, bad = 0, ff = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            total++;
            size_t len = strlen(e->d_name);
            bool ok = (len == 13 && e->d_name[0] == 'T' && strcmp(e->d_name + 9, ".jpg") == 0);
            for (int i = 1; ok && i < 9; i++) if (e->d_name[i] < '0' || e->d_name[i] > '9') ok = false;
            if (!ok) {
                bad++;
                if ((unsigned char)e->d_name[0] == 0xFF) ff++;
            }
        }
        closedir(d);
    }
    *out_total = total; *out_bad = bad; *out_ff = ff;
}

static void sdtest_task(void *arg)
{
    (void)arg;
    const char *dir = SD_STORAGE_MOUNT_POINT "/sdtest";
    char path[64], tmp[72];
    if (s_sdtest_clean) {
        int removed = 0;
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                snprintf(path, sizeof(path), "%s/%.40s", dir, e->d_name);
                if (unlink(path) == 0) removed++;
            }
            closedir(d);
        }
        ESP_LOGW(TAG, "SDTEST clean: %d files removed", removed);
        s_sdtest_running = false;
        vTaskDelete(NULL);
        return;
    }
    mkdir(dir, 0777);
    const size_t chunk = 16 * 1024;
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "SDTEST buffer alloc failed"); s_sdtest_running = false; vTaskDelete(NULL); return; }
    int start_total, start_bad, start_ff;
    sdtest_scan(dir, &start_total, &start_bad, &start_ff);
    int base = start_total;
    ESP_LOGW(TAG, "SDTEST start: %d files x %dKB, dir has %d entries (bad %d, FF %d)",
             s_sdtest_files, s_sdtest_kb, start_total, start_bad, start_ff);
    int fails = 0;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < s_sdtest_files; i++) {
        int n = base + i;
        snprintf(path, sizeof(path), "%s/T%08d.jpg", dir, n);
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        memset(buf, (uint8_t)n, chunk);
        FILE *f = fopen(tmp, "wb");
        bool ok = (f != NULL);
        for (int k = 0; ok && k < s_sdtest_kb / 16; k++) ok = (fwrite(buf, 1, chunk, f) == chunk);
        if (f) {
            if (ok) ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
            fclose(f);
        }
        if (ok) ok = (rename(tmp, path) == 0);
        if (!ok) {
            fails++;
            ESP_LOGW(TAG, "SDTEST file %d failed (errno=%d)", n, errno);
            unlink(tmp);
        }
        if ((i + 1) % 50 == 0 || i + 1 == s_sdtest_files) {
            int total, bad, ff;
            sdtest_scan(dir, &total, &bad, &ff);
            ESP_LOGW(TAG, "SDTEST %d/%d written (fails %d): dir entries %d, bad %d, FF %d, %llds",
                     i + 1, s_sdtest_files, fails, total, bad, ff, (esp_timer_get_time() - t0) / 1000000);
        }
    }
    free(buf);
    ESP_LOGW(TAG, "SDTEST done");
    s_sdtest_running = false;
    vTaskDelete(NULL);
}

/* 동시성 모드(?par=1) — 두 캠처럼 서로 다른 폴더에 청크 단위로 번갈아 쓰는 태스크 2개 + 한 폴더를 계속 훑으며 stat하는
 * 태스크 1개(사진 개수 세기/목록 읽기 흉내). 각 쓰기 태스크는 끝날 때 자기 폴더를 검사해 결과를 찍음 */
typedef struct { const char *dir; int files; } sdtest_par_arg_t;
static volatile int s_sdtest_par_left = 0;

static void sdtest_par_writer(void *arg)
{
    const sdtest_par_arg_t *a = (const sdtest_par_arg_t *)arg;
    char path[64], tmp[72];
    mkdir(a->dir, 0777);
    const size_t chunk = 16 * 1024;
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int fails = 0;
    for (int i = 0; buf && i < a->files; i++) {
        snprintf(path, sizeof(path), "%s/T%08d.jpg", a->dir, i);
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        memset(buf, (uint8_t)i, chunk);
        FILE *f = fopen(tmp, "wb");
        bool ok = (f != NULL);
        for (int k = 0; ok && k < s_sdtest_kb / 16; k++) {
            ok = (fwrite(buf, 1, chunk, f) == chunk);
            vTaskDelay(1);   /* 다른 쓰기/읽기와 번갈아 가게(청크가 CAN으로 조금씩 오는 실제 흐름 흉내) */
        }
        if (f) {
            if (ok) ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
            fclose(f);
        }
        if (ok) ok = (rename(tmp, path) == 0);
        if (!ok) { fails++; ESP_LOGW(TAG, "SDTEST-PAR %s file %d failed (errno=%d)", a->dir, i, errno); unlink(tmp); }
        if ((i + 1) % 50 == 0) {
            int total, bad, ff;
            sdtest_scan(a->dir, &total, &bad, &ff);
            ESP_LOGW(TAG, "SDTEST-PAR %s %d/%d (fails %d): entries %d, bad %d, FF %d", a->dir, i + 1, a->files, fails, total, bad, ff);
        }
    }
    free(buf);
    s_sdtest_par_left--;
    vTaskSuspend(NULL);  /* WithCaps 태스크 자기 삭제 대신 정지(시험용 — 스택 약간 남음) */
}

static void sdtest_par_reader(void *arg)
{
    const char *dir = (const char *)arg;
    char path[64];
    int scans = 0;
    while (s_sdtest_par_left > 0) {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                struct stat st;
                snprintf(path, sizeof(path), "%s/%.40s", dir, e->d_name);
                stat(path, &st);
            }
            closedir(d);
        }
        scans++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "SDTEST-PAR reader done: %d scans of %s", scans, dir);
    s_sdtest_running = false;
    vTaskSuspend(NULL);  /* WithCaps 태스크 자기 삭제 대신 정지(시험용 — 스택 약간 남음) */
}

static esp_err_t api_sdtest_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    char query[48] = { 0 };
    char v[12] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    s_sdtest_clean = (httpd_query_key_value(query, "clean", v, sizeof(v)) == ESP_OK && v[0] == '1');
    s_sdtest_files = (httpd_query_key_value(query, "files", v, sizeof(v)) == ESP_OK) ? atoi(v) : 300;
    s_sdtest_kb = (httpd_query_key_value(query, "kb", v, sizeof(v)) == ESP_OK) ? atoi(v) : 64;
    if (s_sdtest_kb < 16) s_sdtest_kb = 16;
    bool started = false;
    bool par = (httpd_query_key_value(query, "par", v, sizeof(v)) == ESP_OK && v[0] == '1');
    if (par && !s_sdtest_running && sd_storage_is_mounted()) {
        static sdtest_par_arg_t a = { SD_STORAGE_MOUNT_POINT "/sdtesta", 0 };
        static sdtest_par_arg_t b = { SD_STORAGE_MOUNT_POINT "/sdtestb", 0 };
        a.files = b.files = s_sdtest_files;
        s_sdtest_running = true;
        s_sdtest_par_left = 2;
        ESP_LOGW(TAG, "SDTEST-PAR start: 2 writers x %d files x %dKB + reader", s_sdtest_files, s_sdtest_kb);
        /* 스택은 PSRAM(feedback_prefer_psram_for_buffers) */
        started = xTaskCreatePinnedToCoreWithCaps(sdtest_par_writer, "sdt_wa", 6144, &a, 10, NULL, 1, MALLOC_CAP_SPIRAM) == pdPASS &&
                  xTaskCreatePinnedToCoreWithCaps(sdtest_par_writer, "sdt_wb", 6144, &b, 10, NULL, 1, MALLOC_CAP_SPIRAM) == pdPASS &&
                  xTaskCreatePinnedToCoreWithCaps(sdtest_par_reader, "sdt_r", 4096, (void *)a.dir, 9, NULL, 0, MALLOC_CAP_SPIRAM) == pdPASS;
    } else if (!s_sdtest_running && sd_storage_is_mounted()) {
        s_sdtest_running = true;
        static StaticTask_t s_tcb;
        static StackType_t *s_stack = NULL;
        if (!s_stack) s_stack = heap_caps_malloc(6144, MALLOC_CAP_SPIRAM);
        started = s_stack && xTaskCreateStaticPinnedToCore(sdtest_task, "sdtest", 6144 / sizeof(StackType_t), NULL, 10,
                                                            s_stack, &s_tcb, 1) != NULL;
        if (!started) s_sdtest_running = false;
    }
    char body[48];
    int len = snprintf(body, sizeof(body), "{\"started\":%s}", started ? "true" : "false");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, len);
}

/* 2026-09-28(임시 진단 — 그래프 계열이 안 나오는 원인 조사) — name 없으면 /sdcard/stats 목록(이름 크기),
 * name=파일이면 그 파일 원본을 그대로 내려줌. 측정값 기록과 겹치지 않게 stats_store_io_suspend 아래에서 읽음 */
static esp_err_t api_statsfile_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    char query[64] = { 0 };
    char name[32] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    bool want_file = (httpd_query_key_value(query, "name", name, sizeof(name)) == ESP_OK && name[0] != '\0' &&
                      strchr(name, '/') == NULL);
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, want_file ? "application/octet-stream" : "text/plain");
    stats_store_io_suspend();
    if (want_file) {
        char path[64];
        snprintf(path, sizeof(path), SD_STORAGE_MOUNT_POINT "/stats/%s", name);
        FILE *f = fopen(path, "rb");
        if (f) {
            size_t n;
            while ((n = fread(buf, 1, 4096, f)) > 0) {
                if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
            }
            fclose(f);
        }
    } else {
        int len = snprintf(buf, 4096, "now %lu\n", (unsigned long)rtc_sync_get_unix_time());
        httpd_resp_send_chunk(req, buf, len);
        DIR *d = opendir(SD_STORAGE_MOUNT_POINT "/stats");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                char path[64];
                struct stat st = { 0 };
                snprintf(path, sizeof(path), SD_STORAGE_MOUNT_POINT "/stats/%.40s", e->d_name);
                stat(path, &st);
                len = snprintf(buf, 4096, "%s %ld\n", e->d_name, (long)st.st_size);
                httpd_resp_send_chunk(req, buf, len);
            }
            closedir(d);
        }
    }
    stats_store_io_resume();
    free(buf);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t api_set_response_interval_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    char query[32] = { 0 };
    char sec_str[8] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "sec", sec_str, sizeof(sec_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing sec");
        return ESP_FAIL;
    }
    uint32_t sec = (uint32_t)strtoul(sec_str, NULL, 10);
    bool ok = ui_main_inject_set_response_interval(sec);

    char body[64];
    int len = snprintf(body, sizeof(body), "{\"ok\":%s}", ok ? "true" : "false");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, len);
}

/* 2026-09-07(사용자 지시 — 어젯밤 쌓인 이산화탄소 0 레코드 정리용) — 통계 전체 삭제
 * 버튼+확인팝업 합성. 되돌릴 수 없는 동작이라 이 엔드포인트도 실제 온디바이스 탭과
 * 동일 경로(버튼->확인팝업->Yes)를 그대로 탐.
 *
 * 2026-09-10(임시 비상탈출구 — SD 블록8256 반복실패로 LVGL 태스크가 몇 초씩 계속 막혀서
 * 위 정상경로(run_on_lvgl_task, 1초 데드라인)가 항상 타임아웃남 — "콘이 죽었는데 어떻게
 * 닫니" 상황. ?force=1이면 LVGL/화면 상태와 완전히 무관하게 httpd 태스크에서 직접
 * stats_store_delete_all()을 불러서 화면이 죽어있어도 파일만 지움. 평소엔(force 없이)
 * 원래 설계(실제 UI 경로 그대로 타기) 그대로 유지 — 상황 해결되면 이 분기는 제거 예정 */
static esp_err_t api_delete_stats_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    char query[16] = { 0 };
    bool force = (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                  strstr(query, "force=1") != NULL);
    bool ok = force ? (stats_store_delete_all(), true) : ui_main_inject_delete_stats();
    char body[64];
    int len = snprintf(body, sizeof(body), "{\"ok\":%s,\"force\":%s}", ok ? "true" : "false", force ? "true" : "false");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, len);
}

static esp_err_t api_disconnect_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
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
     * 합성이 성공하면 그 안에서 이미 완료된 것(node_hub_unpair()가 동기적) */
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
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    node_hub_node_t cams[NODE_HUB_MAX_NODES];
    int n = node_hub_get_nodes(HUB_NODE_KIND_CAM, cams, NODE_HUB_MAX_NODES);
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

    /* 2026-09-19(SD 제거 재설계 — "웹은 콘에 기생") — 목록이 이제 콘 SD 로컬 읽기라 CAM
     * 응답을 기다릴 이유(세대번호/25초 대기 등, 예전 ESP-NOW 왕복 시절 설계)가 없어짐.
     * 콘 화면의 "목록갱신" 버튼 탭을 그대로 합성하면 반환 시점에 이미 완료돼 있으므로,
     * 그 결과(콘 화면이 지금 보여주는 바로 그 목록)를 그대로 읽어감 */
    bool ok = ui_main_inject_list_refresh();
    const char *state_str = ok ? "ready" : "error";

    int len = snprintf(body, 4096, "{\"cam\":true,\"state\":\"%s\",\"items\":[", state_str);
    if (ok) {
        photo_storage_item_t items[32];
        int cnt = ui_main_get_photo_list(items, 32);
        for (int i = 0; i < cnt && len < 4096 - 150; i++) {
            char tag[16];
            format_file_tag((char)items[i].kind, items[i].seq, tag, sizeof(tag));
            len += snprintf(body + len, 4096 - len,
                             "%s{\"kind\":\"%c\",\"seq\":%u,\"tag\":\"%s\",\"capture_time\":%u,\"file_size\":%u}",
                             i == 0 ? "" : ",",
                             (char)items[i].kind, (unsigned)items[i].seq, tag,
                             (unsigned)items[i].mtime, (unsigned)items[i].file_size);
        }
    }
    /* 2026-09-04(사용자 설계: "앱의 문구들을 그대로 웹에서 써야한다") */
    const char *msg = ok ? ui_str(STR_LIST_FETCH_SUCCESS) : ui_str(STR_LIST_FETCH_FAILED);
    len += snprintf(body + len, 4096 - len, "],\"msg\":\"%s\"}", msg);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, body, len);
    heap_caps_free(body);
    return ret;
}

/* 2026-09-19(SD 제거 재설계 — "웹은 콘에 기생") — 로컬 SD 읽기라 콘 화면의 실제 사진목록
 * 행 탭을 합성하면(ui_main_inject_photo_select) 반환 시점에 이미 선택+판넬 표시까지
 * 동기로 끝나있음. 예전의 "캐시 우선 확인 -> 없으면 요청 후 25초 이벤트 대기"가 통째로
 * 불필요해짐(그건 CAM 응답을 기다려야 했던 시절 설계) */
static esp_err_t api_photo_fetch_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    char query[32] = { 0 };
    char kind_str[4] = { 0 }, seq_str[16] = { 0 };
    bool has_kind = false, has_seq = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        has_kind = (httpd_query_key_value(query, "kind", kind_str, sizeof(kind_str)) == ESP_OK);
        has_seq  = (httpd_query_key_value(query, "seq", seq_str, sizeof(seq_str)) == ESP_OK);
    }
    bool ok = false;
    if (has_kind && has_seq && kind_str[0] != '\0') {
        uint32_t seq = (uint32_t)strtoul(seq_str, NULL, 10);
        ok = ui_main_inject_photo_select((uint8_t)kind_str[0], seq);
    }
    /* 2026-09-04(사용자 설계: "앱의 문구들을 그대로 웹에서 써야한다") — 사진 성공/실패는
     * 콘 자신이 예전에 쓰던 STR_FETCH_DONE/STR_FETCH_FAILED를 그대로 재사용 */
    char body[96];
    int len = snprintf(body, sizeof(body), "{\"ok\":%s,\"msg\":\"%s\"}",
                        ok ? "true" : "false",
                        ok ? ui_str(STR_FETCH_DONE) : ui_str(STR_FETCH_FAILED));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, len);
}

/* 2026-08-30 — 정적 프론트엔드(assets에 업로드된 app.html)를 깔끔한 URL로 서빙. 파일이
 * 아직 없으면(최초 배포 전) 404 — /admin/upload?file=app.html로 올리면 그때부터 동작
 * 2026-10-03(할 일 AD — SPA) — app.html.gz가 있으면 그걸 gzip 그대로 보냄(전송량 감소). ETag(크기+수정시각)로
 * 브라우저가 가진 것과 같으면 304 — 첫 접속 뒤로는 본문을 다시 안 보냄(Cache-Control: no-cache = 매번 확인만) */
/* 2026-10-05(사용자 요청 — 브라우저 상·하단 없이) — 홈 화면에 추가해 앱처럼 열 때 필요한 manifest·아이콘도 같은 방식으로
 * (LittleFS 파일, 로그인 없이 — 홈 화면 추가 때 휴대폰이 쿠키 없이 받아 감). 경로마다 user_ctx = 이 표의 한 줄 */
typedef struct {
    const char *gz_path;   /* 있으면 먼저(gzip 그대로), NULL = 없음 */
    const char *path;
    const char *type;
} web_static_file_t;
static const web_static_file_t s_static_app = { FS_MOUNT_POINT "/app.html.gz", FS_MOUNT_POINT "/app.html", "text/html; charset=utf-8" };
static const web_static_file_t s_static_manifest = { NULL, FS_MOUNT_POINT "/app.webmanifest", "application/manifest+json" };
static const web_static_file_t s_static_icon = { NULL, FS_MOUNT_POINT "/app_icon.png", "image/png" };

static esp_err_t app_get_handler(httpd_req_t *req)
{
    const web_static_file_t *sf = (const web_static_file_t *)req->user_ctx;
    const char *path = sf->gz_path;
    bool gz = (path != NULL);
    struct stat st;
    if (!gz || stat(path, &st) != 0) {
        path = sf->path;
        gz = false;
        if (stat(path, &st) != 0) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not uploaded yet");
            return ESP_FAIL;
        }
    }
    /* ETag·If-None-Match 문자열은 PSRAM(스택에 두지 않음). ETag 헤더는 응답을 보낼 때까지 살아 있어야 해서 끝에 해제 */
    char *etag = heap_caps_calloc(1, 80, MALLOC_CAP_SPIRAM);
    if (!etag) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char *inm = etag + 40;
    snprintf(etag, 40, "\"%lx-%llx%s\"", (unsigned long)st.st_size, (unsigned long long)st.st_mtime, gz ? "g" : "");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", etag);
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, 40) == ESP_OK && strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        esp_err_t r304 = httpd_resp_send(req, NULL, 0);
        heap_caps_free(etag);
        return r304;
    }

    FILE *f = fopen(path, "rb");
    char *buf = f ? heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM) : NULL;
    if (!buf) {
        if (f) fclose(f);
        heap_caps_free(etag);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t rd = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);

    httpd_resp_set_type(req, sf->type);
    if (gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    esp_err_t ret = httpd_resp_send(req, buf, rd);
    heap_caps_free(buf);
    heap_caps_free(etag);
    return ret;
}

/* 2026-10-03(할 일 AD — SPA 모델 API) — 노드 전체(캠·센스) 목록과 연결 상태. 읽기 전용. kind=cam|sens, status는
 * /api/devices와 같은 코드(waiting/active/paired). 문구는 안 보냄 — SPA가 코드를 한/영 문구로 바꿈(설계) */
static esp_err_t api_nodes_get_handler(httpd_req_t *req)
{
    WEB_SCREEN_API_BEGIN(req);  /* 10-03 — 인증 + 웹 세션(콘 잠금) */
    /* 노드 배열(구조체가 큼)과 본문은 PSRAM — 스택에 두지 않음 */
    node_hub_node_t *nodes = heap_caps_malloc(sizeof(node_hub_node_t) * NODE_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    const size_t cap = 4096;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!nodes || !body) {
        heap_caps_free(nodes);
        heap_caps_free(body);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int n = node_hub_get_nodes(HUB_NODE_KIND_UNKNOWN, nodes, NODE_HUB_MAX_NODES);
    int len = snprintf(body, cap, "{\"nodes\":[");
    for (int i = 0; i < n && len < (int)cap - 200; i++) {
        hub_conn_state_t cs = node_hub_get_conn_state(nodes[i].mac);
        const char *status = (cs == HUB_CONN_STATE_WAITING) ? "waiting"
                            : (cs == HUB_CONN_STATE_ACTIVE)  ? "active" : "paired";
        const char *kind = (nodes[i].kind == HUB_NODE_KIND_CAM) ? "cam"
                         : (nodes[i].kind == HUB_NODE_KIND_SENS) ? "sens" : "unknown";
        len += snprintf(body + len, cap - len,
                        "%s{\"mac\":\"%02x%02x%02x%02x%02x%02x\",\"name\":\"%s\",\"kind\":\"%s\",\"status\":\"%s\","
                        "\"paired\":%s}",
                        i == 0 ? "" : ",",
                        nodes[i].mac[0], nodes[i].mac[1], nodes[i].mac[2],
                        nodes[i].mac[3], nodes[i].mac[4], nodes[i].mac[5],
                        nodes[i].name, kind, status,
                        nodes[i].conn_state == NODE_CONN_PAIRED ? "true" : "false");
    }
    len += snprintf(body + len, cap - len, "],\"time\":%lu}", (unsigned long)rtc_sync_get_unix_time());
    heap_caps_free(nodes);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, body, len);
    heap_caps_free(body);
    return ret;
}

/* 2026-10-03(할 일 AD 1단계 — 시험용, 상용화 때 제거: 조건부 C9) — 휴대폰 알림 시험. msg=로 본문 지정(없으면 기본 문구).
 * 큐에 넣기만 하고 바로 응답(실제 전송 결과는 시리얼 NOTIFY 로그) */
static esp_err_t api_notify_test_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
    /* query·msg는 PSRAM(스택에 두지 않음) */
    char *query = heap_caps_calloc(1, 160 + 128, MALLOC_CAP_SPIRAM);
    if (!query) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char *msg = query + 160;
    snprintf(msg, 128, "Test notification from Cntl");
    if (httpd_req_get_url_query_str(req, query, 160) == ESP_OK) {
        httpd_query_key_value(query, "msg", msg, 128);
    }
    bool ok = notify_send("Cntl test", msg);
    heap_caps_free(query);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
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
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
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
    if (strcmp(filename, "notify.cfg") == 0) notify_reload_cfg();  /* 10-03 — 콘 화면 Web 주소·QR 바로 반영 */

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t admin_download_get_handler(httpd_req_t *req)
{
    WEB_AUTH_REQUIRE(req);  /* 2026-10-03(할 일 AD) — 바깥에 열리는 웹이라 로그인 필요 */
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

    ESP_LOGD(TAG, "Web: entered web_dashboard_start()");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* 2026-08-21 — 5005(httpd_start 실패) 원인 확인됨: 부팅 이 시점엔 내부(비-PSRAM) DRAM이
     * 거의 바닥남(실기 확인: free internal=1419B) — HTTPD_DEFAULT_CONFIG()의 task_caps
     * 기본값이 MALLOC_CAP_INTERNAL이라 태스크 스택을 내부 RAM에서만 찾다가 실패함. PSRAM은
     * 넉넉하니(같은 시점 free heap=263660B) 여기로 돌림(esp_lv_adapter의 stack_in_psram,
     * 폰트 버퍼의 font_buf_malloc과 동일 원칙) */
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    /* 2026-08-30 — 핸들러들이 지역 노드 배열(node_hub_node_t[8])을 스택에 두는데,
     * 기본 스택 크기가 빠듯할 수 있어 확대(PSRAM이라 비용 낮음) */
    config.stack_size = 8192;
    /* 2026-08-30 — URI 핸들러가 계속 늘어나서(root/photo/admin 2개 + API) 기본
     * max_uri_handlers(8)를 넘을 수 있어 여유있게 확대 */
    /* 2026-09-27 — capture_now/devlog 추가로 16을 채움, 여유. 10-03 — nodes/notify_test로 20을 채워 24,
     * login/password/logout_all/session_claim 4개 추가로 28, 웹 화면 API(장치·릴레이·카메라 등 18개) 여유 포함 48 */
    /* 2026-10-05 — 통계 3개·홈 화면 2개·설정 7개 추가로 50 → 64 */
    config.max_uri_handlers = 64;
    /* 2026-10-03(할 일 AD — 웹 접속은 한 곳) — 동시 연결 3개(브라우저 한 대가 여는 연결 몇 개), 넘치면 가장 오래 쉰 연결을 닫음.
     * 웹 전송 하나가 내부 RAM 약 20K를 잠깐 씀(10-01 실측) — 연결 수를 묶어 그 이상 커지지 않게 */
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
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
    ESP_LOGD(TAG, "Web: httpd_start SUCC");
    static const httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    esp_err_t root_err = httpd_register_uri_handler(server, &root_uri);
    ESP_LOGD(TAG, "Web: '/' register %s", root_err == ESP_OK ? "SUCC" : "FAIL");
    static const httpd_uri_t photo_uri = { .uri = "/photo", .method = HTTP_GET, .handler = photo_get_handler };
    esp_err_t photo_err = httpd_register_uri_handler(server, &photo_uri);
    ESP_LOGD(TAG, "Web: '/photo' register %s", photo_err == ESP_OK ? "SUCC" : "FAIL");
    static const httpd_uri_t admin_upload_uri = { .uri = "/admin/upload", .method = HTTP_POST,
                                                    .handler = admin_upload_post_handler };
    esp_err_t admin_upload_err = httpd_register_uri_handler(server, &admin_upload_uri);
    ESP_LOGD(TAG, "Web: '/admin/upload' register %s", admin_upload_err == ESP_OK ? "SUCC" : "FAIL");
    static const httpd_uri_t admin_download_uri = { .uri = "/admin/download", .method = HTTP_GET,
                                                      .handler = admin_download_get_handler };
    esp_err_t admin_download_err = httpd_register_uri_handler(server, &admin_download_uri);
    ESP_LOGD(TAG, "Web: '/admin/download' register %s", admin_download_err == ESP_OK ? "SUCC" : "FAIL");
    static const httpd_uri_t api_devices_uri = { .uri = "/api/devices", .method = HTTP_GET,
                                                   .handler = api_devices_get_handler };
    httpd_register_uri_handler(server, &api_devices_uri);
    static const httpd_uri_t api_connect_uri = { .uri = "/api/connect", .method = HTTP_GET,
                                                   .handler = api_connect_get_handler };
    httpd_register_uri_handler(server, &api_connect_uri);
    static const httpd_uri_t api_disconnect_uri = { .uri = "/api/disconnect", .method = HTTP_GET,
                                                      .handler = api_disconnect_get_handler };
    httpd_register_uri_handler(server, &api_disconnect_uri);
    static const httpd_uri_t api_connect_sensor_uri = { .uri = "/api/connect_sensor", .method = HTTP_GET,
                                                          .handler = api_connect_sensor_get_handler };
    httpd_register_uri_handler(server, &api_connect_sensor_uri);
    static const httpd_uri_t api_set_response_interval_uri = { .uri = "/api/set_response_interval",
                                                                  .method = HTTP_GET,
                                                                  .handler = api_set_response_interval_get_handler };
    httpd_register_uri_handler(server, &api_set_response_interval_uri);
    static const httpd_uri_t api_capture_now_uri = { .uri = "/api/capture_now", .method = HTTP_GET,
                                                     .handler = api_capture_now_get_handler };
    httpd_register_uri_handler(server, &api_capture_now_uri);
    static const httpd_uri_t api_devlog_uri = { .uri = "/api/devlog", .method = HTTP_GET,
                                                .handler = api_devlog_get_handler };
    httpd_register_uri_handler(server, &api_devlog_uri);
    static const httpd_uri_t api_devlog_dump_uri = { .uri = "/api/devlog_dump", .method = HTTP_GET,
                                                     .handler = api_devlog_dump_get_handler };
    httpd_register_uri_handler(server, &api_devlog_dump_uri);
    static const httpd_uri_t api_sdtest_uri = { .uri = "/api/sdtest", .method = HTTP_GET,
                                                .handler = api_sdtest_get_handler };
    httpd_register_uri_handler(server, &api_sdtest_uri);
    static const httpd_uri_t api_statsfile_uri = { .uri = "/api/statsfile", .method = HTTP_GET,
                                                   .handler = api_statsfile_get_handler };
    httpd_register_uri_handler(server, &api_statsfile_uri);
    static const httpd_uri_t api_delete_stats_uri = { .uri = "/api/delete_stats", .method = HTTP_GET,
                                                        .handler = api_delete_stats_get_handler };
    httpd_register_uri_handler(server, &api_delete_stats_uri);
    static const httpd_uri_t api_photos_uri = { .uri = "/api/photos", .method = HTTP_GET,
                                                  .handler = api_photos_get_handler };
    httpd_register_uri_handler(server, &api_photos_uri);
    static const httpd_uri_t api_photo_fetch_uri = { .uri = "/api/photo_fetch", .method = HTTP_GET,
                                                       .handler = api_photo_fetch_get_handler };
    httpd_register_uri_handler(server, &api_photo_fetch_uri);
    ESP_LOGD(TAG, "Web: API endpoints registered");
    static const httpd_uri_t app_uri = { .uri = "/app", .method = HTTP_GET, .handler = app_get_handler, .user_ctx = (void *)&s_static_app };
    httpd_register_uri_handler(server, &app_uri);
    static const httpd_uri_t app_manifest_uri = { .uri = "/app.webmanifest", .method = HTTP_GET, .handler = app_get_handler,
                                                  .user_ctx = (void *)&s_static_manifest };
    httpd_register_uri_handler(server, &app_manifest_uri);
    static const httpd_uri_t app_icon_uri = { .uri = "/app_icon.png", .method = HTTP_GET, .handler = app_get_handler,
                                              .user_ctx = (void *)&s_static_icon };
    httpd_register_uri_handler(server, &app_icon_uri);
    static const httpd_uri_t api_nodes_uri = { .uri = "/api/nodes", .method = HTTP_GET, .handler = api_nodes_get_handler };
    httpd_register_uri_handler(server, &api_nodes_uri);
    static const httpd_uri_t api_notify_test_uri = { .uri = "/api/notify_test", .method = HTTP_GET,
                                                       .handler = api_notify_test_get_handler };
    httpd_register_uri_handler(server, &api_notify_test_uri);
    web_auth_register_handlers(server);
    web_session_register_handlers(server);
    web_api_register_handlers(server);
    /* 2026-08-21 — 성공할 때도 같은 여유메모리를 남김(사용자 지시) — 실패할 때만 찍으면
     * "언제부터 빠듯해지기 시작했는지" 추세를 못 봄. 5005는 이 시점 내부RAM이 간당간당할
     * 때만 뜨는 경계선 증상이라, 성공한 부팅들의 수치도 같이 쌓여야 나중에 진짜 임계점을
     * 추적할 수 있음 */
    unsigned free_heap = (unsigned)esp_get_free_heap_size();
    unsigned free_internal = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Web dashboard (stub) started (free heap=%u, free internal=%u)", free_heap, free_internal);
    ESP_LOGD(TAG, "Web server started (heap=%uB, internal=%uB)", free_heap, free_internal);
}

/* 2026-08-21 — 예전엔 app_main() 맨 끝에서 node_hub_init() 직후 곧바로 불렀는데, 그
 * 시점엔 WiFi가 아직 인증/연결 단계라 IP를 받기도 전이었음(실기 로그로 확인: httpd_start가
 * IP_EVENT_STA_GOT_IP보다 1초 이상 먼저 실행됨) — 이게 5005(httpd_start 실패)가 항상 뜨던
 * 원인. IP를 실제로 받은 뒤에 시작하도록 이벤트로 미룸. 재연결로 GOT_IP가 여러 번 올 수
 * 있는데, 그 중복 방어는 web_dashboard_start() 내부로 옮김(AP 모드 직접 호출 경로와
 * 공유해야 해서, 2026-08-30) */
static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg; (void)event_base; (void)event_data;
    ESP_LOGD(TAG, "IP_EVENT(id=%ld) received", (long)event_id);
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
    notify_init();  /* 2026-10-03(할 일 AD) — 휴대폰 알림 보내기 태스크(설정 notify.cfg는 LittleFS) */
    alarm_init();   /* 2026-10-05(할 일 AD 5단계) — 알림 기록 큐(사건은 여기서부터 쌓이고 처리는 alarm_start 뒤) */
    web_auth_init();  /* 2026-10-03(할 일 AD) — 웹 로그인(web_auth.bin) */
    /* 2026-09-27(로그 정리) — 개발 로그: ESP_LOG 가로채기 시작 + 저장 문턱 적용(설정은 /assets/devlog.cfg라 fs 뒤) */
    dev_log_init();

    /* 영구 저장 설정값(언어 등) 복원 — UI 생성(ui_init) 전에 해야 라벨이 처음부터
     * 올바른 언어로 뜸 */
    ui_lang_load();
    /* CAM/SENS 원격 설정값(Cntl이 주인, 2026-08-08 설계) — UI 생성 전에 로드해야 설정탭
     * 드롭다운이 처음부터 저장된 값을 보여줌(부팅 시 "값 미리 로드" 요구사항) */
    device_config_load();
    /* SR(Power Control) 설정(2026-09-16) — 같은 이유로 UI 생성 전에 로드 */
    power_relay_load();

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
    const uint8_t frame_buffer_count = esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation);

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_touch_handle_t touch_handle = NULL;

    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init(
        frame_buffer_count,
        &panel_handle,
        &touch_handle));
    /* 2026-09-22 — waveshare_rgb_lcd_backlight_on() 내부가 이제 I2C 실패 시 abort하지 않고
     * 항상 ESP_OK를 반환하도록 바뀜(실기 크래시로 발견된 abort 지점) — 여기서도
     * ESP_ERROR_CHECK로 다시 감싸면 안 됨(그러면 이 바깥 지점에서 또 abort하게 됨) */
    waveshare_rgb_lcd_backlight_on();

    /* 2026-09-06(사용자 지시) — SD카드 마운트, 통계탭 시계열 저장용. LCD/CH422G가 이미
     * 초기화된 뒤에 불러야 함(CH422G 공유 I2C 버스/섀도우 상태 의존, sd_storage.h 참고).
     * 실패해도(SD 미장착 등) 앱 전체를 막지 않음 — 화면/통신 등 다른 기능은 SD와 무관 */
    /* 2026-09-07(임시 진단 — 사용자 지시: "통계탭 넣기 전보다 30~40KB 줄었어") — 통계탭
     * 위젯 자체 비용(ui_main.c의 MEMDIAG 로그)은 10.5KB로 이미 확인됐는데, 이거보다 훨씬
     * 큰 차이가 나서 여기(SD/SPI DMA 마운트, ui_init()보다 먼저라 그 측정 범위 밖) 비용도
     * 별도로 재봄 */
    size_t heap_before_sd = MEMDIAG_HEAP();
    esp_err_t sd_err = sd_storage_init();
    size_t heap_after_sd = MEMDIAG_HEAP();
    ESP_LOGD(TAG, "MEMDIAG SD mount cost: internal %u -> %u (used %d bytes)",
             (unsigned)heap_before_sd, (unsigned)heap_after_sd,
             (int)heap_before_sd - (int)heap_after_sd);
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) - continuing without stats storage", esp_err_to_name(sd_err));
        ui_log_add_err(UI_ERR_SD_MOUNT_FAILED, "SD card mount failed: %s", esp_err_to_name(sd_err));
    } else {
        ui_log_add("SD card mounted OK");
    }
    /* 2026-09-26 — SD 사용량 관리/정리 전담 파일처리 태스크(storage_mgr.h). 시작하자마자 한 번
     * 재스캔(사진 폴더/측정값 주 파일) — 미마운트여도 띄워 둠(재연결 성공 시 재스캔 요청이 옴) */
    storage_mgr_start();
    /* 2026-09-19(통계 분류 영구저장) — SD 마운트 이후에만 의미 있음(파일이 SD에 있음) */
    sens_kind_store_load();

    /* 보드 실장 PCF85063A RTC — I2C 버스가 막 만들어진 직후, UI가 뜨기 전에 시각을
     * 읽어와야 로고 부제(시계)가 처음부터 맞는 값으로 뜸 */
    esp_err_t rtc_ret = rtc_sync_init();
    ESP_LOGI(TAG, "rtc_sync_init: %s", rtc_ret == ESP_OK ? "OK" : "FAILED");
    alarm_start();  /* SD(기록 파일)·RTC(시각)가 준비된 뒤 — 부팅 중 쌓인 사건(SD 마운트 실패 등)도 여기서 처리 */
    {   /* 2026-10-09(사용자 지시 — 콘 크래시를 하루 가까이 모르고 지남) — 콘 재시작을 알림 목록에. 크래시·워치독·전압 저하는 푸시,
         * 플래시(USB)·전원·소프트웨어 재시작은 기록만 */
        esp_reset_reason_t rr = esp_reset_reason();
        const char *why = "other";
        bool fault = false;
        switch (rr) {
            case ESP_RST_POWERON:  why = "power on"; break;
            case ESP_RST_USB:      why = "USB/flash"; break;
            case ESP_RST_SW:       why = "software"; break;
            case ESP_RST_EXT:      why = "reset pin"; break;
            case ESP_RST_PANIC:    why = "crash (panic)"; fault = true; break;
            case ESP_RST_INT_WDT:  why = "interrupt watchdog"; fault = true; break;
            case ESP_RST_TASK_WDT: why = "task watchdog"; fault = true; break;
            case ESP_RST_WDT:      why = "watchdog"; fault = true; break;
            case ESP_RST_BROWNOUT: why = "brownout"; fault = true; break;
            default: break;
        }
        if (fault) alarm_post(NOTIFY_TYPE_ERROR, "Cntl restarted", "Cntl restarted: %s (reason %d)", why, (int)rr);
        else alarm_record(NOTIFY_TYPE_WARN, "Cntl restarted", "Cntl restarted: %s (reason %d)", why, (int)rr);
    }

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.stack_in_psram = true;
    /* 2026-09-25(사용자 설계 — 통신/UI 코어 분리) — LVGL은 코어 0, CAN 통신은 코어 1.
     * Wi-Fi(웹 전용)도 코어 0이지만 웹 접속 중엔 앱 UI를 직접 조작할 일이 없어 충돌 없음 */
    adapter_config.task_core_id = 0;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t disp_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        panel_handle,
        NULL,
        EXAMPLE_LCD_H_RES,
        EXAMPLE_LCD_V_RES,
        rotation);
    disp_config.profile.use_psram = true;
    /* 2026-09-06(내부 RAM 위기 대응) — TRIPLE_PARTIAL 찢김방지 모드의 부분버퍼는
     * display_manager.c가 PSRAM 여부를 무시하고 항상 내부 RAM에 고정 할당함(벤더 컴포넌트,
     * 직접 패치 안 함). 기본 buffer_height=50이면 800*50*2=78KB — 내부 RAM 106KB 소비의
     * 대부분을 차지(project_cntl_stats_tab_memory_2026_09_06 메모리 참고). 10으로 낮춰서
     * 800*10*2=16KB로 줄임(측정: 위기 시점 대비 약 50KB 회수) — 실기에서 화면 깜빡임/찢김
     * 없음 확인(사용자), 20에서 한 단계 더 낮춘 값 */
    disp_config.profile.buffer_height = 10;

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
        web_session_init(ui_main_on_web_session_lock);  /* 2026-10-03(할 일 AD) — 웹 접속 중 콘 잠금 */
        esp_lv_adapter_unlock();
    }

    /* Cntl 통합 테스트 4단계 → CAM 연결 기능: WiFi+ESP-NOW 허브(페어링/노드테이블 포함) —
     * Cntl main.c와 동일하게 UI 뜬 뒤 마지막에 켬.
     * 2026-09-22(브릿지 역할 벤치테스트 시도 1 — 실패) — 이 초기화 전체를 건너뛰려 했더니
     * UI의 주기 갱신(refresh_dashboard 등)이 node_hub/tx가 만드는 세마포어/큐를 그대로
     * 가정하고 있어서 NULL 핸들로 assert 크래시(xQueueSemaphoreTake)남. UI는 그대로 두면서
     * 초기화만 건너뛰는 건 이 코드베이스 구조상 안전하지 않음 — 그래서 초기화는 항상 정상
     * 진행하고, 대신 아래에서 recv_cb만 브릿지용으로 바꿔치기하는 방식으로 변경 */
    photo_rx_init();
    /* 2026-09-26(실기 크래시 — 부팅 루프) — node_hub_init()이 CAN 수신을 시작하면 브가 중계한 캠 광고가 곧바로
     * 도착해 자동연결로 node_request_enqueue()가 불림. node_request_init()이 아래(Wi-Fi 초기화 뒤)에 있어서 큐가
     * NULL인 채 assert(uxQueueMessagesWaiting)로 재부팅, 캠이 광고 중이면 매 부팅 반복됐음 → CAN 수신보다 먼저 */
    node_request_init();
    node_hub_init();
    /* 2026-09-26 — node_hub_init() 안에 있던 Wi-Fi 초기화를 분리. 순서 유지: node_hub가 먼저(노드
     * 뮤텍스 생성 — AP 모드에선 wifi_sta_init() 안에서 웹 대시보드가 바로 시작되고, 그 핸들러가
     * node_hub를 부름) */
    wifi_sta_init();  /* 내부에서 esp_netif_init()+esp_event_loop_create_default() 호출 —
                             아래 이벤트 등록은 반드시 그 다음이어야 함 */

    /* SR(Power Control) 판정 루프 시작(2026-09-16) — GPIO 초기화 + 15초 주기 태스크.
     * ui_main_query_power_source_value()가 node_hub 노드 테이블/SD 집계를 읽으므로 그
     * 둘이 갖춰진 뒤(SD는 위에서 이미 마운트됨, node_hub는 방금 init)가 안전 */
    power_relay_start();

    /* 웹 대시보드는 실제로 IP를 받은 뒤에 시작(위 ip_event_handler 참고, 5005 버그 수정) */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &ip_event_handler, NULL, NULL));

    ui_main_register_wifi_events();  /* 같은 이유로 여기서(wifi_sta_init() 이후) 등록 */
}
