#include "ui_main.h"
#include "ui_strings.h"
#include "ui_font.h"
#include "esp_now_hub.h"
#include "esp_now_photo.h"
#include "ui_log.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ui_main";

/* 벤더 lv_demo_widgets 내부 함수/전역 — 로고 타이틀 스타일(style_text_muted)은 계속 씀.
 * 통계 탭의 Analytics 위젯은 로그박스로 교체함(2026-08-01, 사용자 지시). 공개
 * 헤더(lv_demos.h)엔 없고 데모 내부 전용 헤더에만 선언돼 있어서 직접 extern 선언해서 씀. */
extern void lv_demo_widgets_components_init(void);
extern lv_obj_t *lv_demo_widgets_title_create(lv_obj_t *parent, const char *text);
extern lv_style_t style_text_muted;

static lv_obj_t *s_page_control = NULL;
static lv_obj_t *s_logo_title = NULL;   /* "플렉스팜/FlexFarm" — 언어 전환 시 갱신 필요 */
static lv_obj_t *s_clock_label = NULL;  /* 로고부제 자리 — "Widgets demo" 대신 실시간 시계(hh:mm:ss) */
static lv_obj_t *s_lang_label = NULL;
static lv_obj_t *s_btn_ko = NULL;
static lv_obj_t *s_btn_en = NULL;

/* 로고아이콘 정상/경고 두 상태 — 확인 안 한 실패(메모리/통신)가 하나라도 있으면 경고
 * 아이콘(빨간 바탕+노란 느낌표)으로 바뀜(2026-08-01, 사용자 지시). 토스트는 몇 초 뒤
 * 사라지지만 이 아이콘은 세션 내내(재부팅 전까지) 남아있어서 "한 번이라도 실패가
 * 있었다"를 계속 알려줌 */
static lv_obj_t *s_logo_icon    = NULL;
static lv_obj_t *s_logo_warning = NULL;
static bool      s_error_active = false;

/* 상단 에러 토스트 — 진행팝업(create_modal)과 달리 배경을 안 가리고 입력도 안 막음,
 * 몇 초 뒤 자동으로 없어짐 */
static lv_obj_t *s_toast = NULL;
static uint32_t  s_toast_expire_ms = 0;

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
static lv_obj_t          *s_camera_delete_all_lbl = NULL;
static lv_obj_t          *s_list_title          = NULL;
static lv_obj_t          *s_list_info_label     = NULL;  /* "N개(Pic.)  XX%" — 목록 제목 오른쪽 */
static lv_obj_t          *s_picture_title       = NULL;
static lv_obj_t          *s_photo_list          = NULL;  /* 목록 판넬 — 설정탭 CAM 발견 리스트(s_camera_list)와는 다른 위젯 */
static lv_obj_t          *s_photo_box           = NULL;  /* 사진 판넬 — 사진 오면 이 안의 라벨을 lv_image로 교체 */
static lv_obj_t          *s_photo_image         = NULL;  /* s_photo_box 안의 lv_image(사진 오기 전엔 NULL) */
static esp_now_hub_node_t s_dash_nodes[ESP_NOW_HUB_MAX_NODES];
static esp_now_hub_node_t s_dash_nodes_prev[ESP_NOW_HUB_MAX_NODES];
static int                s_dash_count_prev = -1;  /* -1: 아직 비교 대상 없음(첫 실행은 항상 그림) */
static uint8_t            s_selected_cam_mac[6];
static bool               s_has_selected_cam = false;  /* 지금촬영/목록/삭제 등이 쏠 대상 —
                                                            이제 "자동으로 찾은 유일한 CAM"이
                                                            아니라 "드롭다운에서 선택된 CAM"
                                                            (2026-08-05, 여러 CAM 동시 페어링
                                                            지원) */

/* CAM 선택 드롭다운(camera_toolbar 맨 앞) — 이 앱 첫 lv_dropdown 사용. 옵션 문자열의 각
 * 줄(인덱스)이 어느 mac인지는 LVGL이 몰라서 별도로 같이 들고 있어야 함 */
static lv_obj_t          *s_camera_select_dd = NULL;
static uint8_t            s_cam_dd_macs[ESP_NOW_HUB_MAX_NODES][6];
static int                s_cam_dd_count = 0;

/* 대시보드 썸네일(판넬) 디코드 버퍼 — 목표 해상도가 고정(320x240)이라 매번 free+새로
 * alloc 하지 않고 ui_init()에서 한 번만 잡아서 계속 재사용, 내용만 덮어쓰고 LVGL 이미지
 * 캐시만 invalidate(lv_image_cache_drop)함(2026-08-01, 사용자 지적 — 반복 사용할 버퍼는
 * 처음에 미리 잡아두고 계속 쓰는 게 맞음). 예전엔 사진 선택마다 jpeg_free_align+
 * jpeg_calloc_align을 반복해서 PSRAM 조각화의 원인이 됐었음(사진 수신버퍼를 고정
 * 크기로 바꾼 것과 같은 이유 — esp_now_photo.c 참고). 전체화면 뷰어(1600x960)는
 * PSRAM 예산에 안 맞아 제거함(2026-08-01, 사용자 지시) */
#define PHOTO_PANEL_BUF_CAP  ((size_t)PHOTO_PANEL_DECODE_W  * PHOTO_PANEL_DECODE_H  * 2 * 11 / 10)
static uint8_t          *s_photo_jpeg_buf = NULL;
static lv_image_dsc_t    s_photo_dsc;

/* 지금촬영 진행 팝업 — 1.명령전달/2.촬영결과/3.목록갱신 3줄(팝업 뼈대 자체는 아래 공용
 * 진행 팝업 모듈이 담당, 이 라벨 배열만 지금촬영 전용) */
static lv_obj_t   *s_capture_stage_label[3];

/* CAM SD카드 사진 목록(내용 없이 file_id+크기만) — 탭하면 그 사진을 fetch_by_id로 받아서
 * 플레이스홀더에 표시, 삭제 버튼은 확인 팝업 거쳐서 삭제 */
static esp_now_photo_list_item_t s_current_list[ESP_NOW_PHOTO_LIST_MAX];
static int                        s_current_list_count = 0;

/* 선택 상태의 진짜 모델은 file_id(s_selected_file_id) — s_selected_row는 그 모델을 지금
 * 그려진 목록 위에 표시하기 위한 뷰 캐시일 뿐(2026-08-02, 사용자 지적: "View는 Model의
 * 그림자일 뿐이야"). 목록이 다시 그려지면(refresh_photo_list_ui) 행 객체는 매번 새로
 * 만들어지므로 s_selected_row 포인터는 그때마다 무효가 되지만, s_selected_file_id는
 * 그대로 유지되고 다시 그릴 때 그 file_id를 찾아 강조표시만 복원함(재요청 없이) */
static lv_obj_t  *s_selected_row = NULL;
static uint32_t   s_selected_file_id = 0;
static bool       s_has_selected_file_id = false;

/* 통계 탭 로그박스 — 시리얼 모니터가 리셋을 유발하는 문제 때문에(2026-08-01) ui_log 모듈에
 * 쌓인 로그를 화면에서 직접 보는 용도로 벤더 데모(analytics 위젯) 대신 넣음 */
static lv_obj_t *s_log_container = NULL;
static lv_obj_t *s_log_label     = NULL;

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

/* ════════════════════════════════════════════════════════════
 * 에러 토스트 + 로고 경고 아이콘 — 메모리/통신 실패를 로그(통계 탭)에만 남기지 않고
 * 화면에 바로 보여주기 위함(2026-08-01, 사용자 지시: "네 코드가 에러처리가 없어서
 * 지금까지 헤멘거잖아"). ui_log_add_err()로 남긴 실패가 있으면 200ms 폴링 타이머가
 * 잡아서 토스트로 띄우고, 로고를 경고 아이콘으로 바꿈.
 * ════════════════════════════════════════════════════════════ */
static void show_error_toast(const char *msg)
{
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    s_toast = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_toast, LV_PCT(85), LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, 85);  /* tab bar(75px) 바로 아래 */
    lv_obj_set_style_bg_color(s_toast, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(s_toast, 10, 0);

    lv_obj_t *lbl = lv_label_create(s_toast);
    lv_label_set_text(lbl, msg);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_font(lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);

    s_toast_expire_ms = lv_tick_get() + 4000;
}

