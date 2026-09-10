#include "ui_strings.h"
#include "fs.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ui_strings";
#define SETTINGS_PATH  FS_MOUNT_POINT "/settings.bin"

/* 2026-09-07(임시 실험 — 비트맵 폰트엔 한글 글리프가 없음) — 기본값을 EN으로 강제.
 * 원복: UI_LANG_KO로 되돌리면 됨 */
static ui_lang_t s_lang = UI_LANG_EN;

static const char *s_table[STR_COUNT][UI_LANG_COUNT] = {
    [STR_LOGO_TITLE]     = { "플렉스팜", "FlexFarm" },
    [STR_TAB_DASHBOARD]  = { "상황판", "Dashboard" },
    [STR_TAB_STATISTICS] = { "통계",   "Sensor-Statistics" },
    [STR_TAB_OPTION]     = { "설정",   "Settings" },
    [STR_TAB_LOG]        = { "로그",   "Log" },
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
    [STR_CAPTURE_STAGE2_INIT_NEEDED] = { "2. 카메라 초기화 중...",     "2. Initializing camera..." },
    [STR_CAPTURE_STAGE2_INIT_DONE]   = { "2. 카메라 초기화 완료",     "2. Camera initialized" },
    [STR_CAPTURE_STAGE2_CAPTURING]   = { "2. 촬영 중...",             "2. Capturing..." },
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
    [STR_DELETEALL_STAGE1_PROGRESS]  = { "1. 접수 확인 중...",          "1. Waiting for camera to receive..." },
    [STR_DELETEALL_STAGE1_DELETING_FMT] = { "1. 삭제 중... (%u개)",     "1. Deleting... (%u files)" },
    [STR_DELETEALL_STAGE1_DONE]      = { "1. 삭제 완료",               "1. Deleted" },
    [STR_DELETEALL_STAGE1_FAILED]    = { "1. 삭제 실패",               "1. Delete failed" },
    [STR_DELETEALL_STAGE1_NORESPONSE] = { "1. 카메라 응답 없음",        "1. No response from camera" },
    [STR_DELETEALL_STAGE1_STOPPED_FMT] = { "1. 삭제 중단됨 — 완료 응답 없음(%u개 접수 후)",
                                          "1. Delete stopped — no completion response (after receiving %u)" },
    [STR_DELETEALL_STAGE2_PROGRESS]  = { "2. 목록 갱신 중...",         "2. Refreshing list..." },
    [STR_DELETEALL_STAGE2_DONE]      = { "2. 목록 갱신 완료",         "2. List refreshed" },
    [STR_DELETEALL_STAGE2_UNKNOWN]   = { "2. 상태 확인 불가 — 목록갱신으로 다시 확인하세요",
                                          "2. Unable to verify — use Renew list to check again" },
    [STR_LIST_STAGE1_PROGRESS]       = { "1. 가져오기 명령 전송 중...", "1. Sending fetch command..." },
    [STR_LIST_STAGE1_DONE]           = { "1. 가져오기 명령 전송됨",     "1. Fetch command sent" },
    [STR_LIST_STAGE1_NORESPONSE]     = { "1. 카메라 응답 없음",         "1. No response from camera" },
    [STR_LIST_STAGE2_PROGRESS]       = { "2. 목록 수신 중...",           "2. Receiving list..." },
    [STR_LIST_STAGE2_SUCCESS]        = { "2. 목록 수신 성공",           "2. List received" },
    [STR_LIST_STAGE2_MISMATCH_FMT]   = { "2. 목록 수신 실패: 개수 불일치(%u/%u개)",
                                          "2. List receive failed: count mismatch (%u/%u)" },
    [STR_LIST_STAGE2_STALLED_FMT]    = { "2. 목록 수신 실패: 응답 없음(%u/%u개)",
                                          "2. List receive failed: no response (%u/%u)" },
    [STR_LIST_EMPTY]                 = { "사진 없음",                   "No Picture" },
    [STR_LABEL_CAPTURE_INTERVAL]     = { "촬영 주기",                   "Capture interval" },
    [STR_LABEL_RESPONSE_INTERVAL]    = { "응답성",                     "Responsiveness" },
    [STR_LABEL_AGC]                  = { "자동게인(AGC)",               "Auto gain (AGC)" },
    [STR_LABEL_AEC]                  = { "자동노출(AEC)",               "Auto exposure (AEC)" },
    [STR_LABEL_XCLK]                 = { "픽셀클럭(XCLK)",              "Pixel clock (XCLK)" },
    [STR_OPT_XCLK_LIST]              = { "5MHz\n10MHz\n20MHz\n24MHz",  "5MHz\n10MHz\n20MHz\n24MHz" },
    [STR_LABEL_SENS_MEASURE_INTERVAL]    = { "측정 주기",                 "Measure period" },
    [STR_OPT_SENS_MEASURE_INTERVAL_LIST] = { "10초\n30초\n1분\n5분\n30분", "10S\n30S\n1M\n5M\n30M" },
    [STR_BTN_APPLY]                  = { "적용",                       "Apply" },
    [STR_CONFIG_APPLY_PROGRESS]      = { "설정 적용 중...",             "Applying settings..." },
    [STR_CONFIG_APPLY_STALLED]       = { "응답 없음 — 연결 상태를 확인하세요", "No response — check connection" },
    [STR_TITLE_ERROR_LIST]           = { "에러 코드 목록",               "Error code list" },
    [STR_ERROR_LIST_EMPTY]           = { "(없음)",                     "(None)" },
    [STR_TITLE_WEB_QR]               = { "웹 접속 QR",                 "Web Access QR" },
    [STR_MSG_WEB_QR_NO_IP]           = { "아직 IP가 없습니다",          "No IP yet" },
    [STR_LABEL_RESTART_DEVICE]       = { "장치 재시작",                 "Restart device" },
    [STR_BTN_RESTART]                = { "재시작",                     "Restart" },
    [STR_MSG_RESTART_CONFIRM]        = { "정말 재시작하시겠습니까?",     "Do you want restart?" },
    /* 영문일 때는 단위를 S/M/H로 축약(2026-08-08, 사용자 지시) — 값(초 단위)은
     * s_capture_interval_values/s_response_interval_values와 순서가 반드시 같아야 함 */
    [STR_OPT_CAPTURE_INTERVAL_LIST]  = { "끄기\n30분\n1시간\n3시간\n10시간", "Off\n30M\n1H\n3H\n10H" },
    /* 2026-08-10 CAM Deep Sleep 전환 — 값 구간 재정의(1/3/10/30/1800초), 짧은 라벨만
     * 표시하고 뜻은 별도 도움말 텍스트로(STR_RESPONSE_HELP_0..4, 아래) — 순서는
     * ui_main.c의 s_response_interval_values와 반드시 같이 맞출 것 */
    /* 2026-08-11, 사용자 지시 — 첫 단계는 더 이상 리터럴 초 값이 아니라(값 자체가 0으로
     * 바뀜, ui_main.c의 s_response_interval_values 참고) "즉시"/"Live" 라벨로 고정 */
    /* 2026-09-05(사용자 지시) — 마지막 단계를 30분->1분으로 조정(센스 MIN(응답성,측정주기)
     * 계산의 상한이 너무 크면 실효성이 없어서, ui_main.c의 s_response_interval_values와
     * 반드시 같이 맞출 것) */
    [STR_OPT_RESPONSE_INTERVAL_LIST] = { "즉시\n3초\n10초\n30초\n1분",      "Live\n3S\n10S\n30S\n1M" },
    [STR_LABEL_TIME]                 = { "시각",                       "Time" },
    [STR_BTN_SET_TIME]               = { "설정",                       "Set" },
    [STR_TITLE_SET_TIME]             = { "시각 설정",                   "Set time" },
    [STR_PANEL_DEEPSLEEP]            = { "절전 상태",                   "Power Status" },
    /* 2026-08-10 사용자 지시로 축약 포맷 확정 — awake/I/slept/R 라벨은 언어 무관 고정,
     * 라벨-숫자 사이 공백 추가(가독성), interval->I, "RWDT n회"/"RWDT xn"->"R n" */
    /* awake만 ms 단위(2026-08-10) — 필요시 초기화+채널기억 최적화 이후 1초 미만이 흔해져서
     * 초 단위로는 대부분 0으로 뭉개짐(정보 없음) */
    /* 2026-08-22 — 끝에 배터리 진단정보(mV/%%/raw) 추가. USB 없이 배터리만으로 테스트할 때
     * 이 화면(전력로그판넬)이 유일한 확인 수단이라 raw ADC값까지 남김(실측 대조/보정용,
     * CH32V003 ADC 비트폭·기준전압 미확정 상태) */
    [STR_DEEPSLEEP_LINE_FMT]         = { "%s: C#%lu [%s] Aw %lums/I %lu초, SL %lu초, R %lu, bat %umV(%u%%) raw=%u",
                                          "%s: C#%lu [%s] Aw %lums/I %lus, SL %lus, R %lu, bat %umV(%u%%) raw=%u" },
    /* 2026-08-22 — 전력로그 한 줄이 배터리 진단정보 추가로 길어져서 화면폭을 넘기고 "..."로
     * 잘리는 문제(사용자 지적) — 줄여서 여유 확보. RWDT는 그대로 둠(카운터 R과 별개, 그
     * 자체로 이미 짧음) */
    [STR_WAKE_REASON_TIMER]          = { "NM",                         "NM" },
    [STR_WAKE_REASON_RWDT]           = { "RWDT복구",                   "RWDT-caught" },
    [STR_WAKE_REASON_POWERON]        = { "PO",                         "PO" },
    [STR_WAKE_REASON_OTHER]          = { "OT",                         "OT" },
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
    [STR_OPT_ADAPTIVE_RESPONSE_LIST]  = { "10초\n30초\n1분\n5분",           "10S\n30S\n1M\n5M" },
    [STR_HELP_ADAPTIVE_RESPONSE]      = { "조작이 멈출 때까지 절전하지 않음",
                                           "Won't power-save until you stop interacting" },
    [STR_BTN_PAUSE]                   = { "일시멈춤",                  "Pause" },
    [STR_BTN_RESUME]                  = { "재개",                      "Resume" },
    [STR_PANEL_GENERAL_LOG]           = { "일반 로그",                 "General Log" },
    [STR_LABEL_WEB]                   = { "웹",                       "Web" },
    [STR_LABEL_MEMORY]                = { "메모리",                   "Memory" },
    [STR_LABEL_STORAGE]               = { "저장공간",                 "Storage" },
    [STR_LABEL_PICTURE]               = { "사진",                     "Picture" },
    [STR_LABEL_TOTAL]                 = { "전체",                     "Total" },
    /* 인자 순서(%s 카테고리, %u 개수)는 두 언어 버전이 반드시 동일해야 함 — printf 포맷은
     * 언어별로 인자 순서를 못 바꿈(위치지정자 미사용) */
    [STR_MSG_STORAGE_CLEANUP]         = { "저장공간 정리: %s 오래된 기록 %u개 삭제(공간 확보)",
                                           "Storage cleanup: %s - deleted %u oldest records to free space" },
    [STR_LABEL_BATTERY]               = { "배터리",                   "Battery" },
    [STR_LABEL_NETWORK]               = { "네트워크",                 "Network" },
    [STR_NETWORK_MODE_AP]             = { "독립",                     "AP" },
    [STR_NETWORK_MODE_STA]            = { "종속",                     "STA" },
    [STR_BTN_FIND]                    = { "찾기",                     "Find" },
    [STR_MSG_NETWORK_MODE_RESTART_CONFIRM] = { "네트워크 모드를 바꾸려면 재시작해야 합니다. 지금 재시작할까요?",
                                                "Changing network mode requires a restart. Restart now?" },
    [STR_TITLE_WIFI_SCAN]             = { "WiFi 검색",                 "WiFi Scan" },
    [STR_MSG_WIFI_SCANNING]           = { "검색 중...",                "Scanning..." },
    [STR_MSG_WIFI_SCAN_EMPTY]         = { "검색된 네트워크가 없습니다", "No networks found" },
    [STR_LABEL_WIFI_PASSWORD]         = { "비밀번호",                 "Password" },
    [STR_BTN_CONNECT]                 = { "연결",                     "Connect" },
    [STR_MSG_WIFI_CONNECTING]         = { "연결 중...",                "Connecting..." },
    [STR_MSG_WIFI_CONNECT_FAILED]     = { "연결 실패",                 "Connection failed" },
    [STR_MSG_WIFI_STAGE_DISCONNECTING] = { "기존 연결 정리 중...",     "Clearing previous connection..." },
    [STR_MSG_WIFI_STAGE_AUTHENTICATING] = { "인증 시도 중...",         "Authenticating..." },
    [STR_STATUS_NOT_CONNECTED]        = { "아직 연결된 네트워크 없음", "Not connected yet" },
    [STR_STATUS_NO_AP]                = { "AP 없음",                  "No AP" },
    [STR_TAG_WIFI_CONNECTED]          = { "[연결됨] ",                "[Connected] " },
    [STR_BTN_RESCAN]                  = { "다시 찾기",                 "Rescan" },
    [STR_BTN_CLOSE]                   = { "닫기",                     "Close" },
    [STR_MSG_WIFI_CREDENTIALS_RESTART_CONFIRM] = { "네트워크 정보가 저장되었습니다. 적용하려면 재시작해야 합니다. 지금 재시작할까요?",
                                                     "Network settings saved. Restart to apply. Restart now?" },
    [STR_BTN_SHOW]                    = { "보기",                     "Show" },
    [STR_BTN_HIDE]                    = { "숨김",                     "Hide" },
    [STR_CONNECT_FAILED]              = { "연결 실패",                 "Connect failed" },
    [STR_DISCONNECT_SUCCESS]          = { "연결 해제됨",               "Disconnected" },
    [STR_LIST_FETCH_SUCCESS]          = { "목록 수신 완료",             "List received" },
    [STR_LIST_FETCH_FAILED]           = { "목록 가져오기 실패",         "Failed to fetch list" },
    /* 2026-09-05(사용자 설계) — sensor_channel_type_t(esp_now_link.h) enum 순서와 반드시
     * 같이 맞출 것: NONE=0(미사용), TEMP_C, HUMI_PCT, CO2_PPM */
    [STR_CHAN_LABEL_TEMP_C]           = { "온도",                       "Temperature" },
    [STR_CHAN_UNIT_TEMP_C]            = { "\xC2\xB0""C",                "\xC2\xB0""C" },
    [STR_CHAN_LABEL_HUMI_PCT]         = { "습도",                       "Humidity" },
    [STR_CHAN_UNIT_HUMI_PCT]          = { "%",                          "%" },
    [STR_CHAN_LABEL_CO2_PPM]          = { "이산화탄소",                  "CO2" },
    [STR_CHAN_UNIT_CO2_PPM]           = { "ppm",                        "ppm" },
    /* 2026-09-05 — %f 안 씀(newlib-nano+LVGL 둘 다 float printf 미지원 전례,
     * format_battery_display()와 동일 회피 — feedback_lvgl_no_percent_f 메모리 참고).
     * 값을 정수부/소수부(2자리)로 미리 쪼개서 넘김 */
    [STR_SENSOR_VALUE_ROW_FMT]        = { "%s %d.%02d%s (ID:%lu, Time: %02u:%02u:%02u)",
                                           "%s %d.%02d%s (ID:%lu, Time: %02u:%02u:%02u)" },
    [STR_SENSOR_VALUE_PENDING]        = { "측정 중...",                 "Measuring..." },
    [STR_SENSOR_VALUE_INVALID]        = { "부적합",                     "Invalid" },
    [STR_CHAN_LABEL_NH3_PPM]          = { "암모니아",                   "Ammonia" },
    [STR_CHAN_UNIT_NH3_PPM]           = { "ppm",                        "ppm" },
    /* 2026-09-06(사용자 설계) — 통계탭 값 판넬 */
    [STR_PANEL_STATS_PEAK]            = { "최대/최소",                  "Peak" },
    [STR_STATS_PEAK_ROW_FMT]          = { "%s: 최대 %d.%02d%s / 최소 %d.%02d%s",
                                           "%s: max %d.%02d%s / min %d.%02d%s" },
    [STR_STATS_PEAK_NO_DATA]          = { "데이터 없음",                "No data" },
    [STR_STATS_TABLE_HEADER_ITEM]     = { "항목",                       "Item" },
    [STR_STATS_TABLE_HEADER_VALUE]    = { "값",                         "Value" },
    [STR_STATS_TABLE_HEADER_TIME]     = { "시간",                       "Time" },
    [STR_STATS_TABLE_EMPTY]           = { "저장된 값 없음",              "No data yet" },
    [STR_STATS_PAGE_FMT]              = { "%lu / %lu",                  "%lu / %lu" },
    /* 2026-09-08(사용자 지시 — "Prev10 -> '<< 10', <Prev -> '<', Next> -> '>', Next10 ->
     * '10 >>'") — 기호는 언어 무관이라 ko/en 동일 */
    [STR_BTN_PREV_PAGE]               = { " < ",                    " < " },
    [STR_BTN_NEXT_PAGE]               = { " > ",                    " > " },
    /* 2026-09-07(통계탭 레이아웃 재설계, 사용자 설계) */
    /* 2026-09-07 재수정 — X/N/A를 매 줄마다 반복하면 이산화탄소처럼 긴 값이 줄바꿈되던 문제가
     * 있어서(사용자 실기 확인: "흰색 밑줄"), 범례를 제목 옆에 한 번만 두고 각 줄은 숫자만 */
    [STR_PANEL_STATS_OVERVIEW]        = { "개괄 (최대 / 최소 / 평균)",   "Overview (Max / Min / Avg)" },
    [STR_STATS_SCALE_OPTIONS]         = { "1시간\n12시간\n1일\n3일\n1주",
                                           "1h\n12h\n1d\n3d\n1w" },
    [STR_STATS_OVERVIEW_ROW_FMT]      = { "%s[%s]: %d.%02d / %d.%02d / %d.%02d",
                                           "%s[%s]: %d.%02d / %d.%02d / %d.%02d" },
    [STR_STATS_OVERVIEW_NO_DATA]      = { "데이터 없음",                "No data" },
    [STR_BTN_JUMP_PREV10]             = { "<< 10",                    "<< 10" },
    [STR_BTN_JUMP_NEXT10]             = { "10 >>",                    "10 >>" },
    [STR_BTN_DELETE_STATS]            = { "전체 삭제",                   "Delete All" },
    [STR_CONFIRM_DELETE_STATS]        = { "저장된 통계값을 전부 삭제할까요?\n되돌릴 수 없습니다.",
                                           "Delete all saved statistics?\nThis cannot be undone." },
    [STR_LABEL_GRAPH_PLACEHOLDER]     = { "그래프 (준비 중)",           "Graph (coming soon)" },
    [STR_BTN_DISCONNECT]              = { "연결끊기",                   "Disconnect" },
    [STR_LABEL_ALIAS]                 = { "별칭",                       "Alias" },
    [STR_LABEL_CONNECTED]             = { "연결됨",                     "Connected" },
    [STR_LABEL_PENDING]               = { "대기중",                     "Pending" },
    [STR_LABEL_AUTO_CONNECT_KNOWN]    = { "이전 연결 장치 자동연결",     "Auto-connect known devices" },
    [STR_LABEL_AUTO_CONNECT_NEW]      = { "신규 접속 장치 자동연결",     "Auto-connect incoming devices" },
    [STR_LABEL_MEASURE_SHORT]         = { "측정",                       "Measure" },
    [STR_ERR_DESC_CACHE_TOO_BIG]         = { "사진이 캐시 용량보다 큼",         "Photo bigger than cache" },
    [STR_ERR_DESC_CACHE_NO_BUF]          = { "캐시 슬롯 버퍼 없음",             "Cache slot buffer missing" },
    [STR_ERR_DESC_RECV_BUF_ALLOC]        = { "수신 버퍼 할당 실패",             "Receive buffer alloc failed" },
    [STR_ERR_DESC_CACHE_SLOT_ALLOC]      = { "캐시 슬롯 할당 실패",             "Cache slot alloc failed" },
    [STR_ERR_DESC_PANEL_BUF_ALLOC]       = { "판넬 버퍼 할당 실패",             "Panel buffer alloc failed" },
    [STR_ERR_DESC_STA_CRED_ALLOC]        = { "STA 자격증명 할당 실패",          "STA credential alloc failed" },
    [STR_ERR_DESC_SEND_PHOTO_REQ]        = { "사진 요청 전송 실패",             "Photo request send failed" },
    [STR_ERR_DESC_SEND_CAPTURE_REQ]      = { "지금촬영 요청 전송 실패",         "Capture request send failed" },
    [STR_ERR_DESC_SEND_LIST_REQ]         = { "목록 요청 전송 실패",             "List request send failed" },
    [STR_ERR_DESC_SEND_DELETE_REQ]       = { "삭제 요청 전송 실패",             "Delete request send failed" },
    [STR_ERR_DESC_SEND_DELETE_ALL_REQ]   = { "전체삭제 요청 전송 실패",         "Delete-all request send failed" },
    [STR_ERR_DESC_REQUEST_BUSY]          = { "요청 무시됨(이미 수신중)",        "Request ignored (busy)" },
    [STR_ERR_DESC_NOT_PAIRED]            = { "페어링 끊김 — 재연결 시도",       "Not paired - reconnecting" },
    [STR_ERR_DESC_TX_QUEUE_FULL]         = { "전송 큐 가득 — 요청 버려짐",       "TX queue full - request dropped" },
    [STR_ERR_DESC_META_TOO_BIG]          = { "사진이 수신 버퍼보다 큼",         "Photo bigger than recv buffer" },
    [STR_ERR_DESC_CHUNK_MISSING]         = { "청크 누락",                     "Chunk missing" },
    [STR_ERR_DESC_CRC_MISMATCH]          = { "CRC 불일치",                    "CRC mismatch" },
    [STR_ERR_DESC_DECODE_FAIL]           = { "JPEG 디코드 실패",               "JPEG decode failed" },
    [STR_ERR_DESC_LIST_COUNT_MISMATCH]   = { "목록 항목 유실",                 "List items lost" },
    [STR_ERR_DESC_FETCH_NORESPONSE]      = { "사진 가져오기 무응답",           "Photo fetch no response" },
    [STR_ERR_DESC_LIST_NORESPONSE]       = { "목록 갱신 무응답",               "List refresh no response" },
    [STR_ERR_DESC_PHOTO_SELECTION_STALE] = { "선택-도착 불일치(낡은 응답)",     "Selection/arrival mismatch (stale)" },
    [STR_ERR_DESC_DELETE_FAILED]         = { "사진 삭제 실패",                 "Photo delete failed" },
    [STR_ERR_DESC_DELETE_ALL_FAILED]     = { "전체삭제 실패",                 "Delete-all failed" },
    [STR_ERR_DESC_CAPTURE_FAILED]        = { "촬영 실패",                     "Capture failed" },
    [STR_ERR_DESC_CAPTURE_NORESPONSE]    = { "지금촬영 무응답",               "Capture no response" },
    [STR_ERR_DESC_CONFIG_NORESPONSE]     = { "설정 적용 무응답",               "Config apply no response" },
    [STR_ERR_DESC_DELETE_ALL_NORESPONSE] = { "전체삭제 접수 무응답(통신 끊김)", "Delete-all receipt no response" },
    [STR_ERR_DESC_DELETE_ALL_STOPPED]    = { "전체삭제 중단됨(완료 무응답)",   "Delete-all stopped (no completion)" },
    [STR_ERR_DESC_SET_TIME_NORESPONSE]   = { "시각동기화 무응답",             "Time sync no response" },
    [STR_ERR_DESC_FONT_FILE_MISSING]     = { "폰트 파일 없음",                 "Font file missing" },
    [STR_ERR_DESC_FONT_BUF_ALLOC]        = { "폰트 버퍼 할당 실패",             "Font buffer alloc failed" },
    [STR_ERR_DESC_FONT_FILE_OPEN]        = { "폰트 파일 열기 실패",             "Font file open failed" },
    [STR_ERR_DESC_FONT_CREATE]           = { "폰트 생성 실패",                 "Font creation failed" },
    [STR_ERR_DESC_HTTPD_START]           = { "웹서버 시작 실패",               "Web server start failed" },
    [STR_ERR_DESC_RTC_SET_FAILED]        = { "RTC 시각설정 실패",             "RTC time set failed" },
    [STR_ERR_DESC_CONFIG_FILE_MISMATCH]  = { "설정 파일 형식 불일치(기본값)",   "Config file mismatch (defaults)" },
    [STR_ERR_DESC_SD_MOUNT_FAILED]       = { "SD카드 마운트 실패",             "SD card mount failed" },
    [STR_ERR_DESC_UNKNOWN]               = { "알 수 없는 에러",               "Unknown error" },
};

void ui_lang_load(void)
{
    /* 2026-09-07(임시 실험) — 저장된 값(과거 KO 선택)이 EN 강제를 덮어쓰지 않도록 건너뜀.
     * 원복: 아래 주석 해제 */
    return;
#if 0
    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (!f) return;  /* 파일 없음 — 첫 부팅, 기본값(UI_LANG_KO) 유지 */
    uint8_t val = 0;
    if (fread(&val, 1, 1, f) == 1 && val < UI_LANG_COUNT) {
        s_lang = (ui_lang_t)val;
    }
    fclose(f);
#endif
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
