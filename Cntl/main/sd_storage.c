/**
 * @file    sd_storage.c
 * @brief   SD카드(SPI모드, CS는 CH422G EXIO4) 마운트 구현
 */
#include "sd_storage.h"
#include "storage_mgr.h"
#include "photo_storage.h"
#include "stats_store.h"
#include "ch422g.h"
#include "waveshare_rgb_lcd_port.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sd_health.h"
#include "sd_guard.h"
#include "esp_vfs_fat.h"
#include <stdbool.h>
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static const char *TAG = "SYS";

/* ════════════════════════════════════════════════════════════
 * 2026-10-15(SD 손상 대응, 사용자 결정 — "1번을 수정") — 명령마다 CS를 내렸다 올림.
 * 예전(09-06~)엔 CS(CH422G EXIO4)를 마운트 때 한 번 LOW로 내린 뒤 계속 LOW로 두고 드라이버엔 SDSPI_SLOT_NO_CS를
 * 줬음 — ESP-IDF sdspi 드라이버는 명령마다 CS를 올리고 내리는 걸 전제로 짜여 있는데(명령 뒤 "CS를 올린 채" 0xFF를
 * 보내 MISO를 놓게 함, CMD0 앞 80클럭은 "CS를 내리기 전에"), 그 동작이 전부 아무 일도 안 했고 카드는 한 번도
 * 선택 해제되지 않은 채 콘 리셋 동안의 SCK/MOSI 흔들림까지 명령 입력으로 받았음.
 * 드라이버(IDF 원본)는 그대로 두고 호스트의 명령 실행 함수를 감싸서: 잠금 → CS LOW → 원래 함수 → CS HIGH → 풀기.
 * CS를 못 내리면 그 명령은 보내지 않음(실패로 돌려줌 — 카드가 선택 안 된 채 보내면 응답이 없어 어차피 실패) */
static SemaphoreHandle_t s_cs_mutex = NULL;
static StaticSemaphore_t s_cs_mutex_buf;
static struct {
    uint32_t cmds, cs_fail, slow;   /* 명령 수, CS 바꾸기 실패, CS 처리에 2ms 넘게 걸린 명령 */
    uint32_t cs_us_max;             /* CS 내리기+올리기에 걸린 최대 시간(us) */
} s_cs;

static esp_err_t sd_do_transaction_cs(int slot, sdmmc_command_t *cmd)
{
    xSemaphoreTake(s_cs_mutex, portMAX_DELAY);
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = ch422g_set_io(CH422G_IO_SD_CS, false);   /* LOW = 선택 */
    int64_t t1 = esp_timer_get_time();
    if (e != ESP_OK) {
        s_cs.cs_fail++;
        ESP_LOGE(TAG, "SD_CS select failed (%s) - CMD%u not sent", esp_err_to_name(e), (unsigned)cmd->opcode);
        xSemaphoreGive(s_cs_mutex);
        return e;
    }
    esp_err_t r = sdspi_host_do_transaction(slot, cmd);
    int64_t t2 = esp_timer_get_time();
    /* 2026-10-21 — 처음 손상 원인 추적: 명령 기록·FAT 기대값(sd_health_on_cmd) */
    sd_health_on_cmd((uint8_t)cmd->opcode, cmd->arg, cmd->data, (uint32_t)(cmd->datalen / 512), (uint32_t)(t2 - t1), (int)r);
    e = ch422g_set_io(CH422G_IO_SD_CS, true);              /* HIGH = 해제 */
    int64_t t3 = esp_timer_get_time();
    if (e != ESP_OK) {
        s_cs.cs_fail++;
        ESP_LOGE(TAG, "SD_CS deselect failed (%s) after CMD%u - card stays selected", esp_err_to_name(e), (unsigned)cmd->opcode);
    }
    uint32_t cs_us = (uint32_t)((t1 - t0) + (t3 - t2));
    s_cs.cmds++;
    if (cs_us > s_cs.cs_us_max) s_cs.cs_us_max = cs_us;
    if (cs_us > 2000) s_cs.slow++;
    xSemaphoreGive(s_cs_mutex);
    return r;
}