static void set_logo_warning(bool active)
{
    if (active == s_error_active) return;
    s_error_active = active;
    if (s_logo_icon) {
        if (active) lv_obj_add_flag(s_logo_icon, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_remove_flag(s_logo_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_logo_warning) {
        if (active) lv_obj_remove_flag(s_logo_warning, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(s_logo_warning, LV_OBJ_FLAG_HIDDEN);
    }
}

static void error_poll_tick(lv_timer_t *t)
{
    (void)t;
    char err[128];
    if (ui_log_get_pending_error(err, sizeof(err))) {
        show_error_toast(err);
        set_logo_warning(true);
    }
    if (s_toast && lv_tick_get() >= s_toast_expire_ms) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
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

/* 카메라 리스트(설정탭)의 "비교 대상 없음" 상태(-1)로 되돌림 — 정의는 s_camera_count_prev
 * 선언부 근처(아래), 여기(refresh_lang_texts)보다 늦게 선언돼서 전방선언만 둠 */
static void force_camera_list_redraw(void);
/* 목록 개수 라벨("N개"/"N Pic.") 갱신 — 정의는 refresh_photo_list_ui 근처(아래) */
static void update_list_info_label(void);

static void refresh_lang_texts(void)
{
    /* 카메라 리스트(설정탭)/요약 리스트(상황판)는 내용이 안 바뀌면 다시 안 그리는
     * 최적화가 있어서(터치 반응성 문제로 도입) 언어만 바뀐 경우는 안 걸림 — 다음 tick에
     * 무조건 다시 그리도록 "비교 대상 없음" 상태로 되돌림(2026-08-01, 사용자 지적:
     * "연결 대기 중" 상태 문구가 언어 전환해도 항상 한글로 남아있던 버그) */
    force_camera_list_redraw();
    s_dash_count_prev = -1;

    lv_label_set_text(s_logo_title, ui_str(STR_LOGO_TITLE));

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
    lv_label_set_text(s_camera_delete_all_lbl, ui_str(STR_BTN_DELETE_ALL));
    lv_label_set_text(s_list_title, ui_str(STR_PANEL_LIST));
    lv_label_set_text(s_picture_title, ui_str(STR_PANEL_PICTURE));
    update_list_info_label();
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

/* 경고 로고 탭 — 지금까지 쌓인 에러 코드를 전부 목록으로 보여줌(2026-08-01, 사용자
 * 지시: "로고를 찍으면 error code를 보여주는 팝업... 누적된 게 있으면 여러 개를
 * 보여줄 수도"). 코드+짧은 설명을 한 줄씩, 확인 누르면 닫힘(목록 자체는 안 지움) */
static void cb_logo_warning_tap(lv_event_t *e)
{
    (void)e;
    int codes[UI_ERR_HISTORY_CAP];
    int n = ui_log_get_error_history(codes, UI_ERR_HISTORY_CAP);

    lv_obj_t *box = create_modal();

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "에러 코드 목록");
    lv_obj_set_style_text_font(title, ui_font_get(UI_FONT_SIZE_18), 0);

    if (n == 0) {
        lv_obj_t *lbl = lv_label_create(box);
        lv_label_set_text(lbl, "(없음)");
        lv_obj_set_style_text_font(lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    } else {
        for (int i = 0; i < n; i++) {
            char buf[160];
            snprintf(buf, sizeof(buf), "[%04d] %s", codes[i], ui_log_err_desc(codes[i]));
            lv_obj_t *lbl = lv_label_create(box);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_font(lbl, ui_font_get(UI_FONT_SIZE_18), 0);
        }
    }

    lv_obj_t *btn_row = create_modal_btn_row(box);
    add_modal_button(btn_row, STR_BTN_CONFIRM, cb_modal_close, NULL);
}

/* ════════════════════════════════════════════════════════════
 * 예취소(Yes/Cancel) 공용 확인 팝업 — 연결해제/사진삭제/전체삭제 등 "예/취소로 묻고 예를
 * 누르면 콜백 실행" 패턴이 반복돼서 공통화(2026-08-01). LVGL 이벤트 콜백은 시그니처가
 * 고정이라 실제 콜백+ctx는 static 구조체 하나에 담아 트램폴린으로 전달 — 모달은 한 번에
 * 하나만 뜨므로 static 싱글턴으로 충분(재진입 없음).
 * ════════════════════════════════════════════════════════════ */
typedef void (*confirm_yes_fn_t)(void *ctx);

typedef struct {
    confirm_yes_fn_t fn;
    void *ctx;
} confirm_popup_state_t;

static confirm_popup_state_t s_confirm_state;

static void cb_confirm_yes_trampoline(lv_event_t *e)
{
    confirm_popup_state_t *st = (confirm_popup_state_t *)lv_event_get_user_data(e);
    confirm_yes_fn_t fn = st->fn;
    void *ctx = st->ctx;
    cb_modal_close(e);
    if (fn) fn(ctx);
}

static void show_confirm_popup(const char *message, confirm_yes_fn_t on_yes, void *ctx)
{
    s_confirm_state.fn  = on_yes;
    s_confirm_state.ctx = ctx;

    lv_obj_t *box = create_modal();

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, message);
    lv_obj_set_style_text_font(msg, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_row = create_modal_btn_row(box);
    add_modal_button(btn_row, STR_BTN_YES, cb_confirm_yes_trampoline, &s_confirm_state);
    add_modal_button(btn_row, STR_BTN_CANCEL, cb_modal_close, NULL);
}

/* 아래에서 씀 — 정의는 판넬 표시 코드 근처(display_photo 옆) */
static bool decode_jpeg_scaled(const uint8_t *jpeg_data, size_t jpeg_len,
                                uint16_t target_w, uint16_t target_h,
                                uint8_t *out_buf, size_t out_cap,
                                uint16_t *out_w, uint16_t *out_h, size_t *out_len);
static void fill_rgb565_dsc(lv_image_dsc_t *dsc, uint8_t *pixel_buf, uint16_t w, uint16_t h, size_t len);

/* ════════════════════════════════════════════════════════════
 * 장치 연결/설정 팝업 — 미연결(연결 허용 확인) / 연결됨(설정+연결해제)
 * ════════════════════════════════════════════════════════════ */
static void cb_pair_confirm(lv_event_t *e)
{
    esp_now_hub_node_t *node = (esp_now_hub_node_t *)lv_event_get_user_data(e);
    esp_now_hub_pair(node->mac);
    cb_modal_close(e);
}

static void cb_unpair_confirm(void *ctx)
{
    esp_now_hub_node_t *node = (esp_now_hub_node_t *)ctx;
    esp_now_hub_unpair(node->mac);
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
    char msg[64];
    snprintf(msg, sizeof(msg), "%s\n%s", node->name, ui_str(STR_MSG_UNPAIR_CONFIRM));
    show_confirm_popup(msg, cb_unpair_confirm, node);
}

/* ════════════════════════════════════════════════════════════
 * 영상(Camera) 그룹박스 — 발견된 CAM 리스트(연결중/연결됨), 탭하면 위 팝업
 * ════════════════════════════════════════════════════════════ */
static lv_obj_t          *s_camera_list = NULL;
static esp_now_hub_node_t s_camera_nodes[ESP_NOW_HUB_MAX_NODES];
static esp_now_hub_node_t s_camera_nodes_prev[ESP_NOW_HUB_MAX_NODES];
static int                s_camera_count_prev = -1;  /* -1: 아직 비교 대상 없음(첫 실행은 항상 그림) */

static void force_camera_list_redraw(void)
{
    s_camera_count_prev = -1;
}

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
static void show_fetch_progress_popup(void);  /* 아래 공용 진행팝업 모듈 정의 뒤에 구현 */
static void display_photo(uint32_t file_id);  /* 아래 정의 — 캐시 히트 시 여기서 바로 씀 */
static void consume_ready_photo_if_current(void);  /* 아래 정의 — display_photo() 직후 */

/* CAM의 실제 파일명 표기(base36 4자리, 0-9A-Z, CAM/main/cam_storage.c의 encode_seq()와
 * 동일 인코딩)를 그대로 미러링 — 예전엔 file_id를 %u로 그냥 10진수로 찍어서 CAM SD카드의
 * 실제 파일명("M0001.jpg")과 목록에 보이는 숫자가 달랐음(2026-08-02, 사용자 지적: CNTL이
 * 임의로 번호를 매기는 것처럼 보였던 원인) */
static void encode_file_seq_base36(uint32_t seq, char *out /* 5바이트: 4자리+NUL */)
{
    static const char digits[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    seq %= 1679616u;  /* 36^4 — CAM_STORAGE_SEQ_MOD와 동일 */
    for (int i = 3; i >= 0; i--) {
        out[i] = digits[seq % 36];
        seq /= 36;
    }
    out[4] = '\0';
}

/* Model: 선택 상태 그 자체(s_selected_file_id) 하나만 바꿈 — View/Action은 절대 안 건드림.
 * OnTap(cb_photo_row_select)/목록 재구성(refresh_photo_list_ui) 둘 다 "선택이 바뀌었다"는
 * 사실만 여기로 알림 */
static void set_selected_file_id(uint32_t file_id)
{
    s_selected_file_id = file_id;
    s_has_selected_file_id = true;
}

/* reconcile_selection — "View/Action은 Model의 그림자일 뿐"(2026-08-02, 사용자 지적)을
 * 실제로 구현: file_id를 인자로 안 받고 s_selected_file_id(모델)를 직접 읽어서 반영함.
 * 호출부(OnTap 등)가 "무엇을 선택했는지"를 여기 전달하는 게 아니라, 여기가 모델을 스스로
 * 관찰해서 반응하는 구조 — 그래야 "탭 이벤트 안에서 가져오기를 처리한다"는 게 안 됨(사용자가
 * 세 번째로 지적한 부분). 강조표시(뷰)는 매번 모델과 동기화하고, 가져오기(액션)는 마지막으로
 * 반영했던 값(s_synced_file_id)과 실제로 달라졌을 때만 함 — 이전 사진을 들고 있다가
 * 재사용하는 캐시 개념 없이, 선택이 바뀔 때마다 무조건 새로 받아옴(2026-08-02, 사용자 지시) */
static uint32_t s_synced_file_id = 0;
static bool     s_has_synced_file_id = false;

static void reconcile_selection(lv_obj_t *row, bool show_popup)
{
    if (s_selected_row && s_selected_row != row) {
        lv_obj_set_style_bg_opa(s_selected_row, LV_OPA_TRANSP, 0);
    }
    lv_obj_set_style_bg_color(row, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_30, 0);
    s_selected_row = row;

    if (s_has_synced_file_id && s_synced_file_id == s_selected_file_id) return;  /* 이미 반영됨 — 끝 */
    s_synced_file_id = s_selected_file_id;
    s_has_synced_file_id = true;

    if (!s_has_selected_cam) return;

    ui_log_add("SELECT file_id=%u", (unsigned)s_selected_file_id);

    /* 새 요청을 걸기 전에, 직전 사진이 방금 도착했는데(READY) 아직 판넬에 반영 안 된
     * 상태면 먼저 처리하고 넘어감 — esp_now_photo_fetch_by_id()가 새 요청 시작하면서
     * 상태를 무조건 IDLE로 되돌리므로, 그 전에 이걸 안 하면 도착한 사진을 영영 못 봄
     * (2026-08-01 실기에서 확인). 다만 이 시점엔 s_selected_file_id가 이미 "새" 선택으로
     * 바뀌어 있어서, 대기 중이던 READY는 대부분 "이전" 선택의 응답 — 그대로 그리면 안
     * 되고 consume_ready_photo_if_current()가 file_id 일치 여부를 확인해서 처리함
     * (2026-08-05, 선택-도착 불일치 버그 수정) */
    consume_ready_photo_if_current();

    esp_now_photo_fetch_by_id(s_selected_cam_mac, s_selected_file_id);
    if (show_popup) show_fetch_progress_popup();
}

/* OnTap — Model만 바꾸고(set_selected_file_id) reconcile_selection에 반영을 맡김. 탭
 * 핸들러 자신은 "어떤 행이 눌렸는지" 알아내는 것 이상은 하지 않음 */
static void cb_photo_row_select(lv_event_t *e)
{
    uint32_t file_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_target(e);
    set_selected_file_id(file_id);
    reconcile_selection(row, true);
}

static void cb_photo_delete_confirm(void *ctx)
{
    uint32_t file_id = (uint32_t)(uintptr_t)ctx;
    if (s_has_selected_cam) {
        esp_now_photo_delete(s_selected_cam_mac, file_id);
        esp_now_photo_list_request(s_selected_cam_mac);  /* 삭제 반영된 목록으로 갱신 */
    }
}

static void show_photo_delete_confirm(uint32_t file_id)
{
    show_confirm_popup(ui_str(STR_MSG_DELETE_PHOTO_CONFIRM), cb_photo_delete_confirm, (void *)(uintptr_t)file_id);
}

static void cb_photo_delete_btn(lv_event_t *e)
{
    uint32_t file_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    show_photo_delete_confirm(file_id);
}

/* 목록 제목 옆 "N개 XX%" / "N Pics XX%" 라벨 — 언어 전환 시(refresh_lang_texts)와
 * 목록 갱신 시(refresh_photo_list_ui) 둘 다에서 다시 그려야 해서 분리(2026-08-04).
 * "개(Pic.)" 표기는 괄호 안이 영문 모드일 때만 쓰는 표기라는 뜻이었음(사용자 정정:
 * 한글모드="N개", 영문모드="N Pic.", 둘 다 같이 보이면 안 됨) */
static void update_list_info_label(void)
{
    uint32_t sd_total_kb = 0, sd_used_kb = 0;
    esp_now_photo_list_get_sd_usage(&sd_total_kb, &sd_used_kb);
    bool en = (ui_lang_get() == UI_LANG_EN);
    char info_buf[32];
    if (sd_total_kb > 0) {
        unsigned pct = (unsigned)((uint64_t)sd_used_kb * 100 / sd_total_kb);
        snprintf(info_buf, sizeof(info_buf), en ? "%d Pics  %u%%" : "%d개  %u%%", s_current_list_count, pct);
    } else {
        snprintf(info_buf, sizeof(info_buf), en ? "%d Pics" : "%d개", s_current_list_count);
    }
    lv_label_set_text(s_list_info_label, info_buf);
}

/* select_index: 이 인덱스의 행을 선택 표시(예: 지금촬영 직후엔 0=최신). -1이면 선택 없음 */
static void refresh_photo_list_ui(int select_index)
{
    s_current_list_count = esp_now_photo_list_get_items(s_current_list, ESP_NOW_PHOTO_LIST_MAX);
    update_list_info_label();

    lv_indev_reset(NULL, s_photo_list);
    lv_obj_clean(s_photo_list);
    /* 뷰 캐시(행 객체 포인터)만 리셋 — 선택 모델(s_selected_file_id)은 목록이 다시
     * 그려져도 그대로 유지, 아래 루프에서 강조표시만 다시 그림(2026-08-02) */
    s_selected_row = NULL;

    if (s_current_list_count == 0) {
        /* 목록이 진짜 0장인지, 갱신 요청 자체가 응답을 못 받은 건지 구분이 안 된다는
         * 사용자 지적(2026-08-02) — 이 함수는 CAM한테서 실제로 목록이 도착했을 때만
         * 불리므로(무응답이면 아예 호출 안 됨) 여기 도달했다는 건 "진짜 0장"이 확정된
         * 것. 그걸 회색 문구로 명시 */
        lv_obj_t *empty_lbl = lv_label_create(s_photo_list);
        lv_label_set_text(empty_lbl, ui_str(STR_LIST_EMPTY));
        lv_obj_set_style_text_font(empty_lbl, ui_font_get(UI_FONT_SIZE_18), 0);
        lv_obj_set_style_text_color(empty_lbl, lv_palette_main(LV_PALETTE_GREY), 0);
        return;
    }

    for (int i = 0; i < s_current_list_count; i++) {
        lv_obj_t *row = lv_obj_create(s_photo_list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(row, 4, 0);      /* 기본 lv_obj 패딩 제거 — 리스트 박스 꽉 채움 */
        lv_obj_set_style_pad_ver(row, 6, 0);      /* 세로만 살짝 더 — 행 높이 ~20% 키움(2026-08-01, 사용자 지시) */
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb_photo_row_select, LV_EVENT_CLICKED,
                             (void *)(uintptr_t)s_current_list[i].file_id);

        /* 목록 번호는 위치 기반(i+1) 대신 CAM이 실제로 갖고 있는 file_id(+kind)를 그대로
         * 보여줌(2026-08-01, 사용자 지시 — 중간 삭제 시 번호가 밀리지 않게). file_id는
         * 더 이상 타임스탬프가 아니라서(CAM 재설계 참고) 촬영시각은 별도 capture_time
         * 필드(파일의 FAT 수정시각)로 표시 — file_id를 파싱해서 뽑지 않음 */
        time_t t = (time_t)s_current_list[i].capture_time;
        struct tm tm_buf;
        gmtime_r(&t, &tm_buf);
        char time_buf[24];
        strftime(time_buf, sizeof(time_buf), "%m-%d %H:%M:%S", &tm_buf);

        char seq_str[5];
        encode_file_seq_base36(s_current_list[i].file_id, seq_str);

        char buf[56];
        snprintf(buf, sizeof(buf), "%c%s  %s (%uKB)",
                 (char)s_current_list[i].kind, seq_str, time_buf,
                 (unsigned)(s_current_list[i].file_size / 1024));

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, buf);
        lv_obj_set_style_text_font(label, ui_font_get(UI_FONT_SIZE_18), 0);

        /* 삭제 버튼 — LVGL 기본 버튼 패딩이 터치용으로 커서 행 높이 전체가 이 버튼
         * 크기에 끌려 부풀어 있었음(row는 LV_SIZE_CONTENT라 가장 큰 자식에 맞춰짐).
         * 패딩을 줄여서 행 높이를 텍스트 기준으로 자연스럽게 줄임(2026-08-01, 사용자 지적) */
        lv_obj_t *del_btn = lv_button_create(row);
        lv_obj_set_style_pad_hor(del_btn, 8, 0);
        lv_obj_set_style_pad_ver(del_btn, 2, 0);
        lv_obj_add_event_cb(del_btn, cb_photo_delete_btn, LV_EVENT_CLICKED,
                             (void *)(uintptr_t)s_current_list[i].file_id);
        lv_obj_t *del_lbl = lv_label_create(del_btn);
        lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);

        if (i == select_index) {
            /* 새로운 선택(예: 지금촬영 직후 최신 항목) — Model만 바꾸고 반영은
             * reconcile_selection에 위임. 지금촬영 팝업이 이미 떠 있는 상태에서 호출되므로
             * 진행팝업은 또 안 띄움(show_popup=false) */
            set_selected_file_id(s_current_list[i].file_id);
            reconcile_selection(row, false);
        } else if (s_has_selected_file_id && s_current_list[i].file_id == s_selected_file_id) {
            /* 이미 선택돼 있던(모델 기준) 항목이 목록 재구성으로 다시 그려진 것뿐 —
             * 강조표시(뷰)만 모델에 맞춰 복원, 재요청은 안 함(2026-08-02) */
            lv_obj_set_style_bg_color(row, lv_palette_main(LV_PALETTE_BLUE), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_30, 0);
            s_selected_row = row;
        }
    }
}

/* CAM 목록 동기화 공용 — "요청 → 도착 대기 → UI 갱신" 트리플이 목록갱신 버튼/지금촬영
 * 팝업/모두지우기 팝업에 각각 필요해서 공통화(2026-08-01). request와 tick을 분리한 이유:
 * 요청은 한 번만 보내면 되고 tick은 매 폴링(200ms/1s)마다 불러 도착 여부만 확인하기 때문. */
static void request_photo_list_sync(void)
{
    esp_now_photo_list_request(s_selected_cam_mac);
}

/* READY 도착 시 select_index 행을 선택 표시하며 UI 갱신하고 true 반환(호출부가 다음
 * 단계로 넘어가거나 팝업을 닫는 데 씀) — 아직이면 false */
static bool sync_photo_list_tick(int select_index)
{
    if (esp_now_photo_list_get_state() != ESP_NOW_PHOTO_LIST_STATE_READY) return false;
    refresh_photo_list_ui(select_index);
    esp_now_photo_list_ack();
    return true;
}

/* 압축 JPEG를 target_w x target_h 근사치로 축소 디코드해서 out_buf(호출부가 미리 잡아둔
 * 고정 버퍼, 용량 out_cap)에 채움 — RGB565, LVGL 자동 디코더(esp_lv_decoder)는 scale 옵션이
 * 없어서 esp_jpeg_dec를 직접 부름(2026-08-01). scale이 8의 배수 제약이라 실제 디코드된
 * 크기가 요청값과 다를 수 있어서 out_w/out_h로 같이 돌려줌.
 * 예전엔 이 함수가 매번 jpeg_calloc_align으로 새로 할당했는데, 판넬/뷰어 둘 다 목표
 * 해상도가 고정이라 그럴 필요가 없고, 오히려 반복되는 free+malloc이 압축본 캐시 슬롯들과
 * 뒤섞이며 PSRAM을 조각내서 이 malloc 자체가 실패하는 원인이 됐음(recv 버퍼를 고정 크기로
 * 바꾼 것과 동일한 문제 — esp_now_photo.c 참고). 이제 버퍼는 호출부 소유, 여기선 안 잡고
 * 안 해제함 — 성공하면 true. */
static bool decode_jpeg_scaled(const uint8_t *jpeg_data, size_t jpeg_len,
                                uint16_t target_w, uint16_t target_h,
                                uint8_t *out_buf, size_t out_cap,
                                uint16_t *out_w, uint16_t *out_h, size_t *out_len)
{
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type  = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.scale.width   = target_w;
    config.scale.height  = target_h;

    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&config, &dec) != JPEG_ERR_OK || !dec) {
        ESP_LOGW(TAG, "jpeg_dec_open 실패");
        return false;
    }

    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpeg_data, .inbuf_len = (int)jpeg_len };
    jpeg_dec_header_info_t info = { 0 };
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg_dec_parse_header 실패");
        jpeg_dec_close(dec);
        return false;
    }

    int outbuf_len = 0;
    if (jpeg_dec_get_outbuf_len(dec, &outbuf_len) != JPEG_ERR_OK || outbuf_len <= 0) {
        ESP_LOGW(TAG, "jpeg_dec_get_outbuf_len 실패");
        jpeg_dec_close(dec);
        return false;
    }
    if ((size_t)outbuf_len > out_cap) {
        ESP_LOGE(TAG, "디코드 결과가 고정 버퍼보다 큼(%d > %u bytes)", outbuf_len, (unsigned)out_cap);
        jpeg_dec_close(dec);
        return false;
    }
    io.outbuf = out_buf;

    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg_dec_process 실패");
        jpeg_dec_close(dec);
        return false;
    }
    jpeg_dec_close(dec);

    *out_w   = (config.scale.width  != 0) ? config.scale.width  : info.width;
    *out_h   = (config.scale.height != 0) ? config.scale.height : info.height;
    *out_len = (size_t)outbuf_len;
    return true;
}

