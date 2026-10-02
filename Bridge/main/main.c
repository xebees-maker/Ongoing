#include "esp_log.h"
#include "nvs_flash.h"

#include "bridge_esp_now.h"
#include "can_link.h"

/**
 * 2026-09-22 — 브(브릿지) 프로젝트, 1차 개발. 사용자 지시대로 콘의 소스/초기화 구조를
 * 전혀 가져오지 않고 처음부터 새로 짬(project_cntl_i2c_bridge_design_2026_09_21 설계 —
 * 역할분담은 그대로, 전송 계층만 I2C에서 CAN으로). 지금은 실물 브릿지 보드(외장안테나+CAN)
 * 구매 전이라 LCD가 달린 콘 보드를 빌려 벤치테스트했음(화면: 메모리 표시 + 무선/CAN 로그창).
 * 2026-10-02 — 실물 브 보드(Waveshare ESP32-S3-RS485-CAN: ESP32-S3R8, 16MB 플래시, 화면·터치 없음, 상시 전원)로
 * 옮김. CAN은 절연 트랜시버(TJA1051, π163E31 절연) — TX=IO15, RX=IO16으로 예전 보드와 같음. 화면이 없어서
 * LCD/LVGL/터치/CH422G를 모두 빼고 로그는 시리얼만(bridge_log.h). RS485·RTC는 안 씀.
 *
 * 초기화 순서: NVS -> CAN(콘과의 링크) -> ESP-NOW(라디오 소유). 콘의 허브/사진/TX/전력릴레이/웹 대시보드는
 * 전혀 없음 — 그건 전부 콘 쪽(진짜 CASK 판단)의 몫.
 */

static const char *TAG = "SYS";

void app_main(void)
{
    /* 브 자체 통신 태그만 D(빌드 상한 D, IDF 내부 태그는 기본 I 유지) — 브엔 실행 중 로그 레벨을 바꿀 UI가 없음 */
    esp_log_level_set("PHOTO", ESP_LOG_DEBUG);
    esp_log_level_set("LINK", ESP_LOG_DEBUG);
    esp_log_level_set("CAN", ESP_LOG_DEBUG);
    /* 2026-10-02 — 예전 화면 로그창 두 개의 내용(bridge_log.h) */
    esp_log_level_set("wireless", ESP_LOG_DEBUG);
    esp_log_level_set("can_ui", ESP_LOG_DEBUG);

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* 2026-09-22(실기 크래시로 발견 — StoreProhibited, can_bridge_send() 안에서 NULL ctx
     * 역참조) — bridge_esp_now_init()이 만드는 relay_task가 can_link_get_data_ctx()를
     * 태스크 시작 시점에 한 번 캡처하는데, 그때 can_link_init()이 아직 안 불려서 ctx가
     * NULL이었음. CAN 링크를 먼저 세운 뒤 ESP-NOW를 켜야 함 */
    can_link_init();
    bridge_esp_now_init();

    ESP_LOGI(TAG, "Bridge started (standalone init)");
}
