#include "ui_main.h"
#include "ui_strings.h"
#include "ui_font.h"
#include "esp_now_hub.h"
#include "esp_now_photo.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* 벤더 lv_demo_widgets 내부 함수/전역 — "설정" 탭 말고는 고치기 전(로고 + Profile/Analytics
 * 위젯 콘텐츠 그대로) 상태를 활용하기로 함(2026-07-31 사용자 지시). 공개 헤더(lv_demos.h)엔
 * 없고 데모 내부 전용 헤더에만 선언돼 있어서 직접 extern 선언해서 씀. */
extern void lv_demo_widgets_components_init(void);
extern void lv_demo_widgets_analytics_create(lv_obj_t *parent);
extern lv_obj_t *lv_demo_widgets_title_create(lv_obj_t *parent, const char *text);
extern lv_style_t style_text_muted;

static lv_obj_t *s_page_control = NULL;
static lv_obj_t *s_clock_label = NULL;  /* 로고부제 자리 — "Widgets demo" 대신 실시간 시계(hh:mm:ss) */
static lv_obj_t *s_lang_label = NULL;
static lv_obj_t *s_btn_ko = NULL;
static lv_obj_t *s_btn_en = NULL;

/* 그룹박스 — 제목이 있는 판넬(내용 없어도 제목은 항상 있음) */
static lv_obj_t *s_group_title[STR_GROUP_SYSTEM - STR_GROUP_CNTL + 1];

/* 상황판 판넬 3개(요약/측정기/카메라) — refresh_lang_texts()가 참조하므로 그 정의보다
 * 먼저 선언돼야 함(파일 스코프 static은 선언 지점 이후부터만 참조 가능) */
static lv_obj_t          *s_dash_title[3];  /* 0=요약, 1=측정기, 2=카메라 */
static lv_obj_t          *s_summary_list        = NULL;
static lv_obj_t          *s_summary_empty       = NULL;
static lv_obj_t          *s_sensor_empty        = NULL;
static lv_obj_t          *s_sensor_todo         = NULL;
static lv_obj_t          *s_camera_empty        = NULL;
static lv_obj_t          *s_camera_content      = NULL;  /* 카메라 판넬 툴바 — 아래 split_row와 함께 토글 */
static lv_obj_t          *s_camera_split_row    = NULL;
static lv_obj_t          *s_camera_photo_label  = NULL;
static lv_obj_t          *s_camera_capture_lbl  = NULL;
static lv_obj_t          *s_camera_renew_lbl    = NULL;
static lv_obj_t          *s_list_title          = NULL;
static lv_obj_t          *s_picture_title       = NULL;
static lv_obj_t          *s_photo_list          = NULL;  /* 목록 판넬 — 설정탭 CAM 발견 리스트(s_camera_list)와는 다른 위젯 */
static lv_obj_t          *s_photo_box           = NULL;  /* 사진 판넬 — 사진 오면 이 안의 라벨을 lv_image로 교체 */
static lv_obj_t          *s_photo_image         = NULL;  /* s_photo_box 안의 lv_image(사진 오기 전엔 NULL) */
static esp_now_hub_node_t s_dash_nodes[ESP_NOW_HUB_MAX_NODES];
static esp_now_hub_node_t s_dash_nodes_prev[ESP_NOW_HUB_MAX_NODES];
static int                s_dash_count_prev = -1;  /* -1: 아직 비교 대상 없음(첫 실행은 항상 그림) */
static uint8_t            s_paired_cam_mac[6];
static bool               s_has_paired_cam = false;  /* 지금촬영 버튼이 쏠 대상 */

/* 마지막으로 받은 사진 — JPEG 원본 바이트를 그대로 들고 있다가 lv_image 소스로 씀
 * (esp_now_photo_consume()이 소유권을 넘겨줌, 여기서 heap_caps_free로 해제할 책임도 짐).
 * 대시보드 썸네일과 전체화면 뷰어가 같은 dsc를 공유해서 그림 */
static uint8_t          *s_photo_jpeg_buf = NULL;
static lv_image_dsc_t    s_photo_dsc;

/* 지금촬영 진행 팝업 — 1.명령전달/2.촬영결과/3.영상수신/4.목록갱신 4줄 */
static lv_obj_t   *s_capture_popup_overlay = NULL;
static lv_obj_t   *s_capture_stage_label[4];
static lv_timer_t *s_capture_popup_timer = NULL;

/* CAM SD카드 사진 목록(내용 없이 file_id+크기만) — 탭하면 그 사진을 fetch_by_id로 받아서
 * 플레이스홀더에 표시, 삭제 버튼은 확인 팝업 거쳐서 삭제 */
static esp_now_photo_list_item_t s_current_list[ESP_NOW_PHOTO_LIST_MAX];
static int                        s_current_list_count = 0;
static lv_obj_t                  *s_selected_row = NULL;

/* 사진 전체화면 뷰어 — 더블탭 확대/축소 토글, 확대 상태에서 드래그 패닝 */
#define PHOTO_VIEWER_ZOOM_IN        512  /* 2배 */
#define PHOTO_VIEWER_ZOOM_OUT       256  /* 1배(원본) */
#define PHOTO_VIEWER_DOUBLE_TAP_MS  400
static lv_obj_t  *s_viewer_image_area = NULL;
static bool       s_viewer_zoomed = false;
static uint32_t   s_viewer_last_tap_ms = 0;
static lv_point_t s_viewer_pan_start;
static lv_point_t s_viewer_offset;
static lv_point_t s_viewer_offset_start;

/* 설정탭 카메라 리스트/상황판 1초 갱신 타이머 — 팝업/뷰어가 떠 있는 동안은 일시정지
 * (모달 위에서 터치하는 도중에 뒤에서 리스트를 지우고 다시 그리면 터치 처리와 간섭해서
 * 반응이 느려지거나 아예 안 먹는 문제가 있었음, 실기로 확인) */
static lv_timer_t *s_camera_list_timer = NULL;
static lv_timer_t *s_dashboard_timer   = NULL;

static void pause_bg_timers(void)
{
    lv_timer_pause(s_camera_list_timer);
    lv_timer_pause(s_dashboard_timer);
}

static void resume_bg_timers(void)
{
    lv_timer_resume(s_camera_list_timer);
    lv_timer_resume(s_dashboard_timer);
}

