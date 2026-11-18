#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* 2026-11-18(사용자 결정) — 보드만 보고 재브·보브 구분: RS485 LED의 초록(LED2, 공통 양극 3V3, 캐소드가 TXD1=IO17 — LOW면 켜짐).
 * 브는 RS485를 안 써서 IO17을 그냥 출력으로 씀. 파랑(RXD1)은 RS485 칩 출력이라 못 씀.
 *  - 번호 있음: 5초마다 재브 1번, 보브 2번(200 ms 켜고 200 ms 끔)
 *  - 번호 없음(콘이 정해 주기 전): 계속 켜짐
 *  - 콘이 사용자에게 이 브릿지를 묻는 중: 빠르게(100 ms) 깜빡임 */
#define ID_LED_GPIO      GPIO_NUM_17
#define ID_LED_STEP_MS   100
#define ID_LED_BLINK_MS  400    /* 한 번 = 켬 200 + 끔 200 */
#define ID_LED_PERIOD_MS 5000

static void id_led_task(void *arg)
{
    (void)arg;
    uint32_t t = 0;
    for (;;) {
        int br = can_link_bridge_index();
        bool on;
        if (can_link_identify_active()) {
            on = ((t / ID_LED_STEP_MS) & 1) == 0;
        } else if (br < 0) {
            on = true;
        } else {
            uint32_t ph = t % ID_LED_PERIOD_MS;
            on = ph < (uint32_t)(br + 1) * ID_LED_BLINK_MS && (ph % ID_LED_BLINK_MS) < ID_LED_BLINK_MS / 2;
        }
        gpio_set_level(ID_LED_GPIO, on ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(ID_LED_STEP_MS));
        t += ID_LED_STEP_MS;
    }
}

static void id_led_start(void)
{
    gpio_config_t c = { .pin_bit_mask = 1ULL << ID_LED_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&c);
    gpio_set_level(ID_LED_GPIO, 1);
    xTaskCreate(id_led_task, "id_led", 2048, NULL, 1, NULL);
}

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
    id_led_start();      /* 2026-11-18 — can_link_init이 콘의 번호를 기다리는 동안에도 LED가 돌게 먼저 */
    can_link_init();     /* 콘이 번호를 정해 줄 때까지 기다림 */
    bridge_esp_now_init();

    ESP_LOGI(TAG, "Bridge started (standalone init)");
}
