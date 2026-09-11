/**
 * @file    sd_storage.c
 * @brief   SD카드(SPI모드, CS는 CH422G EXIO4) 마운트 구현
 */
#include "sd_storage.h"
#include "ch422g.h"
#include "waveshare_rgb_lcd_port.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include <stdbool.h>
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include <sys/stat.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sd_storage";

#define SD_SPI_HOST     SPI2_HOST
#define SD_PIN_MOSI     11
#define SD_PIN_MISO     13
#define SD_PIN_SCK      12

static sdmmc_card_t *s_card = NULL;

esp_err_t sd_storage_init(void)
{
    /* 2026-09-11(재설계 — 실기에서 "새 카드로 교체+재연결도 실패, 리붓하면 연결됨" 재현 후
     * 원인 확인) — 예전엔 CS를 여기서 한 번 LOW로 내린 뒤 프로젝트 전체에서 다시 HIGH로
     * 올리는 코드가 전혀 없어서, 재연결(reconnect)처럼 이미 한 번 LOW였던 상태에서 다시
     * 이 함수가 불리면 CS가 계속 LOW로 고정된 채 카드를 새로 프로브하게 됨. SD SPI모드
     * 표준 진입 절차는 "CS를 HIGH로 둔 채 전원안정화+더미클럭 이후 → CS를 LOW로 내리고
     * CMD0"인데, CS가 계속 LOW였던 채로는 이 절차가 성립 안 함(카드 리부팅 없이 교체된
     * 새 카드가 이 신호를 못 받아서 SPI모드에 못 들어간 것으로 추정 — 반대로 완전 리붓은
     * CH422G 자체도 전원과 함께 리셋되며 결과적으로 이 HIGH->LOW 엣지가 자연히 한 번
     * 생겨서 됐던 것으로 보임). 그래서 매번 명시적으로 HIGH(비선택) -> 잠깐 대기(전원/신호
     * 안정화, ESP32-S3 커뮤니티에도 보고된 완화책) -> LOW(선택) 순서로 만듦 */
    esp_err_t err = ch422g_set_io(CH422G_IO_SD_CS, true);  /* HIGH = deselect, 먼저 확실히 비선택 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD_CS deassert 실패: %s", esp_err_to_name(err));
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

    vTaskDelay(pdMS_TO_TICKS(100));  /* CS HIGH인 채로 전원/신호 안정화 대기 */

    err = ch422g_set_io(CH422G_IO_SD_CS, false);  /* LOW = select, 이제 명확한 엣지로 선택 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD_CS assert 실패: %s", esp_err_to_name(err));
        spi_bus_free(SD_SPI_HOST);
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    /* 2026-09-10(원복 — 짧은 타임아웃은 불필요했음) — 처음엔 실패를 빨리 포기시키려고
     * command_timeout_ms를 200ms로 줄였는데, 다시 짚어보니 필요 없었음: 기본값(1000ms)도
     * 어차피 결국은 리턴하는 fail이라 진짜 "행"이 아니었고, 실제 문제는 이 fail을 부르는
     * 쪽(통계탭 스캔)이 반복되는 실패를 무시하고 계속 누적시킨 것(별도로 고쳐야 함)이었지
     * 명령 하나의 타임아웃 길이가 아니었음. 게다가 open은 파일을 안 건드리니 아무리
     * 오래 걸려도(재시도든 뭐든) 크래시 때 파일이 깨지는 것과 무관 — 오히려 오래 걸려도
     * 되니까 짧게 자를 이유가 없음(사용자 지시). 그래서 ESP-IDF 기본값 그대로 씀 */

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

bool sd_storage_get_capacity(uint64_t *out_total_bytes, uint64_t *out_free_bytes)
{
    if (!s_card) return false;  /* 미마운트 */
    esp_err_t err = esp_vfs_fat_info(SD_STORAGE_MOUNT_POINT, out_total_bytes, out_free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_info 실패: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* 2026-09-10(사용자 설계 — [[project_cntl_sd_reliability_redesign_2026_09_10]] "SD 재연결
 * 시도" 버튼) — 하드웨어 카드감지 핀이 없어서(스키매틱상 CS만 CH422G EXIO4, 별도 감지선
 * 없음) 자동 핫스왑 감지는 못 함. 대신 사용자가 명시적으로 누르면 깔끔하게 언마운트하고
 * sd_storage_init()과 동일한 시퀀스를 다시 돌림 — 같은 카드를 PC에서 손보고 다시 꽂았든,
 * 완전히 새 카드로 바꿨든 둘 다 이걸로 커버됨 */
esp_err_t sd_storage_reconnect(void)
{
    ESP_LOGW(TAG, "SD 재연결 시도");
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_STORAGE_MOUNT_POINT, s_card);
        s_card = NULL;
        spi_bus_free(SD_SPI_HOST);
    }
    return sd_storage_init();
}

/* 2026-09-10(사용자 설계 — "포맷" 버튼) — esp_vfs_fat_sdcard_format()이 포맷+재마운트까지
 * 알아서 함(마운트된 상태에서만 호출 가능 — 요구사항). 포맷하면 빈 카드가 되므로 stats/
 * photos 폴더를 sd_storage_init()과 동일하게 다시 만들어줌 */
esp_err_t sd_storage_format(void)
{
    if (!s_card) {
        ESP_LOGW(TAG, "포맷 실패 — SD 미마운트, 먼저 재연결 필요");
        return ESP_ERR_INVALID_STATE;
    }

    /* 2026-09-11(실기에서 "포맷 실패" 재현 후 근본원인 확인 — f_mkfs failed (3), FatFs
     * ff.c의 f_mkfs()는 시작하자마자 disk_initialize(pdrv)를 부르고 STA_NOINIT이면 곧장
     * FR_NOT_READY(3)로 리턴함(포맷 자체를 시도조차 안 함). 이 프로젝트의 diskio_sdmmc.c
     * (ff_sdmmc_initialize -> ff_sdmmc_card_available)는 이 상태확인을 sdmmc_get_status()
     * (CMD13)로 하는데, 몇 시간에 걸쳐 반복된 읽기실패(0x107) 뒤라 이 단순 상태확인 명령
     * 조차 실패하는 상태로 카드/SPI버스가 남아있었던 것으로 보임(카드가 SPI 프로토콜
     * 상태머신에서 헤어나오지 못한 것 — CS를 매 트랜잭션마다 안 띄우는 이 보드의 배선
     * 특성상 명령 타임아웃 뒤 자연 복구가 안 될 수 있음). 재연결(sd_storage_reconnect())은
     * SPI버스를 완전히 새로 초기화하고 카드에 CMD0(GO_IDLE_STATE)부터 다시 보내는 완전한
     * 재프로브라 이 상태를 확실히 리셋함 — 포맷 직전에 항상 한 번 거쳐서 깨끗한 상태에서
     * 시도하게 함 */
    esp_err_t reconnect_err = sd_storage_reconnect();
    if (reconnect_err != ESP_OK) {
        ESP_LOGE(TAG, "포맷 실패 — 포맷 전 재연결 자체가 실패: %s", esp_err_to_name(reconnect_err));
        return reconnect_err;
    }

    ESP_LOGW(TAG, "SD 포맷 시작");
    esp_err_t err = esp_vfs_fat_sdcard_format(SD_STORAGE_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD 포맷 실패: %s", esp_err_to_name(err));
        return err;
    }
    if (mkdir(SD_STORAGE_MOUNT_POINT "/stats", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "stats 폴더 생성 실패(errno=%d)", errno);
    }
    if (mkdir(SD_STORAGE_MOUNT_POINT "/photos", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "photos 폴더 생성 실패(errno=%d)", errno);
    }
    ESP_LOGI(TAG, "SD 포맷 완료");
    return ESP_OK;
}

/* 미마운트 등으로 지금 SD를 아예 못 쓰는 상태인지 — 주화면 상태표시가 씀 */
bool sd_storage_is_mounted(void)
{
    return s_card != NULL;
}
