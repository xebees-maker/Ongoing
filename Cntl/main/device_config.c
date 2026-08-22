#include "device_config.h"
#include "fs.h"
#include "ui_log.h"

#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "device_config";

#define DEVICE_CONFIG_PATH    FS_MOUNT_POINT "/device_config.bin"
#define DEVICE_CONFIG_VERSION 4  /* 2026-08-21: xclk_mhz 필드 추가로 3->4
                                    (구버전 파일은 버전 불일치로 기본값으로 자연 폴백 —
                                    UI_ERR_CONFIG_FILE_MISMATCH로 화면에도 보임, device_config_load
                                    참고) */

#define CAM_CAPTURE_INTERVAL_SEC_DEFAULT 1800  /* CAM Kconfig 기본(30분)과 동일 */
#define RESPONSE_INTERVAL_SEC_DEFAULT    2
#define ADAPTIVE_RESPONSE_SEC_DEFAULT    10    /* 적응형 반응시간(2026-08-10) — 마지막 사용자
                                                   조작 후 이만큼 조용하면 CAM에 SLEEP_NOW */
#define AGC_ENABLE_DEFAULT true   /* 센서 전원인가 기본값과 일치(2026-08-21) */
#define AEC_ENABLE_DEFAULT true
#define XCLK_MHZ_DEFAULT 10  /* 2026-08-21 — 24MHz에서 10MHz로 변경. 카메라 고정+동일 밝기
                                 still-cut 비교 테스트에서 10MHz가 화질 최적점으로 확인됨
                                 (20MHz부터 노이즈 심해짐). UI 드롭다운은 그대로 5/10/20/24MHz
                                 유지, 빌드 시 기본값만 변경(사용자 지시) */

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint32_t cam_capture_interval_sec;
    uint32_t response_interval_sec;
    uint32_t adaptive_response_sec;
    uint8_t  agc_enable;
    uint8_t  aec_enable;
    uint8_t  xclk_mhz;
} device_config_file_t;

static uint32_t s_cam_capture_interval_sec = CAM_CAPTURE_INTERVAL_SEC_DEFAULT;
static uint32_t s_response_interval_sec    = RESPONSE_INTERVAL_SEC_DEFAULT;
static uint32_t s_adaptive_response_sec    = ADAPTIVE_RESPONSE_SEC_DEFAULT;
static bool     s_agc_enable               = AGC_ENABLE_DEFAULT;
static bool     s_aec_enable               = AEC_ENABLE_DEFAULT;
static uint8_t  s_xclk_mhz                 = XCLK_MHZ_DEFAULT;

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
        .agc_enable              = s_agc_enable ? 1 : 0,
        .aec_enable              = s_aec_enable ? 1 : 0,
        .xclk_mhz                = s_xclk_mhz,
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
        /* 2026-08-11 — 화면에도 반드시 보이게 함(UI_ERR_CONFIG_FILE_MISMATCH 주석 참고).
         * 이 폴백 자체가 문제가 아니라, 그동안 아무 표시가 없어서 사용자가 모르는 채로
         * 다른 설정 하나만 Apply해도 이 기본값들이 그대로 새 파일에 영구 저장돼버리는 게
         * 진짜 문제였음 — 지금 당장 설정탭에서 실제로 원하는 값들을 다시 Apply해야
         * 파일이 정상화됨을 사용자가 즉시 알아야 함 */
        ui_log_add_err(UI_ERR_CONFIG_FILE_MISMATCH,
                        "Config file format mismatch - fell back to defaults (re-apply in Option tab)");
        return;
    }
    s_cam_capture_interval_sec = s.cam_capture_interval_sec;
    /* 2026-08-11 버그수정 — 예전엔 response_interval_sec==0을 "저장 안 됨"으로 보고 조용히
     * 기본값(2)으로 되돌렸는데, 이제 0은 "즉시/Live"(딥슬립 자체를 안 함)라는 진짜 의미가
     * 있는 값이라 이 되돌림 때문에 "즉시"를 저장해도 다음 로드 때 다시 2로 바뀌어버림 —
     * cam_node.c의 동일 버그(cam_node_set_response_interval_sec)와 같은 패턴, 실사용 중
     * CAM이 계속 I=2로 보고하는 걸로 발견. adaptive_response_sec은 0이 여전히 무의미한
     * 값(적응형 반응시간에 0=즉시 개념이 없음)이라 그대로 둠 */
    s_response_interval_sec    = s.response_interval_sec;
    s_adaptive_response_sec    = s.adaptive_response_sec ? s.adaptive_response_sec : ADAPTIVE_RESPONSE_SEC_DEFAULT;
    s_agc_enable                = s.agc_enable != 0;
    s_aec_enable                = s.aec_enable != 0;
    s_xclk_mhz                  = s.xclk_mhz ? s.xclk_mhz : XCLK_MHZ_DEFAULT;
    ESP_LOGI(TAG, "설정 복원: CAM촬영주기=%us 응답성=%us 적응형반응=%us AGC=%d AEC=%d XCLK=%uMHz",
             (unsigned)s_cam_capture_interval_sec, (unsigned)s_response_interval_sec,
             (unsigned)s_adaptive_response_sec, (int)s_agc_enable, (int)s_aec_enable,
             (unsigned)s_xclk_mhz);
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
    /* 2026-08-11 버그수정 — device_config_load()와 동일 이유로 되돌림 제거 */
    s_response_interval_sec = sec;
    device_config_save();
}

uint32_t device_config_get_adaptive_response_sec(void) { return s_adaptive_response_sec; }

void device_config_set_adaptive_response_sec(uint32_t sec)
{
    s_adaptive_response_sec = sec ? sec : ADAPTIVE_RESPONSE_SEC_DEFAULT;
    device_config_save();
}

bool device_config_get_agc_enable(void) { return s_agc_enable; }

void device_config_set_agc_enable(bool enable)
{
    s_agc_enable = enable;
    device_config_save();
}

bool device_config_get_aec_enable(void) { return s_aec_enable; }

void device_config_set_aec_enable(bool enable)
{
    s_aec_enable = enable;
    device_config_save();
}

uint8_t device_config_get_xclk_mhz(void) { return s_xclk_mhz; }

void device_config_set_xclk_mhz(uint8_t mhz)
{
    s_xclk_mhz = mhz ? mhz : XCLK_MHZ_DEFAULT;
    device_config_save();
}

#define NACK_MAX_ROUNDS_DEFAULT 3  /* esp_now_photo.c 기존 PHOTO_NACK_MAX_ROUNDS/esp_now_cam.c
                                       기존 MAX_NACK_ROUNDS와 동일 값 — 이제 이 한 곳이 유일한
                                       출처(위 device_config_get_nack_max_rounds 선언부 참고) */

uint8_t device_config_get_nack_max_rounds(void) { return NACK_MAX_ROUNDS_DEFAULT; }
