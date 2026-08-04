#include "ui_strings.h"
#include "fs.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ui_strings";
#define SETTINGS_PATH  FS_MOUNT_POINT "/settings.bin"

static ui_lang_t s_lang = UI_LANG_KO;

static const char *s_table[STR_COUNT][UI_LANG_COUNT] = {
    [STR_LOGO_TITLE]     = { "플렉스팜", "FlexFarm" },
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
    [STR_PANEL_PICTURE]           = { "미리보기 - 원본은 웹으로 볼 수 있습니다.", "Thumbnail - Full size image only on Web" },
    [STR_MSG_DELETE_PHOTO_CONFIRM]   = { "이 사진을 삭제할까요?",       "Delete this photo?" },
    [STR_CAPTURE_STAGE1_PROGRESS]    = { "1. 카메라에 명령 전달 중...", "1. Sending command to camera..." },
    [STR_CAPTURE_STAGE1_DONE]        = { "1. 카메라에 명령 전달 완료", "1. Command delivered" },
    [STR_CAPTURE_STAGE2_SUCCESS]     = { "2. 촬영 완료",               "2. Capture complete" },
    [STR_CAPTURE_STAGE2_FAILED]      = { "2. 촬영 실패",               "2. Capture failed" },
    [STR_CAPTURE_STAGE2_NORESPONSE]  = { "2. 카메라 응답 없음",        "2. No response from camera" },
    [STR_CAPTURE_STAGE3_PROGRESS]    = { "3. 목록 갱신 중...",         "3. Refreshing list..." },
    [STR_CAPTURE_STAGE3_DONE]        = { "3. 목록 갱신 완료",         "3. List refreshed" },
    [STR_CAPTURE_STAGE3_UNKNOWN]     = { "3. 상태 확인 불가 — 목록갱신으로 다시 확인하세요",
                                          "3. Unable to verify — use Renew list to check again" },
    [STR_FETCH_CONNECTING]           = { "사진 가져오는 중...",         "Fetching photo..." },
    [STR_FETCH_PROGRESS_FMT]         = { "사진 가져오는 중... %d%%",   "Fetching photo... %d%%" },
    [STR_FETCH_PROGRESS_ETA_FMT]     = { "사진 가져오는 중... %d%% (약 %d초 남음)",
                                          "Fetching photo... %d%% (~%ds left)" },
    [STR_FETCH_DONE]                 = { "가져오기 완료",               "Photo received" },
    [STR_FETCH_FAILED]               = { "가져오기 실패",               "Failed to fetch photo" },
    [STR_FETCH_STALLED]              = { "응답 없음 — 연결 상태를 확인하세요", "No response — check connection" },
    [STR_BTN_DELETE_ALL]             = { "모두 지우기",                 "Delete all" },
    [STR_MSG_DELETE_ALL_CONFIRM]     = { "모두 지울까요?",             "Delete all photos?" },
    [STR_DELETEALL_STAGE1_PROGRESS]  = { "1. 삭제 중...",               "1. Deleting..." },
    [STR_DELETEALL_STAGE1_DONE]      = { "1. 삭제 완료",               "1. Deleted" },
    [STR_DELETEALL_STAGE1_FAILED]    = { "1. 삭제 실패",               "1. Delete failed" },
    [STR_DELETEALL_STAGE1_NORESPONSE] = { "1. 카메라 응답 없음",        "1. No response from camera" },
    [STR_DELETEALL_STAGE2_PROGRESS]  = { "2. 목록 갱신 중...",         "2. Refreshing list..." },
    [STR_DELETEALL_STAGE2_DONE]      = { "2. 목록 갱신 완료",         "2. List refreshed" },
    [STR_DELETEALL_STAGE2_UNKNOWN]   = { "2. 상태 확인 불가 — 목록갱신으로 다시 확인하세요",
                                          "2. Unable to verify — use Renew list to check again" },
    [STR_LIST_RENEW_PROGRESS]        = { "목록 갱신 중...",             "Refreshing list..." },
    [STR_LIST_EMPTY]                 = { "사진 없음",                   "No Picture" },
};

void ui_lang_load(void)
{
    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (!f) return;  /* 파일 없음 — 첫 부팅, 기본값(UI_LANG_KO) 유지 */
    uint8_t val = 0;
    if (fread(&val, 1, 1, f) == 1 && val < UI_LANG_COUNT) {
        s_lang = (ui_lang_t)val;
    }
    fclose(f);
}

void ui_lang_set(ui_lang_t lang)
{
    if (lang >= UI_LANG_COUNT) return;
    s_lang = lang;

    FILE *f = fopen(SETTINGS_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "언어 설정 저장 실패(fopen)");
        return;
    }
    uint8_t val = (uint8_t)lang;
    fwrite(&val, 1, 1, f);
    fclose(f);
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
