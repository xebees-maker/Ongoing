/**
 * 2026-09-21 — Bridge(XIAO Seeed, ESP32-**C6**): CNTL의 ESP-NOW 라디오 스택을 통째로 대신
 * 떠맡음. Sens/CAM 쪽에서 보면 이 보드가 예전 CNTL과 똑같은 "허브"로 보임(esp_now_link.h
 * 프로토콜 그대로, 채널 스캔/ADVERTISE/PAIR_REQUEST 등 전부 노드 쪽 코드 변경 없음).
 *
 * 이 프로젝트는 CASK 프로토콜을 전혀 해석하지 않는 투명 중계임 — 그 판단(허브 로직)은 전부
 * CNTL의 esp_now_hub.c에 그대로 남아있고, CNTL은 I2C로 이 브릿지에게 "이 MAC한테 이거 보내",
 * "reliable로 이거 보내고 결과 줘" 를 시키고, 이 브릿지가 받은 건 뭐든 INCOMING_MSG로 그대로
 * CNTL에 올려보낸다. project_cntl_i2c_bridge_design_2026_09_21 메모리 참고.
 */

#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_now_reliable.h"
#include "i2c_slave_link.h"
#include "driver/gpio.h"

static const char *TAG = "bridge_main";

/* 2026-09-21(사용자 확인, XIAO ESP32-C6 안테나 스위치) — GPIO3=Low로 RF 인에이블,
 * GPIO14=Low는 내장(PCB) 안테나/High는 외부 안테나(u.FL) 선택. 이 프로젝트의 전제 자체가
 * "브릿지에서 긴 케이블로 건물 내부 안테나까지 뽑아낸다"는 것(project_cntl_i2c_bridge_design_
 * 2026_09_21 메모리, "외부[콘-I2C-브릿지]-벽-[내부]안테나" 형상)이라 반드시 외부 안테나
 * (GPIO14=High)여야 함 — 이 두 핀을 안 건드리면(디폴트 상태가 뭔지 불확실) 라디오가 아예
 * 안 나갈 수 있어서 무선 스택보다 먼저 설정함 */
#define RF_ENABLE_PIN      GPIO_NUM_3
#define RF_ANT_SELECT_PIN  GPIO_NUM_14

static void antenna_switch_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << RF_ENABLE_PIN) | (1ULL << RF_ANT_SELECT_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(RF_ENABLE_PIN, 0);      /* Low = Enable */
    gpio_set_level(RF_ANT_SELECT_PIN, 1);  /* High = External(u.FL, 긴 케이블 경유 내부 안테나) */
    ESP_LOGI(TAG, "안테나 스위치: RF enable, 외부 안테나 선택");
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 2 || !info) return;
    uint8_t msg_type = data[1];

    /* CNTL의 esp_now_hub.c recv_cb()와 동일한 관례 — reliable 대기 매칭을 먼저 시도(대기
     * 중인 게 없으면 조용히 무시), 그 다음 무조건 원본 그대로 CNTL에 중계(투명 릴레이 원칙 —
     * 이 보드는 msg_type 종류를 몰라도 됨, CASK 판단은 전부 CNTL 쪽) */
    esp_now_reliable_on_recv(msg_type, info->src_addr, data, len);

    int8_t rssi = (info->rx_ctrl) ? (int8_t)info->rx_ctrl->rssi : 0;
    i2c_slave_link_queue_incoming(info->src_addr, rssi, data, (size_t)len);
}

static void wifi_bringup(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* AP에 실제로 접속하지 않음 — ESP-NOW만 쓰는 순수 STA(무선 라디오만 켬). CNTL이
     * SET_CHANNEL로 실제 채널을 지정해줄 때까지는 기본 채널(1)에 머무름 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    antenna_switch_init();
    wifi_bringup();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    i2c_slave_link_init();

    ESP_LOGI(TAG, "Bridge 시작됨");
}
