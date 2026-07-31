#include "ui_main.h"
#include "ui_strings.h"
#include "ui_font.h"
#include "lvgl.h"

/* 벤더 lv_demo_widgets 내부 함수/전역 — "설정" 탭 말고는 고치기 전(로고 + Profile/Analytics
 * 위젯 콘텐츠 그대로) 상태를 활용하기로 함(2026-07-31 사용자 지시). 공개 헤더(lv_demos.h)엔
 * 없고 데모 내부 전용 헤더에만 선언돼 있어서 직접 extern 선언해서 씀. */
extern void lv_demo_widgets_components_init(void);
extern void lv_demo_widgets_profile_create(lv_obj_t *parent);
extern void lv_demo_widgets_analytics_create(lv_obj_t *parent);
extern lv_obj_t *lv_demo_widgets_title_create(lv_obj_t *parent, const char *text);
extern lv_style_t style_text_muted;

static lv_obj_t *s_page_control = NULL;
static lv_obj_t *s_lang_label = NULL;
static lv_obj_t *s_btn_ko = NULL;
static lv_obj_t *s_btn_en = NULL;

/* 그룹박스 — 제목이 있는 판넬(내용 없어도 제목은 항상 있음) */
static lv_obj_t *s_group_title[STR_GROUP_SYSTEM - STR_GROUP_CNTL + 1];

#define COLOR_LANG_SELECTED   lv_palette_main(LV_PALETTE_BLUE)
#define COLOR_LANG_UNSELECTED lv_palette_main(LV_PALETTE_GREY)

static void update_lang_buttons(void)
{
    ui_lang_t lang = ui_lang_get();
    lv_obj_set_style_bg_color(s_btn_ko, lang == UI_LANG_KO ? COLOR_LANG_SELECTED : COLOR_LANG_UNSELECTED, 0);
    lv_obj_set_style_bg_color(s_btn_en, lang == UI_LANG_EN ? COLOR_LANG_SELECTED : COLOR_LANG_UNSELECTED, 0);
}

static void refresh_lang_texts(void)
{
    lv_tabview_set_tab_text(s_page_control, 0, ui_str(STR_TAB_DASHBOARD));
    lv_tabview_set_tab_text(s_page_control, 1, ui_str(STR_TAB_STATISTICS));
    lv_tabview_set_tab_text(s_page_control, 2, ui_str(STR_TAB_OPTION));

    for (ui_str_id_t id = STR_GROUP_CNTL; id <= STR_GROUP_SYSTEM; id++) {
        lv_label_set_text(s_group_title[id - STR_GROUP_CNTL], ui_str(id));
    }

    lv_label_set_text(s_lang_label, ui_str(STR_LABEL_LANGUAGE));
    update_lang_buttons();
}

static void cb_lang_ko(lv_event_t *e)
{
    (void)e;
    ui_lang_set(UI_LANG_KO);
    refresh_lang_texts();
}

static void cb_lang_en(lv_event_t *e)
{
    (void)e;
    ui_lang_set(UI_LANG_EN);
    refresh_lang_texts();
}

/* 그룹박스 하나 생성 — 제목 라벨 + 내용 넣을 판넬을 반환(내용은 호출부가 채움) */
static lv_obj_t *create_group_box(lv_obj_t *parent, ui_str_id_t title_id)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, ui_str(title_id));
    lv_obj_set_style_text_font(title, ui_font_get(UI_FONT_SIZE_18), 0);
    s_group_title[title_id - STR_GROUP_CNTL] = title;

    lv_obj_t *content = lv_obj_create(box);
    lv_obj_set_size(content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);

    return content;
}

/* 페이지콘트롤 — 페이지탭 3개(상황판/통계/설정). 로고 + 상황판/통계 탭 내용(원래 데모의
 * Profile/Analytics 위젯)은 고치기 전 상태 그대로 활용 — 설정 탭만 새로 만든 그룹박스로 교체 */
