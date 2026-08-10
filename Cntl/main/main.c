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
#include "esp_now_hub.h"
#include "esp_now_photo.h"
#include "esp_now_tx.h"
#include "esp_lv_decoder.h"
#include "ui_log.h"
#include "rtc_sync.h"
#include "device_config.h"
#include <string.h>
#include <stdlib.h>

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

/* Cntl 통합 테스트 5단계: HTTP 서버만 최소로(esp_now_hub 노드 데이터 없이 더미 페이지) —
 * "esp_http_server 자체가 RGB 패널을 깨는지"만 격리해서 확인 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char page[] = "<html><body><h2>Cntl web test</h2></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

/* 원본 해상도 그대로 원격에서 보기 — Cntl은 캐시된 압축 JPEG 바이트를 그대로 던져줄 뿐,
 * 디코드는 요청한 브라우저가 함(PC/폰은 메모리 여유가 있어서 원본을 그대로 풀 수 있음,
 * Cntl 자체 화면은 PSRAM이 부족해서 못 함 — 2026-08-01). 기기에서 한 번도 안 열어본
 * (캐시에 없는) 사진은 404 — 웹 요청으로 새로 CAM에서 받아오는 건 이번 범위 밖 */
static esp_err_t photo_get_handler(httpd_req_t *req)
{
    char query[32] = { 0 };
    char id_str[16] = { 0 };
    uint32_t file_id = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", id_str, sizeof(id_str));
        file_id = (uint32_t)strtoul(id_str, NULL, 10);
    }

    const uint8_t *data = NULL;
    size_t len = 0;
    if (file_id == 0 || !esp_now_photo_cache_get(file_id, &data, &len)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "photo not cached — open it on the device first");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    return httpd_resp_send(req, (const char *)data, len);
}

static void web_dashboard_start_stub(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        ui_log_add_err(UI_ERR_HTTPD_START, "웹서버 시작 실패");
        return;
    }
    static const httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(server, &root_uri);
    static const httpd_uri_t photo_uri = { .uri = "/photo", .method = HTTP_GET, .handler = photo_get_handler };
    httpd_register_uri_handler(server, &photo_uri);
    ESP_LOGI(TAG, "Web dashboard (stub) started");
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
    esp_now_hub_init();
    esp_now_tx_init();

    /* Cntl 통합 테스트 5단계: 웹 대시보드(HTTP 서버, 최소 버전) */
    web_dashboard_start_stub();
}