static void set_checked(lv_obj_t *cb, bool checked)
{
    if (checked) lv_obj_add_state(cb, LV_STATE_CHECKED);
    else         lv_obj_remove_state(cb, LV_STATE_CHECKED);
}

static void update_lang_buttons(void)
{
    ui_lang_t lang = ui_lang_get();
    set_checked(s_btn_ko, lang == UI_LANG_KO);
    set_checked(s_btn_en, lang == UI_LANG_EN);
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

    lv_label_set_text(s_dash_title[0], ui_str(STR_PANEL_SUMMARY));
    lv_label_set_text(s_dash_title[1], ui_str(STR_GROUP_SENSOR));
    lv_label_set_text(s_dash_title[2], ui_str(STR_GROUP_CAMERA));
    lv_label_set_text(s_summary_empty, ui_str(STR_PANEL_NO_PAIRED_DEVICE));
    lv_label_set_text(s_sensor_empty, ui_str(STR_PANEL_NO_SENSOR));
    lv_label_set_text(s_sensor_todo, ui_str(STR_PANEL_SENSOR_TODO));
    lv_label_set_text(s_camera_empty, ui_str(STR_PANEL_NO_CAMERA));
    /* 사진이 이미 도착해서 플레이스홀더 라벨이 지워졌으면(display_photo 참고) NULL —
     * 그 상태에서 그냥 호출하면 지워진 객체를 건드리게 됨 */
    if (s_camera_photo_label) lv_label_set_text(s_camera_photo_label, ui_str(STR_PANEL_NO_PHOTO_YET));
    lv_label_set_text(s_camera_capture_lbl, ui_str(STR_BTN_CAPTURE_NOW));
    lv_label_set_text(s_camera_renew_lbl, ui_str(STR_BTN_RENEW_LIST));
    lv_label_set_text(s_list_title, ui_str(STR_PANEL_LIST));
    lv_label_set_text(s_picture_title, ui_str(STR_PANEL_PICTURE));
}

/* 실제 반영(ui_lang_set — s_lang 갱신 + nvs 저장)이 끝난 뒤에만 라디오/라벨을 갱신한다
 * (refresh_lang_texts 안의 update_lang_buttons가 두 체크박스 상태를 최종 확정) — 클릭
 * 이벤트 처리 자체가 LVGL 싱글스레드 루프 안에서 끝까지 실행되고 나서야 다음 입력을
 * 받아들이므로, 이 함수가 도는 동안은 다른 입력도 자연히 막힌다(SendMessage와 동일한
 * 성격). 화면에 "선택됨"을 먼저 보여주고 나중에 실제로 반영하는 방식(PostMessage 성격)은
 * 반영이 실패해도 화면은 계속 성공한 것처럼 보일 수 있어서 쓰지 않기로 함.
 * 이미 선택된 언어를 다시 누른 경우엔 nvs 쓰기만 생략(불필요한 플래시 쓰기 방지) —
 * update_lang_buttons는 여전히 호출해서 LVGL이 클릭 시 자동으로 토글한 체크박스 상태를
 * 되돌린다. */
static void cb_lang_ko(lv_event_t *e)
{
    (void)e;
    if (ui_lang_get() != UI_LANG_KO) ui_lang_set(UI_LANG_KO);
    refresh_lang_texts();
}

static void cb_lang_en(lv_event_t *e)
{
    (void)e;
    if (ui_lang_get() != UI_LANG_EN) ui_lang_set(UI_LANG_EN);
    refresh_lang_texts();
}

/* ════════════════════════════════════════════════════════════
 * 공용 모달 팝업 — 화면 전체 반투명 오버레이 + 가운데 박스
 * ════════════════════════════════════════════════════════════ */
static void cb_modal_close(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    /* btn -> btn_row -> box -> overlay : 3단계 위로 올라가야 오버레이(반투명 배경)까지
     * 같이 지워짐 — box만 지우면 오버레이가 화면에 그대로 남음 */
    lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(btn)));
    lv_obj_delete(overlay);
    resume_bg_timers();
}

static lv_obj_t *create_modal(void)
{
    pause_bg_timers();
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);

    lv_obj_t *box = lv_obj_create(overlay);
    lv_obj_set_size(box, 420, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    return box;
}

static lv_obj_t *add_modal_button(lv_obj_t *btn_row, ui_str_id_t text_id, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(btn_row);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, ui_str(text_id));
    lv_obj_set_style_text_font(lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    return btn;
}

/* 팝업 하단 버튼 줄 — 오른쪽 정렬(모달 박스의 마지막 자식이라 세로로는 이미 맨 아래) */
static lv_obj_t *create_modal_btn_row(lv_obj_t *box)
{
    lv_obj_t *btn_row = lv_obj_create(box);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    return btn_row;
}

/* ════════════════════════════════════════════════════════════
 * 사진 전체화면 뷰어 — 더블탭 확대/축소, 확대 상태에서 드래그 패닝, 닫기
 * (진짜 두 손가락 핀치는 지금 안 함 — 싱글터치 더블탭+드래그로 충분하다고 확인받음)
 * ════════════════════════════════════════════════════════════ */
static void apply_viewer_transform(void)
{
    int32_t scale = s_viewer_zoomed ? PHOTO_VIEWER_ZOOM_IN : PHOTO_VIEWER_ZOOM_OUT;
    lv_obj_set_style_transform_scale_x(s_viewer_image_area, scale, 0);
    lv_obj_set_style_transform_scale_y(s_viewer_image_area, scale, 0);
    lv_obj_set_style_translate_x(s_viewer_image_area, s_viewer_offset.x, 0);
    lv_obj_set_style_translate_y(s_viewer_image_area, s_viewer_offset.y, 0);
}

