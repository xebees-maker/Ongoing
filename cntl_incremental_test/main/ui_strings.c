#include "ui_strings.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "ui_strings";
static const char *NVS_NAMESPACE = "cntl_cfg";
static const char *NVS_KEY_LANG  = "lang";

static ui_lang_t s_lang = UI_LANG_KO;

static const char *s_table[STR_COUNT][UI_LANG_COUNT] = {
    [STR_TAB_DASHBOARD]  = { "상황판", "Dashboard" },
    [STR_TAB_STATISTICS] = { "통계",   "Statistics" },
    [STR_TAB_OPTION]     = { "설정",   "Option" },
    [STR_GROUP_CNTL]     = { "제어기", "CNTL" },
    [STR_GROUP_SENSOR]   = { "측정기", "Sensor" },
    [STR_GROUP_CAMERA]   = { "영상",   "Camera" },
    [STR_GROUP_SYSTEM]   = { "시스템", "System" },
    [STR_LABEL_LANGUAGE] = { "언어",   "Language" },
    [STR_STATUS_CONNECTING]  = { "연결 대기 중",             "Waiting for connection" },
    [STR_STATUS_CONNECTED]   = { "연결됨",                 "Connected" },
    [STR_MSG_PAIR_CONFIRM]   = { "연결을 허용할까요?",     "Allow this connection?" },
    [STR_MSG_UNPAIR_CONFIRM] = { "이 연결을 해제할까요?",   "Disconnect this connection?" },
    [STR_BTN_CONFIRM]        = { "확인",                   "OK" },
    [STR_BTN_CANCEL]         = { "취소",                   "Cancel" },
    [STR_BTN_YES]            = { "예",                     "Yes" },
    [STR_PANEL_SUMMARY]           = { "요약",                   "Summary" },
    [STR_PANEL_NO_SENSOR]         = { "연결된 측정기 없음",     "No sensor device" },
    [STR_PANEL_NO_CAMERA]         = { "연결된 카메라 없음",     "No camera device" },
    [STR_PANEL_NO_PAIRED_DEVICE]  = { "연결된 장치 없음",       "No connected device" },
    [STR_PANEL_SENSOR_TODO]       = { "(값/명령 UI는 다음에 추가 예정)", "(Values/commands coming soon)" },
    [STR_PANEL_NO_PHOTO_YET]      = { "(사진 없음)",             "(No photo yet)" },
    [STR_STATUS_OK]               = { "정상",                   "OK" },
    [STR_BTN_CAPTURE_NOW]         = { "지금촬영",                 "Manual shot" },
    [STR_BTN_RENEW_LIST]          = { "목록갱신",                 "Renew list" },
    [STR_PANEL_LIST]              = { "목록",                     "List" },
    [STR_PANEL_PICTURE]           = { "사진",                     "Picture" },
    [STR_BTN_CLOSE]                  = { "닫기",                       "Close" },
    [STR_MSG_DELETE_PHOTO_CONFIRM]   = { "이 사진을 삭제할까요?",       "Delete this photo?" },
    [STR_CAPTURE_STAGE1_PROGRESS]    = { "1. 카메라에 명령 전달 중...", "1. Sending command to camera..." },
    [STR_CAPTURE_STAGE1_DONE]        = { "1. 카메라에 명령 전달 완료", "1. Command delivered" },
    [STR_CAPTURE_STAGE2_SUCCESS]     = { "2. 촬영 완료",               "2. Capture complete" },
    [STR_CAPTURE_STAGE2_FAILED]      = { "2. 촬영 실패",               "2. Capture failed" },
    [STR_CAPTURE_STAGE3_PROGRESS]    = { "3. 영상 가져오는 중...",     "3. Receiving photo..." },
    [STR_CAPTURE_STAGE3_DONE]        = { "3. 영상 가져오기 완료",     "3. Photo received" },
    [STR_CAPTURE_STAGE3_FAILED]      = { "3. 영상 가져오기 실패",     "3. Photo transfer failed" },
    [STR_CAPTURE_STAGE4_PROGRESS]    = { "4. 목록 갱신 중...",         "4. Refreshing list..." },
    [STR_CAPTURE_STAGE4_DONE]        = { "4. 목록 갱신 완료",         "4. List refreshed" },
};

void ui_lang_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;  /* 네임스페이스가 아직 없음 — 첫 부팅, 기본값(UI_LANG_KO) 유지 */
    }
    uint8_t val = 0;
    if (nvs_get_u8(h, NVS_KEY_LANG, &val) == ESP_OK && val < UI_LANG_COUNT) {
        s_lang = (ui_lang_t)val;
    }
    nvs_close(h);
}

void ui_lang_set(ui_lang_t lang)
{
    if (lang >= UI_LANG_COUNT) return;
    s_lang = lang;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "언어 설정 저장 실패(nvs_open)");
        return;
    }
    nvs_set_u8(h, NVS_KEY_LANG, (uint8_t)lang);
    nvs_commit(h);
    nvs_close(h);
}

ui_lang_t ui_lang_get(void)
{
    return s_lang;
}

const char *ui_str(ui_str_id_t id)
{
    if (id >= STR_COUNT) return "";
    return s_table[id][s_lang];
}