/* 디코드된 RGB565 픽셀버퍼를 lv_image_dsc_t에 채움(이미 디코드된 원시 픽셀이라 LVGL
 * 자동 디코더를 안 거침 — header.cf를 명시해야 함). header.magic도 반드시 채워야 함 —
 * LVGL 내장 lv_bin_decoder가 magic != LV_IMAGE_HEADER_MAGIC이면 크래시/에러 없이 그냥
 * 조용히 그리기를 거부함(2026-08-01 실기에서 확인: 판넬 미리보기가 안 뜨는데 로그도
 * 전혀 안 남던 원인) */
static void fill_rgb565_dsc(lv_image_dsc_t *dsc, uint8_t *pixel_buf, uint16_t w, uint16_t h, size_t len)
{
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc->header.w      = w;
    dsc->header.h      = h;
    dsc->header.stride = (uint32_t)w * 2;
    dsc->data          = pixel_buf;
    dsc->data_size      = len;
}

#define PHOTO_PANEL_DECODE_W 320
#define PHOTO_PANEL_DECODE_H 240

/* 목록에서 선택된 file_id를 캐시에서 판넬 크기로 디코드해서 대시보드 썸네일(s_photo_box)에
 * 반영 — 캐시에 없으면(아직 안 받아온 사진) 조용히 무시 */