static void cb_viewer_image_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();

    if (code == LV_EVENT_PRESSED) {
        if (indev) lv_indev_get_point(indev, &s_viewer_pan_start);
        s_viewer_offset_start = s_viewer_offset;
    } else if (code == LV_EVENT_PRESSING) {
        if (!s_viewer_zoomed || !indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        s_viewer_offset.x = s_viewer_offset_start.x + (p.x - s_viewer_pan_start.x);
        s_viewer_offset.y = s_viewer_offset_start.y + (p.y - s_viewer_pan_start.y);
        apply_viewer_transform();
    } else if (code == LV_EVENT_CLICKED) {
        uint32_t now = lv_tick_get();
        if (now - s_viewer_last_tap_ms < PHOTO_VIEWER_DOUBLE_TAP_MS) {
            /* 더블탭 — 확대/축소 토글, 축소로 돌아갈 땐 패닝 오프셋도 초기화 */
            s_viewer_zoomed = !s_viewer_zoomed;
            if (!s_viewer_zoomed) {
                s_viewer_offset.x = 0;
                s_viewer_offset.y = 0;
            }
            apply_viewer_transform();
            s_viewer_last_tap_ms = 0;  /* 3번째 탭이 다시 더블탭으로 안 잡히게 리셋 */
        } else {
            s_viewer_last_tap_ms = now;
        }
    }
}

static void cb_viewer_close(lv_event_t *e)
{
    lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_delete(overlay);
    s_viewer_image_area = NULL;
    resume_bg_timers();
}

static void show_photo_viewer(void)
{
    pause_bg_timers();
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);

    s_viewer_zoomed = false;
    s_viewer_offset.x = 0;
    s_viewer_offset.y = 0;

    /* 이미지 자리 — 확대/축소·패닝은 이 컨테이너 자체에 transform을 걸어서 처리하므로
     * 자식이 라벨(플레이스홀더)이든 lv_image(실제 사진)든 그대로 재사용 가능 */
    s_viewer_image_area = lv_obj_create(overlay);
    lv_obj_set_size(s_viewer_image_area, LV_PCT(90), LV_PCT(90));
    lv_obj_center(s_viewer_image_area);
    lv_obj_set_style_bg_color(s_viewer_image_area, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_radius(s_viewer_image_area, 8, 0);
    lv_obj_set_flex_flow(s_viewer_image_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_viewer_image_area, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_viewer_image_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_viewer_image_area, cb_viewer_image_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_viewer_image_area, cb_viewer_image_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_viewer_image_area, cb_viewer_image_event, LV_EVENT_CLICKED, NULL);
    apply_viewer_transform();

    if (s_photo_jpeg_buf) {
        lv_obj_t *img = lv_image_create(s_viewer_image_area);
        lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
        lv_image_set_src(img, &s_photo_dsc);
    } else {
        lv_obj_t *placeholder_lbl = lv_label_create(s_viewer_image_area);
        lv_label_set_text(placeholder_lbl, ui_str(STR_PANEL_NO_PHOTO_YET));
        lv_obj_set_style_text_font(placeholder_lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    }

    lv_obj_t *btn_close = lv_button_create(overlay);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_event_cb(btn_close, cb_viewer_close, LV_EVENT_CLICKED, overlay);
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, LV_SYMBOL_CLOSE);
}

static void cb_open_photo_viewer(lv_event_t *e)
{
    (void)e;
    show_photo_viewer();
}

/* ════════════════════════════════════════════════════════════
 * 장치 연결/설정 팝업 — 미연결(연결 허용 확인) / 연결됨(설정+연결해제)
 * ════════════════════════════════════════════════════════════ */
static void cb_pair_confirm(lv_event_t *e)
{
    esp_now_hub_node_t *node = (esp_now_hub_node_t *)lv_event_get_user_data(e);
    esp_now_hub_pair(node->mac);
    cb_modal_close(e);
}

static void cb_unpair_confirm(lv_event_t *e)
{
    esp_now_hub_node_t *node = (esp_now_hub_node_t *)lv_event_get_user_data(e);
    esp_now_hub_unpair(node->mac);
    cb_modal_close(e);
}

static void show_pair_confirm_popup(esp_now_hub_node_t *node)
{
    lv_obj_t *box = create_modal();

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text_fmt(msg, "%s\n%s", node->name, ui_str(STR_MSG_PAIR_CONFIRM));
    lv_obj_set_style_text_font(msg, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_row = create_modal_btn_row(box);
    add_modal_button(btn_row, STR_BTN_CONFIRM, cb_pair_confirm, node);
    add_modal_button(btn_row, STR_BTN_CANCEL, cb_modal_close, NULL);
}

/* 연결된 장치를 탭했을 때 — 지금은 장치별 설정 항목이 없어서 연결 해제 확인만 함
 * (설정 항목 생기면 여기 확장 예정, 예: CAM 화이트밸런스/촬영주기) */
static void show_unpair_confirm_popup(esp_now_hub_node_t *node)
{
    lv_obj_t *box = create_modal();

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text_fmt(msg, "%s\n%s", node->name, ui_str(STR_MSG_UNPAIR_CONFIRM));
    lv_obj_set_style_text_font(msg, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_row = create_modal_btn_row(box);
    add_modal_button(btn_row, STR_BTN_YES, cb_unpair_confirm, node);
    add_modal_button(btn_row, STR_BTN_CANCEL, cb_modal_close, NULL);
}

/* ════════════════════════════════════════════════════════════
 * 영상(Camera) 그룹박스 — 발견된 CAM 리스트(연결중/연결됨), 탭하면 위 팝업
 * ════════════════════════════════════════════════════════════ */
static lv_obj_t          *s_camera_list = NULL;
static esp_now_hub_node_t s_camera_nodes[ESP_NOW_HUB_MAX_NODES];
static esp_now_hub_node_t s_camera_nodes_prev[ESP_NOW_HUB_MAX_NODES];
static int                s_camera_count_prev = -1;  /* -1: 아직 비교 대상 없음(첫 실행은 항상 그림) */

/* 화면에 실제로 보이는 정보만 비교(last_seen_ms는 keepalive마다 바뀌지만 화면엔 안 나오므로
 * 제외) — 매초 리스트를 통째로 지우고 다시 그리던 게 터치 처리와 간섭해서 반응이 느려지거나
 * 안 먹는 문제(연결해제 팝업, 언어 라디오 버튼 모두)의 원인이었음, 바뀐 게 없으면 건너뜀 */
static bool node_display_equal(const esp_now_hub_node_t *a, const esp_now_hub_node_t *b)
{
    return memcmp(a->mac, b->mac, sizeof(a->mac)) == 0 &&
           a->kind == b->kind && a->paired == b->paired &&
           strcmp(a->name, b->name) == 0;
}

static void cb_camera_item_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    esp_now_hub_node_t *node = (esp_now_hub_node_t *)lv_obj_get_user_data(btn);
    if (!node) return;
    if (node->paired) show_unpair_confirm_popup(node);
    else              show_pair_confirm_popup(node);
}

static void refresh_camera_list(lv_timer_t *t)
{
    (void)t;
    int count = esp_now_hub_get_nodes(HUB_NODE_KIND_CAM, s_camera_nodes, ESP_NOW_HUB_MAX_NODES);

    bool changed = (count != s_camera_count_prev);
    for (int i = 0; !changed && i < count; i++) {
        if (!node_display_equal(&s_camera_nodes[i], &s_camera_nodes_prev[i])) changed = true;
    }
    if (!changed) return;  /* 화면에 보이는 내용이 그대로면 지우고 다시 그릴 필요 없음 */
    memcpy(s_camera_nodes_prev, s_camera_nodes, sizeof(esp_now_hub_node_t) * count);
    s_camera_count_prev = count;

    /* 그 순간 사용자가 행을 누르고 있는 중이면 LVGL 입력장치가 방금 지워진 객체를 계속
     * 참조하게 돼서 이후 터치가 깨짐 — clean 직전에 이 리스트(자식 포함) 관련 입력장치
     * 상태를 먼저 리셋 */
    lv_indev_reset(NULL, s_camera_list);
    lv_obj_clean(s_camera_list);

    /* 대기중/연결된 장치가 하나도 없으면 "없음" 문구 대신 리스트 자체를 숨김 —
     * 측정기/카메라 상황판 판넬과 다르게, 이 리스트는 원래 대기중인 게 있을 때만
     * 보이는 컨트롤이라 "없음" 메시지 자체가 나올 상황이 아님(사용자 확인) */
    if (count == 0) {
        lv_obj_add_flag(s_camera_list, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_camera_list, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < count; i++) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s (%s)", s_camera_nodes[i].name,
                 ui_str(s_camera_nodes[i].paired ? STR_STATUS_CONNECTED : STR_STATUS_CONNECTING));
        lv_obj_t *row = lv_list_add_button(s_camera_list, NULL, buf);
        lv_obj_set_style_text_font(row, ui_font_get(UI_FONT_SIZE_18), 0);
        lv_obj_set_user_data(row, &s_camera_nodes[i]);
        lv_obj_add_event_cb(row, cb_camera_item_clicked, LV_EVENT_CLICKED, NULL);
    }
}

