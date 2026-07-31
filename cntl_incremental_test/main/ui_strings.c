#include "ui_strings.h"

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
};

void ui_lang_set(ui_lang_t lang)
{
    if (lang < UI_LANG_COUNT) s_lang = lang;
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