static void display_photo(uint32_t file_id)
{
    if (!s_photo_jpeg_buf) {
        ESP_LOGE(TAG, "display_photo: 판넬 버퍼 없음(초기 할당 실패?)");
        return;
    }

    const uint8_t *jpeg_data = NULL;
    size_t jpeg_len = 0;
    if (!esp_now_photo_cache_get(file_id, &jpeg_data, &jpeg_len)) {
        ESP_LOGW(TAG, "display_photo: 캐시에 없음(file_id=%u)", (unsigned)file_id);
        ui_log_add("DISPLAY 캐시MISS file_id=%u", (unsigned)file_id);
        return;
    }
    ESP_LOGI(TAG, "display_photo: file_id=%u jpeg_len=%u jpeg_data=%p",
             (unsigned)file_id, (unsigned)jpeg_len, (void *)jpeg_data);
    ui_log_add("DISPLAY file_id=%u jpeg_len=%u", (unsigned)file_id, (unsigned)jpeg_len);

    uint16_t w = 0, h = 0;
    size_t pixel_len = 0;
    if (!decode_jpeg_scaled(jpeg_data, jpeg_len, PHOTO_PANEL_DECODE_W, PHOTO_PANEL_DECODE_H,
                             s_photo_jpeg_buf, PHOTO_PANEL_BUF_CAP, &w, &h, &pixel_len)) {
        ESP_LOGE(TAG, "display_photo: decode_jpeg_scaled 실패(file_id=%u)", (unsigned)file_id);
        ui_log_add_err(UI_ERR_DECODE_FAIL, "사진 표시 실패(디코드) file_id=%u", (unsigned)file_id);
        return;
    }
    ESP_LOGI(TAG, "display_photo: decode OK w=%u h=%u pixel_len=%u buf=%p",
             w, h, (unsigned)pixel_len, (void *)s_photo_jpeg_buf);
    ui_log_add("DISPLAY decode OK file_id=%u w=%u h=%u", (unsigned)file_id, w, h);

    fill_rgb565_dsc(&s_photo_dsc, s_photo_jpeg_buf, w, h, pixel_len);
    /* 버퍼 내용은 바뀌었지만 &s_photo_dsc 주소는 고정이라, LVGL의 이미지 캐시가 그 주소를
     * 키로 삼아 예전 사진을 그대로 다시 그리는 버그가 있었음(2026-08-01 실기 확인) —
     * 매번 갱신 전에 캐시를 무효화해야 함 */
    lv_image_cache_drop(&s_photo_dsc);

    if (!s_photo_image) {
        lv_obj_clean(s_photo_box);  /* "사진 없음" 플레이스홀더 라벨 제거 */
        s_camera_photo_label = NULL;  /* 방금 지워짐 — refresh_lang_texts()가 참조하지 않게 */
        s_photo_image = lv_image_create(s_photo_box);
        lv_obj_set_size(s_photo_image, LV_PCT(100), LV_PCT(100));
        lv_image_set_inner_align(s_photo_image, LV_IMAGE_ALIGN_CONTAIN);
    }
    lv_image_set_src(s_photo_image, &s_photo_dsc);
    ui_log_add("DISPLAY set_src 완료 file_id=%u", (unsigned)file_id);
}

/* READY 상태 사진을 "지금 선택된 항목과 일치할 때만" 화면에 반영(2026-08-05) — 취소가
 * 실제 거래를 못 끝내는 문제(위 cb_progress_popup_cancel 참고)를 고쳤어도, 방어적으로
 * 한 번 더 확인함. 안 맞으면(이미 다른 걸 선택해서 이 응답은 낡은 것) 조용히 버리지
 * 않고 에러로 표시(사용자 지시: "2번도 에러니까 안 그리는 것보다 에러를 띄워줘") +
 * 지금 선택된 항목을 다시 요청 — 그 사이 busy로 무시됐을 수 있는 진짜 요청을 벌충함
 * (start_single_receive()의 기존 busy 가드가 중복 무선 전송은 로컬에서 안전하게 막음) */
static void consume_ready_photo_if_current(void)
{
    if (esp_now_photo_get_state() != ESP_NOW_PHOTO_STATE_READY) return;
    uint32_t ready_id = esp_now_photo_get_ready_file_id();
    esp_now_photo_ready_ack();
    if (s_has_selected_file_id && ready_id == s_selected_file_id) {
        display_photo(ready_id);
        return;
    }
    ui_log_add_err(UI_ERR_PHOTO_SELECTION_STALE,
                    "도착 file_id=%u != 선택 file_id=%u — 무시하고 재요청",
                    (unsigned)ready_id, (unsigned)s_selected_file_id);
    if (s_has_selected_cam && s_has_selected_file_id) {
        esp_now_photo_fetch_by_id(s_selected_cam_mac, s_selected_file_id);
    }
}

/* ════════════════════════════════════════════════════════════
 * CAM(추후 SENS) 진행 팝업 공용 틀 — 오버레이 + "취소" 버튼(언제든 닫기) + 200ms 폴링
 * 타이머 + 완료시 자동 닫힘. 지금촬영/모두지우기 등 CAM과 통신하며 단계를 보여주는 모든
 * 흐름이 공유(2026-08-01) — 호출부는 tick_fn만 공급: box 안에 자기 stage 라벨을 채우고,
 * true를 반환하면 완료로 보고 팝업이 자동으로 닫힘. "취소"는 로컬 UI만 닫을 뿐 CAM에
 * 보낸 요청 자체를 취소하지는 않음(이 프로토콜에 그런 abort 메시지가 없음) — CAM은 계속
 * 처리하고 응답이 오면 esp_now_photo 쪽 상태는 갱신되지만 화면에 반영은 안 됨.
 * ════════════════════════════════════════════════════════════ */
typedef bool (*progress_tick_fn_t)(lv_obj_t *box);  /* true=완료, 팝업 자동 닫힘 */

/* CAM 응답을 무한정 기다리지 않기 위한 공용 타임아웃 — 지금촬영/모두지우기/사진가져오기가
 * 전부 이 값을 씀(각자 계산 기준은 다를 수 있음: 총 경과시간 vs 마지막 진행 이후 경과시간) */
#define CAM_RESPONSE_TIMEOUT_MS 8000

static lv_obj_t          *s_progress_popup_overlay = NULL;
static lv_obj_t          *s_progress_popup_box = NULL;
static lv_timer_t        *s_progress_popup_timer = NULL;
static progress_tick_fn_t s_progress_tick_fn = NULL;
static lv_obj_t          *s_progress_popup_cancel_btn = NULL;
static lv_obj_t          *s_progress_popup_cancel_lbl = NULL;
static bool               s_progress_popup_cancel_requested = false;

static void close_progress_popup(void)
{
    if (s_progress_popup_timer) {
        lv_timer_delete(s_progress_popup_timer);
        s_progress_popup_timer = NULL;
    }
    if (s_progress_popup_overlay) {
        lv_obj_delete(s_progress_popup_overlay);
        s_progress_popup_overlay = NULL;
    }
    s_progress_popup_box = NULL;
    s_progress_tick_fn = NULL;
    s_progress_popup_cancel_btn = NULL;
    s_progress_popup_cancel_lbl = NULL;
    s_progress_popup_cancel_requested = false;
    resume_bg_timers();
}

/* "취소"는 먹통 방지를 위한 심리적 안전장치로만 남기고, 실제 거래(CAM으로 보낸 요청)는
 * 취소할 방법이 프로토콜에 없어서 그대로 계속 진행됨(2026-08-05, 사용자 지시) — 예전엔
 * 여기서 바로 close_progress_popup()을 불러서 모달/배경타이머를 즉시 풀어버렸는데, 그
 * 직후 사용자가 다른 항목을 선택하면 아직 끝나지 않은 이전 요청의 뒤늦은 응답이 새
 * 선택 위에 잘못 표시되는 경쟁 상태로 이어짐(실기에서 재현: "선택한 사진과 다른 사진이
 * 보임"). 이제는 라벨/버튼만 "종료 대기 중"으로 바꾸고 모달은 유지 — 각 흐름의
 * tick_fn이 실제 완료(READY/ERROR)나 자체 타임아웃(CAM_RESPONSE_TIMEOUT_MS)을 만나
 * true를 반환할 때만 진짜로 닫힘(아래 progress_popup_tick, 안 건드림) — 그래서 무한정
 * 막히진 않고 상한이 있음 */
static void cb_progress_popup_cancel(lv_event_t *e)
{
    (void)e;
    if (s_progress_popup_cancel_requested) return;  /* 중복 클릭 무시 */
    s_progress_popup_cancel_requested = true;
    if (s_progress_popup_cancel_btn) lv_obj_add_state(s_progress_popup_cancel_btn, LV_STATE_DISABLED);
    if (s_progress_popup_cancel_lbl) lv_label_set_text(s_progress_popup_cancel_lbl, ui_str(STR_BTN_CANCEL_PENDING));
}

static void progress_popup_tick(lv_timer_t *t)
{
    (void)t;
    if (s_progress_tick_fn && s_progress_tick_fn(s_progress_popup_box)) {
        close_progress_popup();
    }
}

/* box를 반환 — 호출부가 여기에 자기 stage 라벨을 채운 뒤 반드시 start_progress_popup()을
 * 마지막으로 호출해서 취소 버튼(라벨들 아래로 와야 함)+타이머를 붙여야 함 */
static lv_obj_t *show_progress_popup(progress_tick_fn_t tick_fn)
{
    lv_obj_t *box = create_modal();  /* pause_bg_timers()도 여기서 같이 됨 */
    s_progress_popup_overlay = lv_obj_get_parent(box);
    s_progress_popup_box = box;
    s_progress_tick_fn = tick_fn;
    s_progress_popup_cancel_requested = false;
    return box;
}

static void start_progress_popup(lv_obj_t *box)
{
    lv_obj_t *btn_row = create_modal_btn_row(box);
    s_progress_popup_cancel_btn = add_modal_button(btn_row, STR_BTN_CANCEL, cb_progress_popup_cancel, NULL);
    s_progress_popup_cancel_lbl = lv_obj_get_child(s_progress_popup_cancel_btn, 0);
    s_progress_popup_timer = lv_timer_create(progress_popup_tick, 200, NULL);
}

static void set_stage_label(lv_obj_t **labels, int idx, ui_str_id_t str_id, lv_color_t color)
{
    lv_label_set_text(labels[idx], ui_str(str_id));
    lv_obj_set_style_text_color(labels[idx], color, 0);
}

/* ════════════════════════════════════════════════════════════
 * 지금촬영 진행 팝업 — 1.명령전달 2.촬영결과 3.목록갱신. 모두지우기와 동일 원칙: 2단계
 * (촬영성공/실패/무응답) 결과가 뭐든 3단계(목록 재조회)는 항상 실행하고, 그것마저
 * 타임아웃되면 "상태 확인 불가"만 보여줌(2026-08-01 — 예전엔 촬영 성공 시 CAM이 사진을
 * 자동 전송하는 4단계가 있었는데, 느리고 실기에서 자주 실패해서 팝업이 안 끝나는 문제가
 * 반복돼 촬영과 전송을 분리함. 사진을 실제로 보려면 목록에서 선택 — cb_photo_row_select
 * 아래 참고) */