void sd_storage_cs_stats(uint32_t *cmds, uint32_t *cs_fail, uint32_t *slow, uint32_t *cs_us_max)
{
    *cmds = s_cs.cmds; *cs_fail = s_cs.cs_fail; *slow = s_cs.slow; *cs_us_max = s_cs.cs_us_max;
}

/* 2026-10-14(사용자 지시 — 카드 정체 확인, SD 손상 추적) — 마운트와 상관없이 카드 CID 전체를 로그에 남김. 예전엔
 * 마운트 성공 뒤 sdmmc_card_print_info()만 찍어 제조사 ID·OEM·일련번호·제조 연월이 없었고, 마운트가 실패하는 카드
 * (예: GPT로 포맷된 카드)는 IDF가 카드 정보를 지워 아무것도 안 남았음. 공개 API로 장치를 잠깐 붙여 초기화만 하고 뗌 */
static void log_card_cid(const sdmmc_host_t *host, const sdspi_device_config_t *slot_cfg)
{
    sdmmc_card_t *card = heap_caps_calloc(1, sizeof(*card), MALLOC_CAP_SPIRAM);
    if (!card) return;
    sdspi_dev_handle_t h;
    if (sdspi_host_init_device(slot_cfg, &h) != ESP_OK) { heap_caps_free(card); return; }
    sdmmc_host_t hh = *host;
    hh.slot = h;
    esp_err_t e = sdmmc_card_init(&hh, card);
    if (e == ESP_OK) {
        const sdmmc_cid_t *c = &card->cid;
        char name[sizeof(c->name) + 1] = { 0 };
        for (size_t i = 0; i < sizeof(c->name) && c->name[i]; i++) name[i] = (c->name[i] >= 32 && c->name[i] < 127) ? c->name[i] : '?';
        char line[112];
        snprintf(line, sizeof(line), "MID=0x%02X OID=0x%04X PNM='%s' PRV=%d.%d PSN=0x%08X MDT=%d-%02d %lluMB",
                 c->mfg_id, c->oem_id, name, (c->revision >> 4) & 0xF, c->revision & 0xF, (unsigned)c->serial,
                 2000 + ((c->date >> 4) & 0xFF), c->date & 0xF,
                 (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
        ESP_LOGW(TAG, "SD card CID: %s", line);
        sd_health_set_card(line);   /* 사후 분석 기록에 남김 */
    } else {
        ESP_LOGW(TAG, "SD card CID: card init failed (%s)", esp_err_to_name(e));
    }
    sdspi_host_remove_device(h);
    heap_caps_free(card);
}

#define SD_SPI_HOST     SPI2_HOST
#define SD_PIN_MOSI     11
#define SD_PIN_MISO     13
#define SD_PIN_SCK      12

static sdmmc_card_t *s_card = NULL;
/* 2026-10-15 — 포맷 요청 중에만 true: 마운트가 실패하면(FAT 없음·손상) 그 자리에서 FAT을 만들고 마운트(sd_storage_format) */
static bool s_format_if_mount_failed = false;

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
        ESP_LOGE(TAG, "SD_CS deassert failed: %s", esp_err_to_name(err));
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
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));  /* CS HIGH인 채로 전원/신호 안정화 대기 */

    /* 2026-10-15 — 여기서 CS를 LOW로 고정하던 것 없앰: 이제 명령마다 sd_do_transaction_cs가 내렸다 올림(위 설명) */
    if (!s_cs_mutex) s_cs_mutex = xSemaphoreCreateMutexStatic(&s_cs_mutex_buf);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.do_transaction = sd_do_transaction_cs;   /* 2026-10-15 — 명령마다 CS */
    /* 2026-09-10(원복 — 짧은 타임아웃은 불필요했음) — 처음엔 실패를 빨리 포기시키려고
     * command_timeout_ms를 200ms로 줄였는데, 다시 짚어보니 필요 없었음: 기본값(1000ms)도
     * 어차피 결국은 리턴하는 fail이라 진짜 "행"이 아니었고, 실제 문제는 이 fail을 부르는
     * 쪽(통계탭 스캔)이 반복되는 실패를 무시하고 계속 누적시킨 것(별도로 고쳐야 함)이었지
     * 명령 하나의 타임아웃 길이가 아니었음. 게다가 open은 파일을 안 건드리니 아무리
     * 오래 걸려도(재시도든 뭐든) 크래시 때 파일이 깨지는 것과 무관 — 오히려 오래 걸려도
     * 되니까 짧게 자를 이유가 없음(사용자 지시). 그래서 ESP-IDF 기본값 그대로 씀 */

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SD_SPI_HOST;
    /* CS는 GPIO가 아니라 CH422G라 드라이버엔 없음으로 줌(SDSPI_SLOT_NO_CS — 드라이버의 cs_low/cs_high는 아무 일도
     * 안 함). 2026-10-15부터 그 일을 sd_do_transaction_cs가 명령마다 함 */
    slot_cfg.gpio_cs = SDSPI_SLOT_NO_CS;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = s_format_if_mount_failed,
        .max_files              = 8,   /* 2026-10-14 — 5 → 8: 쓰고 다시 읽어 확인하는 핸들이 동시에 하나씩 더 열림(sd_health) */
        .allocation_unit_size   = 16 * 1024,
    };

    log_card_cid(&host, &slot_cfg);   /* 2026-10-14 */
    err = esp_vfs_fat_sdspi_mount(SD_STORAGE_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        spi_bus_free(SD_SPI_HOST);
        return err;
    }

    sdmmc_card_print_info(stdout, s_card);

    /* 2026-09-06(사용자 설계) — 통계/사진 폴더 구조 미리 마련. mkdir이 이미 있으면
     * EEXIST로 실패하는 게 정상이라 반환값은 로그만(치명적 아님) */
    if (mkdir(SD_STORAGE_MOUNT_POINT "/stats", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "stats folder create failed (errno=%d)", errno);
    }
    if (mkdir(SD_STORAGE_MOUNT_POINT "/photos", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "photos folder create failed (errno=%d)", errno);
    }

    ESP_LOGI(TAG, "SD mounted: %s", SD_STORAGE_MOUNT_POINT);
    sd_guard_attach(s_card);   /* 2026-10-22 — FatFs 쓰기를 보호 쓰기로(sd_guard.h) */
    sd_health_on_mount();   /* 2026-10-15 — 부트 영역 기준값을 새로 잡음(손상 감시) */
    /* 2026-09-26 — 재연결로 다시 마운트된 경우 사용량 합계를 새로 만들게 함(부팅 때는 파일처리
     * 태스크가 아직 없어서 무시되고, 태스크가 시작하면서 어차피 재스캔함) */
    storage_mgr_request_rescan();
    return ESP_OK;
}

