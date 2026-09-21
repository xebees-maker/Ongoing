/**
 * 2026-09-21 — Bridge(XIAO Seeed, ESP32-C3): CNTL의 ESP-NOW 라디오 스택을 통째로 대신 떠맡음.
 * Sens/CAM 쪽에서 보면 이 보드가 예전 CNTL과 똑같은 "허브"로 보임(esp_now_link.h 프로토콜
 * 그대로, 채널 스캔/ADVERTISE/PAIR_REQUEST 등 전부 노드 쪽 코드 변경 없음).
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

static const char *TAG = "bridge_main";

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

    wifi_bringup();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    i2c_slave_link_init();

    ESP_LOGI(TAG, "Bridge 시작됨");
}