typedef enum {
    CAPTURE_POPUP_STAGE_WAIT_RESULT = 0,
    CAPTURE_POPUP_STAGE_SYNC_LIST = 1,
} capture_popup_stage_t;

static capture_popup_stage_t s_capture_popup_stage;
static uint32_t              s_capture_popup_stage_start_ms;

static bool capture_popup_tick_fn(lv_obj_t *box)
{
    (void)box;
    lv_color_t grey  = lv_palette_main(LV_PALETTE_GREY);
    lv_color_t green = lv_palette_main(LV_PALETTE_GREEN);
    lv_color_t red   = lv_palette_main(LV_PALETTE_RED);

    if (s_capture_popup_stage == CAPTURE_POPUP_STAGE_WAIT_RESULT) {
        esp_now_capture_stage_t stage = esp_now_photo_get_capture_stage();

        if (stage == ESP_NOW_CAPTURE_STAGE_SENT) {
            set_stage_label(s_capture_stage_label, 0, STR_CAPTURE_STAGE1_PROGRESS, grey);
        } else if (stage == ESP_NOW_CAPTURE_STAGE_ACKED) {
            set_stage_label(s_capture_stage_label, 0, STR_CAPTURE_STAGE1_DONE, green);
        }

        bool resolved  = (stage == ESP_NOW_CAPTURE_STAGE_CAPTURED || stage == ESP_NOW_CAPTURE_STAGE_CAPTURE_FAILED);
        bool timedout  = !resolved && lv_tick_elaps(s_capture_popup_stage_start_ms) > CAM_RESPONSE_TIMEOUT_MS;
        if (!resolved && !timedout) return false;

        if (!resolved) {
            /* 진짜 무응답 — CAM과 통신 자체가 안 되는 상태라 이어서 목록을 확인해봤자
             * 똑같이 타임아웃될 뿐이라 의미 없음(2026-08-02, 사용자 지적: "응답이 없는데
             * 목록 갱신 중은 왜 하는거야?"). 여기서 바로 닫음.
             * 라벨에 NORESPONSE를 써도 이 함수가 true를 반환하는 즉시 progress_popup_tick이
             * 같은 틱 안에서 팝업을 지워버려서 화면엔 실제로 한 번도 안 그려짐(2026-08-02,
             * 사용자 지적: "그냥 대기 프로그레스만 돌다가 닫혀") — ui_log_add_err의
             * 토스트/경고아이콘이 실제 사용자에게 보이는 유일한 통보 경로라 반드시 호출 */
            set_stage_label(s_capture_stage_label, 1, STR_CAPTURE_STAGE2_NORESPONSE, red);
            ui_log_add_err(UI_ERR_CAPTURE_NORESPONSE, "지금촬영 요청에 CAM 응답 없음(타임아웃)");
            return true;
        }

        bool ok = (stage == ESP_NOW_CAPTURE_STAGE_CAPTURED);
        set_stage_label(s_capture_stage_label, 0, STR_CAPTURE_STAGE1_DONE, green);
        set_stage_label(s_capture_stage_label, 1, ok ? STR_CAPTURE_STAGE2_SUCCESS : STR_CAPTURE_STAGE2_FAILED,
                         ok ? green : red);
        esp_now_photo_capture_stage_clear();

        /* 2단계가 성공이든 실패든(무응답은 위에서 이미 처리하고 끝났음) 목록은 항상
         * 다시 확인 — 실패 응답이어도 목록 갱신 자체는 무해(목록이 그대로 옴) */
        request_photo_list_sync();
        s_capture_popup_stage = CAPTURE_POPUP_STAGE_SYNC_LIST;
        s_capture_popup_stage_start_ms = lv_tick_get();
        set_stage_label(s_capture_stage_label, 2, STR_CAPTURE_STAGE3_PROGRESS, grey);
        return false;
    }

    /* CAPTURE_POPUP_STAGE_SYNC_LIST */
    if (sync_photo_list_tick(0)) {  /* 방금 찍었으면 목록에서 가장 최신 = index 0 */
        set_stage_label(s_capture_stage_label, 2, STR_CAPTURE_STAGE3_DONE, green);
        return true;
    }
    if (lv_tick_elaps(s_capture_popup_stage_start_ms) > CAM_RESPONSE_TIMEOUT_MS) {
        set_stage_label(s_capture_stage_label, 2, STR_CAPTURE_STAGE3_UNKNOWN, red);
        return true;
    }
    return false;
}

static void show_capture_popup(void)
{
    s_capture_popup_stage = CAPTURE_POPUP_STAGE_WAIT_RESULT;
    s_capture_popup_stage_start_ms = lv_tick_get();

    lv_obj_t *box = show_progress_popup(capture_popup_tick_fn);

    for (int i = 0; i < 3; i++) {
        s_capture_stage_label[i] = lv_label_create(box);
        lv_obj_set_style_text_font(s_capture_stage_label[i], ui_font_get(UI_FONT_SIZE_18), 0);
        lv_label_set_text(s_capture_stage_label[i], "");
    }
    set_stage_label(s_capture_stage_label, 0, STR_CAPTURE_STAGE1_PROGRESS, lv_palette_main(LV_PALETTE_GREY));

    start_progress_popup(box);
}

static void cb_capture_now(lv_event_t *e)
{
    (void)e;
    if (!s_has_selected_cam) return;
    show_capture_popup();
    esp_now_photo_capture_now(s_selected_cam_mac);
}

/* ════════════════════════════════════════════════════════════
 * 목록에서 사진 선택 → 가져오기 진행 팝업 — 모래시계(스피너) + 퍼센트 + 남은시간 추정.
 * 청크가 한동안 안 늘면(무응답/정체) 실패로 간주 — 총 경과시간이 아니라 "마지막 진행
 * 이후 경과시간" 기준(파일이 커서 원래 오래 걸리는 것과 진짜 멈춘 것을 구분하기 위해,
 * 2026-08-01). 완료(READY)/실패(ERROR) 판정만 하고 실제 사진 표시는 팝업이 닫힌 뒤
 * refresh_dashboard()의 일반 수신 처리 경로가 함(지금촬영과 동일 원칙).
 * ════════════════════════════════════════════════════════════ */
static lv_obj_t *s_fetch_progress_label = NULL;
static uint32_t  s_fetch_start_ms;
static uint32_t  s_fetch_last_progress_ms;
static uint16_t  s_fetch_last_received;

static bool fetch_popup_tick_fn(lv_obj_t *box)
{
    (void)box;
    esp_now_photo_state_t state = esp_now_photo_get_state();

    if (state == ESP_NOW_PHOTO_STATE_READY) {
        lv_label_set_text(s_fetch_progress_label, ui_str(STR_FETCH_DONE));
        lv_obj_set_style_text_color(s_fetch_progress_label, lv_palette_main(LV_PALETTE_GREEN), 0);
        return true;
    }
    if (state == ESP_NOW_PHOTO_STATE_ERROR) {
        lv_label_set_text(s_fetch_progress_label, ui_str(STR_FETCH_FAILED));
        lv_obj_set_style_text_color(s_fetch_progress_label, lv_palette_main(LV_PALETTE_RED), 0);
        esp_now_photo_clear();
        return true;
    }

    uint16_t received = 0, total = 0;
    esp_now_photo_get_chunk_progress(&received, &total);

    if (received != s_fetch_last_received) {
        s_fetch_last_received = received;
        s_fetch_last_progress_ms = lv_tick_get();
    } else if (lv_tick_elaps(s_fetch_last_progress_ms) > CAM_RESPONSE_TIMEOUT_MS) {
        /* 라벨에 STALLED를 써도 true 반환 즉시 팝업이 같은 틱에서 지워져서 실제로는
         * 한 번도 화면에 안 그려짐(2026-08-02, 사용자 지적 — capture_popup_tick_fn의
         * NORESPONSE와 동일한 문제) — ui_log_add_err의 토스트가 실제 통보 경로 */
        lv_label_set_text(s_fetch_progress_label, ui_str(STR_FETCH_STALLED));
        lv_obj_set_style_text_color(s_fetch_progress_label, lv_palette_main(LV_PALETTE_RED), 0);
        ui_log_add_err(UI_ERR_FETCH_NORESPONSE, "사진 가져오기 진행 정체(%u/%u청크, 타임아웃)",
                        (unsigned)received, (unsigned)total);
        return true;
    }

    if (total == 0) {
        lv_label_set_text(s_fetch_progress_label, ui_str(STR_FETCH_CONNECTING));
        return false;
    }

    int percent = (int)((uint32_t)received * 100 / total);
    if (received > 0) {
        /* 정수 연산만(이 코드베이스는 lv_label_set_text_fmt에 %f를 못 씀) —
         * 남은 청크 수 * (지금까지 걸린 시간/받은 청크 수) */
        uint32_t elapsed_ms = lv_tick_elaps(s_fetch_start_ms);
        uint32_t eta_ms = (uint32_t)(total - received) * elapsed_ms / received;
        lv_label_set_text_fmt(s_fetch_progress_label, ui_str(STR_FETCH_PROGRESS_ETA_FMT),
                               percent, (int)(eta_ms / 1000));
    } else {
        lv_label_set_text_fmt(s_fetch_progress_label, ui_str(STR_FETCH_PROGRESS_FMT), percent);
    }
    lv_obj_set_style_text_color(s_fetch_progress_label, lv_palette_main(LV_PALETTE_GREY), 0);
    return false;
}

static void show_fetch_progress_popup(void)
{
    s_fetch_start_ms         = lv_tick_get();
    s_fetch_last_progress_ms = s_fetch_start_ms;
    s_fetch_last_received    = 0;

    lv_obj_t *box = show_progress_popup(fetch_popup_tick_fn);

    lv_obj_t *spinner = lv_spinner_create(box);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 0);

    s_fetch_progress_label = lv_label_create(box);
    lv_obj_set_style_text_font(s_fetch_progress_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_text(s_fetch_progress_label, ui_str(STR_FETCH_CONNECTING));

    start_progress_popup(box);
}

/* 목록갱신 버튼 — 예전엔 요청만 보내고 끝이라 응답이 없어도 사용자가 알 방법이
 * 없었음(2026-08-02, 사용자 지적: "아무 짓도 안하는 건지 목록이 없는 건지 모르겠다") —
 * 지금촬영/모두지우기/사진가져오기와 같은 공용 진행팝업+타임아웃 토스트로 통일 */
static uint32_t s_renew_list_start_ms;

static bool renew_list_tick_fn(lv_obj_t *box)
{
    (void)box;
    if (sync_photo_list_tick(-1)) return true;
    if (lv_tick_elaps(s_renew_list_start_ms) > CAM_RESPONSE_TIMEOUT_MS) {
        ui_log_add_err(UI_ERR_LIST_NORESPONSE, "목록 갱신 요청에 CAM 응답 없음(타임아웃)");
        return true;
    }
    return false;
}