/* ════════════════════════════════════════════════════════════
 * 상황판 — 판넬 3개: 요약 / 측정기 / 카메라
 * ════════════════════════════════════════════════════════════ */

/* 판넬 하나 생성 — 제목 라벨을 넣고 box 자체를 반환(내용물은 호출부가 box의 직접 자식으로
 * 채움 — 예전엔 여기서 빈 content 래퍼를 하나 더 만들어서 반환했는데, 자식이 하나뿐이거나
 * box와 같은 방향(COLUMN)인 경우엔 그 래퍼가 아무 역할도 안 해서 제거함). idx는
 * s_dash_title[] 저장 위치(언어 전환 갱신용) — 0=요약,1=측정기,2=카메라 */
static lv_obj_t *create_dashboard_panel(lv_obj_t *parent, ui_str_id_t title_id, int idx)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, ui_str(title_id));
    lv_obj_set_style_text_font(title, ui_font_get(UI_FONT_SIZE_18), 0);
    s_dash_title[idx] = title;

    return box;
}

/* ════════════════════════════════════════════════════════════
 * 목록(CAM SD카드 사진 목록) — 번호/촬영시간/크기 + 삭제버튼, 탭하면 그 사진을 요청해서
 * 플레이스홀더에 표시
 * ════════════════════════════════════════════════════════════ */
static void refresh_photo_list_ui(int select_index);  /* capture 팝업이 완료 시점에 씀 */

static void cb_photo_row_select(lv_event_t *e)
{
    uint32_t file_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_target(e);
    if (s_selected_row && s_selected_row != row) {
        lv_obj_set_style_bg_opa(s_selected_row, LV_OPA_TRANSP, 0);
    }
    lv_obj_set_style_bg_color(row, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_30, 0);
    s_selected_row = row;
    if (s_has_paired_cam) esp_now_photo_fetch_by_id(s_paired_cam_mac, file_id);
}

static void cb_photo_delete_confirm(lv_event_t *e)
{
    uint32_t file_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (s_has_paired_cam) {
        esp_now_photo_delete(s_paired_cam_mac, file_id);
        esp_now_photo_list_request(s_paired_cam_mac);  /* 삭제 반영된 목록으로 갱신 */
    }
    cb_modal_close(e);
}

static void show_photo_delete_confirm(uint32_t file_id)
{
    lv_obj_t *box = create_modal();

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, ui_str(STR_MSG_DELETE_PHOTO_CONFIRM));
    lv_obj_set_style_text_font(msg, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_row = create_modal_btn_row(box);
    add_modal_button(btn_row, STR_BTN_YES, cb_photo_delete_confirm, (void *)(uintptr_t)file_id);
    add_modal_button(btn_row, STR_BTN_CANCEL, cb_modal_close, NULL);
}

static void cb_photo_delete_btn(lv_event_t *e)
{
    uint32_t file_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    show_photo_delete_confirm(file_id);
}

/* select_index: 이 인덱스의 행을 선택 표시(예: 지금촬영 직후엔 0=최신). -1이면 선택 없음 */
static void refresh_photo_list_ui(int select_index)
{
    s_current_list_count = esp_now_photo_list_get_items(s_current_list, ESP_NOW_PHOTO_LIST_MAX);

    lv_indev_reset(NULL, s_photo_list);
    lv_obj_clean(s_photo_list);
    s_selected_row = NULL;

    for (int i = 0; i < s_current_list_count; i++) {
        lv_obj_t *row = lv_obj_create(s_photo_list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb_photo_row_select, LV_EVENT_CLICKED,
                             (void *)(uintptr_t)s_current_list[i].file_id);

        /* file_id 자체가 촬영 시각의 유닉스 타임스탬프 — CAM 쪽 주석 참고 */
        time_t t = (time_t)s_current_list[i].file_id;
        struct tm tm_buf;
        gmtime_r(&t, &tm_buf);
        char time_buf[24];
        strftime(time_buf, sizeof(time_buf), "%m-%d %H:%M:%S", &tm_buf);

        char buf[56];
        snprintf(buf, sizeof(buf), "%d  %s (%uKB)", i + 1, time_buf,
                 (unsigned)(s_current_list[i].file_size / 1024));

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, buf);
        lv_obj_set_style_text_font(label, ui_font_get(UI_FONT_SIZE_18), 0);

        lv_obj_t *del_btn = lv_button_create(row);
        lv_obj_add_event_cb(del_btn, cb_photo_delete_btn, LV_EVENT_CLICKED,
                             (void *)(uintptr_t)s_current_list[i].file_id);
        lv_obj_t *del_lbl = lv_label_create(del_btn);
        lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);

        if (i == select_index) {
            lv_obj_set_style_bg_color(row, lv_palette_main(LV_PALETTE_BLUE), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_30, 0);
            s_selected_row = row;
        }
    }
}

