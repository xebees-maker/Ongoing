#include "device_config.h"
#include "fs.h"
#include "ui_log.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "device_config";

#define DEVICE_CONFIG_PATH    FS_MOUNT_POINT "/device_config.bin"
#define DEVICE_CONFIG_VERSION 7  /* 2026-09-05: sens_intervals[] 슬롯 배열 추가(노드별 샘플주기,
                                    사용자 지시)로 6->7 (구버전 파일은 버전 불일치로 기본값으로
                                    자연 폴백 — UI_ERR_CONFIG_FILE_MISMATCH로 화면에도 보임,
                                    device_config_load 참고) */

/* 2026-08-29(사용자 지시: "여러 개 비번 저장 가능하지?") — SSID별로 비밀번호를 기억. 슬롯[0]이
 * 항상 "가장 최근에 set된(=활성)" 자격증명 — set할 때마다 해당 항목을 맨 앞으로 옮기는
 * LRU 방식이라 device_config_get_sta_ssid/password()의 기존 "활성 값 하나" 의미가 그대로
 * 유지됨(esp_now_hub.c 등 기존 호출부 변경 불필요) */
#define STA_CREDENTIAL_SLOTS 8

typedef struct __attribute__((packed)) {
    char ssid[33];
    char password[65];
} sta_credential_t;

/* 2026-09-05(사용자 지시: "센스마다 만들 필요도 있겠는데") — Sens 노드별 샘플링 주기.
 * STA_CREDENTIAL_SLOTS와 같은 mac 키 슬롯 배열이지만, "가장 최근이 활성"이라는 LRU 개념은
 * 필요 없음(여러 노드가 동시에 각자 유효) — 그냥 mac으로 찾아서 없으면 빈 슬롯에 새로 삽입 */
#define SENS_INTERVAL_SLOTS 8

typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint8_t  in_use;
    uint32_t sample_interval_sec;
} sens_interval_entry_t;

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
    uint8_t  wifi_ap_mode;
    sta_credential_t sta_credentials[STA_CREDENTIAL_SLOTS];
    sens_interval_entry_t sens_intervals[SENS_INTERVAL_SLOTS];
} device_config_file_t;

static uint32_t s_cam_capture_interval_sec = CAM_CAPTURE_INTERVAL_SEC_DEFAULT;
static uint32_t s_response_interval_sec    = RESPONSE_INTERVAL_SEC_DEFAULT;
static uint32_t s_adaptive_response_sec    = ADAPTIVE_RESPONSE_SEC_DEFAULT;
static bool     s_agc_enable               = AGC_ENABLE_DEFAULT;
static bool     s_aec_enable               = AEC_ENABLE_DEFAULT;
static uint8_t  s_xclk_mhz                 = XCLK_MHZ_DEFAULT;
static bool     s_wifi_ap_mode             = false;

/* 2026-08-29(사용자 지시: "PSRAM도 133KB밖에 안남았지만, 최대한 몰아 넣어") — 내부 RAM이
 * 오늘 12K->1.7K로 급감한 것 때문에, 새로 늘어나는 저장공간(8슬롯 x 98B=784B)은 처음부터
 * 내부 .bss가 아니라 PSRAM에 할당. device_config_load()에서 최초 1회 할당 */
static sta_credential_t *s_sta_credentials = NULL;
static sens_interval_entry_t *s_sens_intervals = NULL;

/* 2026-08-30 — device_config.bin 암호화(assets 파일 업로드/다운로드 엔드포인트로 평문 WiFi
 * 비번이 노출되는 문제 대비) 시도했으나, 이 ESP-IDF의 mbedtls가 aes.h를 공개 API에서 제거하고
 * PSA Crypto로 옮겨서 구현이 복잡해짐 — 사용자 판단: 지금 급한 요구사항 아니므로 다음 버전으로
 * 보류(사용자: "암호화는 나중에(다음 버전 정도) 하는 걸로 기억하고") */

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
        .wifi_ap_mode            = s_wifi_ap_mode ? 1 : 0,
    };
    memcpy(s.sta_credentials, s_sta_credentials, sizeof(s.sta_credentials));
    memcpy(s.sens_intervals, s_sens_intervals, sizeof(s.sens_intervals));
    fwrite(&s, sizeof(s), 1, f);
    fclose(f);
}