static void cb_renew_list(lv_event_t *e)
{
    (void)e;
    if (!s_has_selected_cam) return;
    esp_now_photo_list_request(s_selected_cam_mac);

    s_renew_list_start_ms = lv_tick_get();
    lv_obj_t *box = show_progress_popup(renew_list_tick_fn);

    lv_obj_t *spinner = lv_spinner_create(box);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *label = lv_label_create(box);
    lv_obj_set_style_text_font(label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_text(label, ui_str(STR_LIST_RENEW_PROGRESS));

    start_progress_popup(box);
}

/* ════════════════════════════════════════════════════════════
 * 모두 지우기 진행 팝업 — 1.삭제 2.목록갱신. 1단계 결과(성공/실패/무응답)와 무관하게
 * 2단계(목록 재조회)는 항상 실행 — 부분 삭제 가능성 때문에 로컬 목록을 신뢰하지 않고
 * CAM에서 실제 남은 목록을 다시 받아와야만 화면이 진실을 반영함(2026-08-01, 실기 테스트
 * 전 설계 논의로 확정). 2단계마저 타임아웃되면 "상태 확인 불가"만 보여주고 기존 목록은
 * 그대로 둠(성공한 것처럼 지우지 않음) — 사용자가 나중에 목록갱신으로 직접 재확인.
 * ════════════════════════════════════════════════════════════ */
typedef enum {
    DELETE_ALL_STAGE_WAIT_ACK = 0,
    DELETE_ALL_STAGE_SYNC_LIST = 1,
} delete_all_stage_t;

static lv_obj_t          *s_delete_all_stage_label[2];
static delete_all_stage_t s_delete_all_stage;
static uint32_t           s_delete_all_stage_start_ms;

static bool delete_all_tick_fn(lv_obj_t *box)
{
    (void)box;
    lv_color_t grey  = lv_palette_main(LV_PALETTE_GREY);
    lv_color_t green = lv_palette_main(LV_PALETTE_GREEN);
    lv_color_t red   = lv_palette_main(LV_PALETTE_RED);

    if (s_delete_all_stage == DELETE_ALL_STAGE_WAIT_ACK) {
        esp_now_delete_all_state_t st = esp_now_photo_delete_all_get_state();
        bool acked   = (st == ESP_NOW_DELETE_ALL_STATE_ACKED);
        bool timedout = !acked && lv_tick_elaps(s_delete_all_stage_start_ms) > CAM_RESPONSE_TIMEOUT_MS;
        if (!acked && !timedout) return false;

        if (acked) {
            bool ok = esp_now_photo_delete_all_get_success();
            set_stage_label(s_delete_all_stage_label, 0,
                             ok ? STR_DELETEALL_STAGE1_DONE : STR_DELETEALL_STAGE1_FAILED, ok ? green : red);
            esp_now_photo_delete_all_clear();
        } else {
            set_stage_label(s_delete_all_stage_label, 0, STR_DELETEALL_STAGE1_NORESPONSE, red);
        }
        /* 1단계가 성공/실패/무응답 무엇이든 실제 상태는 CAM에 다시 물어봐야 앎 */
        request_photo_list_sync();
        s_delete_all_stage = DELETE_ALL_STAGE_SYNC_LIST;
        s_delete_all_stage_start_ms = lv_tick_get();
        set_stage_label(s_delete_all_stage_label, 1, STR_DELETEALL_STAGE2_PROGRESS, grey);
        return false;
    }

    /* DELETE_ALL_STAGE_SYNC_LIST */
    if (sync_photo_list_tick(-1)) {  /* 전체삭제 후라 특정 선택 없음 */
        set_stage_label(s_delete_all_stage_label, 1, STR_DELETEALL_STAGE2_DONE, green);
        return true;
    }
    if (lv_tick_elaps(s_delete_all_stage_start_ms) > CAM_RESPONSE_TIMEOUT_MS) {
        set_stage_label(s_delete_all_stage_label, 1, STR_DELETEALL_STAGE2_UNKNOWN, red);
        return true;  /* 포기하고 닫되, 기존 목록엔 손 안 댐 */
    }
    return false;
}

static void cb_delete_all_confirmed(void *ctx)
{
    (void)ctx;
    esp_now_photo_delete_all(s_selected_cam_mac);

    s_delete_all_stage = DELETE_ALL_STAGE_WAIT_ACK;
    s_delete_all_stage_start_ms = lv_tick_get();

    lv_obj_t *box = show_progress_popup(delete_all_tick_fn);
    for (int i = 0; i < 2; i++) {
        s_delete_all_stage_label[i] = lv_label_create(box);
        lv_obj_set_style_text_font(s_delete_all_stage_label[i], ui_font_get(UI_FONT_SIZE_18), 0);
        lv_label_set_text(s_delete_all_stage_label[i], "");
    }
    set_stage_label(s_delete_all_stage_label, 0, STR_DELETEALL_STAGE1_PROGRESS, lv_palette_main(LV_PALETTE_GREY));
    start_progress_popup(box);
}

static void cb_delete_all(lv_event_t *e)
{
    (void)e;
    if (!s_has_selected_cam) return;
    show_confirm_popup(ui_str(STR_MSG_DELETE_ALL_CONFIRM), cb_delete_all_confirmed, NULL);
}

/* CAM 연결이 끊기는 그 순간에 목록/사진개수/SD사용량/미리보기를 전부 비움(2026-08-01,
 * 사용자 지적) — 예전엔 연결 끊기면 화면(camera_content/split_row)만 숨겼을 뿐 내용은
 * 그대로 남아있어서, 재연결하면 목록을 다시 안 가져왔는데도 예전 값이 그대로(꽉 찬 채로)
 * 보이는 문제가 있었음. 연결될 때가 아니라 끊길 때 지워야 함 — 재연결 시엔 사용자가
 * 목록갱신을 누르거나 자동 동기화로 새로 채워짐 */
static void reset_camera_ui_state(void)
{
    s_current_list_count = 0;
    lv_indev_reset(NULL, s_photo_list);
    lv_obj_clean(s_photo_list);
    s_selected_row = NULL;
    s_has_selected_file_id = false;  /* 연결이 끊기면 선택 모델도 완전히 비움(재연결 후
                                       * 예전 목록에 있던 file_id가 새 목록에 우연히 같은
                                       * 값으로 있어도 잘못 선택된 것처럼 보이지 않게) */
    s_has_synced_file_id = false;    /* reconcile_selection의 "마지막 반영값" 기록도 같이
                                       * 비움 — 안 그러면 재연결 후 같은 file_id를 다시
                                       * 선택했을 때 "이미 반영됨"으로 오판해 새로 안 가져옴 */
    lv_label_set_text(s_list_info_label, "");

    if (s_photo_image) {
        lv_obj_clean(s_photo_box);
        s_photo_image = NULL;
        s_camera_photo_label = lv_label_create(s_photo_box);
        lv_label_set_text(s_camera_photo_label, ui_str(STR_PANEL_NO_PHOTO_YET));
        lv_obj_set_style_text_font(s_camera_photo_label, ui_font_get(UI_FONT_SIZE_18), 0);
        lv_obj_set_style_text_color(s_camera_photo_label, lv_palette_main(LV_PALETTE_GREY), 0);
    }
}

/* CAM 선택 적용 — 드롭다운 수동 탭(cb_camera_select_changed)과 refresh_dashboard의 자동 폴백
 * (선택된 CAM이 언페어되거나 처음 페어링될 때) 둘 다 이 함수 하나로 수렴시켜서 동작을
 * 일관되게 함(2026-08-05). mac이 지금 선택과 같으면 아무것도 안 함(불필요한 재요청/화면
 * 깜빡임 방지) — 실제로 바뀔 때만 옛 CAM의 목록/미리보기를 비우고(reset_camera_ui_state,
 * 원래 "연결 끊길 때"만 쓰던 함수를 재사용 — 의미가 정확히 들어맞음: 어느 쪽이든 지금
 * 화면에 있는 목록/사진이 더 이상 유효하지 않다는 뜻) 새 CAM 목록을 자동으로 가져옴 */
static void select_camera(const uint8_t *mac)
{
    if (s_has_selected_cam && memcmp(s_selected_cam_mac, mac, 6) == 0) return;
    memcpy(s_selected_cam_mac, mac, 6);
    s_has_selected_cam = true;
    reset_camera_ui_state();
    esp_now_photo_list_request(s_selected_cam_mac);
}

static void cb_camera_select_changed(lv_event_t *e)
{
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    uint16_t idx = lv_dropdown_get_selected(dd);
    if (idx < (uint16_t)s_cam_dd_count) select_camera(s_cam_dd_macs[idx]);
}

/* 페어링된 CAM 이름 목록이 실제로 바뀌었을 때만 드롭다운 옵션을 다시 그림 — 매초 무조건
 * 다시 그리면 사용자가 마침 드롭다운을 열어보고 있을 때 깜빡이거나 닫혀버림. 옵션 문자열과
 * 동시에 s_cam_dd_macs(인덱스->mac 매핑)도 같이 갱신 */
static void rebuild_camera_dropdown_if_changed(const esp_now_hub_node_t *nodes, const uint8_t macs[][6], int count)
{
    char options[ESP_NOW_HUB_MAX_NODES * (ESP_NOW_LINK_NAME_LEN + 1)];
    size_t off = 0;
    for (int i = 0; i < count; i++) {
        int n = snprintf(options + off, sizeof(options) - off, "%s%s",
                          i > 0 ? "\n" : "", nodes[i].name);
        if (n < 0 || (size_t)n >= sizeof(options) - off) break;
        off += (size_t)n;
    }
    options[off] = '\0';

    static char s_prev_options[sizeof(options)] = "";
    if (strcmp(options, s_prev_options) == 0) return;
    strcpy(s_prev_options, options);

    lv_dropdown_set_options(s_camera_select_dd, count > 0 ? options : "");
    s_cam_dd_count = count;
    for (int i = 0; i < count; i++) memcpy(s_cam_dd_macs[i], macs[i], 6);
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

    /* 판넬3: 카메라 — 페어링된 CAM을 전부 모아 드롭다운을 채우고, 지금 선택된 CAM이 여전히
     * 그 안에 있으면 유지·아니면 첫 번째로 자동 폴백(2026-08-05, 여러 CAM 동시 페어링 지원
     * — select_camera()/rebuild_camera_dropdown_if_changed() 참고, 위 함수 설명 참고) */
    esp_now_hub_node_t cam_nodes[ESP_NOW_HUB_MAX_NODES];
    uint8_t            cam_macs[ESP_NOW_HUB_MAX_NODES][6];
    int cam_count = 0;
    for (int i = 0; i < total; i++) {
        if (s_dash_nodes[i].paired && s_dash_nodes[i].kind == HUB_NODE_KIND_CAM) {
            cam_nodes[cam_count] = s_dash_nodes[i];
            memcpy(cam_macs[cam_count], s_dash_nodes[i].mac, 6);
            cam_count++;
        }
    }
    rebuild_camera_dropdown_if_changed(cam_nodes, cam_macs, cam_count);

    bool camera_connected = (cam_count > 0);
    if (camera_connected) {
        int  selected_idx = 0;
        bool still_valid  = false;
        for (int i = 0; i < cam_count; i++) {
            if (s_has_selected_cam && memcmp(cam_macs[i], s_selected_cam_mac, 6) == 0) {
                still_valid  = true;
                selected_idx = i;
                break;
            }
        }
        select_camera(still_valid ? cam_macs[selected_idx] : cam_macs[0]);
        lv_dropdown_set_selected(s_camera_select_dd, (uint16_t)(still_valid ? selected_idx : 0));
    } else if (s_has_selected_cam) {
        s_has_selected_cam = false;
        reset_camera_ui_state();
    }
    if (camera_connected) {
        lv_obj_add_flag(s_camera_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_camera_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_camera_split_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_camera_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_camera_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_camera_split_row, LV_OBJ_FLAG_HIDDEN);
    }

    /* 사진 수신 폴링 — recv_cb(ESP-NOW 태스크)가 CRC까지 검증해서 캐시에 넣고 READY로
     * 표시해두면 여기(LVGL 워커 태스크)서 그 file_id를 캐시에서 판넬 크기로 디코드해 그림.
     * LVGL 호출은 항상 이 태스크에서만 해야 해서 esp_now_photo 쪽은 상태 플래그/캐시만
     * 들고 있고 디코드+그리는 건 UI 쪽 책임 */
    esp_now_photo_state_t photo_state = esp_now_photo_get_state();
    if (photo_state == ESP_NOW_PHOTO_STATE_READY) {
        consume_ready_photo_if_current();  /* 2026-08-05 — 지금 선택과 일치할 때만 반영 */
    } else if (photo_state == ESP_NOW_PHOTO_STATE_ERROR) {
        esp_now_photo_clear();  /* 실패 로그는 esp_now_photo.c 쪽에서 이미 남김 */
    }

    /* 목록갱신 버튼(cb_renew_list)으로 요청한 목록 — 지금촬영/모두지우기 팝업 쪽 목록 완료
     * 처리는 각자의 진행 팝업 tick이 하는데, 그동안은 이 타이머 자체가 pause_bg_timers()로
     * 멈춰있어서 여기와 겹칠 일이 없음 */
    sync_photo_list_tick(-1);
}

/* 통계 탭 로그박스 갱신 — ui_log 모듈에 쌓인 스냅샷을 그대로 라벨에 채우고 항상 맨
 * 아래(최신)로 스크롤. 내용이 안 바뀌었으면 다시 안 그림(길이만 비교 — 완벽하진 않지만
 * 이 용도로는 충분) */
static void refresh_log_box(lv_timer_t *t)
{
    (void)t;
    static char snapshot[3072];
    static size_t last_len = 0;

    ui_log_get_snapshot(snapshot, sizeof(snapshot));
    size_t len = strlen(snapshot);
    if (len == last_len) return;
    last_len = len;

    lv_label_set_text(s_log_label, snapshot);
    lv_obj_scroll_to_y(s_log_container, LV_COORD_MAX, LV_ANIM_OFF);
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
    char time_buf[12];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm_buf);

    /* 시간 옆에 WiFi 채널을 항상 같이 표기(2026-08-02, 사용자 지시) — 공유기
     * 자동채널선택으로 세션 중간에 채널이 바뀌는 걸 실기에서 확인했는데, 그동안은
     * 시리얼 없이 확인할 방법이 없어서 헤맸음. 통계탭 로그처럼 찾아봐야 하는 곳이
     * 아니라 항상 보이는 자리에 둠 */
    char buf[20];
    snprintf(buf, sizeof(buf), "%s - CH%u", time_buf, (unsigned)esp_now_hub_get_wifi_channel());
    lv_label_set_text(s_clock_label, buf);
}

