#include "device_config.h"
#include "fs.h"

#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "device_config";

#define DEVICE_CONFIG_PATH    FS_MOUNT_POINT "/device_config.bin"
#define DEVICE_CONFIG_VERSION 2  /* 2026-08-10: adaptive_response_sec 필드 추가로 1->2
                                    (구버전 파일은 버전 불일치로 기본값으로 자연 폴백) */

#define CAM_CAPTURE_INTERVAL_SEC_DEFAULT 1800  /* CAM Kconfig 기본(30분)과 동일 */
#define RESPONSE_INTERVAL_SEC_DEFAULT    2
#define ADAPTIVE_RESPONSE_SEC_DEFAULT    10    /* 적응형 반응시간(2026-08-10) — 마지막 사용자
                                                   조작 후 이만큼 조용하면 CAM에 SLEEP_NOW */

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint32_t cam_capture_interval_sec;
    uint32_t response_interval_sec;
    uint32_t adaptive_response_sec;
} device_config_file_t;

static uint32_t s_cam_capture_interval_sec = CAM_CAPTURE_INTERVAL_SEC_DEFAULT;
static uint32_t s_response_interval_sec    = RESPONSE_INTERVAL_SEC_DEFAULT;
static uint32_t s_adaptive_response_sec    = ADAPTIVE_RESPONSE_SEC_DEFAULT;

static void device_config_save(void)
{
    FILE *f = fopen(DEVICE_CONFIG_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "저장 실패(fopen): %s", DEVICE_CONFIG_PATH);
        return;
    }
    device_config_file_t s = {
        .version = DEVICE_CONFIG_VERSION,
        .cam_capture_interval_sec = s_cam_capture_interval_sec,
        .response_interval_sec   = s_response_interval_sec,
        .adaptive_response_sec   = s_adaptive_response_sec,
    };
    fwrite(&s, sizeof(s), 1, f);
    fclose(f);
}

void device_config_load(void)
{
    FILE *f = fopen(DEVICE_CONFIG_PATH, "rb");
    if (!f) return;
    device_config_file_t s = { 0 };
    bool ok = (fread(&s, sizeof(s), 1, f) == 1) && s.version == DEVICE_CONFIG_VERSION;
    fclose(f);
    if (!ok) {
        ESP_LOGW(TAG, "설정 파일 형식 불일치 — 기본값 유지");
        return;
    }
    s_cam_capture_interval_sec = s.cam_capture_interval_sec;
    s_response_interval_sec    = s.response_interval_sec ? s.response_interval_sec : RESPONSE_INTERVAL_SEC_DEFAULT;
    s_adaptive_response_sec    = s.adaptive_response_sec ? s.adaptive_response_sec : ADAPTIVE_RESPONSE_SEC_DEFAULT;
    ESP_LOGI(TAG, "설정 복원: CAM촬영주기=%us 응답성=%us 적응형반응=%us",
             (unsigned)s_cam_capture_interval_sec, (unsigned)s_response_interval_sec,
             (unsigned)s_adaptive_response_sec);
}

uint32_t device_config_get_cam_capture_interval_sec(void) { return s_cam_capture_interval_sec; }

void device_config_set_cam_capture_interval_sec(uint32_t sec)
{
    s_cam_capture_interval_sec = sec;
    device_config_save();
}

uint32_t device_config_get_response_interval_sec(void) { return s_response_interval_sec; }

void device_config_set_response_interval_sec(uint32_t sec)
{
    s_response_interval_sec = sec ? sec : RESPONSE_INTERVAL_SEC_DEFAULT;
    device_config_save();
}

uint32_t device_config_get_adaptive_response_sec(void) { return s_adaptive_response_sec; }

void device_config_set_adaptive_response_sec(uint32_t sec)
{
    s_adaptive_response_sec = sec ? sec : ADAPTIVE_RESPONSE_SEC_DEFAULT;
    device_config_save();
}