void device_config_load(void)
{
    if (!s_sta_credentials) {
        s_sta_credentials = heap_caps_calloc(STA_CREDENTIAL_SLOTS, sizeof(sta_credential_t), MALLOC_CAP_SPIRAM);
        if (!s_sta_credentials) {
            ESP_LOGE(TAG, "STA 자격증명 PSRAM 할당 실패");
            ui_log_add_err(UI_ERR_STA_CRED_ALLOC, "STA credential PSRAM alloc failed");
            return;
        }
    }
    if (!s_sens_intervals) {
        s_sens_intervals = heap_caps_calloc(SENS_INTERVAL_SLOTS, sizeof(sens_interval_entry_t), MALLOC_CAP_SPIRAM);
        if (!s_sens_intervals) {
            ESP_LOGE(TAG, "Sens 주기 슬롯 PSRAM 할당 실패");
            return;
        }
    }

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
    s_wifi_ap_mode               = s.wifi_ap_mode != 0;
    for (int i = 0; i < STA_CREDENTIAL_SLOTS; i++) {
        s.sta_credentials[i].ssid[sizeof(s.sta_credentials[i].ssid) - 1]         = '\0';
        s.sta_credentials[i].password[sizeof(s.sta_credentials[i].password) - 1] = '\0';
    }
    memcpy(s_sta_credentials, s.sta_credentials, sizeof(s.sta_credentials));
    memcpy(s_sens_intervals, s.sens_intervals, sizeof(s.sens_intervals));
    ESP_LOGI(TAG, "설정 복원: CAM촬영주기=%us 응답성=%us 적응형반응=%us AGC=%d AEC=%d XCLK=%uMHz "
             "WiFi=%s SSID=%s",
             (unsigned)s_cam_capture_interval_sec, (unsigned)s_response_interval_sec,
             (unsigned)s_adaptive_response_sec, (int)s_agc_enable, (int)s_aec_enable,
             (unsigned)s_xclk_mhz, s_wifi_ap_mode ? "AP" : "STA", s_sta_credentials[0].ssid);
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

bool device_config_get_wifi_ap_mode(void) { return s_wifi_ap_mode; }

void device_config_set_wifi_ap_mode(bool ap_mode)
{
    s_wifi_ap_mode = ap_mode;
    device_config_save();
}

/* s_sta_credentials는 device_config_load()에서 PSRAM 할당됨(app_main()이 항상 그걸 먼저
 * 부름) — 방어적으로 NULL 체크만 유지(할당 실패/호출순서 사고 시 크래시 대신 빈 값) */
const char *device_config_get_sta_ssid(void)     { return s_sta_credentials ? s_sta_credentials[0].ssid : ""; }
const char *device_config_get_sta_password(void) { return s_sta_credentials ? s_sta_credentials[0].password : ""; }

/* 2026-08-29(사용자 지시: 여러 SSID/비번 저장) — 이미 있는 SSID면 그 슬롯을 갱신하며 맨
 * 앞으로, 없으면 맨 앞에 새로 삽입(제일 오래된 슬롯이 뒤로 밀려나서 사라짐). 슬롯[0]이
 * 항상 "가장 최근에 set된" 것이라 위 get_sta_ssid/password()가 계속 "활성 값"을 가리킴 */
void device_config_set_sta_credentials(const char *ssid, const char *password)
{
    if (!s_sta_credentials) return;
    int found = -1;
    for (int i = 0; i < STA_CREDENTIAL_SLOTS; i++) {
        if (strcmp(s_sta_credentials[i].ssid, ssid ? ssid : "") == 0) { found = i; break; }
    }
    int src = (found >= 0) ? found : (STA_CREDENTIAL_SLOTS - 1);
    for (int i = src; i > 0; i--) {
        s_sta_credentials[i] = s_sta_credentials[i - 1];
    }
    strncpy(s_sta_credentials[0].ssid, ssid ? ssid : "", sizeof(s_sta_credentials[0].ssid) - 1);
    s_sta_credentials[0].ssid[sizeof(s_sta_credentials[0].ssid) - 1] = '\0';
    strncpy(s_sta_credentials[0].password, password ? password : "", sizeof(s_sta_credentials[0].password) - 1);
    s_sta_credentials[0].password[sizeof(s_sta_credentials[0].password) - 1] = '\0';
    device_config_save();
}

const char *device_config_find_sta_password(const char *ssid)
{
    if (!ssid || !s_sta_credentials) return NULL;
    for (int i = 0; i < STA_CREDENTIAL_SLOTS; i++) {
        if (s_sta_credentials[i].ssid[0] != '\0' && strcmp(s_sta_credentials[i].ssid, ssid) == 0) {
            return s_sta_credentials[i].password;
        }
    }
    return NULL;
}

#define NACK_MAX_ROUNDS_DEFAULT 3  /* esp_now_photo.c 기존 PHOTO_NACK_MAX_ROUNDS/esp_now_cam.c
                                       기존 MAX_NACK_ROUNDS와 동일 값 — 이제 이 한 곳이 유일한
                                       출처(위 device_config_get_nack_max_rounds 선언부 참고) */

uint8_t device_config_get_nack_max_rounds(void) { return NACK_MAX_ROUNDS_DEFAULT; }

/* 없으면 0(미설정) 반환 — 호출부가 기본값(15)으로 폴백 */
uint32_t device_config_get_sens_sample_interval_sec(const uint8_t *mac)
{
    if (!s_sens_intervals || !mac) return 0;
    for (int i = 0; i < SENS_INTERVAL_SLOTS; i++) {
        if (s_sens_intervals[i].in_use && memcmp(s_sens_intervals[i].mac, mac, 6) == 0) {
            return s_sens_intervals[i].sample_interval_sec;
        }
    }
    return 0;
}

void device_config_set_sens_sample_interval_sec(const uint8_t *mac, uint32_t sec)
{
    if (!s_sens_intervals || !mac) return;
    int found = -1, empty = -1;
    for (int i = 0; i < SENS_INTERVAL_SLOTS; i++) {
        if (s_sens_intervals[i].in_use && memcmp(s_sens_intervals[i].mac, mac, 6) == 0) { found = i; break; }
        if (empty < 0 && !s_sens_intervals[i].in_use) empty = i;
    }
    int slot = (found >= 0) ? found : empty;
    if (slot < 0) {
        ESP_LOGW(TAG, "Sens 주기 슬롯 꽉 참(%d개) — 저장 못 함", SENS_INTERVAL_SLOTS);
        return;
    }
    memcpy(s_sens_intervals[slot].mac, mac, 6);
    s_sens_intervals[slot].in_use = 1;
    s_sens_intervals[slot].sample_interval_sec = sec;
    device_config_save();
}
