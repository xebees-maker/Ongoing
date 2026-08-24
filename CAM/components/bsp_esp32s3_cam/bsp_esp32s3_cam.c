#include "bsp_esp32s3_cam.h"
#include "io_expander_ch32v003.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "bsp_esp32s3_cam";

static i2c_master_bus_handle_t s_i2c_bus = NULL;

esp_err_t bsp_esp32s3_cam_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .i2c_port               = BSP_CAM_I2C_PORT,
        .scl_io_num             = BSP_CAM_I2C_SCL,
        .sda_io_num             = BSP_CAM_I2C_SDA,
        .glitch_ignore_cnt      = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c_new_master_bus failed");

    ESP_RETURN_ON_ERROR(ch32v003_init(s_i2c_bus), TAG, "ch32v003_init failed");

    /* 2026-08-23 — 배터리 전원 자체유지(래치). PWR 버튼은 배터리를 레귤레이터에 일시
     * 연결만 해주고, 이 핀을 소프트웨어가 켜야 버튼을 놔도 계속 켜져 있음. 가장 먼저 켬 —
     * 늦으면 그 사이에 버튼을 놓았을 때 전원이 나갈 수 있음 */
    ch32v003_set_output(BSP_CAM_IO_EXPANDER_BAT_EN_PIN, true);

    /* Waveshare 예제(04_SDMMC_Test.ino)가 SD_MMC.begin() 전에 이 두 핀을 켜지 않으면
     * SD카드가 안 잡힘 — 정확한 이유는 불확실하지만 그대로 재현 */
    ch32v003_set_output(BSP_CAM_IO_EXPANDER_SD_ENABLE_PIN_A, true);
    ch32v003_set_output(BSP_CAM_IO_EXPANDER_SD_ENABLE_PIN_B, true);

    ESP_LOGI(TAG, "보드 초기화 완료 (I2C SDA=%d SCL=%d, BAT_EN/SD 인에이블 핀 ON)",
             BSP_CAM_I2C_SDA, BSP_CAM_I2C_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_esp32s3_cam_get_i2c_bus(void)
{
    return s_i2c_bus;
}
