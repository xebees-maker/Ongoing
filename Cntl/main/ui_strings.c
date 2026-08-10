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
    [STR_STATUS_CANCEL_PENDING] = { "종료 대기 중...",         "Finishing..." },
    [STR_BTN_YES]            = { "예",                     "Yes" },
    [STR_PANEL_SUMMARY]           = { "요약",                   "Summary" },
    [STR_PANEL_NO_SENSOR]         = { "연결된 측정기 없음",     "No sensor device" },
    [STR_PANEL_NO_CAMERA]         = { "연결된 카메라 없음",     "No camera device" },
    [STR_PANEL_NO_PAIRED_DEVICE]  = { "연결된 장치 없음",       "No connected device" },
    [STR_PANEL_SENSOR_TODO]       = { "(값/명령 UI는 다음에 추가 예정)", "(Values/commands coming soon)" },
    [STR_PANEL_NO_PHOTO_YET]      = { "(사진 없음)",             "(No photo yet)" },
    [STR_STATUS_OK]               = { "정상",                   "OK" },
    [STR_STATUS_PAIRED]           = { "페어됨",                 "Paired" },
    [STR_STATUS_ACTIVE]           = { "통신 중",                 "Active" },
    [STR_BTN_CAPTURE_NOW]         = { "지금촬영",                 "Manual shot" },
    [STR_BTN_RENEW_LIST]          = { "목록갱신",                 "Renew list" },
    [STR_PANEL_LIST]              = { "목록",                     "List" },
    [STR_PANEL_PICTURE]           = { "미리보기 - 원본은 웹에서", "Thumbnail - Full size on WEB" },
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
    [STR_LABEL_CAPTURE_INTERVAL]     = { "촬영 주기",                   "Capture interval" },
    [STR_LABEL_RESPONSE_INTERVAL]    = { "응답성",                     "Responsiveness" },
    [STR_BTN_APPLY]                  = { "적용",                       "Apply" },
    [STR_CONFIG_APPLY_PROGRESS]      = { "설정 적용 중...",             "Applying settings..." },
    [STR_CONFIG_APPLY_STALLED]       = { "응답 없음 — 연결 상태를 확인하세요", "No response — check connection" },
    [STR_TITLE_ERROR_LIST]           = { "에러 코드 목록",               "Error code list" },
    [STR_ERROR_LIST_EMPTY]           = { "(없음)",                     "(None)" },
    [STR_LABEL_RESTART_DEVICE]       = { "장치 재시작",                 "Restart device" },
    [STR_BTN_RESTART]                = { "재시작",                     "Restart" },
    [STR_MSG_RESTART_CONFIRM]        = { "정말 재시작하시겠습니까?",     "Do you want restart?" },
    /* 영문일 때는 단위를 S/M/H로 축약(2026-08-08, 사용자 지시) — 값(초 단위)은
     * s_capture_interval_values/s_response_interval_values와 순서가 반드시 같아야 함 */
    [STR_OPT_CAPTURE_INTERVAL_LIST]  = { "끄기\n30분\n1시간\n3시간\n10시간", "Off\n30M\n1H\n3H\n10H" },
    /* 2026-08-10 CAM Deep Sleep 전환 — 값 구간 재정의(1/3/10/30/1800초), 짧은 라벨만
     * 표시하고 뜻은 별도 도움말 텍스트로(STR_RESPONSE_HELP_0..4, 아래) — 순서는
     * ui_main.c의 s_response_interval_values와 반드시 같이 맞출 것 */
    [STR_OPT_RESPONSE_INTERVAL_LIST] = { "1초\n3초\n10초\n30초\n30분",     "1S\n3S\n10S\n30S\n30M" },
    [STR_LABEL_TIME]                 = { "시각",                       "Time" },
    [STR_BTN_SET_TIME]               = { "설정",                       "Set" },
    [STR_TITLE_SET_TIME]             = { "시각 설정",                   "Set time" },
    [STR_PANEL_DEEPSLEEP]            = { "절전 상태",                   "Power Status" },
    /* 2026-08-10 사용자 지시로 축약 포맷 확정 — awake/I/slept/R 라벨은 언어 무관 고정,
     * 라벨-숫자 사이 공백 추가(가독성), interval->I, "RWDT n회"/"RWDT xn"->"R n" */
    /* awake만 ms 단위(2026-08-10) — 필요시 초기화+채널기억 최적화 이후 1초 미만이 흔해져서
     * 초 단위로는 대부분 0으로 뭉개짐(정보 없음) */
    [STR_DEEPSLEEP_LINE_FMT]         = { "%s: 사이클#%lu [%s] awake %lums/I %lu초, slept %lu초, R %lu",
                                          "%s: cycle#%lu [%s] awake %lums/I %lus, slept %lus, R %lu" },
    [STR_WAKE_REASON_TIMER]          = { "정상",                       "normal" },
    [STR_WAKE_REASON_RWDT]           = { "RWDT복구",                   "RWDT-caught" },
    [STR_WAKE_REASON_POWERON]        = { "전원인가",                   "power-on" },
    [STR_WAKE_REASON_OTHER]          = { "기타",                       "other" },
    [STR_RESPONSE_HELP_0] = { "성능모드 — 즉시 반응(사실상 상시 동작)",
                               "Performance mode - responds instantly (effectively always on)" },
    [STR_RESPONSE_HELP_1] = { "동작확인 모드 — Sleep이 실제로 도는지 테스트용",
                               "Verify mode - for checking that sleep cycling actually works" },
    [STR_RESPONSE_HELP_2] = { "균형모드 — 절전되면서도 사용성 적절",
                               "Balanced mode - power-saving with reasonable usability" },
    [STR_RESPONSE_HELP_3] = { "절전모드 — 참을 수 있으면 배터리 오래감",
                               "Power-save mode - longer battery life if you can wait" },
    [STR_RESPONSE_HELP_4] = { "최대절전모드 — 수동촬영은 포기, 자동촬영분만 확인",
                               "Max power-save - manual capture unavailable, auto-captures only" },
    [STR_LABEL_ADAPTIVE_RESPONSE]     = { "적응형 반응시간",           "Adaptive responsiveness" },
    [STR_OPT_ADAPTIVE_RESPONSE_LIST]  = { "10초\n30초\n1분",           "10S\n30S\n1M" },
    [STR_HELP_ADAPTIVE_RESPONSE]      = { "조작이 멈출 때까지 절전하지 않음",
                                           "Won't power-save until you stop interacting" },
    [STR_BTN_PAUSE]                   = { "일시멈춤",                  "Pause" },
    [STR_BTN_RESUME]                  = { "재개",                      "Resume" },
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
