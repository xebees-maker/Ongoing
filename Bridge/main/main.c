#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "waveshare_rgb_lcd_port.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "ui_screen.h"
#include "bridge_esp_now.h"
#include "can_link.h"

/**
 * 2026-09-22 — 브(브릿지) 프로젝트, 1차 개발. 사용자 지시대로 콘의 소스/초기화 구조를
 * 전혀 가져오지 않고 처음부터 새로 짬(project_cntl_i2c_bridge_design_2026_09_21 설계 —
 * 역할분담은 그대로, 전송 계층만 I2C에서 CAN으로). 지금은 실물 브릿지 보드(외장안테나+CAN)
 * 구매 전이라, LCD가 달린 콘 보드를 빌려서 이 펌웨어를 플래시해 검증하는 벤치테스트 단계
 * (사용자 계획 2단계) — 그래서 LCD/터치 BSP는 콘의 것을 하드웨어 드라이버로만 재사용.
 *
 * 초기화 순서: NVS -> LCD/LVGL(화면: 메모리 표시 + 무선/CAN 로그창 2개) -> ESP-NOW(라디오
 * 소유) -> CAN(콘과의 링크). 콘의 허브/사진/TX/전력릴레이/웹 대시보드는 전혀 없음 — 그건
 * 전부 콘 쪽(진짜 CASK 판단)의 몫.
 */

static const char *TAG = "bridge_main";

void app_main(void)
{
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
    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init(frame_buffer_count, &panel_handle, &touch_handle));
    waveshare_rgb_lcd_backlight_on();  /* 실패해도 abort 안 함(콘과 동일 방어 로직 그대로 복사) */

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.stack_in_psram = true;
    /* 2026-09-25(사용자 설계 — 코어 분리) — LVGL은 코어 0, CAN은 코어 1. 실제 브엔 UI가 없고
     * 지금 화면은 벤치테스트용 임시 장치라서 있는 것 */
    adapter_config.task_core_id = 0;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t disp_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        panel_handle, NULL, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, rotation);
    disp_config.profile.use_psram = true;
    disp_config.profile.buffer_height = 10;  /* 콘의 내부RAM 위기 대응값 그대로(같은 하드웨어) */
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_config);
    assert(disp != NULL);
    (void)touch_handle;  /* 브는 터치 조작이 필요 없음(로그만 표시) — 등록 안 함 */

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        ui_screen_init();
        esp_lv_adapter_unlock();
    }

    /* 2026-09-22(실기 크래시로 발견 — StoreProhibited, can_bridge_send() 안에서 NULL ctx
     * 역참조) — bridge_esp_now_init()이 만드는 relay_task가 can_link_get_data_ctx()를
     * 태스크 시작 시점에 한 번 캡처하는데, 그때 can_link_init()이 아직 안 불려서 ctx가
     * NULL이었음. CAN 링크를 먼저 세운 뒤 ESP-NOW를 켜야 함 */
    can_link_init();
    bridge_esp_now_init();

    ESP_LOGI(TAG, "브 시작됨 (콘 소스 미사용, 자체 초기화)");
}