/* 새로 받은 사진을 대시보드 썸네일(s_photo_box)에 반영 — 소유권을 넘겨받은 jpeg_data는
 * 이 함수가 앞으로 책임지고 해제(다음 사진이 오거나 화면이 없어질 때) */
static void display_photo(uint8_t *jpeg_data, size_t jpeg_len)
{
    if (s_photo_jpeg_buf) {
        heap_caps_free(s_photo_jpeg_buf);
    }
    s_photo_jpeg_buf = jpeg_data;
    memset(&s_photo_dsc, 0, sizeof(s_photo_dsc));
    s_photo_dsc.data = s_photo_jpeg_buf;
    s_photo_dsc.data_size = jpeg_len;

    if (!s_photo_image) {
        lv_obj_clean(s_photo_box);  /* "사진 없음" 플레이스홀더 라벨 제거 */
        s_camera_photo_label = NULL;  /* 방금 지워짐 — refresh_lang_texts()가 참조하지 않게 */
        s_photo_image = lv_image_create(s_photo_box);
        lv_obj_set_size(s_photo_image, LV_PCT(100), LV_PCT(100));
        lv_image_set_inner_align(s_photo_image, LV_IMAGE_ALIGN_CONTAIN);
    }
    lv_image_set_src(s_photo_image, &s_photo_dsc);
}

/* ════════════════════════════════════════════════════════════
 * 지금촬영 진행 팝업 — 1.명령전달 2.촬영결과 3.영상수신 4.목록갱신, 완료되면 자동으로 닫히고
 * 방금 찍은 사진이 목록 맨 위에 선택된 채로 남음(사진 자체는 팝업이 닫혀 배경 타이머가
 * 재개된 뒤 refresh_dashboard()의 일반 수신 처리 경로가 표시함)
 * ════════════════════════════════════════════════════════════ */
static void close_capture_popup(void)
{
    if (s_capture_popup_timer) {
        lv_timer_delete(s_capture_popup_timer);
        s_capture_popup_timer = NULL;
    }
    if (s_capture_popup_overlay) {
        lv_obj_delete(s_capture_popup_overlay);
        s_capture_popup_overlay = NULL;
    }
    resume_bg_timers();
    esp_now_photo_capture_stage_clear();
}

static void cb_capture_popup_close(lv_event_t *e)
{
    (void)e;
    close_capture_popup();
}

static void set_stage_label(int idx, ui_str_id_t str_id, lv_color_t color)
{
    lv_label_set_text(s_capture_stage_label[idx], ui_str(str_id));
    lv_obj_set_style_text_color(s_capture_stage_label[idx], color, 0);
}

static void capture_popup_tick(lv_timer_t *t)
{
    (void)t;
    lv_color_t grey  = lv_palette_main(LV_PALETTE_GREY);
    lv_color_t green = lv_palette_main(LV_PALETTE_GREEN);
    lv_color_t red   = lv_palette_main(LV_PALETTE_RED);

    switch (esp_now_photo_get_capture_stage()) {
    case ESP_NOW_CAPTURE_STAGE_SENT:
        set_stage_label(0, STR_CAPTURE_STAGE1_PROGRESS, grey);
        break;
    case ESP_NOW_CAPTURE_STAGE_ACKED:
        set_stage_label(0, STR_CAPTURE_STAGE1_DONE, green);
        break;
    case ESP_NOW_CAPTURE_STAGE_CAPTURED:
        set_stage_label(0, STR_CAPTURE_STAGE1_DONE, green);
        set_stage_label(1, STR_CAPTURE_STAGE2_SUCCESS, green);
        set_stage_label(2, STR_CAPTURE_STAGE3_PROGRESS, grey);
        break;
    case ESP_NOW_CAPTURE_STAGE_CAPTURE_FAILED:
        set_stage_label(1, STR_CAPTURE_STAGE2_FAILED, red);
        break;
    case ESP_NOW_CAPTURE_STAGE_TRANSFER_DONE:
        /* 이 분기가 다시 안 타게 바로 capture_stage를 비움 — 사진 자체(consume/표시)는
         * 팝업이 닫힌 뒤 refresh_dashboard()가 처리(아래 참고) */
        set_stage_label(2, STR_CAPTURE_STAGE3_DONE, green);
        set_stage_label(3, STR_CAPTURE_STAGE4_PROGRESS, grey);
        esp_now_photo_list_request(s_paired_cam_mac);
        esp_now_photo_capture_stage_clear();
        break;
    case ESP_NOW_CAPTURE_STAGE_TRANSFER_FAILED:
        set_stage_label(2, STR_CAPTURE_STAGE3_FAILED, red);
        esp_now_photo_capture_stage_clear();
        break;
    default:
        break;
    }

    if (esp_now_photo_list_get_state() == ESP_NOW_PHOTO_LIST_STATE_READY) {
        set_stage_label(3, STR_CAPTURE_STAGE4_DONE, green);
        refresh_photo_list_ui(0);  /* 방금 찍은 게 목록에서 가장 최신 = index 0 */
        esp_now_photo_list_ack();
        close_capture_popup();
    }
}