bool sd_storage_get_capacity(uint64_t *out_total_bytes, uint64_t *out_free_bytes)
{
    if (!s_card) return false;  /* 미마운트 */
    esp_err_t err = esp_vfs_fat_info(SD_STORAGE_MOUNT_POINT, out_total_bytes, out_free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_info failed: %s", esp_err_to_name(err));
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
    ESP_LOGW(TAG, "SD reconnect attempt");
    /* 2026-09-28(사용자 지시) — 사진을 받거나 측정값을 저장하는 중이면 열린 파일부터 닫고 언마운트
     * (예전엔 열린 파일을 둔 채 언마운트해서, 그 파일이 새 마운트 위에서 계속 쓰일 수 있었음) */
    photo_storage_io_suspend();
    stats_store_io_suspend();
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_STORAGE_MOUNT_POINT, s_card);
        s_card = NULL;
        spi_bus_free(SD_SPI_HOST);
    }
    esp_err_t err = sd_storage_init();
    stats_store_io_resume();
    photo_storage_io_resume();
    return err;
}

/* 2026-09-10(사용자 설계 — "포맷" 버튼) — esp_vfs_fat_sdcard_format()이 포맷+재마운트까지
 * 알아서 함(마운트된 상태에서만 호출 가능 — 요구사항). 포맷하면 빈 카드가 되므로 stats/
 * photos 폴더를 sd_storage_init()과 동일하게 다시 만들어줌 */