/* 소프트 재시작 — 물리적으로 전원을 뽑지 않고도 복구할 수 있게(2026-08-01, 사용자 요청).
 * 확인 팝업 거쳐서 esp_restart() */
static void cb_restart_confirmed(void *ctx)
{
    (void)ctx;
    esp_restart();
}

static void cb_restart_btn(lv_event_t *e)
{
    (void)e;
    show_confirm_popup("정말 재시작하시겠습니까?", cb_restart_confirmed, NULL);
}

/* 페이지콘트롤 — 페이지탭 3개(상황판/통계/설정). 로고 + 상황판/통계 탭 내용(원래 데모의
 * Profile/Analytics 위젯)은 고치기 전 상태 그대로 활용 — 설정 탭만 새로 만든 그룹박스로 교체 */
void ui_init(void)
{
    lv_demo_widgets_components_init();  /* profile/analytics가 쓰는 공용 스타일/폰트 초기화 */

    /* 판넬 JPEG 디코드 버퍼 — 부팅 시 한 번만 잡고 계속 재사용(위 s_photo_jpeg_buf
     * 선언부 주석 참고) */
    ui_log_add("INIT 여유PSRAM(폰트 로드 후)=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    s_photo_jpeg_buf = jpeg_calloc_align(PHOTO_PANEL_BUF_CAP, 16);
    if (s_photo_jpeg_buf) {
        ui_log_add("INIT panel_buf=OK 여유PSRAM=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        ESP_LOGE(TAG, "판넬 디코드 버퍼 할당 실패(%u bytes)", (unsigned)PHOTO_PANEL_BUF_CAP);
        ui_log_add_err(UI_ERR_PANEL_BUF_ALLOC, "판넬 버퍼 할당 실패 — 사진 미리보기 불가");
    }

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
    s_logo_icon = lv_image_create(tab_bar);
    lv_obj_add_flag(s_logo_icon, LV_OBJ_FLAG_IGNORE_LAYOUT);
    LV_IMAGE_DECLARE(img_lvgl_logo);
    lv_image_set_src(s_logo_icon, &img_lvgl_logo);
    lv_obj_align(s_logo_icon, LV_ALIGN_LEFT_MID, -LV_HOR_RES / 2 + 25, 0);

    /* 경고 상태일 때만 보이는 아이콘 — 배경 박스 없이 느낌표 삼각형 아이콘 그 자체
     * (2026-08-01, 사용자가 참고 이미지로 지적: 빨간 네모가 아니라 노란/흰색 삼각형
     * 경고 아이콘이어야 함). LVGL 내장 심볼 폰트(LV_SYMBOL_WARNING)가 이미 그 모양이라
     * 별도 이미지 에셋 없이 크기만 2배로 키워서 씀, 기본은 숨김 */
    s_logo_warning = lv_label_create(tab_bar);
    lv_obj_add_flag(s_logo_warning, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_logo_warning, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_logo_warning, LV_SYMBOL_WARNING);
    /* tab_bar가 나눔고딕(한글 TTF)을 상속시키는데 거기엔 LV_SYMBOL_WARNING 글리프가
     * 없어서 "글리프 없음" 네모가 그려짐(2026-08-01 실기에서 확인) — 심볼이 포함된
     * LVGL 기본 폰트로 명시적으로 덮어써야 함. transform_scale로 14pt를 2배 키웠더니
     * 정렬 기준(피벗)이 원래(작은) 박스 기준이라 아래로 쏠려 보이고 확대라 흐릿하기도
     * 했음(2026-08-01 실기 확인) — 스케일 대신 원래 큰 폰트(24pt)를 그대로 씀 */
    lv_obj_set_style_text_font(s_logo_warning, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_logo_warning, lv_color_hex(0xFFCC00), 0);
    lv_obj_align(s_logo_warning, LV_ALIGN_LEFT_MID, -LV_HOR_RES / 2 + 25, 0);
    lv_obj_add_flag(s_logo_warning, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_logo_warning, cb_logo_warning_tap, LV_EVENT_CLICKED, NULL);

    s_logo_title = lv_demo_widgets_title_create(tab_bar, "");
    lv_obj_add_flag(s_logo_title, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_label_set_text(s_logo_title, ui_str(STR_LOGO_TITLE));
    lv_obj_set_style_text_font(s_logo_title, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_align_to(s_logo_title, s_logo_icon, LV_ALIGN_OUT_RIGHT_TOP, 10, 0);

    /* 로고부제 자리 — 보드 실장 RTC(rtc_sync_init, main.c에서 UI보다 먼저 호출)로 세팅된
     * 시스템 클록을 1초마다 hh:mm:ss로 보여줌 */
    s_clock_label = lv_label_create(tab_bar);
    lv_obj_add_flag(s_clock_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_style(s_clock_label, &style_text_muted, 0);
    lv_obj_align_to(s_clock_label, s_logo_icon, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, 0);
    refresh_clock(NULL);  /* 첫 타이머 tick(최대 1초 뒤) 전까지 빈 채로 안 보이게 즉시 한 번 채움 */
    lv_timer_create(refresh_clock, 1000, NULL);

    lv_timer_create(error_poll_tick, 200, NULL);

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
    /* 드롭다운은 맨 왼쪽 고정, 나머지 버튼들은 한 덩어리로 맨 오른쪽에 붙임(2026-08-05,
     * 사용자 지시) — SPACE_BETWEEN을 camera_toolbar 직계 자식 2개(드롭다운, 버튼 묶음
     * camera_btn_group)에만 걸어서 그 사이만 벌어지게 함(자식이 4개면 전부 균등하게
     * 벌어져서 버튼들끼리도 떨어져 보였을 것) */
    lv_obj_set_flex_align(camera_toolbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(camera_toolbar, 0, 0);
    lv_obj_set_style_pad_all(camera_toolbar, 0, 0);
    lv_obj_add_flag(camera_toolbar, LV_OBJ_FLAG_HIDDEN);  /* 초기값: 연결 전이라 숨김 */
    s_camera_content = camera_toolbar;  /* HIDDEN 토글 대상 1/2 — split_row가 2/2 */

    /* CAM 선택 드롭다운(2026-08-05) — camera_toolbar 전체가 이미 미연결 시 숨겨지므로,
     * "CAM 1대뿐이어도 항상 표시"는 자동으로 충족됨(0대일 때만 안 보임, 그건 카메라 판넬
     * 자체가 "없음" 문구로 바뀌는 기존 동작과 동일선상이라 문제 없음 — 사용자 확인).
     * 옵션/매핑은 refresh_dashboard -> rebuild_camera_dropdown_if_changed가 채움.
     * 기본 폭(LV_DPI_DEF=130px)이 좁아 보인다는 지적 — 50px 더 키움 */
    s_camera_select_dd = lv_dropdown_create(camera_toolbar);
    lv_obj_set_width(s_camera_select_dd, LV_DPI_DEF + 50);
    lv_obj_add_event_cb(s_camera_select_dd, cb_camera_select_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *camera_btn_group = lv_obj_create(camera_toolbar);
    lv_obj_set_size(camera_btn_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(camera_btn_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_width(camera_btn_group, 0, 0);
    lv_obj_set_style_pad_all(camera_btn_group, 0, 0);

    lv_obj_t *btn_capture = lv_button_create(camera_btn_group);
    lv_obj_add_event_cb(btn_capture, cb_capture_now, LV_EVENT_CLICKED, NULL);
    s_camera_capture_lbl = lv_label_create(btn_capture);
    lv_label_set_text(s_camera_capture_lbl, ui_str(STR_BTN_CAPTURE_NOW));
    lv_obj_set_style_text_font(s_camera_capture_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_renew = lv_button_create(camera_btn_group);
    lv_obj_add_event_cb(btn_renew, cb_renew_list, LV_EVENT_CLICKED, NULL);
    s_camera_renew_lbl = lv_label_create(btn_renew);
    lv_label_set_text(s_camera_renew_lbl, ui_str(STR_BTN_RENEW_LIST));
    lv_obj_set_style_text_font(s_camera_renew_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_delete_all = lv_button_create(camera_btn_group);
    lv_obj_add_event_cb(btn_delete_all, cb_delete_all, LV_EVENT_CLICKED, NULL);
    s_camera_delete_all_lbl = lv_label_create(btn_delete_all);
    lv_label_set_text(s_camera_delete_all_lbl, ui_str(STR_BTN_DELETE_ALL));
    lv_obj_set_style_text_font(s_camera_delete_all_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 목록(왼쪽)/사진(오른쪽) 판넬 — 툴바와 형제(camera_box 직접 자식), 좌우로 나열 */
    s_camera_split_row = lv_obj_create(camera_box);
    lv_obj_set_size(s_camera_split_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_camera_split_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_width(s_camera_split_row, 0, 0);
    lv_obj_set_style_pad_all(s_camera_split_row, 0, 0);
    lv_obj_add_flag(s_camera_split_row, LV_OBJ_FLAG_HIDDEN);  /* 초기값: 연결 전이라 숨김 */

    lv_obj_t *list_panel = lv_obj_create(s_camera_split_row);
    /* 45/55 -> 55/45로 재배분(2026-08-01, 사용자 지시: 목록 폭은 넓히고, 미리보기
     * 판넬은 실제 판넬 디코드 해상도(320x240)에 맞춰 줄임 — 아래 s_photo_box 참고).
     * 2026-08-05 — 고정 55%는 picture_panel이 320px 고정폭일 때만 우연히 맞는 값이라
     * 화면 크기/여백이 조금만 달라져도 어긋남(실기에서 320px 박스가 picture_panel 밖으로
     * 삐져나가 패닝해야 다 보이는 문제로 확인). flex_grow로 바꿔서 picture_panel이
     * 실제로 차지한 폭(아래 참고, 이제 내용물 크기로 결정됨)을 뺀 나머지를 자동으로
     * 채우게 함 — 퍼센트 계산에 안 의존 */
    lv_obj_set_size(list_panel, 0, LV_SIZE_CONTENT);  /* 폭 0=flex_grow가 결정(관례상 표기),
                                                          높이는 원래대로 내용물 크기 —
                                                          폭만 lv_obj_set_width로 바꾸면서
                                                          높이 지정이 빠져 패널이 작아졌던
                                                          실수 수정(2026-08-05) */
    lv_obj_set_flex_grow(list_panel, 1);
    lv_obj_set_flex_flow(list_panel, LV_FLEX_FLOW_COLUMN);
    /* 기본 테마 패딩(이 화면 DPI/해상도 조합에서 LVGL DISP_LARGE 버킷의 PAD_DEF=20px,
     * LV_DPX_CALC(130,24)로 확인)이 목록 컨트롤 폭을 그만큼 깎아먹고 있었음 — 절반(10px)로
     * 줄여서 목록 폭 확보(2026-08-05, 사용자 지시) */
    lv_obj_set_style_pad_all(list_panel, 10, 0);

    /* 제목 + 사진개수/SD사용량(오른쪽 정렬) — 2026-08-01, 사용자 지시 */
    lv_obj_t *list_title_row = lv_obj_create(list_panel);
    lv_obj_set_size(list_title_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list_title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(list_title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(list_title_row, 0, 0);
    lv_obj_set_style_pad_all(list_title_row, 0, 0);

    s_list_title = lv_label_create(list_title_row);
    lv_label_set_text(s_list_title, ui_str(STR_PANEL_LIST));
    lv_obj_set_style_text_font(s_list_title, ui_font_get(UI_FONT_SIZE_18), 0);

    s_list_info_label = lv_label_create(list_title_row);
    lv_label_set_text(s_list_info_label, "");
    lv_obj_set_style_text_font(s_list_info_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_color(s_list_info_label, lv_palette_main(LV_PALETTE_GREY), 0);

    s_photo_list = lv_list_create(list_panel);
    lv_obj_set_size(s_photo_list, LV_PCT(100), 220);
    lv_obj_set_style_pad_all(s_photo_list, 0, 0);  /* 기본 테마 리스트 안쪽 여백 제거 — 박스 꽉 채움 */
    lv_obj_set_style_pad_row(s_photo_list, 2, 0);  /* 행 사이 최소 간격만 유지 */

    lv_obj_t *picture_panel = lv_obj_create(s_camera_split_row);
    /* 2026-08-05 — 퍼센트 폭(45%) 대신 내용물(s_photo_box, 320x240 고정) 크기에 맞춰
     * 자동으로 정해지도록 변경 — 위 list_panel 주석 참고. 이러면 사진 템플릿이 잘리거나
     * 패닝해야 보이는 일 자체가 구조적으로 없어짐(패널이 항상 템플릿 크기 이상이 됨) */
    lv_obj_set_size(picture_panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    /* 기본 테마 패딩 절반으로(list_panel과 동일 이유) — content-size라서 이 패딩만큼
     * picture_panel의 총 폭도 줄어들어, list_panel(flex_grow)이 그만큼 더 넓어짐
     * (2026-08-05, 사용자 지시) */
    lv_obj_set_style_pad_all(picture_panel, 10, 0);
    lv_obj_set_flex_flow(picture_panel, LV_FLEX_FLOW_COLUMN);
    /* cross_place는 기본값(START/좌측)을 그대로 둠 — "미리보기" 제목 라벨은 원래 좌정렬이었고
     * (사용자 지적, 2026-08-05), s_photo_box는 이제 패널 폭 자체가 320px에 맞춰지므로 정렬과
     * 무관하게 항상 꽉 참 */
    s_picture_title = lv_label_create(picture_panel);
    lv_label_set_text(s_picture_title, ui_str(STR_PANEL_PICTURE));
    lv_obj_set_style_text_font(s_picture_title, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 사진 없을 때 플레이스홀더 — 큼직한 박스만(아이콘은 뺌, 비트맵 폰트를 확대하니
     * 깨져 보였고 어차피 실제 사진이 오면 이 자리를 lv_image로 교체할 예정이라 불필요).
     * 전체화면 뷰어는 제거함(2026-08-01, PSRAM 예산에 안 맞아서 — 원본은 웹으로 봄).
     * 박스 크기를 판넬 디코드 해상도(320x240)에 고정 — 예전엔 100%폭+고정 220 높이라
     * 실제 사진(4:3)과 비율이 안 맞아 CONTAIN으로 줄어든 사진 주위에 여백이 크게 남고
     * "미리보기가 틀보다 작다"고 보였음(2026-08-01, 사용자 지적) */
    s_photo_box = lv_obj_create(picture_panel);
    lv_obj_set_size(s_photo_box, PHOTO_PANEL_DECODE_W, PHOTO_PANEL_DECODE_H);
    lv_obj_set_style_bg_color(s_photo_box, lv_palette_lighten(LV_PALETTE_GREY, 3), 0);
    /* 2026-08-05, 사용자 지시 — 라운드/기본 테마 패딩을 없애서 사진이 320x240에 여백 없이
     * 꽉 차게 함(그 전엔 이 안쪽 패딩 때문에 사진이 실제보다 작게, 여백을 두고 표시됐음).
     * 사진 없을 때 플레이스홀더 느낌을 위해 얇은 보더는 유지 */
    lv_obj_set_style_radius(s_photo_box, 0, 0);
    lv_obj_set_style_pad_all(s_photo_box, 0, 0);
    lv_obj_set_style_border_width(s_photo_box, 1, 0);
    lv_obj_set_style_border_color(s_photo_box, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_flex_flow(s_photo_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_photo_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_camera_photo_label = lv_label_create(s_photo_box);
    lv_label_set_text(s_camera_photo_label, ui_str(STR_PANEL_NO_PHOTO_YET));
    lv_obj_set_style_text_font(s_camera_photo_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_color(s_camera_photo_label, lv_palette_main(LV_PALETTE_GREY), 0);

    s_dashboard_timer = lv_timer_create(refresh_dashboard, 1000, NULL);

    lv_obj_t *stats_page = lv_tabview_add_tab(s_page_control, ui_str(STR_TAB_STATISTICS));
    lv_obj_set_flex_flow(stats_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(stats_page, 4, 0);

    s_log_container = lv_obj_create(stats_page);
    lv_obj_set_size(s_log_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scroll_dir(s_log_container, LV_DIR_VER);
    lv_obj_set_style_pad_all(s_log_container, 6, 0);

    s_log_label = lv_label_create(s_log_container);
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_log_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_log_label, ui_font_get(UI_FONT_SIZE_24), 0);
    lv_label_set_text(s_log_label, "");

    lv_timer_create(refresh_log_box, 500, NULL);

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

    /* 재시작 버튼 — 물리적 전원 재연결 없이 소프트 리셋(2026-08-01, 사용자 요청) */
    lv_obj_t *restart_row = lv_obj_create(cntl_list);
    lv_obj_set_size(restart_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(restart_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(restart_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(restart_row, 0, 0);
    lv_obj_set_style_pad_hor(restart_row, 12, 0);
    lv_obj_set_style_pad_ver(restart_row, 20, 0);

    lv_obj_t *restart_label = lv_label_create(restart_row);
    lv_label_set_text(restart_label, "장치 재시작");
    lv_obj_set_style_text_font(restart_label, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *restart_btn = lv_button_create(restart_row);
    lv_obj_add_event_cb(restart_btn, cb_restart_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *restart_btn_lbl = lv_label_create(restart_btn);
    lv_label_set_text(restart_btn_lbl, "재시작");
    lv_obj_set_style_text_font(restart_btn_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    create_group_box(option_page, STR_GROUP_SENSOR);

    /* 영상(Camera) 그룹박스 — 발견된 CAM 리스트(연결중/연결됨), 1초마다 갱신.
     * 자식이 리스트 하나뿐이라 별도 content 래퍼 없이 box 직접 자식으로 둠 */
    lv_obj_t *camera_group_box = create_group_box(option_page, STR_GROUP_CAMERA);
    s_camera_list = lv_list_create(camera_group_box);
    lv_obj_set_size(s_camera_list, LV_PCT(100), LV_SIZE_CONTENT);
    s_camera_list_timer = lv_timer_create(refresh_camera_list, 1000, NULL);

    create_group_box(option_page, STR_GROUP_SYSTEM);
}
