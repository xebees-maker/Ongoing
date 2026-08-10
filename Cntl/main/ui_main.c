#include "ui_main.h"
#include "ui_strings.h"
#include "ui_font.h"
#include "esp_now_hub.h"
#include "device_config.h"
#include "esp_now_photo.h"
#include "ui_log.h"
#include "rtc_sync.h"
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

/* 요약판넬 행의 상태 문구(페어됨/통신 중)만 매 틱 가볍게 갱신하기 위한 보조 배열
 * (2026-08-10) — 행 자체(리스트 구조)는 ever_paired 기준으로만 다시 그리므로(위
 * node_display_equal 참고) 라디오 레벨 paired 토글은 구조 재생성 없이 이 텍스트만
 * 갱신해서 반영함(요약판넬만 이 세분화를 보여주기로 함, 사용자 지시) */
static lv_obj_t *s_summary_row_objs[ESP_NOW_HUB_MAX_NODES];
static uint8_t   s_summary_row_macs[ESP_NOW_HUB_MAX_NODES][6];
static char      s_summary_row_names[ESP_NOW_HUB_MAX_NODES][ESP_NOW_LINK_NAME_LEN];
static int       s_summary_row_count = 0;
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

/* 원격 설정(2026-08-08 재설계) — Cntl이 값의 주인(device_config.h), CAM은 페어링 때마다
 * 받아서 쓸 뿐 로컬 저장 안 함. 촬영주기=카메라별(지금은 선택된 CAM 하나), 응답성=시스템
 * 전체 공통. 두 행 다 [라벨][드롭다운][Apply] 인라인 — Apply는 드롭다운 값이 "마지막으로
 * 성공 적용된 값"과 달라졌을 때만 활성화(5단계 플로우, 사용자 설계). refresh_lang_texts()가
 * 이 라벨들을 참조하므로 그 함수보다 앞에 선언돼야 함(사용자 지시: "앞으로 모든 label은 그
 * 구조체에 넣어야해" — ui_str_id_t뿐 아니라 이 선언 순서 규칙도 같이 지킬 것). */
static lv_obj_t *s_capture_interval_dd    = NULL;
static lv_obj_t *s_capture_apply_btn      = NULL;
static lv_obj_t *s_capture_interval_label = NULL;
static lv_obj_t *s_capture_apply_lbl      = NULL;
static int       s_capture_interval_applied_idx = -1;  /* -1: 아직 모름(부팅 직후) */

static lv_obj_t *s_response_interval_dd    = NULL;
static lv_obj_t *s_response_apply_btn      = NULL;
static lv_obj_t *s_response_interval_label = NULL;
static lv_obj_t *s_response_apply_lbl      = NULL;
static lv_obj_t *s_response_help_label     = NULL;  /* 2026-08-10 — 드롭다운 선택값의 풀이
                                                        (즉시/빠름/균형/절전/최대절전 의미) */
static int       s_response_interval_applied_idx = -1;

/* 적응형 반응시간 행(2026-08-10) — 마지막 사용자 조작 후 이만큼 조용해야 CAM에 SLEEP_NOW.
 * CAM에는 전송 안 되는 Cntl 내부 판단값이라(esp_now_hub.c 참고), Apply해도 네트워크 왕복이
 * 없어서 진행팝업 없이 즉시 반영됨(다른 두 Apply 버튼과 다른 점) */
static lv_obj_t *s_adaptive_response_dd    = NULL;
static lv_obj_t *s_adaptive_apply_btn      = NULL;
static lv_obj_t *s_adaptive_response_label = NULL;
static lv_obj_t *s_adaptive_apply_lbl      = NULL;
static lv_obj_t *s_adaptive_help_label     = NULL;
static int       s_adaptive_response_applied_idx = -1;

static lv_obj_t *s_restart_label     = NULL;
static lv_obj_t *s_restart_btn_lbl   = NULL;

/* 수동 시각설정 행(2026-08-09, RTC 부팅 시딩 버그 수정에 이어지는 작업) — [라벨][현재
 * 시각][설정] 인라인. 현재 시각 라벨은 refresh_clock()이 로고부제 시계와 같이 1초마다
 * 갱신(언어 무관, 그냥 숫자라 refresh_lang_texts엔 등록 안 함) */
static lv_obj_t *s_time_label       = NULL;
static lv_obj_t *s_time_value_label = NULL;
static lv_obj_t *s_time_set_btn_lbl = NULL;

/* 버튼 폭 통일(2026-08-09, 사용자 지시) — 팝업 버튼 포함 전부 하나의 폭. ui_init()
 * 끝에서 그 시점까지 만들어진 메인 화면 버튼들의 실측 자연폭 중 최댓값으로 한 번 정해짐
 * (하드코딩 대신 실측 — 언어 전환/문구 변경에도 안 깨짐). add_modal_button()이 이후 뜨는
 * 모든 팝업 버튼에 이 값을 그대로 적용 */
static lv_coord_t s_action_btn_width = 0;

/* 드롭다운 옵션 문자열의 줄 순서 == 이 배열의 인덱스 순서(초 단위) — 반드시 같이 바꿀 것.
 * 2026-08-08 — "10초"를 뺌: 실기에서 자동촬영 타이머가 이 짧은 주기로 ESP-NOW 활동과
 * 겹치면 힙이 깨지는 크래시를 확인함(CAM 쪽에도 1800s 미만은 강제로 올리는 안전장치를
 * 넣었지만, UI에서부터 실제로 안 되는 값을 보여주지 않는 게 맞음 — cam_node.c의
 * clamp_capture_interval_sec 참고) */
static const uint32_t s_capture_interval_values[]  = { 0, 1800, 3600, 10800, 36000 };
/* 2026-08-10, CAM Deep Sleep 전환 — 이 값이 곧 딥슬립 사이클 길이가 되므로 절전 정도가
 * 극단적으로 갈리는 5단계로 재정의(즉시/빠름/균형/절전/최대절전). 각 값의 의미는
 * s_response_help_texts(아래)와 사용자 확인된 표 그대로 — 반드시 같이 바꿀 것 */
static const uint32_t s_response_interval_values[] = { 1, 3, 10, 30, 1800 };
/* 적응형 반응시간(2026-08-10) — 10초/30초/1분. STR_OPT_ADAPTIVE_RESPONSE_LIST 순서와
 * 반드시 같이 맞출 것 */
static const uint32_t s_adaptive_response_values[] = { 10, 30, 60 };

static int find_value_index(const uint32_t *values, int count, uint32_t v)
{
    for (int i = 0; i < count; i++) if (values[i] == v) return i;
    return 0;
}

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

/* 통계 탭 좌측 절전상태 판넬 — CAM의 Deep Sleep 사이클 통계(ESP_NOW_MSG_DEEP_SLEEP_STATS,
 * 2026-08-10 Light Sleep 폐기 후 개편)를 로그처럼 한 줄씩 누적(사용자 지시: 최신값으로
 * 덮어쓰는 대신 매번 새 줄로, 2026-08-09) — s_log_container/s_log_label과 같은 구조.
 * s_power_panel_title은 refresh_lang_texts에서 갱신 */
static lv_obj_t *s_power_panel_title = NULL;
static lv_obj_t *s_power_list        = NULL;  /* 스크롤 컨테이너(s_log_container 대응) */
static lv_obj_t *s_power_log_label   = NULL;  /* 누적 텍스트(s_log_label 대응) */
static lv_obj_t   *s_power_log_pause_btn = NULL;
static lv_obj_t   *s_power_log_pause_lbl = NULL;
static lv_timer_t *s_power_panel_timer   = NULL;  /* 일시멈춤 단추가 pause/resume(2026-08-10) */
static bool        s_power_log_paused    = false;

/* ds_cycle_count 하나만 비교하면 됨(2026-08-10) — 매 리포트가 항상 새 사이클이라 Light
 * Sleep 시절처럼 여러 필드를 같이 diff할 필요가 없어짐(단조증가 카운터) */
