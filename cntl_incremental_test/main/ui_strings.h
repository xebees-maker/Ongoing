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
    STR_COUNT,
} ui_str_id_t;

void        ui_lang_set(ui_lang_t lang);
ui_lang_t   ui_lang_get(void);
const char *ui_str(ui_str_id_t id);

#ifdef __cplusplus
}
#endif