void ui_init(void)
{
    lv_demo_widgets_components_init();  /* profile/analytics가 쓰는 공용 스타일/폰트 초기화 */

    s_page_control = lv_tabview_create(lv_screen_active());
    lv_tabview_set_tab_bar_size(s_page_control, 75);  /* 800px 화면은 항상 DISP_LARGE 크기 */

    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(s_page_control);
    /* 페이지탭 라벨(한글)엔 기본 LVGL 폰트(Montserrat)에 한글 글리프가 없어서
     * 로드해둔 나눔고딕(18pt, 이미 로드된 걸 재사용)을 tab bar에 적용 — text_font는
     * 상속되는 스타일이라 탭 버튼 라벨들에 그대로 내려감 */
    lv_obj_set_style_text_font(tab_bar, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 로고아이콘 + 로고 + 로고부제 — tab bar 왼쪽 절반에 절대 위치, 페이지탭 버튼들은
     * 오른쪽 절반으로 밀어냄(원래 데모 레이아웃 그대로) */
    lv_obj_set_style_pad_left(tab_bar, LV_HOR_RES / 2, 0);
    lv_obj_t *logo_icon = lv_image_create(tab_bar);
    lv_obj_add_flag(logo_icon, LV_OBJ_FLAG_IGNORE_LAYOUT);
    LV_IMAGE_DECLARE(img_lvgl_logo);
    lv_image_set_src(logo_icon, &img_lvgl_logo);
    lv_obj_align(logo_icon, LV_ALIGN_LEFT_MID, -LV_HOR_RES / 2 + 25, 0);

    lv_obj_t *logo = lv_demo_widgets_title_create(tab_bar, "");
    lv_obj_add_flag(logo, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_label_set_text(logo, "플렉스팜");
    lv_obj_set_style_text_font(logo, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_align_to(logo, logo_icon, LV_ALIGN_OUT_RIGHT_TOP, 10, 0);

    lv_obj_t *logo_subtitle = lv_label_create(tab_bar);
    lv_label_set_text_static(logo_subtitle, "Widgets demo");
    lv_obj_add_flag(logo_subtitle, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_style(logo_subtitle, &style_text_muted, 0);
    lv_obj_align_to(logo_subtitle, logo_icon, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, 0);

    lv_obj_t *dashboard_page = lv_tabview_add_tab(s_page_control, ui_str(STR_TAB_DASHBOARD));
    lv_demo_widgets_profile_create(dashboard_page);

    lv_obj_t *stats_page = lv_tabview_add_tab(s_page_control, ui_str(STR_TAB_STATISTICS));
    lv_demo_widgets_analytics_create(stats_page);

    lv_obj_t *option_page = lv_tabview_add_tab(s_page_control, ui_str(STR_TAB_OPTION));
    lv_obj_set_flex_flow(option_page, LV_FLEX_FLOW_COLUMN);

    /* 그룹박스 4개(세로로 나열): 제어기/측정기/영상/시스템 — 내용은 아직 시스템(언어
     * 전환)만 채움, 나머지는 제목만 있는 빈 틀. 언어 행: 라벨 왼쪽 정렬 + 선택
     * 버튼(한글/English) 오른쪽 정렬, 선택된 버튼은 다른 색으로 표시 */
    lv_obj_t *cntl_content = create_group_box(option_page, STR_GROUP_CNTL);
    lv_obj_set_flex_align(cntl_content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_lang_label = lv_label_create(cntl_content);
    lv_label_set_text(s_lang_label, ui_str(STR_LABEL_LANGUAGE));
    lv_obj_set_style_text_font(s_lang_label, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_group = lv_obj_create(cntl_content);
    lv_obj_set_size(btn_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(btn_group, 0, 0);
    lv_obj_set_style_border_width(btn_group, 0, 0);

    s_btn_ko = lv_button_create(btn_group);
    lv_obj_add_event_cb(s_btn_ko, cb_lang_ko, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ko = lv_label_create(s_btn_ko);
    lv_label_set_text(lbl_ko, "한글");
    lv_obj_set_style_text_font(lbl_ko, ui_font_get(UI_FONT_SIZE_18), 0);

    s_btn_en = lv_button_create(btn_group);
    lv_obj_add_event_cb(s_btn_en, cb_lang_en, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_en = lv_label_create(s_btn_en);
    lv_label_set_text(lbl_en, "English");

    update_lang_buttons();  /* 초기 선택 상태(기본 UI_LANG_KO) 색상 반영 */

    create_group_box(option_page, STR_GROUP_SENSOR);
    create_group_box(option_page, STR_GROUP_CAMERA);
    create_group_box(option_page, STR_GROUP_SYSTEM);
}