typedef struct {
    uint8_t  mac[6];
    bool     used;
    uint32_t last_cycle_count;
    uint32_t last_sleep_now_send_count;  /* 2026-08-10 — SLEEP_NOW 발신도 별도 줄로(사용자 지시) */
} power_log_track_t;
static power_log_track_t s_power_log_track[ESP_NOW_HUB_MAX_NODES];
static char s_power_log_buf[2048] = "";

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
    /* 에러 이력이 전부 확인/소진되면 경고 아이콘도 같이 끔 — s_error_active일 때만
     * 확인(평소엔 폴링 낭비 없음). 2026-08-10 — 예전엔 ui_log_clear_err()로 "일시적이고
     * 스스로 해소됨"을 자동으로 지우는 별도 경로가 있었는데, 그 경로의 유일한 용도였던
     * 구 UI_ERR_NOT_PAIRED 상시폴링 방식 자체가 require_active_or_report()의 즉시판정
     * 방식으로 바뀌면서 더는 호출되는 곳이 없어 함수째 제거함(죽은 코드) */
    if (s_error_active) {
        int codes[UI_ERR_HISTORY_CAP];
        if (ui_log_get_error_history(codes, UI_ERR_HISTORY_CAP) == 0) {
            set_logo_warning(false);
        }
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
/* 응답성 드롭다운 선택값의 도움말 텍스트 갱신 — 정의는 update_response_apply_enabled
 * 근처(아래) */
static void update_response_help_text(void);

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

    /* 2026-08-08 — 새 라벨은 전부 여기 등록할 것(사용자 지시: "앞으로 모든 label은 그
     * 구조체에 넣어야해" — ui_str_id_t 테이블만으론 부족하고, 이 함수에도 반드시 같이
     * 추가해야 언어전환이 실제로 반영됨) */
    lv_label_set_text(s_capture_interval_label, ui_str(STR_LABEL_CAPTURE_INTERVAL));
    lv_label_set_text(s_capture_apply_lbl, ui_str(STR_BTN_APPLY));
    lv_label_set_text(s_response_interval_label, ui_str(STR_LABEL_RESPONSE_INTERVAL));
    lv_label_set_text(s_response_apply_lbl, ui_str(STR_BTN_APPLY));
    lv_label_set_text(s_adaptive_response_label, ui_str(STR_LABEL_ADAPTIVE_RESPONSE));
    lv_label_set_text(s_adaptive_apply_lbl, ui_str(STR_BTN_APPLY));
    lv_label_set_text(s_adaptive_help_label, ui_str(STR_HELP_ADAPTIVE_RESPONSE));
    lv_label_set_text(s_restart_label, ui_str(STR_LABEL_RESTART_DEVICE));
    lv_label_set_text(s_restart_btn_lbl, ui_str(STR_BTN_RESTART));
    lv_label_set_text(s_time_label, ui_str(STR_LABEL_TIME));
    lv_label_set_text(s_time_set_btn_lbl, ui_str(STR_BTN_SET_TIME));
    lv_label_set_text(s_power_panel_title, ui_str(STR_PANEL_DEEPSLEEP));

    /* 드롭다운 옵션 문자열 자체도 언어별이라 다시 채워야 함 — lv_dropdown_set_options는
     * 선택 인덱스를 0으로 리셋시키므로, 지금 선택돼있던 인덱스를 기억했다가 그대로
     * 되돌려줘야 사용자가 고른 값이 언어 전환 때문에 조용히 바뀌지 않음 */
    uint16_t capture_sel = lv_dropdown_get_selected(s_capture_interval_dd);
    lv_dropdown_set_options(s_capture_interval_dd, ui_str(STR_OPT_CAPTURE_INTERVAL_LIST));
    lv_dropdown_set_selected(s_capture_interval_dd, capture_sel);

    uint16_t response_sel = lv_dropdown_get_selected(s_response_interval_dd);
    lv_dropdown_set_options(s_response_interval_dd, ui_str(STR_OPT_RESPONSE_INTERVAL_LIST));
    lv_dropdown_set_selected(s_response_interval_dd, response_sel);
    update_response_help_text();  /* 도움말도 언어 전환 시 다시 채움(선택 인덱스는 그대로) */

    uint16_t adaptive_sel = lv_dropdown_get_selected(s_adaptive_response_dd);
    lv_dropdown_set_options(s_adaptive_response_dd, ui_str(STR_OPT_ADAPTIVE_RESPONSE_LIST));
    lv_dropdown_set_selected(s_adaptive_response_dd, adaptive_sel);
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
    if (s_action_btn_width > 0) lv_obj_set_width(btn, s_action_btn_width);
    lv_obj_center(lbl);  /* 폭 통일로 버튼이 레이블보다 넓어진 경우 텍스트 중앙정렬(2026-08-09) */
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
    lv_label_set_text(title, ui_str(STR_TITLE_ERROR_LIST));
    lv_obj_set_style_text_font(title, ui_font_get(UI_FONT_SIZE_18), 0);

    if (n == 0) {
        lv_obj_t *lbl = lv_label_create(box);
        lv_label_set_text(lbl, ui_str(STR_ERROR_LIST_EMPTY));
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

/* 요약판넬(s_summary_row_objs)과 동일한 이유(2026-08-10) — 행 구조 자체는 ever_paired
 * 기준으로만 다시 그려지므로, 라디오 레벨 paired 토글(페어됨<->통신 중)만으로는 재생성이
 * 안 트리거됨 — 이 배열로 행 텍스트만 매 틱 따로 갱신 */
static lv_obj_t *s_camera_row_objs[ESP_NOW_HUB_MAX_NODES];
static uint8_t   s_camera_row_macs[ESP_NOW_HUB_MAX_NODES][6];
static char      s_camera_row_names[ESP_NOW_HUB_MAX_NODES][ESP_NOW_LINK_NAME_LEN];
static int       s_camera_row_count = 0;

static void force_camera_list_redraw(void)
{
    s_camera_count_prev = -1;
}

/* 화면에 실제로 보이는 정보만 비교(last_seen_ms는 keepalive마다 바뀌지만 화면엔 안 나오므로
 * 제외) — 매초 리스트를 통째로 지우고 다시 그리던 게 터치 처리와 간섭해서 반응이 느려지거나
 * 안 먹는 문제(연결해제 팝업, 언어 라디오 버튼 모두)의 원인이었음, 바뀐 게 없으면 건너뜀 */
/* 2026-08-10 connectionless 모델 정정(사용자 지적) — 화면에 보이는 "바뀜" 여부는
 * ever_paired(세션 내 한 번이라도 페어링됨, sticky) 기준이어야 함. 라디오 레벨 paired는
 * CAM 딥슬립 사이클마다 정상적으로 순간 false를 스치므로, 이 필드로 비교하면 매 사이클
 * 리스트가 깜빡이며 다시 그려짐(사용자 지적 — 목록/판넬이 끊김처럼 보이는 원인) */
static bool node_display_equal(const esp_now_hub_node_t *a, const esp_now_hub_node_t *b)
{
    return memcmp(a->mac, b->mac, sizeof(a->mac)) == 0 &&
           a->kind == b->kind && a->ever_paired == b->ever_paired &&
           strcmp(a->name, b->name) == 0;
}

static void cb_camera_item_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    esp_now_hub_node_t *node = (esp_now_hub_node_t *)lv_obj_get_user_data(btn);
    if (!node) return;
    if (esp_now_hub_get_conn_state(node->mac) != HUB_CONN_STATE_WAITING) show_unpair_confirm_popup(node);
    else                                                                 show_pair_confirm_popup(node);
}

/* 행 텍스트만(WAITING/PAIRED/ACTIVE 상태문구) 매 틱 갱신 — 구조(행 개수/순서)는 안 건드림,
 * refresh_camera_list()가 실제로 다시 그렸을 때만 s_camera_row_count가 갱신되므로 그 전까진
 * 이전 행 목록을 그대로 갱신함(요약판넬과 동일 패턴, 2026-08-10) */
static void refresh_camera_row_status_text(void)
{
    for (int i = 0; i < s_camera_row_count; i++) {
        hub_conn_state_t st = esp_now_hub_get_conn_state(s_camera_row_macs[i]);
        ui_str_id_t status_id = (st == HUB_CONN_STATE_WAITING) ? STR_STATUS_CONNECTING
                               : (st == HUB_CONN_STATE_ACTIVE) ? STR_STATUS_ACTIVE
                               : STR_STATUS_PAIRED;
        char buf[48];
        snprintf(buf, sizeof(buf), "%s (%s)", s_camera_row_names[i], ui_str(status_id));
        lv_obj_t *lbl = lv_obj_get_child(s_camera_row_objs[i], 0);
        if (lbl) lv_label_set_text(lbl, buf);
    }
}

static void refresh_camera_list(lv_timer_t *t)
{
    (void)t;
    int count = esp_now_hub_get_nodes(HUB_NODE_KIND_CAM, s_camera_nodes, ESP_NOW_HUB_MAX_NODES);

    bool changed = (count != s_camera_count_prev);
    for (int i = 0; !changed && i < count; i++) {
        if (!node_display_equal(&s_camera_nodes[i], &s_camera_nodes_prev[i])) changed = true;
    }
    if (changed) {
        memcpy(s_camera_nodes_prev, s_camera_nodes, sizeof(esp_now_hub_node_t) * count);
        s_camera_count_prev = count;

        /* 그 순간 사용자가 행을 누르고 있는 중이면 LVGL 입력장치가 방금 지워진 객체를 계속
         * 참조하게 돼서 이후 터치가 깨짐 — clean 직전에 이 리스트(자식 포함) 관련 입력장치
         * 상태를 먼저 리셋 */
        lv_indev_reset(NULL, s_camera_list);
        lv_obj_clean(s_camera_list);
        s_camera_row_count = 0;

        /* 대기중/연결된 장치가 하나도 없으면 "없음" 문구 대신 리스트 자체를 숨김 —
         * 측정기/카메라 상황판 판넬과 다르게, 이 리스트는 원래 대기중인 게 있을 때만
         * 보이는 컨트롤이라 "없음" 메시지 자체가 나올 상황이 아님(사용자 확인) */
        if (count == 0) {
            lv_obj_add_flag(s_camera_list, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_camera_list, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < count; i++) {
                lv_obj_t *row = lv_list_add_button(s_camera_list, NULL, "");
                lv_obj_set_style_text_font(row, ui_font_get(UI_FONT_SIZE_18), 0);
                lv_obj_set_user_data(row, &s_camera_nodes[i]);
                lv_obj_add_event_cb(row, cb_camera_item_clicked, LV_EVENT_CLICKED, NULL);
                if (i < ESP_NOW_HUB_MAX_NODES) {
                    s_camera_row_objs[i] = row;
                    memcpy(s_camera_row_macs[i], s_camera_nodes[i].mac, 6);
                    strncpy(s_camera_row_names[i], s_camera_nodes[i].name, ESP_NOW_LINK_NAME_LEN - 1);
                    s_camera_row_names[i][ESP_NOW_LINK_NAME_LEN - 1] = '\0';
                }
            }
            s_camera_row_count = (count < ESP_NOW_HUB_MAX_NODES) ? count : ESP_NOW_HUB_MAX_NODES;
        }
    }
    refresh_camera_row_status_text();  /* 2026-08-10 — 구조 변경 여부와 무관하게 매 틱 갱신 */
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
    /* inner padding(판넬 테두리↔제목/내용)은 기본테마 값(pad_all=20px) 유지가 원칙이지만,
     * 위쪽만 2px 살짝 줄임(2026-08-09, 사용자 지시 — 탭바↔판넬 간격 조정 이어서). 판넬 안
     * 항목 간 간격(row-to-row gap)은 기본값(11px)에서 10px로 명시 지정 */
    lv_obj_set_style_pad_top(box, 18, 0);
    lv_obj_set_style_pad_row(box, 10, 0);

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

/* 2026-08-10 — 사진가져오기/목록갱신/지금촬영/전체삭제 4곳이 전부 같은 문제를 겪고 있었음:
 * WAITING(진짜 연결 안 됨)일 때 그냥 요청+진행팝업을 띄우면, esp_now_photo.c 내부의
 * require_paired()가 요청 자체를 조용히 안 보내는데 팝업은 그걸 몰라서 cam_response_timeout_ms()
 * 예산을 다 채운 뒤에야 "무응답"으로 오인 표시함(3006/3007/4004 등, 원인이 다 같음). 액션마다
 * 반복 작성하지 않고 여기 한 곳으로 모음 — 나중에 SENS를 붙일 때도(같은 connectionless
 * WAITING/PAIRED/ACTIVE 모델이므로) mac만 바꿔 그대로 재사용 가능. what은 로그/토스트에 쓸
 * 짧은 동작 이름("사진 가져오기" 등, esp_now_tx_enqueue의 what과 같은 관례) */
static bool require_active_or_report(const uint8_t *mac, const char *what)
{
    if (esp_now_hub_get_conn_state(mac) == HUB_CONN_STATE_WAITING) {
        ui_log_add_err(UI_ERR_NOT_PAIRED, "%s 불가 — CAM 연결 대기 중", what);
        return false;
    }
    return true;
}

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

    if (!require_active_or_report(s_selected_cam_mac, "사진 가져오기")) return;

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
    if (!s_has_selected_cam) return;
    if (!require_active_or_report(s_selected_cam_mac, "사진 삭제")) return;

    esp_now_photo_delete(s_selected_cam_mac, file_id);
    esp_now_photo_list_request(s_selected_cam_mac);  /* 삭제 반영된 목록으로 갱신 */
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
 * 전부 이 값을 씀(각자 계산 기준은 다를 수 있음: 총 경과시간 vs 마지막 진행 이후 경과시간).
 * 2026-08-10 — 고정 8초였던 걸 "응답성" 설정에 맞춰 늘어나게 바꿈: CAM이 정상적으로(버그
 * 아님) 딥슬립 중이었을 때 명령이 도착하면 esp_now_tx.c도 이제 응답성 예산만큼 재시도하는데,
 * 이 값이 그보다 짧게 고정돼 있으면 실제 재시도가 아직 끝나기도 전에 팝업이 먼저 NORESPONSE로
 * 포기해버려서 esp_now_tx.c 쪽 수정이 무의미해짐(실사용 중 3006 반복으로 발견). 같은
 * 30초 상한/여유마진 원칙을 여기서도 그대로 재사용(esp_now_tx.c의 TX_RESPONSE_BUDGET_CAP_SEC/
 * TX_WAKE_MARGIN_MS와 값 동기화 — 두 곳 중 하나만 바뀌면 다시 어긋나므로 값 바꿀 땐 같이) */
static uint32_t cam_response_timeout_ms(void)
{
    uint32_t sec = device_config_get_response_interval_sec();
    if (sec > 30U) sec = 30U;  /* 최대절전(30분) 티어는 팝업을 30분간 띄워둘 수 없어 상한 적용 */
    return sec * 1000U + 3000U;  /* +3초: CAM 웨이크 후 페어링 핸드셰이크 여유 */
}

static lv_obj_t          *s_progress_popup_overlay = NULL;
static lv_obj_t          *s_progress_popup_box = NULL;
static lv_timer_t        *s_progress_popup_timer = NULL;
static progress_tick_fn_t s_progress_tick_fn = NULL;
static lv_obj_t          *s_progress_popup_cancel_btn = NULL;
static lv_obj_t          *s_progress_popup_status_lbl = NULL;  /* "취소" 눌렀을 때만 보이는
                                                                    상태 문구 — 버튼 자체 텍스트는
                                                                    안 바뀜(2026-08-09, 버튼 폭
                                                                    고정 후 긴 문구가 삐져나오던
                                                                    문제 수정) */
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
    s_progress_popup_status_lbl = NULL;
    s_progress_popup_cancel_requested = false;
    resume_bg_timers();
}

/* "취소"는 먹통 방지를 위한 심리적 안전장치로만 남기고, 실제 거래(CAM으로 보낸 요청)는
 * 취소할 방법이 프로토콜에 없어서 그대로 계속 진행됨(2026-08-05, 사용자 지시) — 예전엔
 * 여기서 바로 close_progress_popup()을 불러서 모달/배경타이머를 즉시 풀어버렸는데, 그
 * 직후 사용자가 다른 항목을 선택하면 아직 끝나지 않은 이전 요청의 뒤늦은 응답이 새
 * 선택 위에 잘못 표시되는 경쟁 상태로 이어짐(실기에서 재현: "선택한 사진과 다른 사진이
 * 보임"). 이제는 라벨/버튼만 "종료 대기 중"으로 바꾸고 모달은 유지 — 각 흐름의
 * tick_fn이 실제 완료(READY/ERROR)나 자체 타임아웃(cam_response_timeout_ms())을 만나
 * true를 반환할 때만 진짜로 닫힘(아래 progress_popup_tick, 안 건드림) — 그래서 무한정
 * 막히진 않고 상한이 있음 */
static void cb_progress_popup_cancel(lv_event_t *e)
{
    (void)e;
    if (s_progress_popup_cancel_requested) return;  /* 중복 클릭 무시 */
    s_progress_popup_cancel_requested = true;
    if (s_progress_popup_cancel_btn) lv_obj_add_state(s_progress_popup_cancel_btn, LV_STATE_DISABLED);
    if (s_progress_popup_status_lbl) {
        lv_label_set_text(s_progress_popup_status_lbl, ui_str(STR_STATUS_CANCEL_PENDING));
        lv_obj_remove_flag(s_progress_popup_status_lbl, LV_OBJ_FLAG_HIDDEN);
    }
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
    s_progress_popup_status_lbl = lv_label_create(box);
    lv_label_set_text(s_progress_popup_status_lbl, "");
    lv_obj_set_style_text_font(s_progress_popup_status_lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_flag(s_progress_popup_status_lbl, LV_OBJ_FLAG_HIDDEN);  /* 취소 누르기 전엔 숨김 */

    lv_obj_t *btn_row = create_modal_btn_row(box);
    s_progress_popup_cancel_btn = add_modal_button(btn_row, STR_BTN_CANCEL, cb_progress_popup_cancel, NULL);
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
        bool timedout  = !resolved && lv_tick_elaps(s_capture_popup_stage_start_ms) > cam_response_timeout_ms();
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
    if (lv_tick_elaps(s_capture_popup_stage_start_ms) > cam_response_timeout_ms()) {
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
    if (!require_active_or_report(s_selected_cam_mac, "지금촬영")) return;

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
    } else if (lv_tick_elaps(s_fetch_last_progress_ms) > cam_response_timeout_ms()) {
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
static uint32_t s_renew_list_last_progress_ms;
static uint16_t s_renew_list_last_received;

/* 2026-08-10 — "시작부터 총 경과시간" 기준에서 fetch_popup_tick_fn()과 동일한 "마지막 진행
 * 이후 경과시간"(정체 감지) 기준으로 변경. 딥슬립 웨이크대기+채널동기화+SR 여러 라운드가
 * 합쳐지면 총 소요시간이 고정예산 하나로는 부족할 수 있는데, 그동안 항목이 계속 들어오고
 * 있다면(=정체 아님) 조급하게 포기할 이유가 없음 — 실사용 중 "데이터는 항상 오는데 팝업만
 * 먼저 3007로 포기" 패턴으로 발견 */
static bool renew_list_tick_fn(lv_obj_t *box)
{
    (void)box;
    if (sync_photo_list_tick(-1)) return true;

    uint16_t received = 0, total = 0;
    esp_now_photo_list_get_progress(&received, &total);
    if (received != s_renew_list_last_received) {
        s_renew_list_last_received = received;
        s_renew_list_last_progress_ms = lv_tick_get();
        return false;
    }
    if (lv_tick_elaps(s_renew_list_last_progress_ms) > cam_response_timeout_ms()) {
        ui_log_add_err(UI_ERR_LIST_NORESPONSE, "목록 갱신 요청에 CAM 응답 없음(정체, %u/%u개)",
                        (unsigned)received, (unsigned)total);
        return true;
    }
    return false;
}

static void cb_renew_list(lv_event_t *e)
{
    (void)e;
    if (!s_has_selected_cam) return;
    if (!require_active_or_report(s_selected_cam_mac, "목록 갱신")) return;

    esp_now_photo_list_request(s_selected_cam_mac);

    s_renew_list_last_progress_ms = lv_tick_get();
    s_renew_list_last_received = 0;
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
        bool timedout = !acked && lv_tick_elaps(s_delete_all_stage_start_ms) > cam_response_timeout_ms();
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
    if (lv_tick_elaps(s_delete_all_stage_start_ms) > cam_response_timeout_ms()) {
        set_stage_label(s_delete_all_stage_label, 1, STR_DELETEALL_STAGE2_UNKNOWN, red);
        return true;  /* 포기하고 닫되, 기존 목록엔 손 안 댐 */
    }
    return false;
}

static void cb_delete_all_confirmed(void *ctx)
{
    (void)ctx;
    if (!require_active_or_report(s_selected_cam_mac, "전체삭제")) return;

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
            if (esp_now_hub_get_conn_state(s_dash_nodes[i].mac) == HUB_CONN_STATE_WAITING) continue;
            lv_obj_t *row = lv_label_create(s_summary_list);
            lv_obj_set_style_text_font(row, ui_font_get(UI_FONT_SIZE_18), 0);
            /* 상태문구(페어됨/통신 중)는 매 틱 아래에서 따로 갱신 — 여기선 자리만 만듦.
             * 행 객체+mac+이름을 기억해뒀다가 구조 재생성 없이 텍스트만 갱신(2026-08-10,
             * 사용자 지시 — 이 자리만 페어됨/통신 중을 세분화해서 보여줌) */
            if (paired_count < ESP_NOW_HUB_MAX_NODES) {
                s_summary_row_objs[paired_count] = row;
                memcpy(s_summary_row_macs[paired_count], s_dash_nodes[i].mac, 6);
                strncpy(s_summary_row_names[paired_count], s_dash_nodes[i].name, ESP_NOW_LINK_NAME_LEN - 1);
                s_summary_row_names[paired_count][ESP_NOW_LINK_NAME_LEN - 1] = '\0';
            }
            paired_count++;
        }
        s_summary_row_count = (paired_count < ESP_NOW_HUB_MAX_NODES) ? paired_count : ESP_NOW_HUB_MAX_NODES;
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

    /* 요약판넬 상태문구(페어됨/통신 중)는 dash_changed와 무관하게 매 틱 갱신 —
     * 라디오 레벨 paired 토글만으로는 구조 재생성(dash_changed)을 안 트리거하므로
     * (위 node_display_equal 참고), 문구만 따로 살아있게 갱신함(2026-08-10) */
    for (int i = 0; i < s_summary_row_count; i++) {
        hub_conn_state_t st = esp_now_hub_get_conn_state(s_summary_row_macs[i]);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s (%s)", s_summary_row_names[i],
                 ui_str(st == HUB_CONN_STATE_ACTIVE ? STR_STATUS_ACTIVE : STR_STATUS_PAIRED));
        lv_label_set_text(s_summary_row_objs[i], buf);
    }

    /* 판넬2: 측정기 — 연결된 SENS 있으면 틀(TODO)만, 없으면 없음 라벨 */
    bool sensor_connected = false;
    for (int i = 0; i < total; i++) {
        if (s_dash_nodes[i].kind == HUB_NODE_KIND_SENS &&
            esp_now_hub_get_conn_state(s_dash_nodes[i].mac) != HUB_CONN_STATE_WAITING) { sensor_connected = true; break; }
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
        /* 2026-08-10 connectionless 모델 — WAITING이 아니면(PAIRED든 ACTIVE든) 계속
         * "아는 카메라"로 취급. CAM이 딥슬립 사이 무선 무응답 구간(라디오 레벨 paired=false)
         * 이어도 목록/판넬이 깜빡이며 빠졌다 나왔다 하지 않게 함(사용자 지적) */
        if (s_dash_nodes[i].kind != HUB_NODE_KIND_CAM) continue;
        if (esp_now_hub_get_conn_state(s_dash_nodes[i].mac) == HUB_CONN_STATE_WAITING) continue;
        cam_nodes[cam_count] = s_dash_nodes[i];
        memcpy(cam_macs[cam_count], s_dash_nodes[i].mac, 6);
        cam_count++;
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

/* 2026-08-10, 사용자 지시 — 값을 읽는 동안 로그가 계속 밀리면 불편하니 일시멈춤 단추 추가.
 * 타이머 자체를 pause/resume(lv_timer_pause/resume) — 멈춰있는 동안은 새 줄이 아예 안 쌓임 */
static void cb_power_log_pause_toggle(lv_event_t *e)
{
    (void)e;
    s_power_log_paused = !s_power_log_paused;
    if (s_power_log_paused) {
        if (s_power_panel_timer) lv_timer_pause(s_power_panel_timer);
        lv_label_set_text(s_power_log_pause_lbl, ui_str(STR_BTN_RESUME));
    } else {
        if (s_power_panel_timer) lv_timer_resume(s_power_panel_timer);
        lv_label_set_text(s_power_log_pause_lbl, ui_str(STR_BTN_PAUSE));
    }
}

/* 통계 탭 좌측 절전상태 판넬 갱신(2026-08-09) — 최신값으로 덮어쓰지 않고 로그처럼 한 줄씩
 * 누적(사용자 지시). 값이 실제로 바뀐 경우에만 새 줄 추가(2초 tick마다 찍으면 스팸이라
 * mac별 마지막 값을 기억해서 diff) — 로그박스(s_log_container/refresh_log_box)와 동일한
 * "누적 버퍼 + wrap 라벨 + 자동 스크롤" 구조 */
static void refresh_power_panel(lv_timer_t *t)
{
    (void)t;
    esp_now_hub_node_t nodes[ESP_NOW_HUB_MAX_NODES];
    int count = esp_now_hub_get_nodes(HUB_NODE_KIND_CAM, nodes, ESP_NOW_HUB_MAX_NODES);

    bool appended = false;
    for (int i = 0; i < count; i++) {
        if (!nodes[i].has_deepsleep_stats) continue;

        power_log_track_t *tr = NULL;
        for (int j = 0; j < ESP_NOW_HUB_MAX_NODES; j++) {
            if (s_power_log_track[j].used && memcmp(s_power_log_track[j].mac, nodes[i].mac, 6) == 0) {
                tr = &s_power_log_track[j];
                break;
            }
        }
        if (!tr) {
            for (int j = 0; j < ESP_NOW_HUB_MAX_NODES; j++) {
                if (!s_power_log_track[j].used) {
                    tr = &s_power_log_track[j];
                    tr->used = true;
                    memcpy(tr->mac, nodes[i].mac, 6);
                    tr->last_cycle_count = UINT32_MAX;  /* 이 장치의 첫 값은 무조건 한 줄 찍히게 */
                    tr->last_sleep_now_send_count = nodes[i].sleep_now_send_count;  /* 첫 값은
                                                                                        과거분이라 안 찍음 */
                    break;
                }
            }
        }
        if (!tr) continue;  /* 자리 없음 — MAX_NODES 이상은 원래 못 옴 */

        /* 2026-08-10 — SLEEP_NOW 발신도 자체 줄로(사용자 지시, -mm:ss 포함) — ds_cycle_count
         * 게이트와 독립적으로 검사해야 "보낸 바로 그 순간"에 줄이 찍힘(다음 사이클 리포트까지
         * 안 기다림) */
        if (tr->last_sleep_now_send_count != nodes[i].sleep_now_send_count) {
            tr->last_sleep_now_send_count = nodes[i].sleep_now_send_count;
            char sn_line[96];
            uint32_t sn_total_sec = lv_tick_get() / 1000;
            /* 2026-08-10, 사용자 지시 — "조용"->"Idle"로, 임계값 표시는 제거(더 이상 판단
             * 근거로 안 씀 — 최초 페어링만 리셋하는 걸로 바뀌어서 매번 다른 임계값 비교가
             * 큰 의미가 없어짐). #ff0000 ... #로 빨간색(CAM 리포트=검정/기본색과 구분) */
            lv_snprintf(sn_line, sizeof(sn_line), "#ff0000 %s: SLEEP_NOW Idle%ums -%02lu:%02lu#",
                        nodes[i].name, (unsigned)nodes[i].last_sleep_now_elapsed_ms,
                        (unsigned long)(sn_total_sec / 60), (unsigned long)(sn_total_sec % 60));
            size_t sn_cur_len  = strlen(s_power_log_buf);
            size_t sn_line_len = strlen(sn_line);
            if (sn_cur_len + sn_line_len + 2 > sizeof(s_power_log_buf)) {
                size_t keep_from = sizeof(s_power_log_buf) / 2;
                memmove(s_power_log_buf, s_power_log_buf + keep_from, sn_cur_len - keep_from + 1);
            }
            strcat(s_power_log_buf, sn_line);
            strcat(s_power_log_buf, "\n");
            appended = true;
        }

        /* ds_cycle_count는 Cntl이 리포트를 받을 때마다 직접 증가시키는 단조증가 카운터라
         * (esp_now_hub.c) 이것 하나만 비교하면 "새 보고서가 왔는가"를 정확히 알 수 있음
         * (2026-08-10, Light Sleep 시절엔 count=0이 계속 이어지는 상태를 여러 필드로 힘겹게
         * 구분해야 했음 — 매 사이클이 곧 새 리포트인 이 구조에선 그 문제 자체가 없어짐) */
        if (tr->last_cycle_count == nodes[i].ds_cycle_count) continue;
        tr->last_cycle_count = nodes[i].ds_cycle_count;

        const char *wake_str;
        switch (nodes[i].ds_last_wake_reason) {
            case CAM_WAKE_REASON_TIMER:   wake_str = ui_str(STR_WAKE_REASON_TIMER); break;
            case CAM_WAKE_REASON_RWDT:    wake_str = ui_str(STR_WAKE_REASON_RWDT); break;
            case CAM_WAKE_REASON_POWERON: wake_str = ui_str(STR_WAKE_REASON_POWERON); break;
            default:                      wake_str = ui_str(STR_WAKE_REASON_OTHER); break;
        }
        char line[144];
        lv_snprintf(line, sizeof(line), ui_str(STR_DEEPSLEEP_LINE_FMT), nodes[i].name,
                    (unsigned long)nodes[i].ds_cycle_count, wake_str,
                    (unsigned long)nodes[i].ds_last_awake_uptime_ms,
                    (unsigned long)nodes[i].ds_last_sleep_interval_sec,
                    (unsigned long)nodes[i].ds_last_actual_sleep_sec,
                    (unsigned long)nodes[i].ds_rwdt_catch_count);
        /* 2026-08-10 — 이 줄이 실제로 몇 시(mm:ss, Cntl 부팅 후 경과) 찍혔는지 붙여서, 줄 사이
         * 실제 간격을 육안으로 바로 잴 수 있게 함(사용자 지시 — "20초마다 뜬다" 같은 관찰을
         * 스톱워치 없이 확인하기 위함) */
        uint32_t total_sec = lv_tick_get() / 1000;
        size_t line_len_now = strlen(line);
        lv_snprintf(line + line_len_now, sizeof(line) - line_len_now, " -%02lu:%02lu",
                    (unsigned long)(total_sec / 60), (unsigned long)(total_sec % 60));

        size_t cur_len  = strlen(s_power_log_buf);
        size_t line_len = strlen(line);
        if (cur_len + line_len + 2 > sizeof(s_power_log_buf)) {
            size_t keep_from = sizeof(s_power_log_buf) / 2;
            memmove(s_power_log_buf, s_power_log_buf + keep_from, cur_len - keep_from + 1);
            cur_len = strlen(s_power_log_buf);
        }
        strcat(s_power_log_buf, line);
        strcat(s_power_log_buf, "\n");
        appended = true;
    }

    if (appended) {
        lv_label_set_text(s_power_log_label, s_power_log_buf);
        lv_obj_scroll_to_y(s_power_list, LV_COORD_MAX, LV_ANIM_OFF);
    } else if (count == 0 && s_power_log_buf[0] == '\0') {
        lv_label_set_text(s_power_log_label, ui_str(STR_PANEL_NO_CAMERA));
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
    /* create_dashboard_panel과 같은 이유(2026-08-09) — 위쪽 inner padding만 2px 축소,
     * row-to-row gap은 기본값(11px)에서 10px로 명시 지정 */
    lv_obj_set_style_pad_top(box, 18, 0);
    lv_obj_set_style_pad_row(box, 10, 0);

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

    /* 설정탭 시각설정 행의 현재값 표시 — 이 타이머가 만들어지는 시점(1786행)엔 아직
     * option_page/system_group_box가 안 생겨서 첫 즉시호출 땐 NULL, 이후 타이머 tick부터
     * 채워짐 */
    if (s_time_value_label) {
        char date_buf[20];
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M", &tm_buf);
        lv_label_set_text(s_time_value_label, date_buf);
    }
}

/* ════════════════════════════════════════════════════════════
 * 수동 시각설정 팝업(2026-08-09) — 연/월/일/시/분 드롭다운 5개 + 확인/취소.
 * RTC 부팅 시딩 버그(assets_root/time_sync.txt가 8/1에 박제된 채 안 갱신되던 문제,
 * [[project-cntl-rtc-and-unified-sleep-plan]]) 수정에 이어지는 항목 — 그 자동 경로가
 * 실패하거나 device가 오래 꺼져있던 경우를 위한 최종 수동 보정 수단.
 * ════════════════════════════════════════════════════════════ */
#define SETTIME_YEAR_SPAN_BEFORE 2   /* 드롭다운 연도 범위: (지금해-2) ~ (지금해+8) */
#define SETTIME_YEAR_SPAN_AFTER  8

/* count개, start부터 fmt(예: "%04d"/"%02d") 형식으로 개행-구분 옵션 문자열을 만듦
 * (연/월/일/시/분 드롭다운 공용) — 정적 문자열로 하드코딩하면 연도 목록이 시간이 지나며
 * 낡는 문제가 있어서(이번에 고친 time_sync.txt 버그와 같은 종류) 매번 현재 연도 기준으로
 * 생성함 */
static void build_numeric_options(char *out, size_t out_cap, int start, int count, const char *fmt)
{
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        int room = (int)(out_cap - pos);
        if (room <= 0) break;
        int written = snprintf(out + pos, (size_t)room, "%s", (i == 0) ? "" : "\n");
        pos += (size_t)written;
        room = (int)(out_cap - pos);
        if (room <= 0) break;
        written = snprintf(out + pos, (size_t)room, fmt, start + i);
        pos += (size_t)written;
    }
}

typedef struct {
    lv_obj_t *year_dd;
    lv_obj_t *month_dd;
    lv_obj_t *day_dd;
    lv_obj_t *hour_dd;
    lv_obj_t *min_dd;
    int       year_base;
} settime_popup_state_t;

static settime_popup_state_t s_settime_state;

static void cb_settime_confirm(lv_event_t *e)
{
    settime_popup_state_t *st = &s_settime_state;
    int year  = st->year_base + (int)lv_dropdown_get_selected(st->year_dd);
    int month = 1 + (int)lv_dropdown_get_selected(st->month_dd);
    int day   = 1 + (int)lv_dropdown_get_selected(st->day_dd);
    int hour  = (int)lv_dropdown_get_selected(st->hour_dd);
    int min   = (int)lv_dropdown_get_selected(st->min_dd);

    esp_err_t err = rtc_sync_set_datetime(year, month, day, hour, min, 0);
    if (err != ESP_OK) {
        ui_log_add_err(UI_ERR_RTC_SET_FAILED, "RTC 시각설정 실패: %s", esp_err_to_name(err));
    }
    refresh_clock(NULL);  /* 로고부제 + 이 행의 표시값을 새 시각으로 즉시 갱신(다음 1초 tick까지 안 기다림) */
    cb_modal_close(e);
}

/* 팝업 안에 [라벨][드롭다운] 한 쌍을 만드는 헬퍼 — 연/월/일/시/분 다섯 번 반복돼서 공통화 */
static lv_obj_t *add_settime_dropdown(lv_obj_t *row, int start, int count, const char *fmt,
                                       int selected, int width)
{
    char options[256];
    build_numeric_options(options, sizeof(options), start, count, fmt);

    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, (uint16_t)selected);
    lv_obj_set_width(dd, width);
    lv_obj_set_style_text_font(dd, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd), ui_font_get(UI_FONT_SIZE_18), 0);
    return dd;
}

static void show_settime_popup(void)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    int cur_year = tm_buf.tm_year + 1900;

    settime_popup_state_t *st = &s_settime_state;
    st->year_base = cur_year - SETTIME_YEAR_SPAN_BEFORE;
    int year_count = SETTIME_YEAR_SPAN_BEFORE + SETTIME_YEAR_SPAN_AFTER + 1;

    lv_obj_t *box = create_modal();
    lv_obj_set_width(box, 460);  /* 드롭다운 5개가 나란히 들어가야 해서 공용 모달 기본폭(420)보다 넓힘 */

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, ui_str(STR_TITLE_SET_TIME));
    lv_obj_set_style_text_font(title, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *picker_row = lv_obj_create(box);
    lv_obj_set_size(picker_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(picker_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(picker_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(picker_row, 0, 0);
    lv_obj_set_style_pad_column(picker_row, 4, 0);

    st->year_dd  = add_settime_dropdown(picker_row, st->year_base, year_count, "%04d",
                                         cur_year - st->year_base, 78);
    st->month_dd = add_settime_dropdown(picker_row, 1, 12, "%02d", tm_buf.tm_mon, 62);
    st->day_dd   = add_settime_dropdown(picker_row, 1, 31, "%02d", tm_buf.tm_mday - 1, 62);

    lv_obj_t *sep = lv_label_create(picker_row);
    lv_label_set_text(sep, " ");

    st->hour_dd  = add_settime_dropdown(picker_row, 0, 24, "%02d", tm_buf.tm_hour, 62);
    st->min_dd   = add_settime_dropdown(picker_row, 0, 60, "%02d", tm_buf.tm_min, 62);

    lv_obj_t *btn_row = create_modal_btn_row(box);
    add_modal_button(btn_row, STR_BTN_CONFIRM, cb_settime_confirm, NULL);
    add_modal_button(btn_row, STR_BTN_CANCEL, cb_modal_close, NULL);
}

static void cb_settime_btn(lv_event_t *e)
{
    (void)e;
    show_settime_popup();
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
    show_confirm_popup(ui_str(STR_MSG_RESTART_CONFIRM), cb_restart_confirmed, NULL);
}

/* 설정탭 Apply 5단계 플로우(2026-08-08, 사용자 설계) — 공용 진행팝업(show_progress_popup)
 * 재사용, capture/response 둘 다 이 하나의 tick 함수로 처리(어느 쪽인지는
 * s_config_apply_target로 구분). 촬영주기/응답성 각각 독립된 Apply 버튼이라 동시에 둘 다
 * 누르는 경우는 없다고 가정(2단계 팝업이 모달이라 물리적으로도 막힘) */
typedef enum { CONFIG_APPLY_TARGET_CAPTURE, CONFIG_APPLY_TARGET_RESPONSE } config_apply_target_t;
static config_apply_target_t s_config_apply_target;
static int                   s_config_apply_pending_idx;
static uint32_t              s_config_apply_start_ms;
static lv_obj_t              *s_config_apply_label;

static void update_capture_apply_enabled(void)
{
    bool changed = (lv_dropdown_get_selected(s_capture_interval_dd) != (uint16_t)s_capture_interval_applied_idx);
    if (changed) lv_obj_clear_state(s_capture_apply_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(s_capture_apply_btn, LV_STATE_DISABLED);
}

static void update_response_apply_enabled(void)
{
    bool changed = (lv_dropdown_get_selected(s_response_interval_dd) != (uint16_t)s_response_interval_applied_idx);
    if (changed) lv_obj_clear_state(s_response_apply_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(s_response_apply_btn, LV_STATE_DISABLED);
}

/* 응답성 드롭다운 선택값의 풀이를 별도 도움말 텍스트로 표시(2026-08-10) — 드롭다운 자체엔
 * 짧은 라벨(1초/3초/...)만 있어서, 그 값이 실제로 뭘 뜻하는지(즉시/빠름/균형/절전/최대절전)
 * 를 s_response_help_label에 채움. 순서는 s_response_interval_values와 반드시 같이 맞출 것 */
static void update_response_help_text(void)
{
    static const ui_str_id_t s_help_ids[] = {
        STR_RESPONSE_HELP_0, STR_RESPONSE_HELP_1, STR_RESPONSE_HELP_2,
        STR_RESPONSE_HELP_3, STR_RESPONSE_HELP_4,
    };
    uint16_t idx = lv_dropdown_get_selected(s_response_interval_dd);
    if (idx < (sizeof(s_help_ids) / sizeof(s_help_ids[0]))) {
        lv_label_set_text(s_response_help_label, ui_str(s_help_ids[idx]));
    }
}

static void cb_capture_interval_changed(lv_event_t *e) { (void)e; update_capture_apply_enabled(); }
static void cb_response_interval_changed(lv_event_t *e)
{
    (void)e;
    update_response_apply_enabled();
    update_response_help_text();
}

static void update_adaptive_apply_enabled(void)
{
    bool changed = (lv_dropdown_get_selected(s_adaptive_response_dd) != (uint16_t)s_adaptive_response_applied_idx);
    if (changed) lv_obj_clear_state(s_adaptive_apply_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(s_adaptive_apply_btn, LV_STATE_DISABLED);
}

static void cb_adaptive_response_changed(lv_event_t *e) { (void)e; update_adaptive_apply_enabled(); }

/* CAM에 안 보내는 Cntl 내부값이라(esp_now_hub.c 참고) 네트워크 왕복이 없음 — 다른 두
 * Apply(촬영주기/응답성)처럼 진행팝업을 띄울 이유가 없어서 즉시 저장하고 버튼만 도로 끔 */
static void cb_apply_adaptive_response(lv_event_t *e)
{
    (void)e;
    uint16_t idx = lv_dropdown_get_selected(s_adaptive_response_dd);
    uint32_t sec = (idx < (sizeof(s_adaptive_response_values) / sizeof(s_adaptive_response_values[0])))
                   ? s_adaptive_response_values[idx] : 0;
    device_config_set_adaptive_response_sec(sec);
    s_adaptive_response_applied_idx = idx;
    update_adaptive_apply_enabled();
}

static bool config_apply_tick_fn(lv_obj_t *box)
{
    (void)box;
    hub_config_apply_stage_t stage = esp_now_hub_get_config_apply_stage();
    if (stage == HUB_CONFIG_APPLY_ACKED) {
        lv_label_set_text(s_config_apply_label, ui_str(STR_STATUS_OK));
        lv_obj_set_style_text_color(s_config_apply_label, lv_palette_main(LV_PALETTE_GREEN), 0);
        if (s_config_apply_target == CONFIG_APPLY_TARGET_CAPTURE) {
            s_capture_interval_applied_idx = s_config_apply_pending_idx;
            update_capture_apply_enabled();
        } else {
            s_response_interval_applied_idx = s_config_apply_pending_idx;
            update_response_apply_enabled();
        }
        esp_now_hub_config_apply_stage_clear();
        return true;
    }
    if (lv_tick_elaps(s_config_apply_start_ms) > cam_response_timeout_ms()) {
        lv_label_set_text(s_config_apply_label, ui_str(STR_CONFIG_APPLY_STALLED));
        lv_obj_set_style_text_color(s_config_apply_label, lv_palette_main(LV_PALETTE_RED), 0);
        ui_log_add_err(UI_ERR_CONFIG_NORESPONSE, "설정 적용 요청에 CAM 응답 없음(타임아웃)");
        esp_now_hub_config_apply_stage_clear();
        return true;
    }
    return false;
}

static void show_config_apply_popup(void)
{
    s_config_apply_start_ms = lv_tick_get();
    lv_obj_t *box = show_progress_popup(config_apply_tick_fn);

    lv_obj_t *spinner = lv_spinner_create(box);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 0);

    s_config_apply_label = lv_label_create(box);
    lv_obj_set_style_text_font(s_config_apply_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_text(s_config_apply_label, ui_str(STR_CONFIG_APPLY_PROGRESS));
    lv_obj_set_style_text_color(s_config_apply_label, lv_palette_main(LV_PALETTE_GREY), 0);

    start_progress_popup(box);
}

static void cb_apply_capture_interval(lv_event_t *e)
{
    (void)e;
    if (!s_has_selected_cam) {
        ui_log_add("촬영주기 적용: 선택된 카메라 없음");
        return;
    }
    uint16_t idx = lv_dropdown_get_selected(s_capture_interval_dd);
    uint32_t sec = (idx < (sizeof(s_capture_interval_values) / sizeof(s_capture_interval_values[0])))
                   ? s_capture_interval_values[idx] : 0;
    s_config_apply_target = CONFIG_APPLY_TARGET_CAPTURE;
    s_config_apply_pending_idx = idx;
    /* device_config에는 항상 저장되고, CAM은 매 웨이크(=매 접속)마다 무조건 최신값을
     * 다시 받아가므로(push_cam_config_to()가 PAIR_ACK 시점에도 자동 호출됨) WAITING이어도
     * "진짜 실패"가 아니라 "다음 접속에 반영될 정상 대기 상태"임 — 2026-08-10, 사용자
     * 지적으로 require_active_or_report()(2007 에러) 대신 응답성 적용과 동일하게 정보
     * 로그만 남기도록 수정(처음엔 실수로 2007과 "저장됨" 안내가 동시에 뜨는 모순이 있었음) */
    esp_now_hub_apply_cam_capture_interval_sec(s_selected_cam_mac, sec);
    if (esp_now_hub_get_conn_state(s_selected_cam_mac) == HUB_CONN_STATE_WAITING) {
        ui_log_add("촬영주기 저장됨 — CAM 재연결 시 자동 반영");
        return;
    }
    show_config_apply_popup();
}

static void cb_apply_response_interval(lv_event_t *e)
{
    (void)e;
    uint16_t idx = lv_dropdown_get_selected(s_response_interval_dd);
    uint32_t sec = (idx < (sizeof(s_response_interval_values) / sizeof(s_response_interval_values[0])))
                   ? s_response_interval_values[idx] : 0;
    s_config_apply_target = CONFIG_APPLY_TARGET_RESPONSE;
    s_config_apply_pending_idx = idx;
    /* 반환값으로 판단(2026-08-10) — 이 설정은 특정 CAM 하나가 아니라 "지금 ACTIVE한 CAM
     * 전부"가 대상이라 require_active_or_report()의 mac 하나 기준 검사가 안 맞음. 대상이
     * 하나도 없으면(전부 WAITING) 값은 저장됐지만 응답 대기 팝업은 안 띄움 — 다른 4개
     * 통신 기능과 동일 원칙 */
    if (!esp_now_hub_apply_response_interval_sec(sec)) {
        ui_log_add("응답성 저장됨 — CAM 재연결 시 자동 반영");
        return;
    }
    show_config_apply_popup();
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
    /* 2026-08-10 — 경고 중이 아니어도 지금까지의 에러 이력을 언제든 확인할 수 있게 정상
     * 상태 로고도 탭 가능하게(cb_logo_warning_tap 재사용 — 팝업 내용 자체는 경고 유무와
     * 무관하게 항상 "지금 남아있는 이력"을 보여줌, 없으면 STR_ERROR_LIST_EMPTY) */
    lv_obj_add_flag(s_logo_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_logo_icon, cb_logo_warning_tap, LV_EVENT_CLICKED, NULL);

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
    /* 기본 테마 패딩(이 화면 DISP_LARGE 버킷 PAD_DEF=20px)이 판넬-화면 가장자리 간격과
     * 판넬 사이 세로 간격 둘 다에 그대로 쓰이고 있었음 — 절반(10px)로 줄임(2026-08-09,
     * 사용자 지시. 오른쪽은 스크롤바가 이미 자리를 차지해서 왼쪽만 특히 신경써 달라고
     * 했지만 좌우 대칭이 자연스러워서 pad_hor로 양쪽 다 줄임 — list_panel의 기존 10px
     * 축소와 같은 값) */
    lv_obj_set_style_pad_hor(dashboard_page, 5, 0);  /* screen-edge gap 추가로 절반(2026-08-09) */
    lv_obj_set_style_pad_row(dashboard_page, 5, 0);  /* inter-panel gap 추가로 절반(2026-08-09) */
    /* 탭바↔첫 판넬 사이 세로 간격 — page 자신의 pad_top(기본값 20, 지금까지 미조정)을
     * 좌우와 같은 비율로 절반. 판넬 자체 내부 padding은 안 건드림(2026-08-09) */
    lv_obj_set_style_pad_top(dashboard_page, 5, 0);  /* 추가로 절반(2026-08-09) */
    /* 페이지 배경을 기본테마 밝은 회색(lighten 4)보다 한 단계 어둡게(2026-08-09, 사용자 지시) */
    lv_obj_set_style_bg_color(dashboard_page, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_bg_opa(dashboard_page, LV_OPA_COVER, 0);

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

    /* 위(절전상태)/아래(기존 로그) 2판넬 세로 배치(2026-08-10, 사용자 지시로 가로->세로 변경 —
     * 절전상태를 더 눈에 띄게 위로) */
    lv_obj_t *stats_page = lv_tabview_add_tab(s_page_control, ui_str(STR_TAB_STATISTICS));
    lv_obj_set_flex_flow(stats_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(stats_page, 4, 0);
    lv_obj_set_style_pad_row(stats_page, 4, 0);
    lv_obj_set_style_bg_color(stats_page, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_bg_opa(stats_page, LV_OPA_COVER, 0);

    lv_obj_t *power_box = lv_obj_create(stats_page);
    lv_obj_set_size(power_box, LV_PCT(100), 320);  /* 2026-08-10, 사용자 지시 — 고정 320px */
    lv_obj_set_flex_flow(power_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(power_box, 6, 0);

    /* 제목 + 일시멈춤 단추를 한 행에(2026-08-10, 사용자 지시 — 값 읽는 동안 로그가 계속
     * 밀리지 않게 멈출 수 있게) */
    lv_obj_t *power_title_row = lv_obj_create(power_box);
    lv_obj_set_size(power_title_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(power_title_row, 0, 0);
    lv_obj_set_style_pad_all(power_title_row, 0, 0);
    lv_obj_set_flex_flow(power_title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(power_title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_power_panel_title = lv_label_create(power_title_row);
    lv_label_set_text(s_power_panel_title, ui_str(STR_PANEL_DEEPSLEEP));
    lv_obj_set_style_text_font(s_power_panel_title, ui_font_get(UI_FONT_SIZE_18), 0);

    s_power_log_pause_btn = lv_button_create(power_title_row);
    lv_obj_add_event_cb(s_power_log_pause_btn, cb_power_log_pause_toggle, LV_EVENT_CLICKED, NULL);
    s_power_log_pause_lbl = lv_label_create(s_power_log_pause_btn);
    lv_label_set_text(s_power_log_pause_lbl, ui_str(STR_BTN_PAUSE));
    lv_obj_set_style_text_font(s_power_log_pause_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* s_log_container와 동일 구조 — 스크롤 컨테이너 + 폭 100% wrap 라벨 하나, 텍스트를
     * 통째로 갈아끼우고 맨 아래로 자동 스크롤(2026-08-09, 로그처럼 누적 지시) */
    s_power_list = lv_obj_create(power_box);
    lv_obj_set_size(s_power_list, LV_PCT(100), 0);
    lv_obj_set_flex_grow(s_power_list, 1);
    lv_obj_set_scroll_dir(s_power_list, LV_DIR_VER);
    lv_obj_set_style_border_width(s_power_list, 0, 0);
    lv_obj_set_style_bg_opa(s_power_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_power_list, 6, 0);

    s_power_log_label = lv_label_create(s_power_list);
    lv_label_set_long_mode(s_power_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_power_log_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_power_log_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_recolor(s_power_log_label, true);  /* 2026-08-10 — C/S 줄을 색으로 구분(사용자
                                                        지시, "로그가 지저분해서 안 보인다") —
                                                        #RRGGBB text# 인라인 색상 문법 활성화 */
    lv_label_set_text(s_power_log_label, "");

    s_power_panel_timer = lv_timer_create(refresh_power_panel, 2000, NULL);

    s_log_container = lv_obj_create(stats_page);
    lv_obj_set_size(s_log_container, LV_PCT(100), 0);
    lv_obj_set_flex_grow(s_log_container, 1);
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
    /* dashboard_page와 같은 이유로 절반(10px) 축소(2026-08-09) — 그룹박스-화면 가장자리
     * 간격 + 그룹박스 사이 세로 간격 */
    lv_obj_set_style_pad_hor(option_page, 5, 0);  /* screen-edge gap 추가로 절반(2026-08-09) */
    lv_obj_set_style_pad_row(option_page, 5, 0);  /* inter-panel gap 추가로 절반(2026-08-09) */
    lv_obj_set_style_pad_top(option_page, 5, 0);  /* 추가로 절반(2026-08-09) */
    lv_obj_set_style_bg_color(option_page, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_bg_opa(option_page, LV_OPA_COVER, 0);

    /* 그룹박스 4개(세로로 나열): 제어기/측정기/영상/시스템 — 내용은 아직 시스템(언어
     * 전환)만 채움, 나머지는 제목만 있는 빈 틀. 언어 행: 라벨 왼쪽 정렬 + 선택
     * 버튼(한글/English) 오른쪽 정렬, 선택된 버튼은 다른 색으로 표시 */
    lv_obj_t *cntl_box = create_group_box(option_page, STR_GROUP_CNTL);

    /* 언어 행 — lv_list로 감쌌던 걸 제거(2026-08-09, 사용자 지적: lv_list는 회색
     * 배경+테두리가 있는 카드 스타일이 기본이라 영상/시스템 그룹(일반 컨테이너 직접 자식,
     * 아래 capture_row/response_row/time_row 참고)과 판넬 간 수직 간격/배경이 눈에 띄게
     * 다르게 보였음 — 다른 그룹들과 똑같이 group box(cntl_box)의 직접 자식으로 둠 */
    lv_obj_t *cntl_row = lv_obj_create(cntl_box);
    lv_obj_set_size(cntl_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cntl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cntl_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(cntl_row, 0, 0);
    lv_obj_set_style_pad_hor(cntl_row, 12, 0);
    lv_obj_set_style_pad_ver(cntl_row, 0, 0);  /* row 자체 상하 padding — 용어정의 후 0으로(2026-08-09) */

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
    lv_obj_t *restart_row = lv_obj_create(cntl_box);
    lv_obj_set_size(restart_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(restart_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(restart_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(restart_row, 0, 0);
    lv_obj_set_style_pad_hor(restart_row, 12, 0);
    lv_obj_set_style_pad_ver(restart_row, 0, 0);

    s_restart_label = lv_label_create(restart_row);
    lv_label_set_text(s_restart_label, ui_str(STR_LABEL_RESTART_DEVICE));
    lv_obj_set_style_text_font(s_restart_label, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *restart_btn = lv_button_create(restart_row);
    lv_obj_add_event_cb(restart_btn, cb_restart_btn, LV_EVENT_CLICKED, NULL);
    s_restart_btn_lbl = lv_label_create(restart_btn);
    lv_label_set_text(s_restart_btn_lbl, ui_str(STR_BTN_RESTART));
    lv_obj_set_style_text_font(s_restart_btn_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    create_group_box(option_page, STR_GROUP_SENSOR);

    /* 영상(Camera) 그룹박스 — 발견된 CAM 리스트(연결중/연결됨), 1초마다 갱신.
     * 자식이 리스트 하나뿐이라 별도 content 래퍼 없이 box 직접 자식으로 둠 */
    lv_obj_t *camera_group_box = create_group_box(option_page, STR_GROUP_CAMERA);
    s_camera_list = lv_list_create(camera_group_box);
    lv_obj_set_size(s_camera_list, LV_PCT(100), LV_SIZE_CONTENT);
    s_camera_list_timer = lv_timer_create(refresh_camera_list, 1000, NULL);

    /* 촬영주기 행(2026-08-08, 사용자 설계) — [라벨][드롭다운][Apply] 한 줄. 카메라별 설정이라
     * 이 그룹박스(영상)에 유지, 응답성은 시스템 공통이라 아래 STR_GROUP_SYSTEM으로 이동 */
    lv_obj_t *capture_row = lv_obj_create(camera_group_box);
    lv_obj_set_size(capture_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(capture_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(capture_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(capture_row, 0, 0);
    lv_obj_set_style_pad_hor(capture_row, 12, 0);
    lv_obj_set_style_pad_ver(capture_row, 0, 0);

    s_capture_interval_label = lv_label_create(capture_row);
    lv_label_set_text(s_capture_interval_label, ui_str(STR_LABEL_CAPTURE_INTERVAL));
    lv_obj_set_style_text_font(s_capture_interval_label, ui_font_get(UI_FONT_SIZE_18), 0);

    s_capture_interval_dd = lv_dropdown_create(capture_row);
    lv_dropdown_set_options(s_capture_interval_dd, ui_str(STR_OPT_CAPTURE_INTERVAL_LIST));
    s_capture_interval_applied_idx = find_value_index(s_capture_interval_values,
        sizeof(s_capture_interval_values) / sizeof(s_capture_interval_values[0]),
        device_config_get_cam_capture_interval_sec());
    lv_dropdown_set_selected(s_capture_interval_dd, (uint16_t)s_capture_interval_applied_idx);
    /* 드롭다운 기본폰트는 한글 글리프가 없는 LVGL 내장 폰트 — 닫힌 상태 표시(MAIN)와 펼친
     * 목록(lv_dropdown_get_list) 둘 다 커스텀 TTF로 따로 지정해야 함(안 하면 깨져 보임,
     * 2026-08-08 실기에서 확인 — cntl_row의 s_btn_ko 체크박스와 같은 이유) */
    lv_obj_set_style_text_font(s_capture_interval_dd, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_capture_interval_dd), ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_event_cb(s_capture_interval_dd, cb_capture_interval_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_capture_apply_btn = lv_button_create(capture_row);
    lv_obj_add_event_cb(s_capture_apply_btn, cb_apply_capture_interval, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_capture_apply_btn, LV_STATE_DISABLED);  /* 부팅 직후엔 표시값==저장값 */
    s_capture_apply_lbl = lv_label_create(s_capture_apply_btn);  /* 전역: refresh_lang_texts에서 갱신 */
    lv_label_set_text(s_capture_apply_lbl, ui_str(STR_BTN_APPLY));
    lv_obj_set_style_text_font(s_capture_apply_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 시스템(System) 그룹박스 — 응답성(연결성/절전, 전체 공통 하나) 행. 촬영주기와 같은
     * [라벨][드롭다운][Apply] 인라인 레이아웃 */
    lv_obj_t *system_group_box = create_group_box(option_page, STR_GROUP_SYSTEM);
    lv_obj_t *response_row = lv_obj_create(system_group_box);
    lv_obj_set_size(response_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(response_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(response_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(response_row, 0, 0);
    lv_obj_set_style_pad_hor(response_row, 12, 0);
    lv_obj_set_style_pad_ver(response_row, 0, 0);

    s_response_interval_label = lv_label_create(response_row);
    lv_label_set_text(s_response_interval_label, ui_str(STR_LABEL_RESPONSE_INTERVAL));
    lv_obj_set_style_text_font(s_response_interval_label, ui_font_get(UI_FONT_SIZE_18), 0);

    s_response_interval_dd = lv_dropdown_create(response_row);
    lv_dropdown_set_options(s_response_interval_dd, ui_str(STR_OPT_RESPONSE_INTERVAL_LIST));
    s_response_interval_applied_idx = find_value_index(s_response_interval_values,
        sizeof(s_response_interval_values) / sizeof(s_response_interval_values[0]),
        device_config_get_response_interval_sec());
    lv_dropdown_set_selected(s_response_interval_dd, (uint16_t)s_response_interval_applied_idx);
    lv_obj_set_style_text_font(s_response_interval_dd, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_response_interval_dd), ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_event_cb(s_response_interval_dd, cb_response_interval_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_response_apply_btn = lv_button_create(response_row);
    lv_obj_add_event_cb(s_response_apply_btn, cb_apply_response_interval, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_response_apply_btn, LV_STATE_DISABLED);
    s_response_apply_lbl = lv_label_create(s_response_apply_btn);
    lv_label_set_text(s_response_apply_lbl, ui_str(STR_BTN_APPLY));
    lv_obj_set_style_text_font(s_response_apply_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 응답성 도움말 행(2026-08-10) — 드롭다운/버튼 없이 라벨 하나만, response_row 바로
     * 아래 형제 행(time_row와 같은 방식으로 system_group_box에 얹음) */
    lv_obj_t *response_help_row = lv_obj_create(system_group_box);
    lv_obj_set_size(response_help_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(response_help_row, 0, 0);
    lv_obj_set_style_pad_hor(response_help_row, 12, 0);
    lv_obj_set_style_pad_ver(response_help_row, 0, 0);

    s_response_help_label = lv_label_create(response_help_row);
    lv_obj_set_width(s_response_help_label, LV_PCT(100));
    lv_label_set_long_mode(s_response_help_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(s_response_help_label, &style_text_muted, 0);
    lv_obj_set_style_text_font(s_response_help_label, ui_font_get(UI_FONT_SIZE_18), 0);
    update_response_help_text();  /* 부팅 직후 현재 선택값 반영 */

    /* 적응형 반응시간 행(2026-08-10) — 응답성 행과 같은 [라벨][드롭다운][Apply] 구조.
     * CAM에 안 보내는 Cntl 내부값(esp_now_hub.c의 adaptive_sleep_timer_cb 참고) */
    lv_obj_t *adaptive_row = lv_obj_create(system_group_box);
    lv_obj_set_size(adaptive_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(adaptive_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(adaptive_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(adaptive_row, 0, 0);
    lv_obj_set_style_pad_hor(adaptive_row, 12, 0);
    lv_obj_set_style_pad_ver(adaptive_row, 0, 0);

    s_adaptive_response_label = lv_label_create(adaptive_row);
    lv_label_set_text(s_adaptive_response_label, ui_str(STR_LABEL_ADAPTIVE_RESPONSE));
    lv_obj_set_style_text_font(s_adaptive_response_label, ui_font_get(UI_FONT_SIZE_18), 0);

    s_adaptive_response_dd = lv_dropdown_create(adaptive_row);
    lv_dropdown_set_options(s_adaptive_response_dd, ui_str(STR_OPT_ADAPTIVE_RESPONSE_LIST));
    s_adaptive_response_applied_idx = find_value_index(s_adaptive_response_values,
        sizeof(s_adaptive_response_values) / sizeof(s_adaptive_response_values[0]),
        device_config_get_adaptive_response_sec());
    lv_dropdown_set_selected(s_adaptive_response_dd, (uint16_t)s_adaptive_response_applied_idx);
    lv_obj_set_style_text_font(s_adaptive_response_dd, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_adaptive_response_dd), ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_event_cb(s_adaptive_response_dd, cb_adaptive_response_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_adaptive_apply_btn = lv_button_create(adaptive_row);
    lv_obj_add_event_cb(s_adaptive_apply_btn, cb_apply_adaptive_response, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_adaptive_apply_btn, LV_STATE_DISABLED);
    s_adaptive_apply_lbl = lv_label_create(s_adaptive_apply_btn);
    lv_label_set_text(s_adaptive_apply_lbl, ui_str(STR_BTN_APPLY));
    lv_obj_set_style_text_font(s_adaptive_apply_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 적응형 반응시간 도움말 행 — response_help_row와 같은 구조(고정 문구 하나뿐, 값별로 안 바뀜) */
    lv_obj_t *adaptive_help_row = lv_obj_create(system_group_box);
    lv_obj_set_size(adaptive_help_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(adaptive_help_row, 0, 0);
    lv_obj_set_style_pad_hor(adaptive_help_row, 12, 0);
    lv_obj_set_style_pad_ver(adaptive_help_row, 0, 0);

    s_adaptive_help_label = lv_label_create(adaptive_help_row);
    lv_obj_set_width(s_adaptive_help_label, LV_PCT(100));
    lv_label_set_long_mode(s_adaptive_help_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(s_adaptive_help_label, &style_text_muted, 0);
    lv_obj_set_style_text_font(s_adaptive_help_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_text(s_adaptive_help_label, ui_str(STR_HELP_ADAPTIVE_RESPONSE));

    /* 시각설정 행(2026-08-09) — [라벨][현재시각][설정] 인라인, 응답성 행과 같은 구조 */
    lv_obj_t *time_row = lv_obj_create(system_group_box);
    lv_obj_set_size(time_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_hor(time_row, 12, 0);
    lv_obj_set_style_pad_ver(time_row, 0, 0);

    s_time_label = lv_label_create(time_row);
    lv_label_set_text(s_time_label, ui_str(STR_LABEL_TIME));
    lv_obj_set_style_text_font(s_time_label, ui_font_get(UI_FONT_SIZE_18), 0);

    s_time_value_label = lv_label_create(time_row);
    lv_obj_add_style(s_time_value_label, &style_text_muted, 0);
    lv_obj_set_style_text_font(s_time_value_label, ui_font_get(UI_FONT_SIZE_18), 0);
    refresh_clock(NULL);  /* 다음 1초 tick 전까지 빈 채로 안 보이게 즉시 한 번 채움(로고부제와 동일 이유) */

    lv_obj_t *time_set_btn = lv_button_create(time_row);
    lv_obj_add_event_cb(time_set_btn, cb_settime_btn, LV_EVENT_CLICKED, NULL);
    s_time_set_btn_lbl = lv_label_create(time_set_btn);
    lv_label_set_text(s_time_set_btn_lbl, ui_str(STR_BTN_SET_TIME));
    lv_obj_set_style_text_font(s_time_set_btn_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 버튼 폭 통일(2026-08-09, 사용자 지시) — 지금까지 만든 메인 화면 버튼들의 실측
     * 자연폭 중 최댓값을 기준폭으로 잡아 전부에 적용. 팝업 버튼(add_modal_button)은
     * s_action_btn_width가 여기서 설정된 뒤부터 뜨므로 자동으로 같은 폭을 받음.
     * 사진목록의 del_btn(휴지통 아이콘, 반복되는 작은 버튼)은 성격이 달라 제외 */
    lv_obj_t *action_buttons[] = {
        btn_capture, btn_renew, btn_delete_all, restart_btn,
        s_capture_apply_btn, s_response_apply_btn, s_adaptive_apply_btn, time_set_btn,
    };
    lv_obj_update_layout(lv_screen_active());
    for (size_t i = 0; i < sizeof(action_buttons) / sizeof(action_buttons[0]); i++) {
        lv_coord_t w = lv_obj_get_width(action_buttons[i]);
        if (w > s_action_btn_width) s_action_btn_width = w;
    }
    for (size_t i = 0; i < sizeof(action_buttons) / sizeof(action_buttons[0]); i++) {
        lv_obj_set_width(action_buttons[i], s_action_btn_width);
        lv_obj_center(lv_obj_get_child(action_buttons[i], 0));  /* 레이블 중앙정렬(2026-08-09) */
    }
}
