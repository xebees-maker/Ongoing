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
    STR_TAB_DASHBOARD = 0,
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
    STR_BTN_YES,
    STR_PANEL_SUMMARY,
    STR_PANEL_NO_SENSOR,
    STR_PANEL_NO_CAMERA,
    STR_PANEL_NO_PAIRED_DEVICE,
    STR_PANEL_SENSOR_TODO,
    STR_PANEL_NO_PHOTO_YET,
    STR_STATUS_OK,
    STR_BTN_CAPTURE_NOW,
    STR_BTN_RENEW_LIST,
    STR_PANEL_LIST,
    STR_PANEL_PICTURE,
    STR_BTN_CLOSE,
    STR_MSG_DELETE_PHOTO_CONFIRM,
    STR_CAPTURE_STAGE1_PROGRESS,
    STR_CAPTURE_STAGE1_DONE,
    STR_CAPTURE_STAGE2_SUCCESS,
    STR_CAPTURE_STAGE2_FAILED,
    STR_CAPTURE_STAGE3_PROGRESS,
    STR_CAPTURE_STAGE3_DONE,
    STR_CAPTURE_STAGE3_FAILED,
    STR_CAPTURE_STAGE4_PROGRESS,
    STR_CAPTURE_STAGE4_DONE,
    STR_COUNT,
} ui_str_id_t;

/* NVS("cntl_cfg" 네임스페이스)에서 저장된 언어값을 불러옴 — app_main()에서 UI 생성 전에
 * 한 번 호출. 저장된 값이 없으면 기본값(UI_LANG_KO) 유지. */
void        ui_lang_load(void);

/* 현재 언어를 바꾸고 즉시 NVS에 저장 — 다른 영구 저장할 설정값도 이 파일에 같은
 * 패턴(load/set 쌍)으로 추가하면 됨 */
void        ui_lang_set(ui_lang_t lang);
ui_lang_t   ui_lang_get(void);
const char *ui_str(ui_str_id_t id);

#ifdef __cplusplus
}
#endif
