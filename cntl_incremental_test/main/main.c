#include <assert.h>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "lv_demos.h"
#include "waveshare_rgb_lcd_port.h"
#include "nvs_flash.h"
#include "fs.h"
#include "esp_heap_caps.h"
#include "draw/lv_draw_buf_private.h"
#include "ui_font.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_http_server.h"
#include <string.h>

static const char *TAG = "lvgl9_demo";

/* Cntl 통합 테스트 4단계: WiFi+ESP-NOW 라디오만 (esp_now_hub.c의 wifi_bringup()+esp_now_init()
 * 부분만 최소 재현 — 노드테이블/페어링/웹대시보드는 사용자 지시로 별도 단계로 분리, 여기선
 * 뺌). Cntl 실제 sdkconfig 기준 STA 모드+SSID/PW 그대로(문자열 리터럴로 간단히). */
#define TEST_WIFI_SSID     "iptime2.4"
#define TEST_WIFI_PASSWORD "sk1234!@#"

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg; (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected - retry");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    }
}

static void esp_now_recv_stub(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    ESP_LOGI(TAG, "ESP-NOW recv: %d bytes", len);
}

static void wifi_esp_now_bringup(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, TEST_WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, TEST_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_stub));

    static const uint8_t broadcast_addr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, broadcast_addr, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    ESP_LOGI(TAG, "WiFi+ESP-NOW bringup complete (STA)");
}

/* Cntl 통합 테스트 5단계: HTTP 서버만 최소로(esp_now_hub 노드 데이터 없이 더미 페이지) —
 * "esp_http_server 자체가 RGB 패널을 깨는지"만 격리해서 확인 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char page[] = "<html><body><h2>Cntl web test</h2></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static void web_dashboard_start_stub(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    static const httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(server, &root_uri);
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
    /* Cntl 통합 테스트 1단계: NVS init (Cntl main.c와 동일) */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

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

    /* Cntl 통합 테스트 2단계: LittleFS "fonts" 파티션 마운트만(폰트 로드는 아직 안 함, 3단계) */
    ESP_ERROR_CHECK(fs_init());

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
        esp_lv_adapter_unlock();
    }

    ESP_LOGI(TAG, "Starting LVGL widgets demo");
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_demo_widgets();
        esp_lv_adapter_unlock();
    }

    /* Cntl 통합 테스트 4단계: WiFi+ESP-NOW — Cntl main.c와 동일하게 UI 뜬 뒤 마지막에 켬 */
    wifi_esp_now_bringup();

    /* Cntl 통합 테스트 5단계: 웹 대시보드(HTTP 서버, 최소 버전) */
    web_dashboard_start_stub();
}