static void show_capture_popup(void)
{
    lv_obj_t *box = create_modal();  /* pause_bg_timers()도 여기서 같이 됨 */
    s_capture_popup_overlay = lv_obj_get_parent(box);

    for (int i = 0; i < 4; i++) {
        s_capture_stage_label[i] = lv_label_create(box);
        lv_obj_set_style_text_font(s_capture_stage_label[i], ui_font_get(UI_FONT_SIZE_18), 0);
        lv_label_set_text(s_capture_stage_label[i], "");
    }
    set_stage_label(0, STR_CAPTURE_STAGE1_PROGRESS, lv_palette_main(LV_PALETTE_GREY));

    lv_obj_t *btn_row = create_modal_btn_row(box);
    add_modal_button(btn_row, STR_BTN_CLOSE, cb_capture_popup_close, NULL);

    s_capture_popup_timer = lv_timer_create(capture_popup_tick, 200, NULL);
}

static void cb_capture_now(lv_event_t *e)
{
    (void)e;
    if (!s_has_paired_cam) return;
    show_capture_popup();
    esp_now_photo_capture_now(s_paired_cam_mac);
}

static void cb_renew_list(lv_event_t *e)
{
    (void)e;
    if (!s_has_paired_cam) return;
    esp_now_photo_list_request(s_paired_cam_mac);
}

