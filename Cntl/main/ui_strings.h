#pragma once

/**
 * 문자열 테이블 — ID로 문자열을 찾고, 현재 언어(한글/영문) 설정에 맞는 값을 반환.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_LANG_KO = 0,
    UI_LANG_EN,
    UI_LANG_COUNT,
} ui_lang_t;

typedef enum {
    STR_LOGO_TITLE = 0,
    STR_TAB_DASHBOARD,
    STR_TAB_STATISTICS,
    STR_TAB_OPTION,
    STR_GROUP_CNTL,
    STR_GROUP_SENSOR,
    STR_GROUP_CAMERA,
    STR_GROUP_SYSTEM,
    STR_LABEL_LANGUAGE,
    STR_STATUS_CONNECTING,
    STR_STATUS_CONNECTED,
    STR_MSG_PAIR_CONFIRM,
    STR_MSG_UNPAIR_CONFIRM,
    STR_BTN_CONFIRM,
    STR_BTN_CANCEL,
    STR_STATUS_CANCEL_PENDING,
    STR_BTN_YES,
    STR_PANEL_SUMMARY,
    STR_PANEL_NO_SENSOR,
    STR_PANEL_NO_CAMERA,
    STR_PANEL_NO_PAIRED_DEVICE,
    STR_PANEL_SENSOR_TODO,
    STR_PANEL_NO_PHOTO_YET,
    STR_STATUS_OK,
    /* connectionless 모델(2026-08-10) — 요약판넬 상태문구, PAIRED(페어됨)/ACTIVE(통신 중) */
    STR_STATUS_PAIRED,
    STR_STATUS_ACTIVE,
    STR_BTN_CAPTURE_NOW,
    STR_BTN_RENEW_LIST,
    STR_PANEL_LIST,
    STR_PANEL_PICTURE,
    STR_MSG_DELETE_PHOTO_CONFIRM,
    STR_CAPTURE_STAGE1_PROGRESS,
    STR_CAPTURE_STAGE1_DONE,
    STR_CAPTURE_STAGE2_INIT_NEEDED,  /* 2026-08-21 핸드셰이크 재설계 — 카메라 초기화 필요할
                                         때만 순서대로 표시(불필요하면 건너뜀) */
    STR_CAPTURE_STAGE2_INIT_DONE,
    STR_CAPTURE_STAGE2_CAPTURING,
    STR_CAPTURE_STAGE2_SUCCESS,
    STR_CAPTURE_STAGE2_FAILED,
    STR_CAPTURE_STAGE2_NORESPONSE,
    STR_CAPTURE_STAGE3_PROGRESS,
    STR_CAPTURE_STAGE3_DONE,
    STR_CAPTURE_STAGE3_UNKNOWN,
    STR_FETCH_CONNECTING,
    STR_FETCH_PROGRESS_FMT,
    STR_FETCH_PROGRESS_ETA_FMT,
    STR_FETCH_DONE,
    STR_FETCH_FAILED,
    STR_FETCH_STALLED,
    STR_BTN_DELETE_ALL,
    STR_MSG_DELETE_ALL_CONFIRM,
    STR_DELETEALL_STAGE1_PROGRESS,
    STR_DELETEALL_STAGE1_DELETING_FMT,  /* 2026-08-21 — 접수 확인(개수 통보) 후 실제 삭제
                                            대기 단계, 개수 표시 */
    STR_DELETEALL_STAGE1_DONE,
    STR_DELETEALL_STAGE1_FAILED,
    STR_DELETEALL_STAGE1_NORESPONSE,
    STR_DELETEALL_STAGE1_STOPPED_FMT,   /* 2026-08-21 — 접수는 됐지만 완료 응답 없음 */
    STR_DELETEALL_STAGE2_PROGRESS,
    STR_DELETEALL_STAGE2_DONE,
    STR_DELETEALL_STAGE2_UNKNOWN,
    /* 목록가져오기 진행팝업 2단계(2026-08-11 재설계 — 배치화 이후 전체가 수백ms 안에 끝나서
     * 단계 표시가 있어야 사람 눈에 실제로 뭐가 되고 있는지 보임, 사용자 지시) */
    STR_LIST_STAGE1_PROGRESS,
    STR_LIST_STAGE1_DONE,
    STR_LIST_STAGE1_NORESPONSE,
    STR_LIST_STAGE2_PROGRESS,
    STR_LIST_STAGE2_SUCCESS,
    STR_LIST_STAGE2_MISMATCH_FMT,
    STR_LIST_STAGE2_STALLED_FMT,
    STR_LIST_EMPTY,
    STR_LABEL_CAPTURE_INTERVAL,
    STR_LABEL_RESPONSE_INTERVAL,
    STR_LABEL_AGC,  /* 2026-08-21 — 세로줄 노이즈 진단용 */
    STR_LABEL_AEC,
    STR_LABEL_XCLK,
    STR_OPT_XCLK_LIST,
    STR_BTN_APPLY,
    STR_CONFIG_APPLY_PROGRESS,
    STR_CONFIG_APPLY_STALLED,
    STR_TITLE_ERROR_LIST,
    STR_ERROR_LIST_EMPTY,
    STR_TITLE_WEB_QR,
    STR_MSG_WEB_QR_NO_IP,
    STR_LABEL_RESTART_DEVICE,
    STR_BTN_RESTART,
    STR_MSG_RESTART_CONFIRM,
    STR_OPT_CAPTURE_INTERVAL_LIST,
    STR_OPT_RESPONSE_INTERVAL_LIST,
    STR_LABEL_TIME,
    STR_BTN_SET_TIME,
    STR_TITLE_SET_TIME,
    STR_PANEL_DEEPSLEEP,
    STR_DEEPSLEEP_LINE_FMT,
    STR_WAKE_REASON_TIMER,
    STR_WAKE_REASON_RWDT,
    STR_WAKE_REASON_POWERON,
    STR_WAKE_REASON_OTHER,
    /* 응답성 드롭다운 도움말(2026-08-10) — s_response_interval_values 순서와 반드시 같이
     * 맞출 것(ui_main.c) */
    STR_RESPONSE_HELP_0,  /* 1초=즉시 */
    STR_RESPONSE_HELP_1,  /* 3초=빠름 */
    STR_RESPONSE_HELP_2,  /* 10초=균형 */
    STR_RESPONSE_HELP_3,  /* 30초=절전 */
    STR_RESPONSE_HELP_4,  /* 30분=최대절전 */
    /* 적응형 반응시간(2026-08-10) — 마지막 사용자 조작 후 이만큼 조용해야 CAM에 SLEEP_NOW.
     * s_adaptive_response_values 순서와 반드시 같이 맞출 것(ui_main.c) */
    STR_LABEL_ADAPTIVE_RESPONSE,
    STR_OPT_ADAPTIVE_RESPONSE_LIST,
    STR_HELP_ADAPTIVE_RESPONSE,
    /* 절전상태 판넬 일시멈춤 단추(2026-08-10) */
    STR_BTN_PAUSE,
    STR_BTN_RESUME,
    /* 일반 로그 판넬 제목(2026-08-11) — 스크롤 안 되는 고정 타이틀 행(페이지 스크롤을
     * 잡기 위한 영역, ui_main.c) */
    STR_PANEL_GENERAL_LOG,
    /* 요약판넬 1~2번째줄(2026-08-21, 사용자 지시) — 웹 대시보드 URL / 여유메모리 상시표시 */
    STR_LABEL_WEB,
    STR_LABEL_MEMORY,
    STR_LABEL_BATTERY,
    /* 네트워크 설정 행(2026-08-29) — 독립(AP)/종속(STA) 선택 + 우측 상태(IP 또는 SSID/찾기) */
    STR_LABEL_NETWORK,
    STR_NETWORK_MODE_AP,
    STR_NETWORK_MODE_STA,
    STR_BTN_FIND,
    STR_MSG_NETWORK_MODE_RESTART_CONFIRM,
    STR_TITLE_WIFI_SCAN,
    STR_MSG_WIFI_SCANNING,
    STR_MSG_WIFI_SCAN_EMPTY,
    STR_LABEL_WIFI_PASSWORD,
    STR_BTN_CONNECT,
    STR_MSG_WIFI_CONNECTING,
    STR_MSG_WIFI_CONNECT_FAILED,
    STR_MSG_WIFI_STAGE_DISCONNECTING,
    STR_MSG_WIFI_STAGE_AUTHENTICATING,
    STR_STATUS_NOT_CONNECTED,
    STR_STATUS_NO_AP,
    STR_TAG_WIFI_CONNECTED,
    STR_BTN_RESCAN,
    STR_BTN_CLOSE,
    STR_MSG_WIFI_CREDENTIALS_RESTART_CONFIRM,
    STR_BTN_SHOW,
    STR_BTN_HIDE,
    /* 2026-09-04(사용자 설계: 웹 응답도 ui_str() 문구를 그대로 실어보냄, JS가 따로 문구를
     * 갖지 않게) — 연결/끊기/목록가져오기의 웹 완료문구. 연결성공은 STR_STATUS_PAIRED,
     * 사진 성공/실패는 STR_FETCH_DONE/STR_FETCH_FAILED를 그대로 재사용(이미 깔끔한 문구) */
    STR_CONNECT_FAILED,
    STR_DISCONNECT_SUCCESS,
    STR_LIST_FETCH_SUCCESS,
    STR_LIST_FETCH_FAILED,
    STR_COUNT,
} ui_str_id_t;

/* /assets/settings.bin에서 저장된 언어값을 불러옴 — app_main()에서 fs_init() 이후,
 * UI 생성 전에 한 번 호출. 파일이 없으면 기본값(UI_LANG_KO) 유지. */
void        ui_lang_load(void);

/* 현재 언어를 바꾸고 즉시 /assets/settings.bin에 저장 — 다른 영구 저장할 설정값도
 * 이 파일에 같은 패턴(load/set 쌍)으로 추가하면 됨 */
void        ui_lang_set(ui_lang_t lang);
ui_lang_t   ui_lang_get(void);
const char *ui_str(ui_str_id_t id);

#ifdef __cplusplus
}
#endif