esp_err_t sd_storage_format(void)
{
    /* 2026-10-15(실기 — FAT이 없는 카드는 마운트가 안 돼서 "SD not mounted, reconnect first"로 포맷 자체가 막혔음) —
     * 예전엔 마운트된 카드만 포맷했음. 이제 아래 재연결을 "마운트가 실패하면 FAT을 만들고 마운트"로 함. 마운트가 된
     * 카드(FAT 있음)는 그대로 마운트되므로 아래 포맷이 원래대로 진행됨(FAT이 없던 카드는 FAT을 만든 뒤 한 번 더 포맷) */

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
    s_format_if_mount_failed = true;
    esp_err_t reconnect_err = sd_storage_reconnect();
    s_format_if_mount_failed = false;
    if (reconnect_err != ESP_OK) {
        ESP_LOGE(TAG, "Format failed - reconnect before format failed: %s", esp_err_to_name(reconnect_err));
        return reconnect_err;
    }

    ESP_LOGW(TAG, "SD format start");
    /* 재연결과 포맷 사이에 새로 열린 저장 파일도 닫고 막음(위 reconnect와 같은 이유) */
    photo_storage_io_suspend();
    stats_store_io_suspend();
    sd_guard_set_enabled(false);   /* 2026-10-22 — f_mkfs 대량 쓰기는 확인하지 않음 */
    esp_err_t err = esp_vfs_fat_sdcard_format(SD_STORAGE_MOUNT_POINT, s_card);
    sd_guard_attach(s_card);
    sd_guard_set_enabled(true);
    stats_store_io_resume();
    photo_storage_io_resume();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD format failed: %s", esp_err_to_name(err));
        return err;
    }
    if (mkdir(SD_STORAGE_MOUNT_POINT "/stats", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "stats folder create failed (errno=%d)", errno);
    }
    if (mkdir(SD_STORAGE_MOUNT_POINT "/photos", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "photos folder create failed (errno=%d)", errno);
    }
    ESP_LOGI(TAG, "SD format done");
    sd_health_on_mount();   /* 2026-10-15 — 포맷으로 부트 영역이 바뀜 — 기준값 새로 */
    storage_mgr_request_rescan();  /* 포맷으로 전부 비었음 — 합계 새로 */
    return ESP_OK;
}

/* 미마운트 등으로 지금 SD를 아예 못 쓰는 상태인지 — 주화면 상태표시가 씀 */
bool sd_storage_is_mounted(void)
{
    return s_card != NULL;
}

/* 2026-10-21(SD 손상 원인 조사 — 카드 원본 섹터로 확인) — FatFs는 쓰기 모드에서 클러스터 연결을 따라가다(이어쓰기 O_APPEND는
 * 쓰기마다 f_lseek(파일 끝), 덮어쓰기는 fseek·fwrite가 클러스터를 넘을 때) 연결 값이 범위를 벗어나면(손상된 FAT 칸) 에러 없이
 * "파일 끝"으로 보고 새 빈 클러스터를 붙여 파일 크기를 채움(create_chain). 10-21 통계 원시 파일이 그렇게 클러스터 52개(FF)를 받아
 * 기록 50,461개가 FF로 읽혔고, 원래 기록은 끊긴 연결 다음 클러스터에 그대로 남아 있었음. 10-14 "FF 구간이 새 클러스터 경계에서
 * 시작"도 같은 모양. 읽기 모드의 f_lseek는 같은 연결을 따라가며 값이 범위를 벗어나면 FR_INT_ERR를 돌려주므로, 쓰기 전에 읽기
 * 모드로 파일 끝까지 옮겨 연결을 확인함. 연결 값이 우연히 범위 안의 다른 클러스터 번호로 바뀐 경우는 못 잡음 */
bool sd_storage_chain_ok(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return true;   /* 없음(새로 만듦) — 다른 이유의 실패는 실제 열기에서 드러남 */
    off_t r = lseek(fd, 0, SEEK_END);
    int err = errno;
    close(fd);
    if (r >= 0) return true;
    ESP_LOGE(TAG, "SD cluster chain broken (errno=%d) - not written: %s", err, path);
    sd_health_report(SDH_EV_CHAIN_BROKEN, path, 0);
    return false;
}

esp_err_t sd_storage_read_sectors(void *dst, size_t start_sector, size_t count)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    return sdmmc_read_sectors(s_card, dst, start_sector, count);
}