static void refresh_dashboard(lv_timer_t *t)
{
    (void)t;

    /* 판넬1: 요약 — 페어링된(연결된) 장치 전부(CAM+SENS), 정상 표시 */
    int total = esp_now_hub_get_nodes(HUB_NODE_KIND_UNKNOWN, s_dash_nodes, ESP_NOW_HUB_MAX_NODES);

    bool dash_changed = (total != s_dash_count_prev);
    for (int i = 0; !dash_changed && i < total; i++) {
        if (!node_display_equal(&s_dash_nodes[i], &s_dash_nodes_prev[i])) dash_changed = true;
    }
    if (dash_changed) {
        memcpy(s_dash_nodes_prev, s_dash_nodes, sizeof(esp_now_hub_node_t) * total);
        s_dash_count_prev = total;

        int paired_count = 0;
        lv_indev_reset(NULL, s_summary_list);  /* refresh_camera_list와 동일한 이유 */
        lv_obj_clean(s_summary_list);
        for (int i = 0; i < total; i++) {
            if (!s_dash_nodes[i].paired) continue;
            paired_count++;
            char buf[48];
            snprintf(buf, sizeof(buf), "%s (%s)", s_dash_nodes[i].name, ui_str(STR_STATUS_OK));
            lv_obj_t *row = lv_label_create(s_summary_list);
            lv_label_set_text(row, buf);
            lv_obj_set_style_text_font(row, ui_font_get(UI_FONT_SIZE_18), 0);
        }
        /* 연결된 장치가 없을 때는 리스트 안에 문구를 넣는 대신(리스트 아이템 스타일이 입혀져서
         * 측정기/카메라 판넬의 "없음" 라벨과 모양·색이 달라 보였음, 사용자 지적) 측정기/카메라
         * 판넬과 똑같은 방식으로 별도의 일반 라벨(s_summary_empty)을 보여줌 */
        if (paired_count == 0) {
            lv_obj_add_flag(s_summary_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_summary_empty, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_summary_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_summary_empty, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 판넬2: 측정기 — 연결된 SENS 있으면 틀(TODO)만, 없으면 없음 라벨 */
    bool sensor_connected = false;
    for (int i = 0; i < total; i++) {
        if (s_dash_nodes[i].paired && s_dash_nodes[i].kind == HUB_NODE_KIND_SENS) { sensor_connected = true; break; }
    }
    if (sensor_connected) {
        lv_obj_add_flag(s_sensor_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_sensor_todo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_sensor_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sensor_todo, LV_OBJ_FLAG_HIDDEN);
    }

    /* 판넬3: 카메라 — 연결된 CAM 있으면 사진+촬영버튼 틀, 없으면 없음 라벨.
     * 지금촬영 버튼이 쏠 대상 mac도 여기서 같이 기억해둠 */
    bool camera_connected = false;
    for (int i = 0; i < total; i++) {
        if (s_dash_nodes[i].paired && s_dash_nodes[i].kind == HUB_NODE_KIND_CAM) {
            camera_connected = true;
            memcpy(s_paired_cam_mac, s_dash_nodes[i].mac, sizeof(s_paired_cam_mac));
            break;
        }
    }
    s_has_paired_cam = camera_connected;
    if (camera_connected) {
        lv_obj_add_flag(s_camera_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_camera_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_camera_split_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_camera_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_camera_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_camera_split_row, LV_OBJ_FLAG_HIDDEN);
    }

    /* 사진 수신 폴링 — recv_cb(ESP-NOW 태스크)가 CRC까지 검증해서 READY로 표시해두면
     * 여기(LVGL 워커 태스크)서 소유권을 받아와 화면에 그림. LVGL 호출은 항상 이 태스크에서만
     * 해야 해서 esp_now_photo 쪽은 상태 플래그만 들고 있고 그리는 건 UI 쪽 책임 */
    esp_now_photo_state_t photo_state = esp_now_photo_get_state();
    if (photo_state == ESP_NOW_PHOTO_STATE_READY) {
        const uint8_t *jpeg_data = NULL;
        size_t jpeg_len = 0;
        if (esp_now_photo_consume(&jpeg_data, &jpeg_len)) {
            display_photo((uint8_t *)jpeg_data, jpeg_len);
        }
    } else if (photo_state == ESP_NOW_PHOTO_STATE_ERROR) {
        esp_now_photo_clear();  /* 실패 로그는 esp_now_photo.c 쪽에서 이미 남김 */
    }

    /* 목록갱신 버튼(cb_renew_list)으로 요청한 목록 — 지금촬영 흐름 쪽 목록 완료 처리는
     * capture_popup_tick()이 하는데, 그동안은 이 타이머 자체가 pause_bg_timers()로 멈춰
     * 있어서 여기와 겹칠 일이 없음 */
    if (esp_now_photo_list_get_state() == ESP_NOW_PHOTO_LIST_STATE_READY) {
        refresh_photo_list_ui(-1);
        esp_now_photo_list_ack();
    }
}

/* 그룹박스 하나 생성 — 제목 라벨을 넣고 box 자체를 반환. 내용물은 호출부가 box의 직접
 * 자식으로 채움 — 여러 개를 가로로 나열해야 하면(예: 제어기 박스의 라벨+버튼) 호출부가
 * 자기 필요에 맞는 row 컨테이너를 직접 만들어서 넣을 것(예전처럼 여기서 자동으로 빈
 * content 래퍼를 만들어주지 않음 — 자식이 하나뿐인 박스엔 그 래퍼가 불필요했음) */
static lv_obj_t *create_group_box(lv_obj_t *parent, ui_str_id_t title_id)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, ui_str(title_id));
    lv_obj_set_style_text_font(title, ui_font_get(UI_FONT_SIZE_18), 0);
    s_group_title[title_id - STR_GROUP_CNTL] = title;

    return box;
}

static void refresh_clock(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char buf[12];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    lv_label_set_text(s_clock_label, buf);
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

    /* 로고부제 자리 — 보드 실장 RTC(rtc_sync_init, main.c에서 UI보다 먼저 호출)로 세팅된
     * 시스템 클록을 1초마다 hh:mm:ss로 보여줌 */
    s_clock_label = lv_label_create(tab_bar);
    lv_obj_add_flag(s_clock_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_style(s_clock_label, &style_text_muted, 0);
    lv_obj_align_to(s_clock_label, logo_icon, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, 0);
    refresh_clock(NULL);  /* 첫 타이머 tick(최대 1초 뒤) 전까지 빈 채로 안 보이게 즉시 한 번 채움 */
    lv_timer_create(refresh_clock, 1000, NULL);

    /* 상황판 — 판넬 3개(요약/측정기/카메라), 1초마다 연결 상태 반영 */
    lv_obj_t *dashboard_page = lv_tabview_add_tab(s_page_control, ui_str(STR_TAB_DASHBOARD));
    lv_obj_set_flex_flow(dashboard_page, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *summary_box = create_dashboard_panel(dashboard_page, STR_PANEL_SUMMARY, 0);
    /* lv_list는 카드 형태(회색 배경+테두리) 기본 스타일이 입혀져 있어서 측정기/카메라
     * 판넬의 민무늬 라벨과 다르게 보였음(사용자 지적) — 리스트 위젯 대신 그냥 세로로 쌓는
     * 빈 컨테이너를 쓰고, 각 항목은 일반 라벨(lv_label_create)로 채움 */
    s_summary_list = lv_obj_create(summary_box);
    lv_obj_set_size(s_summary_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_summary_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(s_summary_list, 0, 0);
    lv_obj_set_style_bg_opa(s_summary_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_summary_list, 0, 0);
    lv_obj_add_flag(s_summary_list, LV_OBJ_FLAG_HIDDEN);  /* 초기값: 연결된 장치 없음 */
    s_summary_empty = lv_label_create(summary_box);
    lv_label_set_text(s_summary_empty, ui_str(STR_PANEL_NO_PAIRED_DEVICE));
    lv_obj_set_style_text_font(s_summary_empty, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *sensor_box = create_dashboard_panel(dashboard_page, STR_GROUP_SENSOR, 1);
    s_sensor_empty = lv_label_create(sensor_box);
    lv_label_set_text(s_sensor_empty, ui_str(STR_PANEL_NO_SENSOR));
    lv_obj_set_style_text_font(s_sensor_empty, ui_font_get(UI_FONT_SIZE_18), 0);
    s_sensor_todo = lv_label_create(sensor_box);
    lv_label_set_text(s_sensor_todo, ui_str(STR_PANEL_SENSOR_TODO));
    lv_obj_set_style_text_font(s_sensor_todo, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_flag(s_sensor_todo, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *camera_box = create_dashboard_panel(dashboard_page, STR_GROUP_CAMERA, 2);
    s_camera_empty = lv_label_create(camera_box);
    lv_label_set_text(s_camera_empty, ui_str(STR_PANEL_NO_CAMERA));
    lv_obj_set_style_text_font(s_camera_empty, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 상단 툴바 — 지금촬영/목록갱신 외에 나중에 다른 컨트롤도 여기 추가될 예정, 그래서
     * 목록/사진 판넬보다 위에 별도 행으로 둠. camera_box의 직접 자식(예전엔 s_camera_content라는
     * 빈 래퍼를 하나 더 씌웠는데 제거 — 이 툴바와 아래 split_row를 각자 HIDDEN 토글) */
    lv_obj_t *camera_toolbar = lv_obj_create(camera_box);
    lv_obj_set_size(camera_toolbar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(camera_toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_width(camera_toolbar, 0, 0);
    lv_obj_set_style_pad_all(camera_toolbar, 0, 0);
    lv_obj_add_flag(camera_toolbar, LV_OBJ_FLAG_HIDDEN);  /* 초기값: 연결 전이라 숨김 */
    s_camera_content = camera_toolbar;  /* HIDDEN 토글 대상 1/2 — split_row가 2/2 */

    lv_obj_t *btn_capture = lv_button_create(camera_toolbar);
    lv_obj_add_event_cb(btn_capture, cb_capture_now, LV_EVENT_CLICKED, NULL);
    s_camera_capture_lbl = lv_label_create(btn_capture);
    lv_label_set_text(s_camera_capture_lbl, ui_str(STR_BTN_CAPTURE_NOW));
    lv_obj_set_style_text_font(s_camera_capture_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_renew = lv_button_create(camera_toolbar);
    lv_obj_add_event_cb(btn_renew, cb_renew_list, LV_EVENT_CLICKED, NULL);
    s_camera_renew_lbl = lv_label_create(btn_renew);
    lv_label_set_text(s_camera_renew_lbl, ui_str(STR_BTN_RENEW_LIST));
    lv_obj_set_style_text_font(s_camera_renew_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 목록(왼쪽)/사진(오른쪽) 판넬 — 툴바와 형제(camera_box 직접 자식), 좌우로 나열 */
    s_camera_split_row = lv_obj_create(camera_box);
    lv_obj_set_size(s_camera_split_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_camera_split_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_width(s_camera_split_row, 0, 0);
    lv_obj_set_style_pad_all(s_camera_split_row, 0, 0);
    lv_obj_add_flag(s_camera_split_row, LV_OBJ_FLAG_HIDDEN);  /* 초기값: 연결 전이라 숨김 */

    lv_obj_t *list_panel = lv_obj_create(s_camera_split_row);
    lv_obj_set_size(list_panel, LV_PCT(45), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list_panel, LV_FLEX_FLOW_COLUMN);
    s_list_title = lv_label_create(list_panel);
    lv_label_set_text(s_list_title, ui_str(STR_PANEL_LIST));
    lv_obj_set_style_text_font(s_list_title, ui_font_get(UI_FONT_SIZE_18), 0);
    s_photo_list = lv_list_create(list_panel);
    lv_obj_set_size(s_photo_list, LV_PCT(100), 220);

    lv_obj_t *picture_panel = lv_obj_create(s_camera_split_row);
    lv_obj_set_size(picture_panel, LV_PCT(55), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(picture_panel, LV_FLEX_FLOW_COLUMN);
    s_picture_title = lv_label_create(picture_panel);
    lv_label_set_text(s_picture_title, ui_str(STR_PANEL_PICTURE));
    lv_obj_set_style_text_font(s_picture_title, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 사진 없을 때 플레이스홀더 — 큼직한 박스만(아이콘은 뺌, 비트맵 폰트를 확대하니
     * 깨져 보였고 어차피 실제 사진이 오면 이 자리를 lv_image로 교체할 예정이라 불필요).
     * 탭하면 전체화면 뷰어(더블탭 확대/축소+드래그 패닝) */
    s_photo_box = lv_obj_create(picture_panel);
    lv_obj_set_size(s_photo_box, LV_PCT(100), 220);
    lv_obj_set_style_bg_color(s_photo_box, lv_palette_lighten(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_radius(s_photo_box, 12, 0);
    lv_obj_set_style_border_width(s_photo_box, 1, 0);
    lv_obj_set_style_border_color(s_photo_box, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_flex_flow(s_photo_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_photo_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_photo_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_photo_box, cb_open_photo_viewer, LV_EVENT_CLICKED, NULL);

    s_camera_photo_label = lv_label_create(s_photo_box);
    lv_label_set_text(s_camera_photo_label, ui_str(STR_PANEL_NO_PHOTO_YET));
    lv_obj_set_style_text_font(s_camera_photo_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_color(s_camera_photo_label, lv_palette_main(LV_PALETTE_GREY), 0);

    s_dashboard_timer = lv_timer_create(refresh_dashboard, 1000, NULL);

    lv_obj_t *stats_page = lv_tabview_add_tab(s_page_control, ui_str(STR_TAB_STATISTICS));
    lv_demo_widgets_analytics_create(stats_page);

    lv_obj_t *option_page = lv_tabview_add_tab(s_page_control, ui_str(STR_TAB_OPTION));
    lv_obj_set_flex_flow(option_page, LV_FLEX_FLOW_COLUMN);

    /* 그룹박스 4개(세로로 나열): 제어기/측정기/영상/시스템 — 내용은 아직 시스템(언어
     * 전환)만 채움, 나머지는 제목만 있는 빈 틀. 언어 행: 라벨 왼쪽 정렬 + 선택
     * 버튼(한글/English) 오른쪽 정렬, 선택된 버튼은 다른 색으로 표시 */
    lv_obj_t *cntl_box = create_group_box(option_page, STR_GROUP_CNTL);

    /* 리스트뷰 컨테이너 — 다른 설정 화면처럼, 제어기 설정 항목들을 리스트 아이템 형태로
     * 담음(지금은 언어 하나뿐이지만 나중에 항목이 늘어나면 같은 리스트 안에 행이 늘어나는
     * 구조). 언어는 탭하면 팝업이 뜨는 방식 대신, 라디오 컨트롤을 행 안에 그대로 박아넣어서
     * 바로 선택되게 함(사용자 지시) */
    lv_obj_t *cntl_list = lv_list_create(cntl_box);
    lv_obj_set_size(cntl_list, LV_PCT(100), LV_SIZE_CONTENT);

    /* 언어 행 — 라벨+버튼그룹을 가로로 나열해야 해서 리스트의 직접 자식으로 별도 row
     * 컨테이너를 만듦 */
    lv_obj_t *cntl_row = lv_obj_create(cntl_list);
    lv_obj_set_size(cntl_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cntl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cntl_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(cntl_row, 0, 0);
    lv_obj_set_style_pad_hor(cntl_row, 12, 0);
    lv_obj_set_style_pad_ver(cntl_row, 20, 0);  /* 사용자 요청: 행 높이 2배 정도로 키움 */

    s_lang_label = lv_label_create(cntl_row);
    lv_label_set_text(s_lang_label, ui_str(STR_LABEL_LANGUAGE));
    lv_obj_set_style_text_font(s_lang_label, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_group = lv_obj_create(cntl_row);
    lv_obj_set_size(btn_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(btn_group, 0, 0);
    lv_obj_set_style_border_width(btn_group, 0, 0);

    /* 버튼 대신 라디오버튼(체크박스를 원형 인디케이터로 스타일링, LVGL엔 전용 라디오
     * 위젯이 없음) — 하나 선택하면 다른 하나는 클릭 콜백에서 수동으로 해제 */
    s_btn_ko = lv_checkbox_create(btn_group);
    lv_checkbox_set_text(s_btn_ko, "한글");
    lv_obj_set_style_text_font(s_btn_ko, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_radius(s_btn_ko, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_btn_ko, cb_lang_ko, LV_EVENT_CLICKED, NULL);

    s_btn_en = lv_checkbox_create(btn_group);
    lv_checkbox_set_text(s_btn_en, "English");
    lv_obj_set_style_radius(s_btn_en, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_btn_en, cb_lang_en, LV_EVENT_CLICKED, NULL);

    update_lang_buttons();  /* 초기 선택 상태(기본 UI_LANG_KO) 반영 */

    create_group_box(option_page, STR_GROUP_SENSOR);

    /* 영상(Camera) 그룹박스 — 발견된 CAM 리스트(연결중/연결됨), 1초마다 갱신.
     * 자식이 리스트 하나뿐이라 별도 content 래퍼 없이 box 직접 자식으로 둠 */
    lv_obj_t *camera_group_box = create_group_box(option_page, STR_GROUP_CAMERA);
    s_camera_list = lv_list_create(camera_group_box);
    lv_obj_set_size(s_camera_list, LV_PCT(100), LV_SIZE_CONTENT);
    s_camera_list_timer = lv_timer_create(refresh_camera_list, 1000, NULL);

    create_group_box(option_page, STR_GROUP_SYSTEM);
}
