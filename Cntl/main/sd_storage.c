/**
 * @file    sd_storage.c
 * @brief   SD카드(SPI모드, CS는 CH422G EXIO4) 마운트 구현
 */
#include "sd_storage.h"
#include "ch422g.h"
#include "waveshare_rgb_lcd_port.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "sd_storage";

#define SD_SPI_HOST     SPI2_HOST
#define SD_PIN_MOSI     11
#define SD_PIN_MISO     13
#define SD_PIN_SCK      12

static sdmmc_card_t *s_card = NULL;

esp_err_t sd_storage_init(void)
{
    /* CS를 여기서 한 번만 LOW로 내리고 계속 유지 — 위 sd_storage.h 파일 헤더 주석 참고.
     * ch422g_init()은 waveshare_esp32_s3_rgb_lcd_init()이 이미 끝냈다고 가정(app_main
     * 호출 순서로 보장) */
    esp_err_t err = ch422g_set_io(CH422G_IO_SD_CS, false);  /* LOW = select */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD_CS assert 실패: %s", esp_err_to_name(err));
        return err;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = SD_PIN_MOSI,
        .miso_io_num     = SD_PIN_MISO,
        .sclk_io_num     = SD_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };
    err = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI 버스 초기화 실패: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SD_SPI_HOST;
    /* CS는 위에서 CH422G로 이미 고정 LOW — 드라이버가 GPIO로 직접 토글하지 않게 함
     * (SDSPI_SLOT_NO_CS는 GPIO_NUM_NC와 동일, esp_driver_sdspi가 공식 지원하는 값) */
    slot_cfg.gpio_cs = SDSPI_SLOT_NO_CS;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_STORAGE_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD카드 마운트 실패: %s", esp_err_to_name(err));
        spi_bus_free(SD_SPI_HOST);
        return err;
    }

    sdmmc_card_print_info(stdout, s_card);

    /* 2026-09-06(사용자 설계) — 통계/사진 폴더 구조 미리 마련. mkdir이 이미 있으면
     * EEXIST로 실패하는 게 정상이라 반환값은 로그만(치명적 아님) */
    if (mkdir(SD_STORAGE_MOUNT_POINT "/stats", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "stats 폴더 생성 실패(errno=%d)", errno);
    }
    if (mkdir(SD_STORAGE_MOUNT_POINT "/photos", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "photos 폴더 생성 실패(errno=%d)", errno);
    }

    ESP_LOGI(TAG, "SD카드 마운트 완료: %s", SD_STORAGE_MOUNT_POINT);
    return ESP_OK;
}
