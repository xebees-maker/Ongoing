#include "ui_main.h"
#include "ui_strings.h"
#include "ui_font.h"
#include "esp_now_hub.h"
#include "device_config.h"
#include "stats_store.h"
#include "sd_storage.h"
#include "esp_now_photo.h"
#include "ui_log.h"
#include "rtc_sync.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ui_main";

/* 벤더 lv_demo_widgets 내부 함수/전역 — 로고 타이틀 스타일(style_text_muted)은 계속 씀.
 * 통계 탭의 Analytics 위젯은 로그박스로 교체함(2026-08-01, 사용자 지시). 공개
 * 헤더(lv_demos.h)엔 없고 데모 내부 전용 헤더에만 선언돼 있어서 직접 extern 선언해서 씀. */
extern void lv_demo_widgets_components_init(void);
extern lv_obj_t *lv_demo_widgets_title_create(lv_obj_t *parent, const char *text);
extern lv_style_t style_text_muted;

/* 2026-09-08(사용자 재설계) — lv_tabview 제거, 단일 주화면 구조. s_main_screen이 화면
 * 전체의 루트, s_top_bar가 그 첫 자식(구 tab_bar 자리) */
static lv_obj_t *s_main_screen = NULL;
static lv_obj_t *s_top_bar     = NULL;
static lv_obj_t *s_logo_title = NULL;   /* "플렉스팜/FlexFarm" — 언어 전환 시 갱신 필요 */
static lv_obj_t *s_lang_label = NULL;
static lv_obj_t *s_btn_ko = NULL;
static lv_obj_t *s_btn_en = NULL;

/* 2026-09-08 — 상단바 좌측 시각/네트워크 컨트롤(구 s_clock_label 하나였던 걸 분리) */
static lv_obj_t *s_time_ctrl_label    = NULL;
static lv_obj_t *s_network_ctrl_label = NULL;

/* 2026-09-08 — 상단바 설정 버튼(구 탭바 버튼 대체). 통계 버튼은 없앰(사용자 지시 —
 * "센서 판넬 Sensor 역상을 누르면 열리게") — cb_stats_btn_tap은 그 트리거로 재사용됨 */
static lv_obj_t *s_settings_btn     = NULL;
static lv_obj_t *s_settings_btn_lbl = NULL;

/* 상태 아이콘 정상/경고/에러 3단계 — 확인 안 한 실패(메모리/통신)가 하나라도 있으면 경고/
 * 에러 아이콘으로 바뀜(2026-08-01, 사용자 지시). 토스트는 몇 초 뒤 사라지지만 이 아이콘은
 * 세션 내내(재부팅 전까지) 남아있어서 "한 번이라도 실패가 있었다"를 계속 알려줌.
 * 2026-08-11, 사용자 지시로 에러/워닝 두 심각도로 분리 — 에러=확인해도 아이콘 유지(재부팅
 * 전까지 안 없어짐), 워닝=팝업으로 확인하면 그 즉시 아이콘 원상복구. 둘 다 활성이면 더
 * 심각한 에러가 우선. 2026-09-08 재설계 — 이진 토글(로고/노랑삼각형)이었던 걸 3개 독립
 * 위젯(녹색원/노랑삼각형/빨강원)으로 확장, 어느 게 보일지는 update_logo_warning_display()가
 * 매번 다시 계산 */
static lv_obj_t *s_status_normal  = NULL;
static lv_obj_t *s_status_warning = NULL;
static lv_obj_t *s_status_error   = NULL;
static bool      s_error_active = false;
static bool      s_warn_active  = false;
/* 2026-09-11(SD 신뢰성 재설계, 사용자 지시: "그럼 상태 버튼이 빨간색으로 되야하는 중대한
 * 에러야") — SD 읽기/쓰기 I/O 실패 회로차단기. s_error_active와 달리 재연결/포맷으로 실제
 * 해소되면 false로 되돌아갈 수 있음(단, 다른 에러가 하나라도 있으면 그쪽 정책대로 계속
 * 빨강 유지 — update_logo_warning_display()에서 OR 조건으로 합쳐짐). 정의는 SD 상태/복구
 * 섹션(mark_sd_io_fail 등)에 있고 여기서는 update_logo_warning_display()가 값만 읽음 */
static bool      s_sd_io_fail_active = false;

/* 상단 에러 토스트 — 진행팝업(create_modal)과 달리 배경을 안 가리고 입력도 안 막음,
 * 몇 초 뒤 자동으로 없어짐 */
static lv_obj_t *s_toast = NULL;
static uint32_t  s_toast_expire_ms = 0;

/* 그룹박스 — 제목이 있는 판넬(내용 없어도 제목은 항상 있음) */
static lv_obj_t *s_group_title[STR_GROUP_SYSTEM - STR_GROUP_CNTL + 1];

/* 상황판 판넬 3개(요약/측정기/카메라) — refresh_lang_texts()가 참조하므로 그 정의보다
 * 먼저 선언돼야 함(파일 스코프 static은 선언 지점 이후부터만 참조 가능) */
static lv_obj_t          *s_dash_title[3];  /* 0=요약, 1=측정기, 2=카메라 */
static bool               s_camera_title_enabled_prev = false;  /* 2026-09-08 — Camera 역상 회색/흰색 전환용 */
static lv_obj_t          *s_web_url_label       = NULL;  /* 2026-08-21 — 요약 맨 윗줄, 웹 대시보드 접속 URL(사용자 지시) */
static lv_obj_t          *s_web_row             = NULL;  /* 2026-09-09 — "Web " 접두문구+URL 둘로 분리(접두문구는 밑줄 없음) 위 행 래퍼 */
static lv_obj_t          *s_web_prefix_label    = NULL;
static lv_obj_t          *s_mem_status_label    = NULL;  /* 2026-08-21 — 요약 둘째줄, 여유 메모리 상시 표시(사용자 지시) */
static lv_obj_t          *s_storage_status_label = NULL;  /* 2026-09-10 — 메모리 줄 바로 아래, SD Storage(Picture/Measure/Total) 상시 표시(사용자 설계) */
/* 2026-09-08(연결 기능 주화면 이관, 사용자 설계) — Summary 실시간 순시치 블록. 통계
 * Overview와 같은 채널 4개(온도/습도/CO2/암모니아)지만 스케일/min/max/avg 없이 그냥
 * "지금 값"만 — 여러 센서가 같은 채널을 보고하면 첫 번째로 찾은 것만 씀(오늘은 센서
 * 1개뿐이라 실질적으로 문제 없음) */
static lv_obj_t          *s_summary_live_temp_label = NULL;
static lv_obj_t          *s_summary_live_humi_label = NULL;
static lv_obj_t          *s_summary_live_co2_label  = NULL;
static lv_obj_t          *s_summary_live_nh3_label  = NULL;
static lv_obj_t          *s_sensor_empty        = NULL;
static lv_obj_t          *s_camera_empty        = NULL;
static lv_obj_t          *s_camera_content      = NULL;  /* 카메라 판넬 툴바 — 아래 split_row와 함께 토글 */
static lv_obj_t          *s_camera_split_row    = NULL;
/* 2026-09-08(카메라 팝업 추출) — camera_box 자체(팝업 열고닫을 때 toolbar/split_row를
 * 여기로/팝업으로 재부모화하는 데 씀), 그리고 팝업이 닫혀있는 평상시 주화면에 남는
 * "연결된 카메라" 목록(요약판넬과 동일한 행 패턴, s_camera_list라는 이름은 설정탭 카메라
 * 발견 리스트가 이미 씀 — 충돌 피하려고 s_camera_dash_* 접두어 사용) */
static lv_obj_t          *s_camera_box          = NULL;
static lv_obj_t          *s_camera_dash_list    = NULL;
static lv_obj_t          *s_camera_photo_label  = NULL;
static lv_obj_t          *s_camera_capture_lbl  = NULL;
static lv_obj_t          *s_camera_renew_lbl    = NULL;
static lv_obj_t          *s_camera_renew_btn    = NULL;  /* 2026-09-04 — 웹 합성용(버튼 자체를
                                                              찾아 탭 이벤트를 보내야 해서 저장) */
static lv_obj_t          *s_camera_delete_all_lbl = NULL;
static lv_obj_t          *s_list_title          = NULL;
static lv_obj_t          *s_list_info_label     = NULL;  /* "N개(Pic.)  XX%" — 목록 제목 오른쪽 */
static lv_obj_t          *s_picture_title       = NULL;
static lv_obj_t          *s_photo_list          = NULL;  /* 목록 판넬 — 설정탭 CAM 발견 리스트(s_camera_list)와는 다른 위젯 */
static lv_obj_t          *s_photo_box           = NULL;  /* 사진 판넬 — 사진 오면 이 안의 라벨을 lv_image로 교체 */
static lv_obj_t          *s_photo_image         = NULL;  /* s_photo_box 안의 lv_image(사진 오기 전엔 NULL) */
/* 2026-08-21 — 내부(비-PSRAM) DRAM이 httpd_start 실패(5005)를 겪을 만큼 빠듯했던 걸 실기로
 * 확인 — PSRAM으로 옮김(ui_init()에서 할당) */
static esp_now_hub_node_t *s_dash_nodes = NULL;
static esp_now_hub_node_t *s_dash_nodes_prev = NULL;
static int                s_dash_count_prev = -1;  /* -1: 아직 비교 대상 없음(첫 실행은 항상 그림) */

/* 2026-09-08(카메라 팝업 추출 — 주화면 카메라판넬에 남는 목록) — CAM 노드만 필터링해서
 * 보여줌 */
static lv_obj_t *s_camera_dash_row_objs[ESP_NOW_HUB_MAX_NODES];
static uint8_t   s_camera_dash_row_macs[ESP_NOW_HUB_MAX_NODES][6];
static char      s_camera_dash_row_names[ESP_NOW_HUB_MAX_NODES][ESP_NOW_LINK_NAME_LEN];  /* 항상 진짜 장치명(ID) */
/* 2026-09-09(사용자 설계 — "장치명 배열에 Alias field를 추가하는 게 맞아보이는데") — name과
 * 나란히, 같은 dash_changed 시점에 채워지는 표시용 Alias(없으면 빈 문자열). name을 절대
 * 덮어쓰지 않음 — build_device_popup() 등 ID가 필요한 곳은 항상 name을 씀 */
static char      s_camera_dash_row_alias[ESP_NOW_HUB_MAX_NODES][DEVICE_CONFIG_ALIAS_MAX_LEN];
static int       s_camera_dash_row_count = 0;
static char      s_camera_dash_row_last_text[ESP_NOW_HUB_MAX_NODES][96];
static lv_obj_t *s_camera_dash_row_signal[ESP_NOW_HUB_MAX_NODES];

/* 2026-09-08(연결 기능 주화면 이관) — Sensor 판넬에 남는 "연결됨" 목록, 카메라 대시 목록과
 * 완전히 동일한 패턴 + 측정주기(개별설정값) 표시만 추가 */
static lv_obj_t          *s_sensor_dash_list    = NULL;
/* "대기중" 소제목 2개(2026-09-08) — refresh_lang_texts에서 갱신하려면 전역이어야 함
 * (s_stats_table_header_lbl과 동일 이유). 2026-09-09(사용자 지시 — "connected 표기는
 * 필요 없어 보여") — "연결됨" 표제는 제거, 목록 자체로 충분 */
static lv_obj_t          *s_sensor_pending_lbl   = NULL;
static lv_obj_t          *s_camera_pending_lbl   = NULL;
static lv_obj_t *s_sensor_dash_row_objs[ESP_NOW_HUB_MAX_NODES];
static uint8_t   s_sensor_dash_row_macs[ESP_NOW_HUB_MAX_NODES][6];
static char      s_sensor_dash_row_names[ESP_NOW_HUB_MAX_NODES][ESP_NOW_LINK_NAME_LEN];  /* 항상 진짜 장치명(ID) */
static char      s_sensor_dash_row_alias[ESP_NOW_HUB_MAX_NODES][DEVICE_CONFIG_ALIAS_MAX_LEN];  /* 표시용, name과 별도(위 카메라 배열 주석 참고) */
static int       s_sensor_dash_row_count = 0;
static char      s_sensor_dash_row_last_text[ESP_NOW_HUB_MAX_NODES][96];
static lv_obj_t *s_sensor_dash_row_signal[ESP_NOW_HUB_MAX_NODES];

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

/* 2026-09-05 — 측정 주기 Apply 대상 센서. 카메라의 상황판 드롭다운 선택기와 달리, 사용자
 * 지시("이미 있는 설정-측정기 목록 ... 선택된 센서의 값을 지정")로 설정탭의 기존 측정기
 * 목록 자체를 선택기로 재사용 — 새 위젯을 따로 안 만듦. 행을 탭하면(연결/해제 확인팝업은
 * 그대로 뜨되) 그 센서가 조용히 선택됨(cb_sensor_item_clicked 참고) */
static uint8_t            s_selected_sensor_mac[6];
static bool               s_has_selected_sensor = false;

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

/* 자동연결 스위치 2개(2026-09-08, 사용자 설계) — "신규 장치도 자동연결"이 켜지면 "이전 연결
 * 장치 자동연결"을 사실상 포함하므로(사용자 지적), UI로 그 종속관계를 표현: new가 켜지면
 * known은 강제로 켜진 채 비활성화(끌 수 없음) — cb_auto_connect_new_changed 참고 */
static lv_obj_t *s_auto_connect_known_switch = NULL;
static lv_obj_t *s_auto_connect_known_label  = NULL;
static lv_obj_t *s_auto_connect_new_switch   = NULL;
static lv_obj_t *s_auto_connect_new_label    = NULL;

/* AGC/AEC On/Off(2026-08-21, 세로줄 노이즈 진단용) — 촬영주기와 같은 카메라별 설정이라
 * 같은 그룹박스(영상). 드롭다운+Apply 대신 스위치로 토글 즉시 반영(값이 불리언 하나뿐이라
 * 별도 확인 팝업 없이 — adaptive_response와 같은 "즉시 저장" 원칙, 다만 이건 CAM에도
 * 전송됨) */
static lv_obj_t *s_agc_switch = NULL;
static lv_obj_t *s_agc_label  = NULL;
static lv_obj_t *s_aec_switch = NULL;
static lv_obj_t *s_aec_label  = NULL;

/* XCLK 프리셋 행(2026-08-21, 화질/노이즈 진단용) — 촬영주기와 같은 [라벨][드롭다운][Apply]
 * 패턴(값이 4단계 프리셋이라 스위치 대신 드롭다운, PLL 재계산이 있어서 토글 즉시적용 대신
 * 명시적 Apply). 카메라별 설정이라 같은 그룹박스(영상) */
static const uint32_t s_xclk_values[] = { 5, 10, 20, 24 };
static lv_obj_t *s_xclk_dd          = NULL;
static lv_obj_t *s_xclk_apply_btn   = NULL;
static lv_obj_t *s_xclk_label       = NULL;
static lv_obj_t *s_xclk_apply_lbl   = NULL;
static int       s_xclk_applied_idx = -1;

/* 측정 주기 행(2026-09-05, 사용자 설계) — [라벨][드롭다운][Apply], 촬영주기와 동일 패턴.
 * 센서별 설정이라 값은 device_config의 mac 키 슬롯에 저장(device_config_get/set_sens_
 * sample_interval_sec) — 대상은 s_selected_sensor_mac(위 참고). 선택된 센서가 바뀔 때마다
 * applied_idx를 그 센서의 저장값으로 다시 맞춰야 함(카메라는 대상이 하나뿐이라 이 재조정이
 * 필요 없었음 — select_sensor() 참고) */
static const uint32_t s_sens_measure_interval_values[] = { 10, 30, 60, 300, 1800 };
static lv_obj_t *s_sens_measure_dd          = NULL;
static lv_obj_t *s_sens_measure_apply_btn   = NULL;
static lv_obj_t *s_sens_measure_label       = NULL;
static lv_obj_t *s_sens_measure_apply_lbl   = NULL;
static int       s_sens_measure_applied_idx = -1;

/* 네트워크 행(2026-08-29) — [라벨][독립/종속 드롭다운][우측: IP 또는 SSID 또는 찾기버튼].
 * 우측 내용은 부팅 시점이 아니라 실제 연결상태(STA는 IP를 받아야 "연결됨")에 따라 바뀔 수
 * 있어서, 두 위젯(라벨+버튼) 다 미리 만들어두고 refresh_network_right_zone()에서 매 틱
 * 보이기/숨기기만 토글(refresh_dashboard 타이머에 얹어서 재사용, ui_main.c 관례) */
static lv_obj_t *s_network_label      = NULL;
static lv_obj_t *s_network_mode_dd    = NULL;
static lv_obj_t *s_network_right_label = NULL;  /* AP모드: IP, STA모드+연결됨: SSID */
static lv_obj_t *s_network_find_btn   = NULL;   /* STA모드+미연결일 때만 보임 */
static lv_obj_t *s_network_find_lbl   = NULL;
static void refresh_network_right_zone(void);  /* fwd — refresh_dashboard(위쪽)와 행 생성부(아래쪽) 둘 다에서 씀 */
static lv_obj_t *create_row_right_cluster(lv_obj_t *row);  /* fwd — refresh_dashboard(위쪽, 대시 목록 행 화살표)와 정의부(아래쪽) 둘 다에서 씀 */

/* 2026-09-08(사용자 재설계 — "UI를 완전히 바꾸려고 해... 단일 화면") — 통계/설정은 상단바
 * 버튼이 여는 전체화면 팝업, 로그는 설정 팝업 안에 중첩(콘텐츠 바꿔치기). 상단바 버튼
 * 클릭 핸들러(cb_stats_btn_tap 등)가 호출하므로 그보다 앞서 fwd 필요 */
static void build_stats_tab(void);
static void build_option_tab(void);
static void build_log_tab(void);
static void teardown_stats_tab(void);
static void teardown_option_tab(void);
static void teardown_log_tab(void);
static void cb_stats_btn_tap(lv_event_t *e);
/* 2026-09-10(SD 신뢰성 재설계, [[project_cntl_sd_reliability_redesign_2026_09_10]]) — SD 자체
 * I/O 오류(fail) 발생 시 호출 — 주화면 SD 상태 표시를 갱신. 정의는 주화면 SD 상태 섹션에 있고,
 * 통계탭 회로차단기(refresh_stats_page)가 그보다 앞에서 이걸 부르므로 fwd 필요 */
static void report_sd_io_fail(const char *context);
/* 2026-09-11(사용자 지시 — "상태 버튼을 눌렀을 때... 이 팝업에서 포맷을 하든지 해야되") —
 * 재연결/포맷 버튼을 기존 에러/워닝 목록 팝업(cb_logo_warning_tap, 이 fwd보다 먼저 정의됨)
 * 안에 넣어야 해서 그보다 앞서 fwd 필요. 정의는 SD 상태/복구 섹션에 있음 */
static void cb_sd_reconnect_tap(lv_event_t *e);
static void cb_sd_format_tap(lv_event_t *e);
/* 2026-09-11(재설계 — "Resolve" 공용 버튼) — cb_logo_warning_tap(이 fwd보다 먼저 정의됨)의
 * 5008/5009 행이 씀. 정의는 SD 상태/복구 섹션에 있음 */
static void cb_sd_resolve_tap(lv_event_t *e);
/* 2026-09-11(에러목록 팝업 행별 "지우기" 재설계) — cb_dismiss_error_row/cb_dismiss_warn_row
 * (cb_logo_warning_tap 근처, 이 fwd보다 먼저 정의됨)가 씀. 정의는 SD 상태/복구 섹션에 있음 */
static void sync_error_warn_active_from_history(void);
static void cb_settings_btn_tap(lv_event_t *e);
static void cb_option_log_btn_tap(lv_event_t *e);
static void cb_network_ctrl_tap(lv_event_t *e);
/* 2026-09-08(카메라 팝업 추출) — 카메라판넬 제목이 여는 전체화면 팝업. 기존 toolbar/
 * split_row 위젯을 새로 안 만들고 재부모화(lv_obj_set_parent)만 해서 여는 방식이라
 * on_photo_result_event 등 기존 비동기 콜백을 하나도 안 건드림(위젯이 죽지 않고 그대로
 * 살아있음) */
static void build_camera_tab(void);
static void teardown_camera_tab(void);
static void cb_camera_btn_tap(lv_event_t *e);
/* 2026-09-08(연결 기능 주화면 이관) — 연결된 장치 행 탭이 여는 개별설정 팝업. select_sensor()/
 * s_sens_measure_dd 등 기존 측정주기 위젯/로직을 그대로 재사용(설정탭에서 이 팝업 안으로
 * 옮겨오는 것뿐, 로직 자체는 안 바뀜)하므로 그보다 뒤에 정의되지만 여기서 fwd 필요 */
static void build_device_popup(const uint8_t *mac, const char *name, bool is_sensor);
static void teardown_device_popup(void);
static void cb_camera_dash_row_clicked(lv_event_t *e);
static void cb_sensor_dash_row_clicked(lv_event_t *e);
/* 2026-09-08(연결 기능 주화면 이관) — Summary 실시간 순시치 블록(refresh_dashboard가 매 틱
 * 호출), 정의는 아래(Summary 판넬 구성부 근처) */
static void refresh_summary_live_values(const esp_now_hub_node_t *nodes, int count);

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
 * s_response_help_texts(아래)와 사용자 확인된 표 그대로 — 반드시 같이 바꿀 것.
 * 2026-08-11, 사용자 지시로 첫 단계를 1(1초마다 반복 취침)에서 0(센티널 — "아예 안 재움")
 * 으로 정정 — "즉시" 등급은 원래부터 짧은 주기로 반복 취침한다는 뜻이 아니라 딥슬립
 * 자체를 안 한다는 의도였음(오해로 1초 리터럴 값이 들어가 있었음). 0이면 CNTL은
 * SLEEP_NOW를 아예 안 보내고(esp_now_hub.c try_send_sleep_now), CAM도 재요청 없이 그냥
 * 계속 깨있음(cam_node.c cam_node_wake_window_done) */
static const uint32_t s_response_interval_values[] = { 0, 3, 10, 30, 60 };
/* 적응형 반응시간(2026-08-10) — 10초/30초/1분/5분(2026-08-11, 사용자 지시로 5분 추가).
 * STR_OPT_ADAPTIVE_RESPONSE_LIST 순서와 반드시 같이 맞출 것 */
static const uint32_t s_adaptive_response_values[] = { 10, 30, 60, 300 };

static int find_value_index(const uint32_t *values, int count, uint32_t v)
{
    for (int i = 0; i < count; i++) if (values[i] == v) return i;
    /* 2026-08-11 버그수정 — 예전엔 못 찾으면 0을 반환했는데, 호출부(s_*_applied_idx)는
     * "-1 = 아직 모름/적용된 적 없음"을 이미 그 의미로 쓰고 있었음(선언부 주석 참고).
     * 0을 반환하면 "못 찾음"과 "인덱스 0이 진짜 적용된 값"이 구분이 안 돼서, 오늘
     * s_response_interval_values의 첫 값을 1->0으로 바꾼 뒤 예전에 저장된 값(1)이 새
     * 배열에 없어 못 찾았는데도 우연히 인덱스 0("즉시")과 같은 값으로 취급돼 Apply
     * 버튼이 처음부터 비활성 상태로 잘못 잠겨있었음(실사용 중 발견) */
    return -1;
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
/* 2026-08-21 — 내부(비-PSRAM) DRAM이 httpd_start 실패(5005)를 겪을 만큼 빠듯했던 걸 실기로
 * 확인 — 화면 표시용 목록이라 PSRAM으로 옮김(ui_init()에서 할당) */
static esp_now_photo_list_view_item_t *s_current_list = NULL;
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
static lv_obj_t *s_log_panel_title = NULL;  /* 2026-08-11 — 스크롤 안 되는 고정 제목 행, 페이지
                                                스크롤을 잡기 위한 영역. refresh_lang_texts에서 갱신 */

/* 통계 탭 좌측 절전상태 판넬 — CAM의 Deep Sleep 사이클 통계(2026-08-10 Light Sleep 폐기 후
 * 개편, 2026-08-25부터 ESP_NOW_MSG_WAKE_HELLO에 실려 옴)를 로그처럼 한 줄씩 누적(사용자 지시: 최신값으로
 * 덮어쓰는 대신 매번 새 줄로, 2026-08-09) — s_log_container/s_log_label과 같은 구조.
 * s_power_panel_title은 refresh_lang_texts에서 갱신 */
static lv_obj_t *s_power_panel_title = NULL;
static lv_obj_t *s_power_list        = NULL;  /* 스크롤 컨테이너(s_log_container 대응) */
static lv_obj_t *s_power_log_label   = NULL;  /* 누적 텍스트(s_log_label 대응) */
static lv_obj_t   *s_power_log_pause_btn = NULL;
static lv_obj_t   *s_power_log_pause_lbl = NULL;
static lv_timer_t *s_power_panel_timer   = NULL;  /* 일시멈춤 단추가 pause/resume(2026-08-10) */

/* 2026-09-08(재설계 — 단일화면+전체화면 팝업) — 로그는 설정 팝업 안에 중첩됨(별도 팝업
 * 아님). s_log_tab_built는 지금 설정 팝업의 콘텐츠 영역(s_option_content)에 로그 내용이
 * 지어져 있는지 플래그(= s_option_showing_log와 사실상 동기화됨) */
static bool        s_log_tab_built = false;
static lv_timer_t *s_log_box_timer   = NULL;  /* refresh_log_box, 예전엔 핸들 없이 생성만 하고 버림 */
static bool        s_power_log_paused    = false;

/* 통계탭(2026-09-06, 사용자 설계) — 실제 시계열 통계 판넬. 그래프는 다음 단계, 이번엔
 * 항목별 최대/최소(peak_label)와 페이지네이션된 값 테이블(stats_table)만 구현.
 * peak_title/prev_lbl/next_lbl은 정적 제목이라 refresh_lang_texts에서 갱신 —
 * peak_label/table/page_label 내용은 refresh_stats_page() 타이머가 매번 새로 채움 */
static lv_obj_t *s_stats_table       = NULL;
static lv_obj_t *s_stats_table_header_lbl = NULL;  /* 2026-09-07 버그수정 — refresh_lang_texts에서 갱신하려면 전역이어야 함 */
static lv_obj_t *s_stats_page_label  = NULL;
static lv_obj_t *s_stats_prev_btn    = NULL;
static lv_obj_t *s_stats_prev_lbl    = NULL;
static lv_obj_t *s_stats_next_btn    = NULL;
static lv_obj_t *s_stats_next_lbl    = NULL;
static lv_obj_t *s_stats_jump_prev_btn = NULL;
static lv_obj_t *s_stats_jump_prev_lbl = NULL;
static lv_obj_t *s_stats_jump_next_btn = NULL;
static lv_obj_t *s_stats_jump_next_lbl = NULL;
static uint32_t  s_stats_page_index  = 0;  /* 0 = 가장 최근 페이지 */
#define STATS_JUMP_PAGE_COUNT 10

/* 2026-09-07(통계탭 레이아웃 재설계, 사용자 설계 — "개괄" 판넬) — 구 "최대/최소 판넬"
 * 대체. 제목+Scale은 좌우 서브판넬 바깥(전체폭), 서브판넬은 좌=온도/이산화탄소,
 * 우=습도/암모니아. Scale을 바꾸면 이 판넬 숫자도 그 기간 기준으로 재계산됨(사용자 확정) */
static lv_obj_t *s_stats_overview_title = NULL;
static lv_obj_t *s_stats_scale_dd       = NULL;
static lv_obj_t *s_overview_temp_label  = NULL;
static lv_obj_t *s_overview_humi_label  = NULL;
static lv_obj_t *s_overview_co2_label   = NULL;
static lv_obj_t *s_overview_nh3_label   = NULL;
/* 2026-09-11(그래프 재설계) — stats_store.h의 STATS_SCALE_SECONDS가 정본(스케일별 사전집계
 * 저장 버킷폭도 이 값을 기준으로 계산되므로) — 예전엔 이 파일에 따로 복제해서 들고 있었음 */

/* 통계탭 테이블<->그래프 스와이프 전환(2026-09-07, 사용자 설계) — 그래프는 뼈대만
 * (실제 lv_chart 내용은 다음 단계) */
static lv_obj_t *s_stats_pager       = NULL;  /* 좌우 스와이프로 이동하는 컨테이너(자식 2개) */
static lv_obj_t *s_stats_table_view  = NULL;
static lv_obj_t *s_stats_graph_view  = NULL;
static lv_obj_t *s_stats_delete_btn  = NULL;
static lv_obj_t *s_stats_delete_lbl  = NULL;

/* 2026-09-10(사용자 설계 — "라인+도트", "계열 4개 선택 표시", "탭하면 값", "청록/빨강/파랑/
 * 까망") — 그래프 실제 구현. 계열 순서 고정: 0=온도(빨강) 1=습도(파랑) 2=CO2(까망)
 * 3=암모니아(짙은 노랑 — 2026-09-10 사용자 지시로 청록에서 변경). 계열마다 실제 단위/범위가
 * 달라서(온도 vs CO2 등) 화면엔 각 계열을 자기 자신의 기간 내 최소~최대 기준으로 0~100
 * 정규화해서 그리고, 탭하면 정규화 전 실제 값을 보여줌(s_stats_chart_real_values에 원본 보관) */
#define STATS_GRAPH_POINT_COUNT   STATS_AGG_POINTS_PER_SCALE  /* stats_store.h가 정본(60) */
#define STATS_GRAPH_SERIES_COUNT  4
static lv_obj_t          *s_stats_chart               = NULL;  /* 실데이터 — 선만(점마커 숨김) */
static lv_obj_t          *s_stats_gap_chart           = NULL;  /* 2026-09-11(그래프 재설계) —
    * "값이 없는 슬롯마다 그 자리에 계열 고정높이로 점만" — s_stats_chart의 자식으로 만들어
    * 똑같은 크기/위치로 겹쳐그림(부모 위에 자식이 그려지는 LVGL 기본 순서 이용), 점마커만
    * 보이고 선은 숨김. CLICKABLE을 꺼서 탭이 밑의 실데이터 차트로 그대로 전달되게 함 */
static lv_chart_series_t *s_stats_chart_series[STATS_GRAPH_SERIES_COUNT];
static lv_chart_series_t *s_stats_gap_series[STATS_GRAPH_SERIES_COUNT];
static lv_obj_t          *s_stats_chart_checkbox[STATS_GRAPH_SERIES_COUNT];
static lv_obj_t          *s_stats_chart_tap_label      = NULL;
static int                 s_stats_chart_tap_shown_series = -1;  /* 지금 탭 박스에 표시 중인 계열, -1=없음 */
static float               s_stats_chart_real_values[STATS_GRAPH_SERIES_COUNT][STATS_GRAPH_POINT_COUNT];
static bool                 s_stats_chart_has_value[STATS_GRAPH_SERIES_COUNT][STATS_GRAPH_POINT_COUNT];
static uint32_t             s_stats_chart_shown_points  = 0;  /* 이번에 실제로 그려진 포인트 수(<=STATS_GRAPH_POINT_COUNT) */
/* 2026-09-11(그래프 재설계 — "값 없는 슬롯은 계열 고정높이로 점만") — 정규화축(0~100) 기준
 * 온도7/10·습도6/10·이산화탄소5/10·암모니아4/10, chan_types 배열과 동일 순서 */
static const int32_t s_stats_gap_ref_height[STATS_GRAPH_SERIES_COUNT] = { 70, 60, 50, 40 };
/* 2026-09-11(사용자 지적 — "내가 원한건 그래프 상단에 4계열의 최대값, 하단에 4계열의
 * 최소값을 표시하는 거야") — 정규화가 계열별 자기 min~max를 0~100에 매핑하므로, 각 계열의
 * 실제 최대값은 그 계열 선이 차트 "맨 위"에 닿는 지점, 최소값은 "맨 아래"에 닿는 지점과
 * 항상 일치함 — 그래서 4계열 최대값을 차트 상단에 한 줄로, 4계열 최소값을 하단에 한 줄로 */
static lv_obj_t          *s_stats_graph_max_row       = NULL;
static lv_obj_t          *s_stats_graph_min_row       = NULL;
static lv_obj_t          *s_stats_graph_max_label[STATS_GRAPH_SERIES_COUNT];
static lv_obj_t          *s_stats_graph_min_label[STATS_GRAPH_SERIES_COUNT];
/* 2026-09-11(그래프 재설계 항목4/6) — 스와이프로 과거로 넘어간 칸 수(0=지금). 매 갱신마다
 * "지금"을 다시 계산해서 이 오프셋 기준으로 창을 다시 잡음(사용자 지시: "갱신이 되면,
 * 다시 12시간 전 창을 보여줘야 한다") — 절대시각을 저장하지 않음 */
static int                  s_stats_graph_offset = 0;
static bool                  s_stats_graph_force_refresh = false;  /* 스와이프 직후 즉시 반영용 */
/* 2026-09-11(사용자 지시 — "기기 반응이 느린 편... 스와이프 인식됨을 알려야") — 제스처
 * 인식 즉시(느릴 수 있는 실제 갱신 전에) 잠깐 보여주는 방향 힌트 */
static lv_obj_t          *s_stats_graph_swipe_hint     = NULL;
/* 2026-09-11(그래프 재설계 항목5) — 스케일별 X축 표기 간격(초). "1H: 10분, 12H: 1H, 1D: 2H,
 * 3D: 12H, 1W: 1D 간격으로 표기해"(사용자 지시) — STATS_SCALE_SECONDS와 동일 순서 */
static const uint32_t s_stats_xaxis_interval_sec[STATS_SCALE_COUNT] = { 600, 3600, 7200, 43200, 86400 };
#define STATS_GRAPH_X_LABEL_MAX 13  /* 1D 스케일이 2H간격=12칸+1=13개로 가장 많음 */
static lv_obj_t          *s_stats_graph_xaxis_row      = NULL;
static lv_obj_t          *s_stats_graph_x_labels[STATS_GRAPH_X_LABEL_MAX];

/* 2026-09-08(재설계 — 단일화면+전체화면 팝업) — 통계는 상단바 버튼이 여는 전체화면 팝업.
 * s_stats_popup은 create_page_popup()이 만든 오버레이 루트(열려있을 때만 존재),
 * s_stats_tab_built는 지금 내용이 지어져 있는지 — refresh_lang_texts()가 이 플래그로
 * 건드릴지 판단(변수명은 구 탭 버전에서 그대로 재사용, 의미는 "팝업 열림"으로 바뀜) */
static lv_obj_t   *s_stats_popup       = NULL;
static lv_obj_t   *s_stats_popup_title = NULL;
static bool        s_stats_tab_built = false;
static lv_timer_t *s_stats_page_timer  = NULL;

/* 2026-09-08(카메라 팝업 추출) — 카메라는 통계/설정과 달리 콘텐츠를 새로 짓지 않고
 * 기존 toolbar(s_camera_content)/split_row(s_camera_split_row)를 이 팝업으로 재부모화만
 * 함 — 그 둘의 내부 위젯(드롭다운/목록/미리보기)에 걸린 기존 이벤트/타이머는 그대로 살아있어
 * 손댈 필요 없음 */
static lv_obj_t   *s_camera_popup       = NULL;
static lv_obj_t   *s_camera_popup_title = NULL;

/* 2026-09-08(사용자 설계 — 연결 기능 주화면 이관) — 연결된 장치 행을 탭하면 뜨는 개별설정
 * 팝업(Alias 편집 + [센서일 때만]측정주기 편집 + 연결끊기). s_device_popup_node는 static
 * 싱글턴 하나뿐(모달은 한 번에 하나만 뜨는 이 앱의 기존 관례와 동일 — show_confirm_popup의
 * ctx가 "Yes" 탭 시점까지 살아있어야 해서 스택 임시변수 대신 static 사용) */
static lv_obj_t          *s_device_popup       = NULL;
static lv_obj_t          *s_device_popup_title = NULL;
static lv_obj_t          *s_device_alias_ta    = NULL;
static lv_obj_t          *s_device_keyboard    = NULL;
static lv_obj_t          *s_device_disconnect_btn = NULL;  /* 2026-09-08 — 웹 인젝션(ui_main_inject_disconnect)용 핸들 */
/* 2026-09-09(사용자 설계 — "Alias도 Apply 버튼 넣고, 눌렀을 때만 적용, 안 누르고 닫으면
 * 적용 안 되게") — 측정주기/촬영주기와 동일한 [값][Apply] 패턴. applied_text는 "마지막으로
 * 저장(Apply)된 값" — 지금 입력창 텍스트와 다를 때만 Apply 버튼 활성화 */
static lv_obj_t          *s_device_alias_apply_btn = NULL;
static char               s_device_alias_applied_text[DEVICE_CONFIG_ALIAS_MAX_LEN];
static bool                s_device_popup_is_sensor = false;
static esp_now_hub_node_t  s_device_popup_node;

/* ds_cycle_count 하나만 비교하면 됨(2026-08-10) — 매 리포트가 항상 새 사이클이라 Light
 * Sleep 시절처럼 여러 필드를 같이 diff할 필요가 없어짐(단조증가 카운터) */
typedef struct {
    uint8_t  mac[6];
    bool     used;
    uint32_t last_cycle_count;
} power_log_track_t;
static power_log_track_t s_power_log_track[ESP_NOW_HUB_MAX_NODES];
/* 2026-08-21 — 내부(비-PSRAM) DRAM이 httpd_start 실패(5005)를 겪을 만큼 빠듯했던 걸 실기로
 * 확인 — 텍스트 로그 버퍼라 PSRAM으로 옮김(ui_init()에서 할당, 다른 고정버퍼들과 동일 원칙) */
#define POWER_LOG_BUF_CAP 2048
static char *s_power_log_buf = NULL;

/* 설정탭 카메라 리스트/상황판 1초 갱신 타이머 — 팝업/뷰어가 떠 있는 동안은 일시정지
 * (모달 위에서 터치하는 도중에 뒤에서 리스트를 지우고 다시 그리면 터치 처리와 간섭해서
 * 반응이 느려지거나 아예 안 먹는 문제가 있었음, 실기로 확인) */
static lv_timer_t *s_camera_list_timer = NULL;
static lv_timer_t *s_sensor_list_timer = NULL;

/* 2026-09-08(재설계) — 설정은 상단바 버튼이 여는 전체화면 팝업. s_option_popup은 오버레이
 * 루트, s_option_content는 헤더 아래 실제 위젯이 지어지는 컨테이너(설정<->로그 콘텐츠
 * 바꿔치기가 이 안에서 일어남), s_option_popup_title은 헤더 제목(설정/로그 전환 시 텍스트만
 * 바뀜), s_option_showing_log는 지금 콘텐츠가 로그인지 설정인지 */
static lv_obj_t *s_option_popup       = NULL;
static lv_obj_t *s_option_content     = NULL;
static lv_obj_t *s_option_popup_title = NULL;
static bool      s_option_tab_built = false;  /* 설정 팝업이 열려있는지(설정/로그 어느 쪽이든) */
static lv_timer_t *s_dashboard_timer   = NULL;

/* 2026-09-07 — s_camera_list_timer/s_sensor_list_timer는 이제 설정탭 팝업이 열려있을
 * 때만 존재함(popup 수명과 같이 감). 팝업이 닫혀있는 동안 다른 모달(예: 상황판에서 뜨는
 * 확인팝업)이 이 함수를 불러도 NULL 타이머를 건드리지 않도록 방어 — lv_timer_pause/resume은
 * NULL을 안전하게 처리 안 함(lv_timer.c 확인) */
static void pause_bg_timers(void)
{
    if (s_camera_list_timer) lv_timer_pause(s_camera_list_timer);
    if (s_sensor_list_timer) lv_timer_pause(s_sensor_list_timer);
    lv_timer_pause(s_dashboard_timer);
}

static void resume_bg_timers(void)
{
    if (s_camera_list_timer) lv_timer_resume(s_camera_list_timer);
    if (s_sensor_list_timer) lv_timer_resume(s_sensor_list_timer);
    lv_timer_resume(s_dashboard_timer);
}

/* ════════════════════════════════════════════════════════════
 * 토스트 + 로고 경고 아이콘 — 메모리/통신 실패를 로그(통계 탭)에만 남기지 않고
 * 화면에 바로 보여주기 위함(2026-08-01, 사용자 지시: "네 코드가 에러처리가 없어서
 * 지금까지 헤멘거잖아"). ui_log_add_err()/ui_log_add_warn()으로 남긴 게 있으면 200ms
 * 폴링 타이머가 잡아서 토스트로 띄우고, 로고를 경고 아이콘으로 바꿈.
 * 2026-08-11, 사용자 지시로 에러/워닝 색 분리(빨강/노랑) — bg_color를 매개변수로 받게
 * 일반화(예전 show_error_toast, 이름도 변경)
 * ════════════════════════════════════════════════════════════ */
static void show_toast(const char *msg, lv_color_t bg_color)
{
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    s_toast = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_toast, LV_PCT(85), LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, 85);  /* tab bar(75px) 바로 아래 */
    lv_obj_set_style_bg_color(s_toast, bg_color, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);  /* 2026-09-07(사용자 지시) — 모든 팝업류 완전 불투명 */
    lv_obj_set_style_pad_all(s_toast, 10, 0);

    lv_obj_t *lbl = lv_label_create(s_toast);
    lv_label_set_text(lbl, msg);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_font(lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);

    s_toast_expire_ms = lv_tick_get() + 4000;
}

/* 2026-09-08(사용자 재설계 — 3단계 상태 아이콘) — s_error_active/s_warn_active가 바뀔 때마다
 * 호출, 정상/경고/에러 3개 중 하나만 보이게 함. 에러가 하나라도 있으면 항상 에러 표시가
 * 우선(둘 다 활성이어도) — 기존 이진 로직(2026-08-11)의 우선순위 그대로 유지 */
static void update_logo_warning_display(void)
{
    if (!s_status_normal || !s_status_warning || !s_status_error) return;
    lv_obj_add_flag(s_status_normal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_warning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_error, LV_OBJ_FLAG_HIDDEN);
    if (s_error_active || s_sd_io_fail_active) {
        lv_obj_remove_flag(s_status_error, LV_OBJ_FLAG_HIDDEN);
    } else if (s_warn_active) {
        lv_obj_remove_flag(s_status_warning, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_status_normal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void error_poll_tick(lv_timer_t *t)
{
    (void)t;
    char err[128];
    if (ui_log_get_pending_error(err, sizeof(err))) {
        show_toast(err, lv_palette_main(LV_PALETTE_RED));
        s_error_active = true;
        update_logo_warning_display();
    }
    char warn[128];
    if (ui_log_get_pending_warn(warn, sizeof(warn))) {
        show_toast(warn, lv_color_hex(0xFFCC00));
        s_warn_active = true;
        update_logo_warning_display();
    }
    if (s_toast && lv_tick_get() >= s_toast_expire_ms) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    /* 2026-09-11 재설계 — 예전엔 에러는 재부팅 전까지 절대 안 지워지고 워닝만 팝업 확인 시
     * 전체 삭제됐는데(2026-08-11), 이제 에러/워닝 둘 다 에러목록 팝업(cb_logo_warning_tap)의
     * 행별 "지우기"/SD 전용 조치버튼으로 개별 해제되는 걸로 통일됨(사용자 지시: "앞으로
     * 경고, 에러를 같은 방식으로 처리하면 되지"). 여기 폴링에서는 지우기를 건드리지 않음 —
     * 새 항목이 생기는 것만 감지 */
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
/* 측정기 리스트(설정탭)의 동일 용도 — 정의는 s_sensor_count_prev 선언부 근처(아래) */
static void force_sensor_list_redraw(void);
/* 측정 주기 Apply 버튼의 활성/비활성 판정 — 정의는 촬영주기의 동일 함수 근처(아래).
 * select_sensor()(측정기 리스트 근처, 여기보다 앞)가 선택이 바뀔 때마다 이걸 불러
 * 새 대상 기준으로 다시 판정해야 해서 전방선언만 둠 */
static void update_sens_measure_apply_enabled(void);
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
    force_sensor_list_redraw();
    s_dash_count_prev = -1;

    lv_label_set_text(s_logo_title, ui_str(STR_LOGO_TITLE));
    lv_label_set_text(s_settings_btn_lbl, ui_str(STR_TAB_OPTION));

    if (s_option_tab_built) {
        for (ui_str_id_t id = STR_GROUP_CNTL; id <= STR_GROUP_SYSTEM; id++) {
            lv_label_set_text(s_group_title[id - STR_GROUP_CNTL], ui_str(id));
        }
        /* 언어 행(cntl_row)도 설정탭 안에 있음 — s_lang_label/s_btn_ko/s_btn_en 전부 */
        lv_label_set_text(s_lang_label, ui_str(STR_LABEL_LANGUAGE));
        update_lang_buttons();
    }

    lv_label_set_text(s_dash_title[0], ui_str(STR_PANEL_SUMMARY));
    lv_label_set_text(s_dash_title[1], ui_str(STR_GROUP_SENSOR));
    lv_label_set_text(s_dash_title[2], ui_str(STR_GROUP_CAMERA));
    lv_label_set_text(s_sensor_empty, ui_str(STR_PANEL_NO_SENSOR));
    lv_label_set_text(s_camera_empty, ui_str(STR_PANEL_NO_CAMERA));
    lv_label_set_text_fmt(s_web_prefix_label, "%s: ", ui_str(STR_LABEL_WEB));
    lv_label_set_text(s_sensor_pending_lbl, ui_str(STR_LABEL_PENDING));
    lv_label_set_text(s_camera_pending_lbl, ui_str(STR_LABEL_PENDING));
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
     * 추가해야 언어전환이 실제로 반영됨).
     * 2026-09-08(팝업→진짜 탭전환) — 통계/설정/로그 위젯은 이제 그 탭이 현재 선택돼있을
     * 때만 존재함(s_stats_tab_built/s_option_tab_built/s_log_tab_built 참고) — 다른 탭에
     * 가있으면 건드리지 않고 건너뜀(다시 그 탭으로 올 때 ui_str()로 새로 지어지므로 언어가
     * 밀릴 일은 없음) */
    if (s_option_tab_built) {
        lv_label_set_text(s_capture_interval_label, ui_str(STR_LABEL_CAPTURE_INTERVAL));
        lv_label_set_text(s_capture_apply_lbl, ui_str(STR_BTN_APPLY));
        lv_label_set_text(s_response_interval_label, ui_str(STR_LABEL_RESPONSE_INTERVAL));
        lv_label_set_text(s_response_apply_lbl, ui_str(STR_BTN_APPLY));
        lv_label_set_text(s_agc_label, ui_str(STR_LABEL_AGC));
        lv_label_set_text(s_aec_label, ui_str(STR_LABEL_AEC));
        lv_label_set_text(s_xclk_label, ui_str(STR_LABEL_XCLK));
        lv_label_set_text(s_xclk_apply_lbl, ui_str(STR_BTN_APPLY));
        lv_label_set_text(s_adaptive_response_label, ui_str(STR_LABEL_ADAPTIVE_RESPONSE));
        lv_label_set_text(s_adaptive_apply_lbl, ui_str(STR_BTN_APPLY));
        lv_label_set_text(s_adaptive_help_label, ui_str(STR_HELP_ADAPTIVE_RESPONSE));
        lv_label_set_text(s_restart_label, ui_str(STR_LABEL_RESTART_DEVICE));
        lv_label_set_text(s_restart_btn_lbl, ui_str(STR_BTN_RESTART));
        lv_label_set_text(s_time_label, ui_str(STR_LABEL_TIME));
        lv_label_set_text(s_time_set_btn_lbl, ui_str(STR_BTN_SET_TIME));
        lv_label_set_text(s_network_label, ui_str(STR_LABEL_NETWORK));
        lv_label_set_text(s_auto_connect_known_label, ui_str(STR_LABEL_AUTO_CONNECT_KNOWN));
        lv_label_set_text(s_auto_connect_new_label, ui_str(STR_LABEL_AUTO_CONNECT_NEW));
        /* 2026-08-29 버그수정 — 캡션을 무조건 "찾기"로 덮어쓰면 연결된 상태(캡션=SSID)일 때
         * 언어 전환 시 SSID가 사라지고 "찾기"로 잘못 바뀜. 현재 상태 기준으로 다시 계산 */
        refresh_network_right_zone();

        /* 드롭다운 옵션 문자열 자체도 언어별이라 다시 채워야 함 — lv_dropdown_set_options는
         * 선택 인덱스를 0으로 리셋시키므로, 지금 선택돼있던 인덱스를 기억했다가 그대로
         * 되돌려줘야 사용자가 고른 값이 언어 전환 때문에 조용히 바뀌지 않음 */
        uint16_t capture_sel = lv_dropdown_get_selected(s_capture_interval_dd);
        lv_dropdown_set_options(s_capture_interval_dd, ui_str(STR_OPT_CAPTURE_INTERVAL_LIST));
        lv_dropdown_set_selected(s_capture_interval_dd, capture_sel);

        uint16_t xclk_sel = lv_dropdown_get_selected(s_xclk_dd);
        lv_dropdown_set_options(s_xclk_dd, ui_str(STR_OPT_XCLK_LIST));
        lv_dropdown_set_selected(s_xclk_dd, xclk_sel);

        uint16_t response_sel = lv_dropdown_get_selected(s_response_interval_dd);
        lv_dropdown_set_options(s_response_interval_dd, ui_str(STR_OPT_RESPONSE_INTERVAL_LIST));
        lv_dropdown_set_selected(s_response_interval_dd, response_sel);
        update_response_help_text();  /* 도움말도 언어 전환 시 다시 채움(선택 인덱스는 그대로) */

        {
            char opts[64];
            snprintf(opts, sizeof(opts), "%s\n%s", ui_str(STR_NETWORK_MODE_AP), ui_str(STR_NETWORK_MODE_STA));
            lv_dropdown_set_options(s_network_mode_dd, opts);
        }
        /* 2026-08-29 버그수정 — 화면에 떠있던 선택 인덱스를 그대로 되돌리는 대신, 항상 진짜
         * 저장된 값(device_config)에서 다시 계산 — set_options()가 내부적으로 sel_opt_id/
         * sel_opt_id_orig를 리셋하는데, 되돌리는 과정에서 화면 표시는 맞아 보여도 내부 비교
         * 로직이 어긋나 "값 변경 → 재시작 확인 팝업"이 언어 전환 이후엔 안 뜨던 버그의 원인으로
         * 의심됨(사용자 리포트) */
        lv_dropdown_set_selected(s_network_mode_dd, device_config_get_wifi_ap_mode() ? 0 : 1);

        uint16_t adaptive_sel = lv_dropdown_get_selected(s_adaptive_response_dd);
        lv_dropdown_set_options(s_adaptive_response_dd, ui_str(STR_OPT_ADAPTIVE_RESPONSE_LIST));
        lv_dropdown_set_selected(s_adaptive_response_dd, adaptive_sel);
    }

    /* 2026-09-08(연결 기능 주화면 이관) — 측정주기 위젯은 이제 개별설정 팝업(센서일 때만)
     * 소유. s_option_tab_built와 별개 조건 — Settings가 닫혀있어도 이 팝업만 열려있을 수
     * 있음 */
    if (s_device_popup && s_device_popup_is_sensor && s_sens_measure_dd) {
        uint16_t sens_measure_sel = lv_dropdown_get_selected(s_sens_measure_dd);
        lv_dropdown_set_options(s_sens_measure_dd, ui_str(STR_OPT_SENS_MEASURE_INTERVAL_LIST));
        lv_dropdown_set_selected(s_sens_measure_dd, sens_measure_sel);
        lv_label_set_text(s_sens_measure_label, ui_str(STR_LABEL_SENS_MEASURE_INTERVAL));
        lv_label_set_text(s_sens_measure_apply_lbl, ui_str(STR_BTN_APPLY));
    }

    if (s_log_tab_built) {
        lv_label_set_text(s_power_panel_title, ui_str(STR_PANEL_DEEPSLEEP));
        lv_label_set_text(s_log_panel_title, ui_str(STR_PANEL_GENERAL_LOG));
    }

    if (s_stats_tab_built) {
        lv_label_set_text(s_stats_overview_title, ui_str(STR_PANEL_STATS_OVERVIEW));
        /* 2026-09-07 버그수정(사용자 지적 — "항목/값/시간은 영문으로 안나와") — 이 라벨을
         * 생성부에서 지역변수로만 갖고 있어서 여기서 갱신할 방법이 아예 없었음(전역화 필요) */
        lv_label_set_text_fmt(s_stats_table_header_lbl, "%s / %s / %s", ui_str(STR_STATS_TABLE_HEADER_ITEM),
                               ui_str(STR_STATS_TABLE_HEADER_VALUE), ui_str(STR_STATS_TABLE_HEADER_TIME));
        lv_label_set_text(s_stats_prev_lbl, ui_str(STR_BTN_PREV_PAGE));
        lv_label_set_text(s_stats_next_lbl, ui_str(STR_BTN_NEXT_PAGE));
        lv_label_set_text(s_stats_jump_prev_lbl, ui_str(STR_BTN_JUMP_PREV10));
        lv_label_set_text(s_stats_jump_next_lbl, ui_str(STR_BTN_JUMP_NEXT10));
        lv_label_set_text(s_stats_delete_lbl, ui_str(STR_BTN_DELETE_STATS));

        uint16_t scale_sel = lv_dropdown_get_selected(s_stats_scale_dd);
        lv_dropdown_set_options(s_stats_scale_dd, ui_str(STR_STATS_SCALE_OPTIONS));
        lv_dropdown_set_selected(s_stats_scale_dd, scale_sel);
    }
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

/* 2026-09-04(사용자 설계: "PC 원격제어처럼", 웹 입력 합성용) — 방금 만들어진 모달을
 * 기억해둠. 웹이 row를 탭 합성한 직후(같은 LVGL 처리 안에서, 팝업 생성엔 무선 왕복이
 * 없어 동기적으로 이어짐) 이 안에서 확인 버튼을 찾아 또 탭 합성하는 데 씀 */
static lv_obj_t *s_last_modal = NULL;

static lv_obj_t *create_modal(void)
{
    pause_bg_timers();
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    /* 2026-09-07(사용자 지시 — malloc 멈춤 근본원인) — 반투명(LV_OPA_50)이면 뒤 화면과
     * 알파블렌딩해야 해서 LVGL이 별도 합성 레이어버퍼를 malloc해야 함(lv_draw_layer_alloc_buf).
     * 내부RAM이 빠듯한 지금 상태에서 이 malloc이 heap을 뒤지느라 몇 초씩 멈추는 원인이었음
     * (찾기 팝업, 페어링 확인팝업 둘 다 이걸로 멈췄음) — 완전 불투명이면 이 레이어버퍼
     * 자체가 필요 없어짐 */
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);

    lv_obj_t *box = lv_obj_create(overlay);
    lv_obj_set_size(box, 420, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    s_last_modal = box;
    return box;
}

/* 2026-09-08 — 통계/설정 전체화면 팝업 전용 배경. create_modal()과 같은 이유(레이어버퍼
 * malloc 회피)로 완전 불투명이지만, 420px 고정폭 대화상자가 아니라 화면 전체를 채우는
 * 콘텐츠 컨테이너라 별도 헬퍼로 분리. lv_tabview가 없어졌으므로 이 팝업들이 이제 유일한
 * 진입/이탈 UI를 제공함(닫기 버튼) */
static lv_obj_t *create_page_popup(void)
{
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    return overlay;
}

/* 팝업 헤더 공용(제목 좌, 닫기 우) — 심볼폰트 글리프 누락 위험 회피를 위해 network_chevron과
 * 동일하게 순수 ASCII "X" 사용 */
static lv_obj_t *add_page_popup_header(lv_obj_t *popup, const char *title, lv_event_cb_t close_cb,
                                       lv_obj_t **title_lbl_out)
{
    lv_obj_t *header = lv_obj_create(popup);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 12, 0);
    lv_obj_set_style_pad_ver(header, 6, 0);

    lv_obj_t *title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, title);
    /* 2026-09-08(사용자 지시 — 팝업 제목도 크게, 볼드는 메모리 때문에 생략) */
    lv_obj_set_style_text_font(title_lbl, ui_font_get(UI_FONT_SIZE_24), 0);
    if (title_lbl_out) *title_lbl_out = title_lbl;

    lv_obj_t *close_btn = lv_button_create(header);
    /* 2026-09-08(사용자 지시 — "닫힘(X표) 배경을 빨간색으로") */
    lv_obj_set_style_bg_color(close_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(close_btn, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "X");
    lv_obj_set_style_text_color(close_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(close_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    return header;
}

static lv_obj_t *add_modal_button(lv_obj_t *btn_row, ui_str_id_t text_id, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(btn_row);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    /* 2026-09-04(웹 합성용) — lv_event_dsc_t는 공개 API에서 불투명해서 등록된 콜백을 밖에서
     * 못 읽음. 대신 위젯 자체의 범용 user_data(이벤트 콜백의 user_data와는 별개 슬롯)에
     * 콜백 함수포인터를 그대로 저장 — find_widget_by_event_cb()가 이걸로 찾음 */
    lv_obj_set_user_data(btn, (void *)cb);
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

/* 2026-09-09(사용자 지적 — "E0007 과 그 뒤의 깨진 글자") — ui_log.c의 ui_log_err_desc()는
 * s_err_table이 하드코딩 한글 문자열이라(이번 세션 영문화 작업에서 빠뜨림) 비트맵 폰트로는
 * 깨져 보였고, 게다가 코드 4개가 테이블에 아예 없어서 "알 수 없는 에러" 폴백으로 떨어졌음
 * (device_config 버전업으로 부팅 때마다 뜨는 5007 CONFIG_FILE_MISMATCH가 바로 이 경우).
 * ui_log.c는 esp_now_photo.c 같은 하위 모듈에서도 쓰는 저수준 모듈이라 ui_strings 의존을
 * 새로 얹지 않고, ui_str()을 이미 쓰는 이 파일(ui_main.c)에 매핑을 둠 — ui_log.h의
 * UI_ERR_* 순서와 1:1 대응 */
static ui_str_id_t err_code_to_desc_str(int code)
{
    switch (code) {
        case UI_ERR_CACHE_TOO_BIG:         return STR_ERR_DESC_CACHE_TOO_BIG;
        case UI_ERR_CACHE_NO_BUF:          return STR_ERR_DESC_CACHE_NO_BUF;
        case UI_ERR_RECV_BUF_ALLOC:        return STR_ERR_DESC_RECV_BUF_ALLOC;
        case UI_ERR_CACHE_SLOT_ALLOC:      return STR_ERR_DESC_CACHE_SLOT_ALLOC;
        case UI_ERR_PANEL_BUF_ALLOC:       return STR_ERR_DESC_PANEL_BUF_ALLOC;
        case UI_ERR_STA_CRED_ALLOC:        return STR_ERR_DESC_STA_CRED_ALLOC;
        case UI_ERR_SEND_PHOTO_REQ:        return STR_ERR_DESC_SEND_PHOTO_REQ;
        case UI_ERR_SEND_CAPTURE_REQ:      return STR_ERR_DESC_SEND_CAPTURE_REQ;
        case UI_ERR_SEND_LIST_REQ:         return STR_ERR_DESC_SEND_LIST_REQ;
        case UI_ERR_SEND_DELETE_REQ:       return STR_ERR_DESC_SEND_DELETE_REQ;
        case UI_ERR_SEND_DELETE_ALL_REQ:   return STR_ERR_DESC_SEND_DELETE_ALL_REQ;
        case UI_ERR_REQUEST_BUSY:          return STR_ERR_DESC_REQUEST_BUSY;
        case UI_ERR_NOT_PAIRED:            return STR_ERR_DESC_NOT_PAIRED;
        case UI_ERR_TX_QUEUE_FULL:         return STR_ERR_DESC_TX_QUEUE_FULL;
        case UI_ERR_META_TOO_BIG:          return STR_ERR_DESC_META_TOO_BIG;
        case UI_ERR_CHUNK_MISSING:         return STR_ERR_DESC_CHUNK_MISSING;
        case UI_ERR_CRC_MISMATCH:          return STR_ERR_DESC_CRC_MISMATCH;
        case UI_ERR_DECODE_FAIL:           return STR_ERR_DESC_DECODE_FAIL;
        case UI_ERR_LIST_COUNT_MISMATCH:   return STR_ERR_DESC_LIST_COUNT_MISMATCH;
        case UI_ERR_FETCH_NORESPONSE:      return STR_ERR_DESC_FETCH_NORESPONSE;
        case UI_ERR_LIST_NORESPONSE:       return STR_ERR_DESC_LIST_NORESPONSE;
        case UI_ERR_PHOTO_SELECTION_STALE: return STR_ERR_DESC_PHOTO_SELECTION_STALE;
        case UI_ERR_DELETE_FAILED:         return STR_ERR_DESC_DELETE_FAILED;
        case UI_ERR_DELETE_ALL_FAILED:     return STR_ERR_DESC_DELETE_ALL_FAILED;
        case UI_ERR_CAPTURE_FAILED:        return STR_ERR_DESC_CAPTURE_FAILED;
        case UI_ERR_CAPTURE_NORESPONSE:    return STR_ERR_DESC_CAPTURE_NORESPONSE;
        case UI_ERR_CONFIG_NORESPONSE:     return STR_ERR_DESC_CONFIG_NORESPONSE;
        case UI_ERR_DELETE_ALL_NORESPONSE: return STR_ERR_DESC_DELETE_ALL_NORESPONSE;
        case UI_ERR_DELETE_ALL_STOPPED:    return STR_ERR_DESC_DELETE_ALL_STOPPED;
        case UI_ERR_SET_TIME_NORESPONSE:   return STR_ERR_DESC_SET_TIME_NORESPONSE;
        case UI_ERR_FONT_FILE_MISSING:     return STR_ERR_DESC_FONT_FILE_MISSING;
        case UI_ERR_FONT_BUF_ALLOC:        return STR_ERR_DESC_FONT_BUF_ALLOC;
        case UI_ERR_FONT_FILE_OPEN:        return STR_ERR_DESC_FONT_FILE_OPEN;
        case UI_ERR_FONT_CREATE:           return STR_ERR_DESC_FONT_CREATE;
        case UI_ERR_HTTPD_START:           return STR_ERR_DESC_HTTPD_START;
        case UI_ERR_RTC_SET_FAILED:        return STR_ERR_DESC_RTC_SET_FAILED;
        case UI_ERR_CONFIG_FILE_MISMATCH:  return STR_ERR_DESC_CONFIG_FILE_MISMATCH;
        case UI_ERR_SD_MOUNT_FAILED:       return STR_ERR_DESC_SD_MOUNT_FAILED;
        case UI_ERR_SD_IO_FAIL:            return STR_ERR_DESC_SD_IO_FAIL;
        default:                           return STR_ERR_DESC_UNKNOWN;
    }
}

/* 경고 로고 탭 — 지금까지 쌓인 에러+워닝 코드를 전부 목록으로 보여줌(2026-08-01, 사용자
 * 지시: "로고를 찍으면 error code를 보여주는 팝업... 누적된 게 있으면 여러 개를
 * 보여줄 수도"). 에러는 "Exxxx"(빨강), 워닝은 "Wxxxx"(어두운 노랑 — 팝업 배경이 밝아서
 * 원래 아이콘/토스트에 쓰는 밝은 노랑 0xFFCC00은 가독성이 떨어짐, 2026-08-11 사용자 지시
 * 반영) 한 줄씩, 각 행마다 자기 조치버튼(지우기, 또는 SD 코드면 재연결/포맷) —
 * 2026-09-11 재설계, [[project_cntl_popup_close_vs_action_buttons]] */
/* 지우기 버튼(행별) — 2026-09-11 재설계, [[project_cntl_popup_close_vs_action_buttons]].
 * code는 lv_obj_add_event_cb()의 user_data로 정수를 그대로 캐스팅해서 받음(별도 할당 불필요,
 * 흔한 관례) */
static void cb_dismiss_error_row(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    ui_log_clear_one_error(code);
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_delete(lv_obj_get_parent(btn));  /* btn의 부모 = 그 행(row) 컨테이너 */
    sync_error_warn_active_from_history();
}

static void cb_dismiss_warn_row(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    ui_log_clear_one_warn(code);
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_delete(lv_obj_get_parent(btn));
    sync_error_warn_active_from_history();
}

/* 2026-09-11(재설계 — [[project_cntl_popup_close_vs_action_buttons]]) — 목록의 각 행이
 * 자기 조치버튼(재연결/포맷/지우기)을 갖는 구조라 "팝업 전체가 하나의 결정"이 아님 → X로만
 * 닫힘(하단 확인버튼 없음). create_modal()의 고정폭(420px)은 이 용도엔 좁아서(재연결+포맷
 * 버튼이 한 행에 다 안 들어감) 재사용 안 하고 화면비율 기반으로 직접 만듦(사용자 지시 —
 * "화면이 바뀔 수 있으니 고정폭 대신 비율로") */
static void cb_logo_warning_tap(lv_event_t *e)
{
    (void)e;
    int err_codes[UI_ERR_HISTORY_CAP];
    int err_n = ui_log_get_error_history(err_codes, UI_ERR_HISTORY_CAP);
    int warn_codes[UI_WARN_HISTORY_CAP];
    int warn_n = ui_log_get_warn_history(warn_codes, UI_WARN_HISTORY_CAP);

    pause_bg_timers();
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);

    lv_obj_t *box = lv_obj_create(overlay);
    lv_obj_set_size(box, LV_PCT(80), LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    s_last_modal = box;

    add_page_popup_header(box, ui_str(STR_TITLE_ERROR_LIST), cb_modal_close, NULL);

    if (err_n == 0 && warn_n == 0) {
        lv_obj_t *lbl = lv_label_create(box);
        lv_label_set_text(lbl, ui_str(STR_ERROR_LIST_EMPTY));
        lv_obj_set_style_text_font(lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    } else {
        for (int i = 0; i < err_n; i++) {
            int code = err_codes[i];
            lv_obj_t *row = lv_obj_create(box);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_border_width(row, 0, 0);

            char buf[160];
            snprintf(buf, sizeof(buf), "E%04d %s", code, ui_str(err_code_to_desc_str(code)));
            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_font(lbl, ui_font_get(UI_FONT_SIZE_18), 0);
            lv_obj_set_style_text_color(lbl, lv_palette_main(LV_PALETTE_RED), 0);

            /* 2026-09-11(재설계 — 사용자 지시: "단순 재시도만으론... 무한루프잖아",
             * "Resolve 누르면 포맷할지 재마운트할지 묻는 팝업") — 5008/5009 둘 다 코드별
             * 전용 버튼 대신 공용 "해결"(Resolve) 버튼 하나 → 선택팝업(cb_sd_resolve_tap)에서
             * 포맷/재연결 아무거나 고를 수 있음(코드에 따라 하나로 강제하지 않음). SD 코드
             * 둘은 수동 지우기 버튼을 안 줌(검증 후에만 지워짐, 사용자 지시) */
            if (code == UI_ERR_SD_MOUNT_FAILED || code == UI_ERR_SD_IO_FAIL) {
                add_modal_button(row, STR_BTN_SD_RESOLVE, cb_sd_resolve_tap, NULL);
            } else {
                add_modal_button(row, STR_BTN_DISMISS, cb_dismiss_error_row, (void *)(intptr_t)code);
            }
        }
        for (int i = 0; i < warn_n; i++) {
            int code = warn_codes[i];
            lv_obj_t *row = lv_obj_create(box);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_border_width(row, 0, 0);

            char buf[160];
            snprintf(buf, sizeof(buf), "W%04d %s", code, ui_log_warn_desc(code));
            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_font(lbl, ui_font_get(UI_FONT_SIZE_18), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xB8860B), 0);

            add_modal_button(row, STR_BTN_DISMISS, cb_dismiss_warn_row, (void *)(intptr_t)code);
        }
    }
}

/* 2026-08-30(사용자 지시: "상단 로고(플렉스팜)을 눌렀을 때 URL QR 팝업 띄워줘", "아이콘 말고,
 * 로고문자에" — s_logo_icon은 이미 cb_logo_warning_tap에 쓰이므로 겹치지 않게 s_logo_title
 * (텍스트)에만 새로 바인딩) 대시보드 웹 URL을 QR코드로 보여줌 — 폰으로 IP 직접 입력할
 * 필요 없이 카메라로 스캔해서 바로 접속 */
static void cb_logo_title_tap(lv_event_t *e)
{
    (void)e;
    lv_obj_t *box = create_modal();

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, ui_str(STR_TITLE_WEB_QR));
    lv_obj_set_style_text_font(title, ui_font_get(UI_FONT_SIZE_18), 0);

    const char *ip = esp_now_hub_get_own_ip_str();
    if (ip[0] == '\0') {
        lv_obj_t *lbl = lv_label_create(box);
        lv_label_set_text(lbl, ui_str(STR_MSG_WEB_QR_NO_IP));
        lv_obj_set_style_text_font(lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    } else {
        char url[40];
        snprintf(url, sizeof(url), "http://%s:80", ip);

        lv_obj_t *qr = lv_qrcode_create(box);
        lv_qrcode_set_size(qr, 220);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_update(qr, url, strlen(url));
        lv_obj_center(qr);

        lv_obj_t *url_lbl = lv_label_create(box);
        lv_label_set_text(url_lbl, url);
        lv_obj_set_style_text_font(url_lbl, ui_font_get(UI_FONT_SIZE_18), 0);
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
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);  /* 2026-08-29 — 긴 메시지 워드랩(사용자 지적) */
    lv_obj_set_style_text_font(msg, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_row = create_modal_btn_row(box);
    add_modal_button(btn_row, STR_BTN_YES, cb_confirm_yes_trampoline, &s_confirm_state);
    add_modal_button(btn_row, STR_BTN_CANCEL, cb_modal_close, NULL);
}

/* 2026-09-10(사용자 설계 — "할당된 용량의 90%가 될 때 10%만큼 오래된 걸 지우겠다는 팝업을
 * 띄운다") — 확인/취소가 아니라 이미 실행된 정리를 알리는 안내뿐(show_confirm_popup과
 * 달리 버튼 하나, QR팝업과 동일 패턴) */
static void show_storage_cleanup_popup(const char *category_name, uint32_t deleted_count)
{
    lv_obj_t *box = create_modal();

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text_fmt(msg, ui_str(STR_MSG_STORAGE_CLEANUP), category_name, (unsigned)deleted_count);
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(msg, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_row = create_modal_btn_row(box);
    add_modal_button(btn_row, STR_BTN_CONFIRM, cb_modal_close, NULL);
}

/* ════════════════════════════════════════════════════════════
 * SD 저장소 상태/복구 — [[project_cntl_sd_reliability_redesign_2026_09_10]]
 * "SD 조회 fail이면 즉시 중단, 사용자에게 알리고, 재연결/포맷으로 대응"(사용자 설계).
 * 2026-09-11 재설계(사용자 지시) — "상태 버튼이 빨간색으로 되야하는 중대한 에러", "토스트도
 * 떠야하고" — 새로 만들지 않고 기존 상태아이콘/토스트 메커니즘(ui_log_add_err +
 * error_poll_tick, s_error_active/s_sd_io_fail_active OR조건)을 그대로 재사용. 조치(재연결/
 * 포맷)는 상태아이콘 탭 팝업(cb_logo_warning_tap)에 통합 — 라벨 자체의 별도 팝업은 없앰
 * (사용자 지시: "상태버튼을 눌러서 조치하라고 문구 표시"). report_sd_io_fail()은 통계탭
 * 읽기 회로차단기(stats_store_had_io_error() 감지 시 refresh_stats_page()가 호출)가 세팅하는
 * 진입점 — 이 플래그가 서면 재연결/포맷으로 사용자가 실제로 해소하기 전까지 유지됨(조용히
 * 넘어가면 또 같은 실패를 반복 무시하게 됨) */

/* s_sd_io_fail_active 세팅(+로그+토스트/아이콘) — 라벨 갱신은 호출부가 직접 하거나
 * refresh_storage_status_label()을 부름. report_sd_io_fail()과 refresh_storage_status_label()
 * 내부의 I/O 실패 감지 둘 다 이걸 거치므로, refresh_storage_status_label() 자기 자신이
 * 이 함수를 부르면 안 됨(순환호출) — refresh_storage_status_label()은 이 함수 대신
 * 플래그를 직접 세팅하고 즉시 리턴 */
static void mark_sd_io_fail(const char *context)
{
    if (!s_sd_io_fail_active) {
        ESP_LOGE(TAG, "SD I/O 오류 감지(%s) — 복구(재연결/포맷) 전까지 SD 조회 회로차단기 작동", context);
        ui_log_add_err(UI_ERR_SD_IO_FAIL, "SD I/O failure (%s)", context);
    }
    s_sd_io_fail_active = true;
    update_logo_warning_display();
}

/* 2026-09-11(사용자 지시 — "포맷 후에도 정상이 아니면 역시 정상으로 돌리면 안되고") —
 * 재연결/포맷 API가 ESP_OK를 반환해도 그대로 믿지 않고, 가벼운 실제 읽기 한 번으로 카드가
 * 진짜 정상인지 검증. stats_store_get_count()는 fopen만 해보는 제일 가벼운 읽기라 이
 * 용도에 적합 — 실패하면 stats_store_had_io_error()가 true로 남음 */
static bool sd_verify_healthy(void)
{
    stats_store_get_count();
    return !stats_store_had_io_error();
}

/* 2026-09-11(에러목록 팝업 행별 재설계 — "지우기" 버튼, s_error_active/s_warn_active를
 * 더 이상 한번 서면 안 지워지는 래치가 아니라 이력 유무로 매번 다시 계산) — 지우기 버튼이나
 * SD 검증-후-지우기 둘 다 이걸 거쳐서 상태아이콘을 갱신함 */
static void sync_error_warn_active_from_history(void)
{
    int err_codes[UI_ERR_HISTORY_CAP];
    s_error_active = ui_log_get_error_history(err_codes, UI_ERR_HISTORY_CAP) > 0;
    int warn_codes[UI_WARN_HISTORY_CAP];
    s_warn_active = ui_log_get_warn_history(warn_codes, UI_WARN_HISTORY_CAP) > 0;
    update_logo_warning_display();
}

/* 2026-09-11 — 재연결/포맷이 실제로 카드를 되살렸을 때만 호출(위 검증 통과 시). 5008/5009
 * 둘 다 이력에서 제거(사용자 지시: "SD는 검증 후 지워야 해") — 어느 쪽 버튼으로
 * 고쳤든 카드가 검증됐다면 둘 다 더 이상 사실이 아니므로. 다른 에러가 이미 있으면
 * sync_error_warn_active_from_history()가 그건 그대로 반영해 아이콘은 계속 빨강으로 남음
 * (사용자 지시: "다른 에러가 있다면 정상으로 돌리면 안되고") */
static void clear_sd_io_fail(void)
{
    s_sd_io_fail_active = false;
    ui_log_clear_one_error(UI_ERR_SD_MOUNT_FAILED);
    ui_log_clear_one_error(UI_ERR_SD_IO_FAIL);
    sync_error_warn_active_from_history();
}

/* 2026-09-11(사용자 지시 — "SD: 뒤에 붙는 정상일 때 문구/에러일 때 써지는 문구들이 다
 * 하나의 함수에서 파라메터에 의해 검정/빨강으로 표시되게 만들어야 잘하는 거야") — 이전엔
 * 이 "SD: <내용>" 조합을 여러 곳에 따로 하드코딩해서 제목도 "Storage"/"SD:"로 갈렸었음.
 * is_error만으로 색 결정, 제목("SD:")은 항상 고정 — 라벨에 lv_label_set_recolor()가 켜져
 * 있어야 함(생성부 참고, 이걸 빠뜨려서 "#ff0000 ..."이 그대로 문자로 찍혔던 전례 있음) */
static void set_storage_label_text(const char *detail, bool is_error)
{
    if (is_error) {
        lv_label_set_text_fmt(s_storage_status_label, "SD: #ff0000 %s#", detail);
    } else {
        lv_label_set_text_fmt(s_storage_status_label, "SD: %s", detail);
    }
}

static void refresh_storage_status_label(void)
{
    if (!s_storage_status_label) return;

    /* 2026-09-11(사용자 지시 — "에러 시에는 용량이나 사용량을 표기할 수 없잖아. 그냥
     * 지금처럼 에러로만 표기하고, 상태버튼을 눌러서 조치하라고 문구 표시") — 라벨 자체는
     * 더 이상 탭 대상이 아님(조치는 상태아이콘 쪽으로 일원화), 문구도 그에 맞게 안내 */
    if (s_sd_io_fail_active) {
        set_storage_label_text(ui_str(STR_STATUS_SD_IO_ERROR_MSG), true);
        return;
    }
    if (!sd_storage_is_mounted()) {
        /* 미마운트도 5008(UI_ERR_SD_MOUNT_FAILED)로 이어지는 에러 상태라 빨간색 처리 */
        set_storage_label_text(ui_str(STR_STATUS_SD_UNMOUNTED), true);
        return;
    }

    uint64_t sd_total = 0, sd_free = 0;
    if (!sd_storage_get_capacity(&sd_total, &sd_free) || sd_total == 0) {
        lv_label_set_text(s_storage_status_label, "");
        return;
    }

    uint64_t picture_budget = sd_total * 9 / 10;
    uint64_t measure_budget = sd_total / 10;
    uint64_t picture_used = 0;  /* TODO(미정): 캠 사진 저장 구현되면 폴더 크기 합산으로 교체 */
    uint64_t measure_used = stats_store_get_used_bytes();
    /* 2026-09-10(SD fail 회로차단기) — 이 조회 자체가 fopen 등에서 진짜 I/O 실패였다면
     * (단순 "기록 0개"가 아니라) 나머지 계산/표시를 이어가지 말고 즉시 에러 상태로 전환.
     * stats_store_get_used_bytes()는 내부에서 stats_store_get_count()를 부르므로 그
     * 함수의 리셋/세팅이 그대로 반영됨 */
    if (stats_store_had_io_error()) {
        mark_sd_io_fail("main screen SD capacity display");
        set_storage_label_text(ui_str(STR_STATUS_SD_IO_ERROR_MSG), true);
        return;
    }
    /* 2026-09-10(임시 진단 — "지금 1주일치가 아니지, 몇시간 정도일 뿐이야" 정확한
     * 수치 확인용, 확인 후 제거) */
    {
        uint64_t recs = measure_used / sizeof(stats_record_t);
        double hours = (double)recs / 4.0 * 30.0 / 3600.0;
        ESP_LOGW(TAG, "MEMDIAG stats_store: used=%llu bytes records=%llu (~%.2fh, 30s/4ch 가정)",
                 (unsigned long long)measure_used, (unsigned long long)recs, hours);
    }
    uint64_t picture_used_clamped = (picture_used > picture_budget) ? picture_budget : picture_used;
    uint64_t measure_used_clamped = (measure_used > measure_budget) ? measure_budget : measure_used;

    uint32_t picture_pct = (uint32_t)(picture_used * 100 / picture_budget);
    uint32_t measure_pct = (uint32_t)(measure_used * 100 / measure_budget);
    uint32_t total_pct   = (uint32_t)((sd_total - sd_free) * 100 / sd_total);
    uint32_t picture_remain_mb = (uint32_t)((picture_budget - picture_used_clamped) / (1024 * 1024));
    uint32_t measure_remain_mb = (uint32_t)((measure_budget - measure_used_clamped) / (1024 * 1024));
    uint32_t total_remain_mb   = (uint32_t)(sd_free / (1024 * 1024));

    /* 2026-09-11(사용자 지시 — 제목을 "SD:"로 통일) — 예전엔 이 정상상태 문구만 자체
     * 제목("Storage")을 갖고 있었음(STR_LABEL_STORAGE). 이제 set_storage_label_text()가
     * 항상 "SD:"를 붙이므로, 여기선 그 뒤에 올 상세 내용만 만듦 */
    char detail[128];
    snprintf(detail, sizeof(detail), "[%%(Remain MB)] %s %u(%u) / %s %u(%u) / %s %u(%u)",
        ui_str(STR_LABEL_PICTURE), (unsigned)picture_pct, (unsigned)picture_remain_mb,
        ui_str(STR_LABEL_MEASURE_SHORT), (unsigned)measure_pct, (unsigned)measure_remain_mb,
        ui_str(STR_LABEL_TOTAL), (unsigned)total_pct, (unsigned)total_remain_mb);
    set_storage_label_text(detail, false);

    /* 정리 트리거 — Measure가 자기 예산의 90% 이상이면 80%까지 삭제. Picture는
     * 실사용 0이라 지금은 절대 안 걸림(사진저장 구현 후 동일 패턴으로 확장 예정) */
    if (measure_used * 100 / measure_budget >= 90) {
        uint32_t deleted = stats_store_trim_to(measure_budget * 80 / 100);
        if (deleted > 0) {
            show_storage_cleanup_popup(ui_str(STR_LABEL_MEASURE_SHORT), deleted);
        }
    }
}

/* stats_store 읽기 함수가 stats_store_had_io_error()로 진짜 I/O 실패(단순 "데이터 없음"이
 * 아니라 fopen 등 자체가 실패)를 알려왔을 때 통계탭 회로차단기(refresh_stats_page)가 호출 */
static void report_sd_io_fail(const char *context)
{
    mark_sd_io_fail(context);
    refresh_storage_status_label();
}

/* 단순 안내(버튼 1개) 공용 — show_storage_cleanup_popup과 동일 구조지만 포맷 인자 없이
 * 완성된 문자열 그대로 표시(재연결/포맷 결과 안내용) */
static void show_alert_popup(const char *message)
{
    lv_obj_t *box = create_modal();

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, message);
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(msg, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_row = create_modal_btn_row(box);
    add_modal_button(btn_row, STR_BTN_CONFIRM, cb_modal_close, NULL);
}

/* 2026-09-11(사용자 지적 — "리마운트, 포맷 실패시 팝업이 모두 다 닫히네, 직전으로
 * 돌아가야되는데") — 실패하면 해결(Resolve) 선택팝업을 안 닫고 그 위에 결과팝업만 띄워서,
 * 결과팝업을 확인하면 다시 해결 팝업(다른 조치를 또 고를 수 있음)으로 돌아가게 함. 성공했을
 * 때만 해결 팝업까지 같이 닫음(문제가 실제로 풀렸으니 더 고를 게 없음) */
static void cb_sd_reconnect_tap(lv_event_t *e)
{
    esp_err_t err = sd_storage_reconnect();
    if (err == ESP_OK && sd_verify_healthy()) {
        clear_sd_io_fail();
        cb_modal_close(e);  /* 성공 — 해결 팝업도 같이 닫음 */
        show_alert_popup(ui_str(STR_MSG_SD_RECONNECT_OK));
    } else {
        show_alert_popup(ui_str(STR_MSG_SD_RECONNECT_FAIL));  /* 실패 — 해결 팝업은 그대로 둠 */
    }
    refresh_storage_status_label();
}

/* ctx = 해결(Resolve) 팝업의 오버레이(cb_sd_format_tap이 확인팝업 열기 전에 미리 챙겨서
 * 넘김) — 성공했을 때만 이걸 직접 닫음. show_confirm_popup()이 내부적으로 s_last_modal을
 * 확인팝업 자신으로 덮어써버려서(그리고 Yes 누르면 그 확인팝업 자체도 먼저 닫혀버려서)
 * s_last_modal로는 더 이상 해결 팝업을 못 찾음 — 그래서 ctx로 직접 전달받음 */
static void cb_sd_format_confirmed(void *ctx)
{
    lv_obj_t *resolve_overlay = (lv_obj_t *)ctx;
    esp_err_t err = sd_storage_format();
    if (err == ESP_OK && sd_verify_healthy()) {
        clear_sd_io_fail();
        if (resolve_overlay) {
            lv_obj_delete(resolve_overlay);
            resume_bg_timers();
        }
        show_alert_popup(ui_str(STR_MSG_SD_FORMAT_OK));
    } else {
        show_alert_popup(ui_str(STR_MSG_SD_FORMAT_FAIL));  /* 실패 — 해결 팝업은 그대로 둠 */
    }
    refresh_storage_status_label();
}

static void cb_sd_format_tap(lv_event_t *e)
{
    /* 해결(Resolve) 팝업은 안 닫고, 그 오버레이를 확인팝업 콜백에 넘겨서 성공 시에만
     * 닫게 함(위 주석 참고) — 되돌릴 수 없는 동작이라 확인팝업이 먼저 뜸 */
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *resolve_overlay = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(btn)));
    show_confirm_popup(ui_str(STR_MSG_SD_FORMAT_CONFIRM), cb_sd_format_confirmed, resolve_overlay);
}

/* 2026-09-11(재설계 — 사용자 지시: "단순 재시도만으론... 무한루프잖아", "Resolve 누르면
 * 포맷할지, 재마운트할지 묻는 팝업이 떠야지") — 에러목록 팝업(5008/5009 행)의 공용 "해결"
 * 버튼. 행위(포맷/재연결)가 있는 팝업이라 X로 닫힘(사용자 지시: "행위가 있는 팝업이니까"),
 * [[project_cntl_popup_close_vs_action_buttons]] */
static void cb_sd_resolve_tap(lv_event_t *e)
{
    cb_modal_close(e);  /* 에러목록 팝업부터 닫고 선택팝업으로 교체 */

    pause_bg_timers();
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);

    lv_obj_t *box = lv_obj_create(overlay);
    lv_obj_set_size(box, LV_PCT(80), LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    s_last_modal = box;

    add_page_popup_header(box, ui_str(STR_TITLE_SD_RESOLVE), cb_modal_close, NULL);

    lv_obj_t *btn_row = lv_obj_create(box);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    add_modal_button(btn_row, STR_BTN_SD_RECONNECT, cb_sd_reconnect_tap, NULL);
    add_modal_button(btn_row, STR_BTN_SD_FORMAT, cb_sd_format_tap, NULL);
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
    esp_now_hub_request_pair(node->mac);
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
/* 2026-08-21 — 내부(비-PSRAM) DRAM이 httpd_start 실패(5005)를 겪을 만큼 빠듯했던 걸 실기로
 * 확인 — PSRAM으로 옮김(ui_init()에서 할당) */
static esp_now_hub_node_t *s_camera_nodes = NULL;
static esp_now_hub_node_t *s_camera_nodes_prev = NULL;
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

/* 2026-09-08(연결 기능 주화면 이관) — 이 목록은 이제 "대기중"(WAITING) 전용이라(연결된
 * 장치는 s_camera_dash_list로 이관, 탭하면 개별설정 팝업) 항상 페어링 확인만 뜸 */
static void cb_camera_item_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    esp_now_hub_node_t *node = (esp_now_hub_node_t *)lv_obj_get_user_data(btn);
    if (!node) return;
    show_pair_confirm_popup(node);
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
    if (!s_camera_nodes || !s_camera_nodes_prev) return;  /* PSRAM 할당 실패 시(극히 드묾) */
    int total = esp_now_hub_get_nodes(HUB_NODE_KIND_CAM, s_camera_nodes, ESP_NOW_HUB_MAX_NODES);

    /* 2026-09-08(연결 기능 주화면 이관) — 이 목록은 이제 "대기중"만 보여줌(연결된 장치는
     * s_camera_dash_list로 이관). node_display_equal은 conn_state를 안 보므로(mac/kind/
     * ever_paired/name만 비교) 대기중<->연결 전이만으로는 원본 노드 비교로 재생성이 안
     * 트리거됨 — WAITING만 걸러낸 별도 스냅샷을 만들어 그걸로 비교해야 전이 시 행이
     * 실제로 나타나거나 사라짐 */
    esp_now_hub_node_t waiting[ESP_NOW_HUB_MAX_NODES];
    int count = 0;
    for (int i = 0; i < total; i++) {
        if (esp_now_hub_get_conn_state(s_camera_nodes[i].mac) != HUB_CONN_STATE_WAITING) continue;
        if (count < ESP_NOW_HUB_MAX_NODES) waiting[count++] = s_camera_nodes[i];
    }

    bool changed = (count != s_camera_count_prev);
    for (int i = 0; !changed && i < count; i++) {
        if (!node_display_equal(&waiting[i], &s_camera_nodes_prev[i])) changed = true;
    }
    if (changed) {
        memcpy(s_camera_nodes_prev, waiting, sizeof(esp_now_hub_node_t) * count);
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
            /* 2026-09-09(사용자 지시 — "Pending 보여줄 필요 없어" [비어있을 때]) */
            lv_obj_add_flag(s_camera_pending_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_camera_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_camera_pending_lbl, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < count; i++) {
                lv_obj_t *row = lv_list_add_button(s_camera_list, NULL, "");
                lv_obj_set_style_text_font(row, ui_font_get(UI_FONT_SIZE_18), 0);
                /* s_camera_nodes_prev는 위에서 이미 waiting[]을 memcpy해뒀으므로(영구 PSRAM
                 * 버퍼) 그 요소를 가리킴 — waiting[]은 이 함수 지역 스택 배열이라 함수
                 * 리턴 후 가리키면 안 됨(2026-09-08, 이관 중 바로잡음) */
                lv_obj_set_user_data(row, &s_camera_nodes_prev[i]);
                lv_obj_add_event_cb(row, cb_camera_item_clicked, LV_EVENT_CLICKED, NULL);
                if (i < ESP_NOW_HUB_MAX_NODES) {
                    s_camera_row_objs[i] = row;
                    memcpy(s_camera_row_macs[i], s_camera_nodes_prev[i].mac, 6);
                    strncpy(s_camera_row_names[i], s_camera_nodes_prev[i].name, ESP_NOW_LINK_NAME_LEN - 1);
                    s_camera_row_names[i][ESP_NOW_LINK_NAME_LEN - 1] = '\0';
                }
            }
            s_camera_row_count = (count < ESP_NOW_HUB_MAX_NODES) ? count : ESP_NOW_HUB_MAX_NODES;
        }
    }
    refresh_camera_row_status_text();  /* 2026-08-10 — 구조 변경 여부와 무관하게 매 틱 갱신 */
}

/* ════════════════════════════════════════════════════════════
 * 측정기(Sensor) 그룹박스 — 발견된 SENS 리스트(대기중/연결됨), 탭하면 위 팝업.
 * 2026-09-05(사용자 지시: "여기에 캠처럼... 같은 콘트롤을 써서 표시되야해") — 카메라
 * 그룹박스(refresh_camera_list 등)와 완전히 같은 패턴, HUB_NODE_KIND_SENS만 필터링.
 * 페어링/해제 팝업(show_pair_confirm_popup/show_unpair_confirm_popup)과 클릭 핸들러가
 * node->mac 기준으로만 동작하는 범용 코드라 그대로 재사용 — 새로 만들 필요 없음.
 * ════════════════════════════════════════════════════════════ */
static lv_obj_t          *s_sensor_list = NULL;
static esp_now_hub_node_t *s_sensor_nodes = NULL;
static esp_now_hub_node_t *s_sensor_nodes_prev = NULL;
static int                s_sensor_count_prev = -1;

static lv_obj_t *s_sensor_row_objs[ESP_NOW_HUB_MAX_NODES];
static uint8_t   s_sensor_row_macs[ESP_NOW_HUB_MAX_NODES][6];
static char      s_sensor_row_names[ESP_NOW_HUB_MAX_NODES][ESP_NOW_LINK_NAME_LEN];
static int       s_sensor_row_count = 0;

static void force_sensor_list_redraw(void)
{
    s_sensor_count_prev = -1;
}

/* 측정 주기 Apply 대상 선택(2026-09-05) — 카메라의 select_camera()와 동일 원칙(mac이 지금
 * 선택과 같으면 아무것도 안 함), 다만 여기선 목록 갱신을 새로 트리거하지 않음(측정기 목록은
 * 페어링 상태만 보여줄 뿐 이 선택과 무관). 선택이 바뀔 때마다 그 센서의 저장된 주기값으로
 * 드롭다운/Apply 버튼을 다시 맞춤 — 카메라는 대상이 하나뿐이라 이 재조정 자체가 없었음 */
static void select_sensor(const uint8_t *mac)
{
    if (s_has_selected_sensor && memcmp(s_selected_sensor_mac, mac, 6) == 0) return;
    memcpy(s_selected_sensor_mac, mac, 6);
    s_has_selected_sensor = true;

    if (s_sens_measure_dd) {
        s_sens_measure_applied_idx = find_value_index(s_sens_measure_interval_values,
            sizeof(s_sens_measure_interval_values) / sizeof(s_sens_measure_interval_values[0]),
            device_config_get_sens_sample_interval_sec(mac));
        lv_dropdown_set_selected(s_sens_measure_dd,
            (uint16_t)(s_sens_measure_applied_idx >= 0 ? s_sens_measure_applied_idx : 0));
        update_sens_measure_apply_enabled();
    }
}

/* 2026-09-08(연결 기능 주화면 이관) — 이 목록은 이제 "대기중"(WAITING) 전용이라(연결된
 * 장치는 s_sensor_dash_list로 이관, 탭하면 개별설정 팝업이 select_sensor()도 그때 대신
 * 불러줌) 항상 페어링 확인만 뜸 — 예전의 "탭하면 조용히 측정주기 대상도 선택" 버그(사용자
 * 지적: "지금도 안되잖아" — 탭할 때마다 연결해제 팝업까지 같이 떠서 대상만 조용히 바꾸는 게
 * 사실상 불가능했음)는 개별설정 팝업으로 대체되며 자연히 해소됨 */
static void cb_sensor_item_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    esp_now_hub_node_t *node = (esp_now_hub_node_t *)lv_obj_get_user_data(btn);
    if (!node) return;
    show_pair_confirm_popup(node);
}

static char s_sensor_row_last_text[ESP_NOW_HUB_MAX_NODES][48];

static void refresh_sensor_row_status_text(void)
{
    for (int i = 0; i < s_sensor_row_count; i++) {
        hub_conn_state_t st = esp_now_hub_get_conn_state(s_sensor_row_macs[i]);
        ui_str_id_t status_id = (st == HUB_CONN_STATE_WAITING) ? STR_STATUS_CONNECTING
                               : (st == HUB_CONN_STATE_ACTIVE) ? STR_STATUS_ACTIVE
                               : STR_STATUS_PAIRED;
        char buf[48];
        snprintf(buf, sizeof(buf), "%s (%s)", s_sensor_row_names[i], ui_str(status_id));
        /* 2026-09-06(실기 발견 — s_summary_row_last_text 선언부 설명 참고) */
        if (strcmp(s_sensor_row_last_text[i], buf) == 0) continue;
        lv_obj_t *lbl = lv_obj_get_child(s_sensor_row_objs[i], 0);
        if (lbl) {
            lv_label_set_text(lbl, buf);
            strncpy(s_sensor_row_last_text[i], buf, sizeof(s_sensor_row_last_text[i]) - 1);
            s_sensor_row_last_text[i][sizeof(s_sensor_row_last_text[i]) - 1] = '\0';
        }
    }
}

static void refresh_sensor_list(lv_timer_t *t)
{
    (void)t;
    if (!s_sensor_nodes || !s_sensor_nodes_prev) return;  /* PSRAM 할당 실패 시(극히 드묾) */
    int total = esp_now_hub_get_nodes(HUB_NODE_KIND_SENS, s_sensor_nodes, ESP_NOW_HUB_MAX_NODES);

    /* 2026-09-08(연결 기능 주화면 이관) — refresh_camera_list와 동일 이유로 WAITING만 걸러낸
     * 별도 스냅샷 사용(node_display_equal은 conn_state를 안 봄) */
    esp_now_hub_node_t waiting[ESP_NOW_HUB_MAX_NODES];
    int count = 0;
    for (int i = 0; i < total; i++) {
        if (esp_now_hub_get_conn_state(s_sensor_nodes[i].mac) != HUB_CONN_STATE_WAITING) continue;
        if (count < ESP_NOW_HUB_MAX_NODES) waiting[count++] = s_sensor_nodes[i];
    }

    bool changed = (count != s_sensor_count_prev);
    for (int i = 0; !changed && i < count; i++) {
        if (!node_display_equal(&waiting[i], &s_sensor_nodes_prev[i])) changed = true;
    }
    if (changed) {
        memcpy(s_sensor_nodes_prev, waiting, sizeof(esp_now_hub_node_t) * count);
        s_sensor_count_prev = count;

        lv_indev_reset(NULL, s_sensor_list);
        lv_obj_clean(s_sensor_list);
        s_sensor_row_count = 0;

        if (count == 0) {
            lv_obj_add_flag(s_sensor_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_sensor_pending_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_sensor_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_sensor_pending_lbl, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < count; i++) {
                lv_obj_t *row = lv_list_add_button(s_sensor_list, NULL, "");
                lv_obj_set_style_text_font(row, ui_font_get(UI_FONT_SIZE_18), 0);
                /* s_sensor_nodes_prev(영구 PSRAM 버퍼)를 가리킴 — waiting[]은 지역 스택
                 * 배열이라 함수 리턴 후 가리키면 안 됨(refresh_camera_list와 동일 수정) */
                lv_obj_set_user_data(row, &s_sensor_nodes_prev[i]);
                lv_obj_add_event_cb(row, cb_sensor_item_clicked, LV_EVENT_CLICKED, NULL);
                if (i < ESP_NOW_HUB_MAX_NODES) {
                    s_sensor_row_objs[i] = row;
                    memcpy(s_sensor_row_macs[i], s_sensor_nodes_prev[i].mac, 6);
                    strncpy(s_sensor_row_names[i], s_sensor_nodes_prev[i].name, ESP_NOW_LINK_NAME_LEN - 1);
                    s_sensor_row_names[i][ESP_NOW_LINK_NAME_LEN - 1] = '\0';
                    s_sensor_row_last_text[i][0] = '\0';  /* 새로 만든 라벨 — 다음 틱에 무조건 한 번은 채워지도록 */
                }
            }
            s_sensor_row_count = (count < ESP_NOW_HUB_MAX_NODES) ? count : ESP_NOW_HUB_MAX_NODES;
        }
    }
    refresh_sensor_row_status_text();
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
static void cb_async_sync_selected_photo(void *user_data);  /* 아래 정의 — lv_async_call 트램폴린,
                                                                 refresh_photo_list_ui()의 자동선택 분기가 씀 */
static bool sync_photo_list_tick(int select_index);  /* 아래 정의 — on_list_result_event()가 씀 */
static bool fetch_popup_is_active(void);  /* 아래 정의(fetch_popup_tick_fn 뒤) —
                                              cb_async_photo_result()가 사진가져오기 진행팝업
                                              자신의 tick과 상태 소비를 두고 경쟁하지 않으려고 씀 */
static bool list_popup_is_active(void);   /* 아래 정의(renew_list_tick_fn 뒤) — cb_async_list_result()가
                                              동일한 이유로 씀 */

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
 * OnTap(cb_photo_row_select)/목록 재구성(refresh_photo_list_ui) 전부 "선택이 바뀌었다"는
 * 사실만 여기로 알림. 2026-09-04부터 웹도 이 함수를 직접 안 부르고 cb_photo_row_select 자체를
 * 탭 합성으로 거쳐 감(ui_main_inject_photo_select, "PC 원격제어처럼" 설계).
 * 2026-08-30 버그수정 — esp_now_hub_note_user_action()이 예전엔 start_single_receive()
 * 안에서만(=실제 요청이 lv_async_call을 거쳐 시작될 때) 불려서, 탭/선택 시점과 그 사이에
 * 간극이 있었음. 이 간극에 WAKE_HELLO가 걸리면 적응형 판단(send_cask_sleep_now)이 아직
 * "방금 조작 있었음"을 못 보고 진짜 sleep_sec을 내보내는 레이스가 있었음(사용자 실기 확인:
 * 사진 전송 중에도 캠이 잠듦). 모델이 바뀌는 이 지점에서 바로(동기) 기록해서 간극을 없앰 */
static void set_selected_file_id(uint32_t file_id)
{
    s_selected_file_id = file_id;
    s_has_selected_file_id = true;
    esp_now_hub_note_user_action();
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
        ui_log_add_err(UI_ERR_NOT_PAIRED, "%s unavailable - waiting for CAM connection", what);
        return false;
    }
    return true;
}

/* 2026-08-21 — reconcile_selection에서 "가져오기"(모델→액션) 부분만 분리 — 강조표시(뷰)와
 * 무관하게 독립 호출 가능하게 함(사용자 설계: "모듈이 분리되게"). 지금촬영/모두지우기 같은
 * 목록가져오기 팝업은 목록 갱신까지만 하고 끝나고, 그 결과로 선택이 바뀐 건 배경 타이머
 * (refresh_dashboard)가 매 틱 이 함수를 불러 스스로 감지해서 별도의 사진가져오기 팝업으로
 * 이어감 — 목록가져오기 팝업이 열려있는 동안은 배경 타이머 자체가 pause_bg_timers()로
 * 멈춰있어서 두 팝업이 겹칠 일이 없음 */
static void sync_selected_photo_if_needed(bool show_popup)
{
    if (s_has_synced_file_id && s_synced_file_id == s_selected_file_id) return;  /* 이미 반영됨 — 끝 */
    if (!s_has_selected_cam || !s_has_selected_file_id) return;

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

    /* 2026-08-21 버그수정 — "반영됨" 기록을 예전엔 require_active_or_report() 통과 여부와
     * 무관하게 무조건 먼저 남겼음(WAITING이면 요청 자체가 안 나갔는데도 반영된 걸로 잘못
     * 기록) — 그러면 같은 사진을 나중에 다시 선택해도 위 가드에서 "이미 반영됨"으로
     * 오판해 영영 못 가져옴(지금촬영 직후 목록은 갱신되는데 정작 새 사진은 안 뜨는 버그로
     * 실사용 중 발견). 요청이 실제로 나갈 때만 기록하도록 순서 이동 */
    s_synced_file_id = s_selected_file_id;
    s_has_synced_file_id = true;

    esp_now_photo_fetch_by_id(s_selected_cam_mac, s_selected_file_id);
    if (show_popup) show_fetch_progress_popup();
}

/* lv_async_call() 트램폴린 — refresh_photo_list_ui()의 자동선택 분기/웹(ui_main_set_selected_photo)이
 * 씀. lv_async_cb_t 시그니처(void*만 받음)에 맞추기 위함, show_popup은 이 경로에선 항상 true */
static void cb_async_sync_selected_photo(void *user_data)
{
    (void)user_data;
    sync_selected_photo_if_needed(true);
}

/* 2026-09-04(사용자 설계: "이벤트로 처리해") — 사진 수신 완료(성공/실패) 이벤트의 앱 쪽
 * 반응. esp_now_photo.c는 LVGL을 몰라서 원시 콜백(esp_now_photo_event_cb_t, 어느 태스크든
 * 될 수 있음)만 주므로, 여기서 바로 lv_async_call()로 LVGL 태스크에 미룸 — 예전에 매틱
 * 폴링하던 refresh_dashboard()의 해당 분기와 정확히 같은 처리를 이벤트 시점에 1회만 함 */
static void cb_async_photo_result(void *user_data)
{
    (void)user_data;
    /* 2026-09-04 리그레션 수정 — 사진가져오기 진행팝업이 떠 있으면 그 팝업 자신의 200ms
     * tick(fetch_popup_tick_fn)이 READY/ERROR를 보고 소비하는 게 원래 경로. 여기서 먼저
     * consume_ready_photo_if_current()(내부에서 esp_now_photo_ready_ack()로 상태를 즉시
     * IDLE로 리셋)를 불러버리면, 팝업의 다음 tick은 READY도 ERROR도 아닌 IDLE만 보게 돼서
     * 진행정체 타임아웃(STALLED)으로 오판 — 실기에서 확인("사진은 받았는데 팝업이 실패로
     * 닫힘"). 팝업이 떠 있는 동안은 이 이벤트 소비를 양보함 */
    if (fetch_popup_is_active()) return;
    esp_now_photo_state_t st = esp_now_photo_get_state();
    if (st == ESP_NOW_PHOTO_STATE_READY) {
        consume_ready_photo_if_current();
    } else if (st == ESP_NOW_PHOTO_STATE_ERROR) {
        esp_now_photo_clear();
    }
}

static void on_photo_result_event(void)
{
    lv_async_call(cb_async_photo_result, NULL);
}

/* 목록판, 위와 동일 패턴 */
static void cb_async_list_result(void *user_data)
{
    (void)user_data;
    /* fetch_popup_is_active()와 동일 이유(2026-09-04 리그레션 수정) — renew_list_tick_fn은
     * 이미 READY를 스스로 소비하는(refresh_photo_list_ui+esp_now_photo_list_ack) 자기완결형
     * 코드라, 여기서 먼저 소비해버리면 팝업이 다음 틱에 상태를 놓쳐 정체 타임아웃
     * (UI_ERR_LIST_NORESPONSE=3007)으로 오판한다 */
    if (list_popup_is_active()) return;
    sync_photo_list_tick(-1);
}

static void on_list_result_event(void)
{
    lv_async_call(cb_async_list_result, NULL);
}

/* 연결/끊기판 — 카메라 판넬이 다음 1초 대시보드 틱까지 안 기다리고 즉시 갱신되게(사용자
 * 설계: "앱도... 이 이벤트에서 다음 절차를 수행"). 실제 목록 재구성은 refresh_camera_list()
 * 자신의 스냅샷 비교가 담당 — 여기선 그걸 "지금 당장 다시 비교해" 하고 트리거만 함 */
static void cb_async_connect_result(void *user_data)
{
    (void)user_data;
    force_camera_list_redraw();
    force_sensor_list_redraw();
}

static void on_connect_result_event(void)
{
    lv_async_call(cb_async_connect_result, NULL);
}

static void reconcile_selection(lv_obj_t *row, bool show_popup)
{
    if (s_selected_row && s_selected_row != row) {
        lv_obj_set_style_bg_opa(s_selected_row, LV_OPA_TRANSP, 0);
    }
    lv_obj_set_style_bg_color(row, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_30, 0);
    s_selected_row = row;

    sync_selected_photo_if_needed(show_popup);
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
    if (!s_current_list) return;  /* PSRAM 할당 실패 시(극히 드묾) */
    s_current_list_count = esp_now_photo_list_get_items(s_current_list, ESP_NOW_PHOTO_LIST_MAX);
    update_list_info_label();

    lv_indev_reset(NULL, s_photo_list);
    lv_obj_clean(s_photo_list);
    /* 뷰 캐시(행 객체 포인터)만 리셋 — 선택 모델(s_selected_file_id)은 목록이 다시
     * 그려져도 그대로 유지, 아래 루프에서 강조표시만 다시 그림(2026-08-02) */
    s_selected_row = NULL;

    if (s_current_list_count == 0) {
        /* 2026-08-21 버그수정 — 목록이 0개가 되면(전체삭제 등) 선택 모델/미리보기도 같이
         * 비워야 하는데 예전엔 여기서 빈 목록 표시만 하고 끝나서, 전체삭제 후에도 미리보기
         * 판넬에 마지막으로 보던 사진이 그대로 남아있는 버그가 있었음(reset_camera_ui_state()
         * 에는 있던 정리 로직이 여기만 빠짐) */
        s_has_selected_file_id = false;
        s_has_synced_file_id = false;
        if (s_photo_image) {
            lv_obj_clean(s_photo_box);
            s_photo_image = NULL;
            s_camera_photo_label = lv_label_create(s_photo_box);
            lv_label_set_text(s_camera_photo_label, ui_str(STR_PANEL_NO_PHOTO_YET));
            lv_obj_set_style_text_font(s_camera_photo_label, ui_font_get(UI_FONT_SIZE_18), 0);
            lv_obj_set_style_text_color(s_camera_photo_label, lv_palette_main(LV_PALETTE_GREY), 0);
        }

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
        /* 2026-09-04(웹 합성용, project_cntl_web_full_ui_injection_design 참고) — 이벤트
         * 콜백의 user_data는 공개 API로 못 읽어서(lv_event_dsc_t가 불투명), 위젯 자체의
         * 범용 user_data 슬롯에 별도로 file_id를 매담 — 강조표시 로직과는 무관(2026-08-30
         * MVC 되돌림과 별개) */
        lv_obj_set_user_data(row, (void *)(uintptr_t)s_current_list[i].file_id);

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
            /* 새로운 선택(예: 지금촬영 직후 최신 항목) — Model만 바꾸고 강조표시(뷰)만
             * 여기서 같이 해줌. "가져오기"(액션)는 여기서 직접 안 부름(2026-08-21, 사용자
             * 설계) — 이 함수는 목록가져오기 팝업(지금촬영/모두지우기 등) 안에서 호출되는
             * 중이라 그 팝업이 아직 안 닫힌 시점. 대신 lv_async_call()로 "이번 LVGL 처리
             * 사이클이 다 끝난 뒤"로 미뤄서 sync_selected_photo_if_needed()를 예약함 — 그
             * 팝업은 이 함수가 리턴한 직후(같은 사이클 안에서) 닫히므로, 예약된 호출이 실제
             * 실행될 땐 이미 깨끗하게 닫힌 뒤라 팝업끼리 안 겹침. 폴링(배경 타이머가 매초
             * 확인하는 방식) 대신 이벤트 기반이라 지연도 없음 —
             * 모듈 분리(reconcile_selection 참고) */
            set_selected_file_id(s_current_list[i].file_id);
            lv_obj_set_style_bg_color(row, lv_palette_main(LV_PALETTE_BLUE), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_30, 0);
            s_selected_row = row;
            lv_async_call(cb_async_sync_selected_photo, NULL);
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
        ui_log_add("DISPLAY cache MISS file_id=%u", (unsigned)file_id);
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
        ui_log_add_err(UI_ERR_DECODE_FAIL, "Photo display failed (decode) file_id=%u", (unsigned)file_id);
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
    ui_log_add("DISPLAY set_src done file_id=%u", (unsigned)file_id);
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
                    "Arrived file_id=%u != selected file_id=%u - ignoring, re-requesting",
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
    /* 2026-08-21 재진입 가드 — 이전엔 팝업이 이미 떠 있어도 그냥 새로 만들어서 전역
     * 단일슬롯(s_progress_popup_overlay 등)을 덮어썼음. 그러면 이전 오버레이 객체가
     * 화면엔 남아있는데 코드에서는 참조를 잃어 다시는 못 닫는 유령 모달이 되고, 이게
     * 전체화면을 덮은 채 터치를 영구히 막아버림(연결해제 버튼조차 안 눌리는 사고로
     * 실기에서 확인, XCLK 저속 설정으로 CAM 응답이 느려지며 팝업들이 겹쳐 트리거되면서
     * 재현됨) — 새 팝업을 열기 전에 이전 것부터 강제로 닫는다 */
    if (s_progress_popup_overlay) {
        close_progress_popup();
    }
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

static capture_popup_stage_t   s_capture_popup_stage;
static uint32_t                s_capture_popup_stage_start_ms;
static esp_now_capture_stage_t s_capture_last_seen_stage;  /* 전환 감지용(2026-08-21) — 아래 참고 */

/* 2026-08-21 핸드셰이크 재설계(project_cntl_cam_capture_now_handshake_redesign 메모리 참고) —
 * 하드웨어 동작(카메라 초기화/실촬영)이 끼는 대기만 이 고정예산을 쓰고, 순수 무선 왕복뿐인
 * 구간(접수 확인, "초기화 필요/촬영 시작" 신호 자체를 받는 것)은 기존 cam_response_timeout_ms()
 * (응답성 설정 기반)를 그대로 씀. XCLK를 낮추면 리드아웃이 느려지는 걸 감안해 넉넉히 잡음 —
 * 응답성 티어와 무관한 값이라는 게 핵심(예전 버그: 무선용 타이머 하나를 하드웨어 대기에도
 * 재사용해서 XCLK를 낮추면 4004가 오탐됐음, 사용자 지적: "네 코드 스타일 문제야") */
#define CAM_CAPTURE_HW_TIMEOUT_MS 15000

static uint32_t capture_stage_timeout_ms(esp_now_capture_stage_t stage)
{
    if (stage == ESP_NOW_CAPTURE_STAGE_INIT_NEEDED || stage == ESP_NOW_CAPTURE_STAGE_CAPTURING) {
        return CAM_CAPTURE_HW_TIMEOUT_MS;
    }
    return cam_response_timeout_ms();
}

static bool capture_popup_tick_fn(lv_obj_t *box)
{
    (void)box;
    lv_color_t grey  = lv_palette_main(LV_PALETTE_GREY);
    lv_color_t green = lv_palette_main(LV_PALETTE_GREEN);
    lv_color_t red   = lv_palette_main(LV_PALETTE_RED);

    if (s_capture_popup_stage == CAPTURE_POPUP_STAGE_WAIT_RESULT) {
        esp_now_capture_stage_t stage = esp_now_photo_get_capture_stage();

        if (stage != s_capture_last_seen_stage) {
            /* 단계가 바뀔 때마다 그 시점부터 새 예산 시작(전체삭제의 RECEIVED/WAIT_DONE
             * 분리와 동일 원칙을 모든 전환으로 일반화) — 카메라 초기화가 필요 없으면
             * INIT_NEEDED/INIT_DONE은 아예 안 와서 이 라벨들 자체가 안 보임(건너뛰기) */
            s_capture_last_seen_stage = stage;
            s_capture_popup_stage_start_ms = lv_tick_get();
            switch (stage) {
                case ESP_NOW_CAPTURE_STAGE_ACKED:
                    set_stage_label(s_capture_stage_label, 0, STR_CAPTURE_STAGE1_DONE, green);
                    break;
                case ESP_NOW_CAPTURE_STAGE_INIT_NEEDED:
                    set_stage_label(s_capture_stage_label, 1, STR_CAPTURE_STAGE2_INIT_NEEDED, grey);
                    break;
                case ESP_NOW_CAPTURE_STAGE_INIT_DONE:
                    set_stage_label(s_capture_stage_label, 1, STR_CAPTURE_STAGE2_INIT_DONE, green);
                    break;
                case ESP_NOW_CAPTURE_STAGE_CAPTURING:
                    set_stage_label(s_capture_stage_label, 1, STR_CAPTURE_STAGE2_CAPTURING, grey);
                    break;
                default:
                    break;
            }
        }

        bool resolved  = (stage == ESP_NOW_CAPTURE_STAGE_CAPTURED || stage == ESP_NOW_CAPTURE_STAGE_CAPTURE_FAILED);
        bool timedout  = !resolved && lv_tick_elaps(s_capture_popup_stage_start_ms) > capture_stage_timeout_ms(stage);
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
            ui_log_add_err(UI_ERR_CAPTURE_NORESPONSE, "Manual shot request: no CAM response (timeout, last stage=%d)", (int)stage);
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

    /* CAPTURE_POPUP_STAGE_SYNC_LIST — 목록갱신만 하고 끝(2026-08-21, 사용자 설계로 원복:
     * 사진 가져오기는 이 팝업의 일부가 아니라, 아래 refresh_photo_list_ui()가 남기는
     * "선택됨" 모델을 배경 타이머(refresh_dashboard)가 독립적으로 감지해서 별도 팝업으로
     * 이어감 — 모듈이 분리되게. 여기선 목록가져오기 결과만 보고 끝 */
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
    s_capture_last_seen_stage = ESP_NOW_CAPTURE_STAGE_SENT;

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
        /* 2026-09-04 수정 — renew_list_tick_fn과 같은 패턴으로 통일: 팝업 자신이 READY를
         * 보면 직접 소비(ack+화면표시)까지 끝낸다. 예전엔 이걸 다른 곳(1초 대시보드 폴링)이
         * 대신 해준다고 전제했는데, 그 폴링이 이벤트 기반으로 바뀌면서 팝업이 떠 있는 동안은
         * 그 이벤트 소비가 양보되므로(fetch_popup_is_active() 참고) 이제 여기서 직접 해야
         * 실제로 소비된다 */
        consume_ready_photo_if_current();
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
        ui_log_add_err(UI_ERR_FETCH_NORESPONSE, "Photo fetch stalled (%u/%u chunks, timeout)",
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

static bool fetch_popup_is_active(void)
{
    return s_progress_tick_fn == fetch_popup_tick_fn;
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
 * 지금촬영/모두지우기/사진가져오기와 같은 공용 진행팝업+타임아웃 토스트로 통일.
 * 2026-08-11 재설계(사용자 지시) — 스피너+퍼센트 대신 지금촬영 팝업과 같은 2줄 스택 라벨로
 * "가져오기 명령 전송/목록 수신 중/성공(실패)" 단계를 직접 보여줌 */
typedef enum {
    LIST_POPUP_STAGE_SENT = 0,   /* 1단계: 명령 전송, COUNT 응답 대기 */
    LIST_POPUP_STAGE_RECEIVING,  /* 2단계: 배치 수신 중, READY/ERROR/정체 대기 */
} list_popup_stage_t;

static list_popup_stage_t s_list_popup_stage;
static uint32_t           s_list_popup_stage_start_ms;
static lv_obj_t           *s_list_stage_label[2];
static uint32_t           s_renew_list_last_progress_ms;
static uint16_t           s_renew_list_last_received;

/* 2026-08-10 — "시작부터 총 경과시간" 기준에서 fetch_popup_tick_fn()과 동일한 "마지막 진행
 * 이후 경과시간"(정체 감지) 기준으로 변경. 딥슬립 웨이크대기+채널동기화 여러 라운드가
 * 합쳐지면 총 소요시간이 고정예산 하나로는 부족할 수 있는데, 그동안 항목이 계속 들어오고
 * 있다면(=정체 아님) 조급하게 포기할 이유가 없음 — 실사용 중 "데이터는 항상 오는데 팝업만
 * 먼저 3007로 포기" 패턴으로 발견 */
static bool renew_list_tick_fn(lv_obj_t *box)
{
    (void)box;
    lv_color_t grey  = lv_palette_main(LV_PALETTE_GREY);
    lv_color_t green = lv_palette_main(LV_PALETTE_GREEN);
    lv_color_t red   = lv_palette_main(LV_PALETTE_RED);

    if (s_list_popup_stage == LIST_POPUP_STAGE_SENT) {
        bool acked    = esp_now_photo_list_count_received();
        bool timedout = lv_tick_elaps(s_list_popup_stage_start_ms) > cam_response_timeout_ms();
        if (!acked && !timedout) return false;

        if (!acked) {
            set_stage_label(s_list_stage_label, 0, STR_LIST_STAGE1_NORESPONSE, red);
            ui_log_add_err(UI_ERR_LIST_NORESPONSE, "List request: no CAM response");
            return true;
        }

        set_stage_label(s_list_stage_label, 0, STR_LIST_STAGE1_DONE, green);
        set_stage_label(s_list_stage_label, 1, STR_LIST_STAGE2_PROGRESS, grey);
        s_list_popup_stage            = LIST_POPUP_STAGE_RECEIVING;
        s_list_popup_stage_start_ms   = lv_tick_get();
        s_renew_list_last_received    = 0;  /* handle_list_count()가 COUNT 도착 시 0으로 리셋함 */
        s_renew_list_last_progress_ms = lv_tick_get();
        return false;
    }

    if (s_list_popup_stage == LIST_POPUP_STAGE_RECEIVING) {
        esp_now_photo_list_state_t state = esp_now_photo_list_get_state();
        uint16_t received = 0, total = 0;
        esp_now_photo_list_get_progress(&received, &total);

        if (received != s_renew_list_last_received) {
            s_renew_list_last_received    = received;
            s_renew_list_last_progress_ms = lv_tick_get();
        }
        bool resolved = (state == ESP_NOW_PHOTO_LIST_STATE_READY || state == ESP_NOW_PHOTO_LIST_STATE_ERROR);
        bool stalled  = !resolved && lv_tick_elaps(s_renew_list_last_progress_ms) > cam_response_timeout_ms();

        if (!resolved && !stalled) return false;

        if (stalled) {
            lv_label_set_text_fmt(s_list_stage_label[1], ui_str(STR_LIST_STAGE2_STALLED_FMT),
                                   (unsigned)received, (unsigned)total);
            lv_obj_set_style_text_color(s_list_stage_label[1], red, 0);
            ui_log_add_err(UI_ERR_LIST_NORESPONSE, "List renew stalled, no CAM response (%u/%u items)",
                            (unsigned)received, (unsigned)total);
        } else if (state == ESP_NOW_PHOTO_LIST_STATE_ERROR) {
            lv_label_set_text_fmt(s_list_stage_label[1], ui_str(STR_LIST_STAGE2_MISMATCH_FMT),
                                   (unsigned)received, (unsigned)total);
            lv_obj_set_style_text_color(s_list_stage_label[1], red, 0);
            esp_now_photo_list_ack();
        } else {  /* READY */
            refresh_photo_list_ui(-1);
            esp_now_photo_list_ack();
            set_stage_label(s_list_stage_label, 1, STR_LIST_STAGE2_SUCCESS, green);
        }
        return true;
    }

    return false;
}

static bool list_popup_is_active(void)
{
    return s_progress_tick_fn == renew_list_tick_fn;
}

static void cb_renew_list(lv_event_t *e)
{
    (void)e;
    if (!s_has_selected_cam) return;
    if (!require_active_or_report(s_selected_cam_mac, "목록 갱신")) return;

    esp_now_photo_list_request(s_selected_cam_mac);

    s_list_popup_stage          = LIST_POPUP_STAGE_SENT;
    s_list_popup_stage_start_ms = lv_tick_get();

    lv_obj_t *box = show_progress_popup(renew_list_tick_fn);

    for (int i = 0; i < 2; i++) {
        s_list_stage_label[i] = lv_label_create(box);
        lv_obj_set_style_text_font(s_list_stage_label[i], ui_font_get(UI_FONT_SIZE_18), 0);
        lv_label_set_text(s_list_stage_label[i], "");
    }
    set_stage_label(s_list_stage_label, 0, STR_LIST_STAGE1_PROGRESS, lv_palette_main(LV_PALETTE_GREY));

    start_progress_popup(box);
}

/* ════════════════════════════════════════════════════════════
 * 모두 지우기 진행 팝업 — 1.접수확인 2.삭제완료대기 3.목록갱신(2026-08-21 3단계 재설계).
 * "CAM/Sens는 지능 없음, Cntl이 상태관리 전담" 원칙 — 예전엔 "접수됐는지"와 "다 지웠는지"를
 * 하나의 고정 타임아웃으로 뭉뚱그려서, 파일이 많으면(수백 개) CAM이 정상 작업 중인데도
 * Cntl이 먼저 포기하는 오탐이 있었음(진짜 ACK는 뒤늦게 도착). 지금촬영의 RECEIVED/SUCCESS
 * 2단계 패턴과 동일하게 분리: CAM이 접수 즉시(삭제 시작 전) 지울 개수를 먼저 알리고
 * (WAIT_RECEIVED), Cntl은 그 개수 기준으로 완료 대기 예산을 계산해서(WAIT_DONE) 진짜 완료
 * ACK만 완료로 인정함 — 타임아웃을 완료로 취급하지 않음. 어느 단계에서 진짜 타임아웃이
 * 나든 통신끊김/CAM중단 둘 중 하나를 가리키는 전용 에러코드를 남김.
 * 접수(WAIT_RECEIVED)조차 안 됐으면 목록 재동기화도 의미 없어 그대로 종료하고, 접수 후
 * 완료(WAIT_DONE)만 실패하면(부분 삭제 가능성) 기존처럼 목록 재조회(SYNC_LIST)로 실제
 * 상태를 다시 확인함 — 그것마저 타임아웃되면 "상태 확인 불가"만 보여주고 기존 목록은
 * 그대로 둠(성공한 것처럼 지우지 않음).
 * ════════════════════════════════════════════════════════════ */
typedef enum {
    DELETE_ALL_STAGE_WAIT_RECEIVED = 0,  /* CAM 접수+지울 개수 통보 대기 */
    DELETE_ALL_STAGE_WAIT_DONE     = 1,  /* 접수 확인됨 — 실제 삭제완료(ACK) 대기,
                                             개수 기준 예산 */
    DELETE_ALL_STAGE_SYNC_LIST     = 2,
} delete_all_stage_t;

static lv_obj_t          *s_delete_all_stage_label[2];
static delete_all_stage_t s_delete_all_stage;
static uint32_t           s_delete_all_stage_start_ms;

/* 삭제 완료 대기 예산 — 현재 파일별 순차삭제 구현 기준 대략치(실측 309개≈17초 ≈ 55ms/파일)에
 * 안전마진을 둠. cam_storage.c의 삭제 알고리즘이 나중에 빨라지면(DIR-table 최적화 등) 이
 * 값도 같이 줄여야 함(TODO) */
#define DELETE_ALL_BASE_MARGIN_MS   3000u
#define DELETE_ALL_PER_FILE_MS      100u

static uint32_t delete_all_done_budget_ms(uint16_t count)
{
    return DELETE_ALL_BASE_MARGIN_MS + (uint32_t)count * DELETE_ALL_PER_FILE_MS;
}

static bool delete_all_tick_fn(lv_obj_t *box)
{
    (void)box;
    lv_color_t grey  = lv_palette_main(LV_PALETTE_GREY);
    lv_color_t green = lv_palette_main(LV_PALETTE_GREEN);
    lv_color_t red   = lv_palette_main(LV_PALETTE_RED);

    if (s_delete_all_stage == DELETE_ALL_STAGE_WAIT_RECEIVED) {
        esp_now_delete_all_state_t st = esp_now_photo_delete_all_get_state();
        if (st == ESP_NOW_DELETE_ALL_STATE_RECEIVED || st == ESP_NOW_DELETE_ALL_STATE_ACKED) {
            uint16_t count = esp_now_photo_delete_all_get_received_count();
            lv_label_set_text_fmt(s_delete_all_stage_label[0], ui_str(STR_DELETEALL_STAGE1_DELETING_FMT),
                                   (unsigned)count);
            lv_obj_set_style_text_color(s_delete_all_stage_label[0], grey, 0);
            s_delete_all_stage = DELETE_ALL_STAGE_WAIT_DONE;
            s_delete_all_stage_start_ms = lv_tick_get();
            return false;
        }
        if (lv_tick_elaps(s_delete_all_stage_start_ms) > cam_response_timeout_ms()) {
            set_stage_label(s_delete_all_stage_label, 0, STR_DELETEALL_STAGE1_NORESPONSE, red);
            ui_log_add_err(UI_ERR_DELETE_ALL_NORESPONSE, "Delete-all request: no CAM receipt ack (link presumed down)");
            esp_now_photo_delete_all_clear();
            return true;  /* 접수조차 안 됐으면 목록 재동기화도 의미 없음 */
        }
        return false;
    }

    if (s_delete_all_stage == DELETE_ALL_STAGE_WAIT_DONE) {
        esp_now_delete_all_state_t st = esp_now_photo_delete_all_get_state();
        bool acked = (st == ESP_NOW_DELETE_ALL_STATE_ACKED);
        uint16_t received_count = esp_now_photo_delete_all_get_received_count();
        bool timedout = !acked &&
            lv_tick_elaps(s_delete_all_stage_start_ms) > delete_all_done_budget_ms(received_count);
        if (!acked && !timedout) return false;

        if (acked) {
            bool ok = esp_now_photo_delete_all_get_success();
            set_stage_label(s_delete_all_stage_label, 0,
                             ok ? STR_DELETEALL_STAGE1_DONE : STR_DELETEALL_STAGE1_FAILED, ok ? green : red);
            esp_now_photo_delete_all_clear();
        } else {
            lv_label_set_text_fmt(s_delete_all_stage_label[0], ui_str(STR_DELETEALL_STAGE1_STOPPED_FMT),
                                   (unsigned)received_count);
            lv_obj_set_style_text_color(s_delete_all_stage_label[0], red, 0);
            ui_log_add_err(UI_ERR_DELETE_ALL_STOPPED,
                            "Delete-all accepted but no completion response (CAM presumed stalled, %u accepted)", (unsigned)received_count);
            esp_now_photo_delete_all_clear();
        }
        /* 결과가 뭐든 실제 상태는 CAM에 다시 물어봐야 앎(부분 삭제 가능성) */
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

    s_delete_all_stage = DELETE_ALL_STAGE_WAIT_RECEIVED;
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
    /* 2026-09-10(사용자 지시 — "카메라 팝업에서 카메라 목록 선택을 alias로 바꿈") — 대시보드
     * 행(3351/3469줄)과 동일한 alias-or-name 패턴. 버퍼는 alias가 name보다 길 수 있어서
     * DEVICE_CONFIG_ALIAS_MAX_LEN 기준으로 잡음(예전엔 ESP_NOW_LINK_NAME_LEN 기준이라 alias
     * 적용 시 넘칠 수 있었음) */
    char options[ESP_NOW_HUB_MAX_NODES * (DEVICE_CONFIG_ALIAS_MAX_LEN + 1)];
    size_t off = 0;
    for (int i = 0; i < count; i++) {
        const char *alias = device_config_get_alias(macs[i]);
        const char *display_name = (alias[0] != '\0') ? alias : nodes[i].name;
        int n = snprintf(options + off, sizeof(options) - off, "%s%s",
                          i > 0 ? "\n" : "", display_name);
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

/* 바이트 값을 B/KB/MB/GB 중 알맞은 단위로 자동 스케일링 + 유효숫자 4자리로 표기
 * (2026-08-21, 사용자 지시) — 예: 12345 -> "12.34KB", 56789012 -> "56.78MB" */
static void format_bytes_human(uint32_t bytes, char *buf, size_t buf_size)
{
    static const char *units[] = { "B", "KB", "MB", "GB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(buf, buf_size, "%u%s", (unsigned)bytes, units[u]);
        return;
    }
    int decimals = (v < 10.0) ? 3 : (v < 100.0) ? 2 : (v < 1000.0) ? 1 : 0;
    snprintf(buf, buf_size, "%.*f%s", decimals, v, units[u]);
}

/* 배터리 표시 공용 포맷 함수(2026-08-22, 사용자 지시) — "{배터리/Battery}: x.xx V (yy%)".
 * CAM에 이어 나중에 Sens 요약행에도 그대로 재사용할 목적으로 여기 분리해둠 — 보드마다
 * mV/%%를 얻는 방식(CAM=I2C 익스팬더, Sens=직접 GPIO ADC)은 다르지만 표시 포맷은 공통 */
static void format_battery_display(char *buf, size_t buf_size, uint16_t battery_mv, uint8_t battery_pct)
{
    /* 2026-09-09(사용자 지적 — "배터리만 Battery : 4.32V (100%) 로 길게 나와") — 연결됨
     * 행에서 다른 항목(예: "Measure 10s")과 나란히 " / "로 이어붙는 짧은 형식이라, 콜론+
     * "V" 앞 공백을 빼서 통일감 있게 함(이 함수는 지금 그 행 표시 용도로만 씀) */
    snprintf(buf, buf_size, "%s %d.%02dV (%u%%)", ui_str(STR_LABEL_BATTERY),
             battery_mv / 1000, (battery_mv % 1000) / 10, (unsigned)battery_pct);
}

/* 2026-09-05(사용자 설계) — sensor_channel_type_t(esp_now_link.h) enum -> 콘 로컬 라벨/단위.
 * 와이어엔 enum만 오가고, 사람이 읽을 텍스트는 여기서만 나옴(i18n, ui_strings) — 새 채널
 * 종류가 생기면 여기 case 하나만 추가하면 됨(프로토콜 구조 자체는 안 바뀜) */
static bool chan_type_to_strs(uint8_t chan_type, ui_str_id_t *label_id, ui_str_id_t *unit_id)
{
    switch (chan_type) {
        case SENSOR_CHAN_TEMP_C:   *label_id = STR_CHAN_LABEL_TEMP_C;   *unit_id = STR_CHAN_UNIT_TEMP_C;   return true;
        case SENSOR_CHAN_HUMI_PCT: *label_id = STR_CHAN_LABEL_HUMI_PCT; *unit_id = STR_CHAN_UNIT_HUMI_PCT; return true;
        case SENSOR_CHAN_CO2_PPM:  *label_id = STR_CHAN_LABEL_CO2_PPM;  *unit_id = STR_CHAN_UNIT_CO2_PPM;  return true;
        case SENSOR_CHAN_NH3_PPM:  *label_id = STR_CHAN_LABEL_NH3_PPM;  *unit_id = STR_CHAN_UNIT_NH3_PPM;  return true;
        default: return false;
    }
}

/* 2026-09-08(연결 기능 주화면 이관, 사용자 설계 — "Summary는 시스템이 잘 돌고 있는지
 * 보여주려는 의도") — Summary 실시간 순시치 블록. 통계 Overview와 채널 4개는 같지만
 * 스케일/min/max/avg 없이 "지금 값"만 "라벨: 값 단위" 한 줄씩. 여러 센서가 같은 채널을
 * 보고하면 먼저 찾은 것만 씀(챈널당 최대 하나만 표시하는 단순화 — 오늘은 센서가 1개뿐이라
 * 실질적으로 문제 없음, 여러 센서 지원은 project_cntl_sensor_panel_needs_per_node_rows 범위) */
static void refresh_summary_live_values(const esp_now_hub_node_t *nodes, int count)
{
    struct { uint8_t chan_type; lv_obj_t *label; } rows[] = {
        { SENSOR_CHAN_TEMP_C,   s_summary_live_temp_label },
        { SENSOR_CHAN_HUMI_PCT, s_summary_live_humi_label },
        { SENSOR_CHAN_CO2_PPM,  s_summary_live_co2_label  },
        { SENSOR_CHAN_NH3_PPM,  s_summary_live_nh3_label  },
    };
    static char s_last_text[4][96];

    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        ui_str_id_t label_id, unit_id;
        if (!chan_type_to_strs(rows[r].chan_type, &label_id, &unit_id)) continue;

        char line[96];
        bool found = false;
        for (int i = 0; i < count && !found; i++) {
            if (nodes[i].kind != HUB_NODE_KIND_SENS || !nodes[i].has_sensor_data) continue;
            for (int c = 0; c < nodes[i].chan_count; c++) {
                if (nodes[i].chan_type[c] != rows[r].chan_type) continue;
                found = true;
                if (nodes[i].chan_ok[c] && nodes[i].chan_invalid[c]) {
                    snprintf(line, sizeof(line), "%s: %s", ui_str(label_id), ui_str(STR_SENSOR_VALUE_INVALID));
                } else if (!nodes[i].chan_ok[c]) {
                    snprintf(line, sizeof(line), "%s: %s", ui_str(label_id), ui_str(STR_SENSOR_VALUE_PENDING));
                } else {
                    float val = nodes[i].chan_val[c];
                    int scaled = (int)(val * 100.0f + 0.5f);
                    int whole = scaled / 100;
                    int frac  = scaled % 100;
                    snprintf(line, sizeof(line), "%s: %d.%02d %s", ui_str(label_id), whole, frac, ui_str(unit_id));
                }
                break;
            }
        }
        if (!found) snprintf(line, sizeof(line), "%s: %s", ui_str(label_id), ui_str(STR_STATS_OVERVIEW_NO_DATA));

        if (strcmp(s_last_text[r], line) != 0) {
            lv_label_set_text(rows[r].label, line);
            strncpy(s_last_text[r], line, sizeof(s_last_text[r]) - 1);
            s_last_text[r][sizeof(s_last_text[r]) - 1] = '\0';
        }
    }
}

/* 2026-09-06(사용자 설계) — 통계탭 값 테이블 한 행의 mac -> 노드 이름. 언페어/이름변경 등으로
 * 지금 노드 목록에서 못 찾으면(오래된 기록) mac 뒤 2바이트로 폴백 표시 */
static void find_node_name_by_mac(const uint8_t mac[6], char *out, size_t out_cap)
{
    esp_now_hub_node_t nodes[ESP_NOW_HUB_MAX_NODES];
    int total = esp_now_hub_get_nodes(HUB_NODE_KIND_SENS, nodes, ESP_NOW_HUB_MAX_NODES);
    for (int i = 0; i < total; i++) {
        if (memcmp(nodes[i].mac, mac, 6) == 0) {
            snprintf(out, out_cap, "%s", nodes[i].name);
            return;
        }
    }
    snprintf(out, out_cap, "%02X%02X", mac[4], mac[5]);
}

/* 개괄 판넬(2026-09-07 재설계, 구 "최대/최소 판넬") — 좌=온도/이산화탄소,
 * 우=습도/암모니아. Scale 드롭다운으로 고른 기간 기준으로 재계산(사용자 확정:
 * "스케일마다 계산해야되"). Max/Min/Average 범례는 제목(STR_PANEL_STATS_OVERVIEW)에
 * 한 번만 있고, 각 줄은 "라벨[단위]: 값 / 값 / 값"만(사용자 재지시 — 이산화탄소처럼
 * 긴 값이 X/N/A 반복으로 줄바꿈되던 문제 해결) */
/* 2026-09-10(임시 진단으로 발견 — "2009가 한번 나면 계속 나네", refresh_stats_page timing
 * 실측: graph만 매번 ~100ms, 개괄판넬과 그래프가 채널당 min/max/avg를 각자 또 계산해서
 * SD 스캔이 중복됨) — 개괄판넬이 계산한 걸 여기 캐시에 남겨서 refresh_stats_graph()가
 * 재사용하게 함(같은 tick 안에서 순서 보장: refresh_stats_page()가 overview -> graph 순).
 * 채널 순서는 STATS_GRAPH_SERIES_COUNT 순서(온도/습도/CO2/암모니아)와 동일 */
static float s_stats_minmax_cache_mn[STATS_GRAPH_SERIES_COUNT];
static float s_stats_minmax_cache_mx[STATS_GRAPH_SERIES_COUNT];
static bool  s_stats_minmax_cache_valid[STATS_GRAPH_SERIES_COUNT];

/* 2026-09-10(재설계 — [[project_cntl_sd_reliability_redesign_2026_09_10]]) — 태스크 격리
 * 아키텍처(별도 워커+뮤텍스+세마포어)는 잘못된 진단(SD 에러를 "행"으로 오판) 위에 지어졌던
 * 구조라 전부 제거함. 실제 SD 에러는 fail(정상 리턴, 유한시간)이었고, 진짜 문제는 그 fail을
 * 무시하고 계속 다음 것도 시도해서 누적된 것 — 고쳐야 할 건 회로차단기(첫 fail에서 그 틱
 * 전체 중단)이지 격리가 아니었음. 그래서 원래대로 LVGL 태스크(2초 타이머)에서 직접 SD를
 * 부르되, 각 단계는 bool을 리턴해서 "SD 자체 문제로 실패했는지"(stats_store_had_io_error(),
 * "데이터 없음"과 구분됨)를 알리고, 그러면 그 틱의 나머지 단계를 전부 건너뜀(사용자 지시:
 * "SD 조회 fail이면, 다른 값도 믿을 수 없어. 즉시 중단이지") */

/* chan_type별 min/max/avg를 개요판넬 라벨에 반영. 리턴값 false = SD 자체 오류(그 틱 중단) */
static bool refresh_stats_overview_panel(void)
{
    uint16_t idx = lv_dropdown_get_selected(s_stats_scale_dd);
    uint32_t scale_sec = (idx < STATS_SCALE_COUNT) ? STATS_SCALE_SECONDS[idx] : STATS_SCALE_SECONDS[0];
    uint32_t now = rtc_sync_get_unix_time();
    uint32_t cutoff = (now > scale_sec) ? now - scale_sec : 0;

    struct { uint8_t chan_type; lv_obj_t *label; } rows[] = {
        { SENSOR_CHAN_TEMP_C,   s_overview_temp_label },
        { SENSOR_CHAN_HUMI_PCT, s_overview_humi_label },
        { SENSOR_CHAN_CO2_PPM,  s_overview_co2_label  },
        { SENSOR_CHAN_NH3_PPM,  s_overview_nh3_label  },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        ui_str_id_t label_id, unit_id;
        if (!chan_type_to_strs(rows[i].chan_type, &label_id, &unit_id)) continue;

        char line[128];
        float mn, mx, avg;
        bool have_range = stats_store_get_min_max_avg_since(cutoff, rows[i].chan_type, &mn, &mx, &avg);
        if (!have_range && stats_store_had_io_error()) return false;  /* SD 자체 문제 — 즉시 중단 */
        s_stats_minmax_cache_valid[i] = have_range;
        if (have_range) {
            s_stats_minmax_cache_mn[i] = mn;
            s_stats_minmax_cache_mx[i] = mx;
            int mx_s = (int)(mx * 100.0f + 0.5f);
            int mn_s = (int)(mn * 100.0f + 0.5f);
            int avg_s = (int)(avg * 100.0f + 0.5f);
            snprintf(line, sizeof(line), ui_str(STR_STATS_OVERVIEW_ROW_FMT), ui_str(label_id), ui_str(unit_id),
                     mx_s / 100, mx_s % 100,
                     mn_s / 100, mn_s % 100,
                     avg_s / 100, avg_s % 100);
        } else {
            snprintf(line, sizeof(line), "%s: %s", ui_str(label_id), ui_str(STR_STATS_OVERVIEW_NO_DATA));
        }
        lv_label_set_text(rows[i].label, line);
    }
    return true;
}

static void stats_graph_swipe_async_refresh(void *user_data);  /* 아래(스와이프 처리부)에 정의 */

static void cb_stats_scale_changed(lv_event_t *e)
{
    (void)e;
    s_stats_graph_offset = 0;  /* 2026-09-11 — 스케일 바꾸면 "지금" 창으로 되돌림(다른
                                   스케일에서의 오프셋을 그대로 들고 가면 혼란스러움) */
    /* 2026-09-12(원 설계 — 탭 값 박스는 "Scale dropdown change"에 사라져야 함) */
    if (s_stats_chart_tap_label) {
        lv_obj_add_flag(s_stats_chart_tap_label, LV_OBJ_FLAG_HIDDEN);
        s_stats_chart_tap_shown_series = -1;
    }
    refresh_stats_overview_panel();
    /* 2026-09-11(사용자 지적 — "스케일 변경 시 X축 라벨이 바뀌지 않아") — 그래프 자체 갱신은
     * 5틱에 한 번 도는 주기적 타이머라 스케일을 바꿔도 바로 안 반영됨. 스와이프와 동일하게
     * force_refresh + async로 즉시 반영 */
    s_stats_graph_force_refresh = true;
    lv_async_call(stats_graph_swipe_async_refresh, NULL);
}

/* 리턴값 false = SD 자체 오류(그 틱 중단) */
static bool refresh_stats_table(void)
{
    stats_record_t recs[STATS_STORE_PAGE_SIZE];
    uint32_t got = stats_store_read_page(s_stats_page_index, STATS_STORE_PAGE_SIZE,
                                          recs, STATS_STORE_PAGE_SIZE);
    if (got == 0 && stats_store_had_io_error()) return false;
    uint32_t total = stats_store_get_count();
    if (total == 0 && stats_store_had_io_error()) return false;
    uint32_t total_pages = (total + STATS_STORE_PAGE_SIZE - 1) / STATS_STORE_PAGE_SIZE;
    if (total_pages == 0) total_pages = 1;

    /* 2026-09-07 — 헤더(항목/값/시간)는 이제 테이블 밖(stats_table_header_row)에 고정으로
     * 따로 그림(사용자 지시: "스크롤 안되야되"), 테이블 자신은 데이터 행만 채움 */
    lv_table_set_row_count(s_stats_table, got > 0 ? got : 1);
    if (got == 0) {
        lv_table_set_cell_value(s_stats_table, 0, 0, ui_str(STR_STATS_TABLE_EMPTY));
        lv_table_set_cell_value(s_stats_table, 0, 1, "");
        lv_table_set_cell_value(s_stats_table, 0, 2, "");
    } else {
        /* stats_store_read_page()는 파일에 쓰인 순서(오래된 것부터)로 채워서 돌려줌 —
         * 화면엔 최신이 위로 오게 역순으로 순회 */
        for (uint32_t i = 0; i < got; i++) {
            const stats_record_t *r = &recs[got - 1 - i];

            char name[ESP_NOW_LINK_NAME_LEN];
            find_node_name_by_mac(r->mac, name, sizeof(name));

            ui_str_id_t label_id, unit_id;
            char item_buf[64];
            char value_buf[32];
            if (chan_type_to_strs(r->chan_type, &label_id, &unit_id)) {
                snprintf(item_buf, sizeof(item_buf), "%s %s", name, ui_str(label_id));
                int scaled = (int)(r->value * 100.0f + 0.5f);
                snprintf(value_buf, sizeof(value_buf), "%d.%02d%s",
                         scaled / 100, scaled % 100, ui_str(unit_id));
            } else {
                snprintf(item_buf, sizeof(item_buf), "%s", name);
                int scaled = (int)(r->value * 100.0f + 0.5f);
                snprintf(value_buf, sizeof(value_buf), "%d.%02d", scaled / 100, scaled % 100);
            }

            struct tm tm_buf;
            time_t tt = (time_t)r->unix_time;
            localtime_r(&tt, &tm_buf);
            char time_buf[16];
            snprintf(time_buf, sizeof(time_buf), "%02u:%02u:%02u",
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

            lv_table_set_cell_value(s_stats_table, i, 0, item_buf);
            lv_table_set_cell_value(s_stats_table, i, 1, value_buf);
            lv_table_set_cell_value(s_stats_table, i, 2, time_buf);
        }
    }

    uint32_t displayed_page = s_stats_page_index + 1;
    char page_buf[32];
    snprintf(page_buf, sizeof(page_buf), ui_str(STR_STATS_PAGE_FMT),
             (unsigned long)displayed_page, (unsigned long)total_pages);
    lv_label_set_text(s_stats_page_label, page_buf);

    bool can_prev = (s_stats_page_index + 1) < total_pages;  /* 더 오래된 페이지 있음 */
    bool can_next = (s_stats_page_index > 0);                /* 더 최신 페이지 있음 */
    if (can_prev) lv_obj_remove_state(s_stats_prev_btn, LV_STATE_DISABLED);
    else          lv_obj_add_state(s_stats_prev_btn, LV_STATE_DISABLED);
    if (can_next) lv_obj_remove_state(s_stats_next_btn, LV_STATE_DISABLED);
    else          lv_obj_add_state(s_stats_next_btn, LV_STATE_DISABLED);
    if (can_prev) lv_obj_remove_state(s_stats_jump_prev_btn, LV_STATE_DISABLED);
    else          lv_obj_add_state(s_stats_jump_prev_btn, LV_STATE_DISABLED);
    if (can_next) lv_obj_remove_state(s_stats_jump_next_btn, LV_STATE_DISABLED);
    else          lv_obj_add_state(s_stats_jump_next_btn, LV_STATE_DISABLED);
    return true;
}

/* 2026-09-11(그래프 재설계 항목5) — 스케일에 맞는 간격으로 X축 아래 상대시각 라벨을 배치.
 * 라벨 위치는 챠트의 실제 픽셀 좌표를 기준으로 계산(오프셋과 무관 — 라벨은 항상 "이
 * 창의 오른쪽 끝(0)"부터 "왼쪽 끝(-전체스케일)"까지의 상대 표기이므로, 절대시각/오프셋에
 * 의존하지 않음 — 그래야 과거로 스와이프해도 라벨이 안 헷갈림) */
/* 2026-09-11(사용자 지적 — "X legend가 잘못 구현됬어") — 오프셋으로 과거 창을 보고 있을 때도
 * 이 함수가 offset을 몰라서 오른쪽 끝을 항상 "0"(지금)으로 라벨링하고 있었음. offset_sec을
 * 받아 모든 라벨에 더해서, 실제 "지금부터 얼마나 전"인지를 보여주게 고침 */
static void refresh_stats_graph_x_labels(uint16_t scale_idx, uint32_t offset_sec)
{
    if (!s_stats_graph_xaxis_row) return;
    uint32_t scale_sec = STATS_SCALE_SECONDS[scale_idx];
    uint32_t step = s_stats_xaxis_interval_sec[scale_idx];
    int label_count = (int)(scale_sec / step) + 1;
    if (label_count > STATS_GRAPH_X_LABEL_MAX) label_count = STATS_GRAPH_X_LABEL_MAX;

    lv_area_t row_coords;
    lv_obj_get_coords(s_stats_graph_xaxis_row, &row_coords);
    int32_t row_w = row_coords.x2 - row_coords.x1;

    for (int i = 0; i < STATS_GRAPH_X_LABEL_MAX; i++) {
        if (!s_stats_graph_x_labels[i]) continue;
        if (i >= label_count) {
            lv_obj_add_flag(s_stats_graph_x_labels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_stats_graph_x_labels[i], LV_OBJ_FLAG_HIDDEN);

        /* 2026-09-11(사용자 지적 — "X축 라벨이 왼쪽 한 곳에 뭉쳐서 표기돼") — 방금 추가한
         * offset_sec을 좌표 계산에도 같이 넣어버린 게 원인. 화면상 위치는 "이 창 안에서
         * 오른쪽 끝에서 얼마나 왼쪽인지"(i*step, 창 폭 기준)만으로 정해야 하고, offset은
         * 텍스트(실제 "지금부터 몇 전"인지)에만 더해야 함 — 섞으면 offset>0일 때 분수가
         * 1을 넘어 px가 음수가 되고, 전부 x=0으로 클램프되어 왼쪽에 뭉침 */
        uint32_t seconds_in_window = (uint32_t)i * step;
        uint32_t seconds_ago = seconds_in_window + offset_sec;
        float frac_from_right = (float)seconds_in_window / (float)scale_sec;
        int32_t px = (int32_t)((1.0f - frac_from_right) * (float)row_w);

        char buf[16];
        if (seconds_ago == 0) {
            snprintf(buf, sizeof(buf), "0");
        } else if (step % 86400 == 0) {
            snprintf(buf, sizeof(buf), "-%ud", (unsigned)(seconds_ago / 86400));
        } else if (step % 3600 == 0) {
            snprintf(buf, sizeof(buf), "-%uh", (unsigned)(seconds_ago / 3600));
        } else {
            snprintf(buf, sizeof(buf), "-%um", (unsigned)(seconds_ago / 60));
        }
        lv_label_set_text(s_stats_graph_x_labels[i], buf);
        /* 오른쪽 끝(i=0) 라벨은 폭 안 벗어나게 오른쪽 정렬, 나머지는 중앙정렬 근사 */
        int32_t lbl_w = lv_obj_get_width(s_stats_graph_x_labels[i]);
        int32_t x = px - lbl_w / 2;
        if (x + lbl_w > row_w) x = row_w - lbl_w;
        if (x < 0) x = 0;
        lv_obj_set_pos(s_stats_graph_x_labels[i], x, 0);
    }
}

/* 2026-09-10/11(재설계 — 라인그래프, 계열 4개 온도/습도/CO2/암모니아, 스케일별 사전집계
 * 저장에서 읽음, [[project_cntl_stats_graph_redesign_2026_09_10]]) — 계열마다 실제 단위/
 * 범위가 달라서 각 계열을 그 창 안의 최소~최대 기준 0~100으로 정규화. 값이 없는 슬롯은
 * 계열별 고정 높이(s_stats_gap_ref_height)에 점만 찍음(별도 겹침 차트, 사용자 지시: "값이
 * 없는 영역은... 찍어야 할 위치마다 찍는거야"). 테이블 보고 있을 때는 SD 조회 생략.
 * 리턴값 false = SD 자체 오류(그 틱 중단) */
static bool refresh_stats_graph(void)
{
    if (!s_stats_chart) return true;
    if (lv_obj_has_flag(s_stats_graph_view, LV_OBJ_FLAG_HIDDEN)) return true;

    /* 2026-09-11(사용자 지시 — 실측 137ms 안팎 확인 후 "갱신 주기는 15초로 결정") — 틱카운트
     * 나눗셈(2s 타이머라 5의 배수만 가능)으론 15000ms를 못 맞춰서 경과시간 직접 비교로 변경.
     * 스와이프/스케일변경 직후엔 force로 즉시 반영 */
    static uint32_t s_graph_last_refresh_tick = 0;
    uint32_t now_tick_ms = lv_tick_get();
    if (!s_stats_graph_force_refresh && (now_tick_ms - s_graph_last_refresh_tick) < 15000) return true;
    s_graph_last_refresh_tick = now_tick_ms;
    s_stats_graph_force_refresh = false;

    uint16_t idx = lv_dropdown_get_selected(s_stats_scale_dd);
    if (idx >= STATS_SCALE_COUNT) idx = 0;
    uint32_t scale_sec = STATS_SCALE_SECONDS[idx];
    uint32_t bucket_width = scale_sec / STATS_GRAPH_POINT_COUNT;
    if (bucket_width == 0) bucket_width = 1;

    uint32_t now = rtc_sync_get_unix_time();
    uint32_t offset_sec = (uint32_t)s_stats_graph_offset * scale_sec;
    uint32_t window_end   = (now > offset_sec) ? now - offset_sec : 0;
    uint32_t window_start = (window_end > scale_sec) ? window_end - scale_sec : 0;

    /* 2026-09-11(사용자 지시 — "데이터 읽기부터 그리기까지 소요 시간 측정해") — SD 읽기
     * 시작부터 차트에 값 세팅 끝까지(실제 픽셀 드로잉은 이후 LVGL 자체 렌더 패스에서 별도로
     * 일어남 — 그건 이 함수 범위 밖이라 여기 포함 안 됨) */
    int64_t t_start_us = esp_timer_get_time();

    static const uint8_t chan_types[STATS_GRAPH_SERIES_COUNT] = {
        SENSOR_CHAN_TEMP_C, SENSOR_CHAN_HUMI_PCT, SENSOR_CHAN_CO2_PPM, SENSOR_CHAN_NH3_PPM
    };

    static stats_bucket_t buf[STATS_GRAPH_SERIES_COUNT][STATS_GRAPH_POINT_COUNT];
    uint32_t got[STATS_GRAPH_SERIES_COUNT];
    for (int s = 0; s < STATS_GRAPH_SERIES_COUNT; s++) {
        got[s] = stats_agg_read_window((uint8_t)idx, chan_types[s], window_start, window_end,
                                        buf[s], STATS_GRAPH_POINT_COUNT);
        if (got[s] == 0 && stats_store_had_io_error()) return false;  /* SD 자체 문제 — 즉시 중단 */
    }

    lv_chart_set_point_count(s_stats_chart, STATS_GRAPH_POINT_COUNT);
    lv_chart_set_point_count(s_stats_gap_chart, STATS_GRAPH_POINT_COUNT);
    s_stats_chart_shown_points = STATS_GRAPH_POINT_COUNT;

    for (int s = 0; s < STATS_GRAPH_SERIES_COUNT; s++) {
        static bool has[STATS_GRAPH_POINT_COUNT];
        static float vals[STATS_GRAPH_POINT_COUNT];
        for (uint32_t i = 0; i < STATS_GRAPH_POINT_COUNT; i++) has[i] = false;

        for (uint32_t i = 0; i < got[s]; i++) {
            long slot_l = ((long)buf[s][i].bucket_start_unix - (long)window_start) / (long)bucket_width;
            if (slot_l < 0 || slot_l >= STATS_GRAPH_POINT_COUNT) continue;
            uint32_t slot = (uint32_t)slot_l;
            has[slot] = true;
            vals[slot] = buf[s][i].avg_value;
        }

        /* 2026-09-11 — 스와이프로 지금이 아닌 다른 창을 볼 수 있게 되면서, "지금까지"
         * 기준으로 미리 계산해둔 개괄판넬 min/max 캐시를 그대로 쓰면 안 맞을 수 있음
         * (오프셋이 0이 아닐 때) — 항상 이 창에서 받아온 값들로 직접 min/max 계산 */
        bool have_range = false;
        float mn = 0.0f, mx = 0.0f;
        for (uint32_t i = 0; i < STATS_GRAPH_POINT_COUNT; i++) {
            if (!has[i]) continue;
            if (!have_range) { mn = mx = vals[i]; have_range = true; }
            else {
                if (vals[i] < mn) mn = vals[i];
                if (vals[i] > mx) mx = vals[i];
            }
        }
        float span = (have_range && mx > mn) ? (mx - mn) : 0.0f;

        int32_t norm_vals[STATS_GRAPH_POINT_COUNT];
        int32_t gap_vals[STATS_GRAPH_POINT_COUNT];
        for (uint32_t i = 0; i < STATS_GRAPH_POINT_COUNT; i++) {
            if (has[i]) {
                s_stats_chart_real_values[s][i] = vals[i];
                s_stats_chart_has_value[s][i] = true;
                norm_vals[i] = (span > 0.0f) ? (int32_t)(((vals[i] - mn) / span) * 100.0f + 0.5f) : 50;
                gap_vals[i] = LV_CHART_POINT_NONE;
            } else {
                s_stats_chart_has_value[s][i] = false;
                norm_vals[i] = LV_CHART_POINT_NONE;
                gap_vals[i] = s_stats_gap_ref_height[s];
            }
        }
        lv_chart_set_series_values(s_stats_chart, s_stats_chart_series[s], norm_vals, STATS_GRAPH_POINT_COUNT);
        lv_chart_set_series_values(s_stats_gap_chart, s_stats_gap_series[s], gap_vals, STATS_GRAPH_POINT_COUNT);

        /* 2026-09-11(사용자 지적 — "상단에 4계열 최대값, 하단에 4계열 최소값") — 정규화상
         * 100=그 계열의 실제 최대, 0=실제 최소라서 차트 맨 위/맨 아래와 항상 일치함 */
        bool checked = lv_obj_has_state(s_stats_chart_checkbox[s], LV_STATE_CHECKED);
        bool show = checked && have_range;
        ui_str_id_t label_id, unit_id;
        chan_type_to_strs(chan_types[s], &label_id, &unit_id);
        (void)label_id;
        char numbuf[24];
        if (s_stats_graph_max_label[s]) {
            if (show) {
                int mx_scaled = (int)(mx * 10.0f + (mx >= 0 ? 0.5f : -0.5f));
                snprintf(numbuf, sizeof(numbuf), "%d.%d%s", mx_scaled / 10, abs(mx_scaled % 10), ui_str(unit_id));
                lv_label_set_text(s_stats_graph_max_label[s], numbuf);
                lv_obj_remove_flag(s_stats_graph_max_label[s], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_stats_graph_max_label[s], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_stats_graph_min_label[s]) {
            if (show) {
                int mn_scaled = (int)(mn * 10.0f + (mn >= 0 ? 0.5f : -0.5f));
                snprintf(numbuf, sizeof(numbuf), "%d.%d%s", mn_scaled / 10, abs(mn_scaled % 10), ui_str(unit_id));
                lv_label_set_text(s_stats_graph_min_label[s], numbuf);
                lv_obj_remove_flag(s_stats_graph_min_label[s], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_stats_graph_min_label[s], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    refresh_stats_graph_x_labels(idx, offset_sec);

    int64_t t_end_us = esp_timer_get_time();
    ESP_LOGW(TAG, "MEMDIAG 그래프 윈도 1회 갱신(읽기~차트데이터세팅) 소요시간: %lld us (스케일idx=%u)",
             (long long)(t_end_us - t_start_us), (unsigned)idx);
    return true;
}

static void cb_stats_chart_series_toggle(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bool checked = lv_obj_has_state(s_stats_chart_checkbox[idx], LV_STATE_CHECKED);
    lv_chart_hide_series(s_stats_chart, s_stats_chart_series[idx], !checked);
    /* 2026-09-11(사용자 지적 — "계열을 해제해도 점은 계속 찍혀") — 실데이터 차트만 숨기고
     * 겹쳐그린 공백점 차트는 안 숨겼던 버그 */
    lv_chart_hide_series(s_stats_gap_chart, s_stats_gap_series[idx], !checked);
    if (!checked) {
        if (s_stats_graph_max_label[idx]) lv_obj_add_flag(s_stats_graph_max_label[idx], LV_OBJ_FLAG_HIDDEN);
        if (s_stats_graph_min_label[idx]) lv_obj_add_flag(s_stats_graph_min_label[idx], LV_OBJ_FLAG_HIDDEN);
        /* 2026-09-12(원 설계 — 탭 값 박스는 "unchecking that series' checkbox"에 사라져야 함,
         * 단 지금 박스에 표시 중인 계열이 그 계열일 때만) */
        if (idx == s_stats_chart_tap_shown_series && s_stats_chart_tap_label) {
            lv_obj_add_flag(s_stats_chart_tap_label, LV_OBJ_FLAG_HIDDEN);
            s_stats_chart_tap_shown_series = -1;
        }
    }
}

/* 2026-09-10(사용자 지시 — "탭하면 값이 나타나는 것") — 탭 x좌표로 가장 가까운 인덱스를
 * 찾고(보이는 계열 중 하나 기준, x좌표는 계열 무관하게 공통), 그 인덱스에서 탭 y좌표와
 * 가장 가까운 보이는 계열 하나를 골라 실제 값을 표시(사용자 지시: "가장 가까운 점") */
static void cb_stats_chart_tap(lv_event_t *e)
{
    (void)e;
    if (!s_stats_chart_tap_label) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    uint32_t point_count = s_stats_chart_shown_points;
    if (point_count == 0) return;

    int ref_series = -1;
    for (int s = 0; s < STATS_GRAPH_SERIES_COUNT; s++) {
        if (lv_obj_has_state(s_stats_chart_checkbox[s], LV_STATE_CHECKED)) { ref_series = s; break; }
    }
    if (ref_series < 0) return;

    lv_area_t chart_coords;
    lv_obj_get_coords(s_stats_chart, &chart_coords);

    uint32_t nearest_idx = 0;
    int32_t best_dist = INT32_MAX;
    for (uint32_t i = 0; i < point_count; i++) {
        lv_point_t pp;
        lv_chart_get_point_pos_by_id(s_stats_chart, s_stats_chart_series[ref_series], i, &pp);
        int32_t px = chart_coords.x1 + pp.x;
        int32_t dist = (p.x > px) ? (p.x - px) : (px - p.x);
        if (dist < best_dist) { best_dist = dist; nearest_idx = i; }
    }

    static const uint8_t chan_types[STATS_GRAPH_SERIES_COUNT] = {
        SENSOR_CHAN_TEMP_C, SENSOR_CHAN_HUMI_PCT, SENSOR_CHAN_CO2_PPM, SENSOR_CHAN_NH3_PPM
    };
    int chosen_series = -1;
    int32_t best_y_dist = INT32_MAX;
    for (int s = 0; s < STATS_GRAPH_SERIES_COUNT; s++) {
        if (!lv_obj_has_state(s_stats_chart_checkbox[s], LV_STATE_CHECKED)) continue;
        if (!s_stats_chart_has_value[s][nearest_idx]) continue;
        lv_point_t pp;
        lv_chart_get_point_pos_by_id(s_stats_chart, s_stats_chart_series[s], nearest_idx, &pp);
        int32_t py = chart_coords.y1 + pp.y;
        int32_t dist = (p.y > py) ? (p.y - py) : (py - p.y);
        if (dist < best_y_dist) { best_y_dist = dist; chosen_series = s; }
    }
    if (chosen_series < 0) return;
    float v = s_stats_chart_real_values[chosen_series][nearest_idx];

    ui_str_id_t label_id, unit_id;
    if (!chan_type_to_strs(chan_types[chosen_series], &label_id, &unit_id)) return;
    int scaled = (int)(v * 100.0f + 0.5f);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: %d.%02d%s", ui_str(label_id), scaled / 100, scaled % 100, ui_str(unit_id));
    lv_label_set_text(s_stats_chart_tap_label, buf);

    /* 2026-09-12(원 설계 carried-forward — "탭 위치에 직접 그려") — 고른 계열/지점의 실제
     * 화면좌표에 박스를 놓음(다음 렌더에 라벨 크기가 반영되므로, 폭을 넘어가지 않게 clamp) */
    lv_point_t chosen_pp;
    lv_chart_get_point_pos_by_id(s_stats_chart, s_stats_chart_series[chosen_series], nearest_idx, &chosen_pp);
    lv_obj_update_layout(s_stats_chart_tap_label);
    int32_t lbl_w = lv_obj_get_width(s_stats_chart_tap_label);
    int32_t lbl_h = lv_obj_get_height(s_stats_chart_tap_label);
    int32_t chart_w = lv_obj_get_width(s_stats_chart);
    int32_t chart_h = lv_obj_get_height(s_stats_chart);
    int32_t x = chosen_pp.x - lbl_w / 2;
    int32_t y = chosen_pp.y - lbl_h - 6;  /* 점 바로 위 */
    if (x < 0) x = 0;
    if (x + lbl_w > chart_w) x = chart_w - lbl_w;
    if (y < 0) y = chosen_pp.y + 6;  /* 위 공간이 없으면 점 아래로 */
    if (y + lbl_h > chart_h) y = chart_h - lbl_h;
    lv_obj_set_pos(s_stats_chart_tap_label, x, y);
    lv_obj_remove_flag(s_stats_chart_tap_label, LV_OBJ_FLAG_HIDDEN);
    s_stats_chart_tap_shown_series = chosen_series;
}

/* 2026-09-10(재설계 — SD I/O를 stats_io_worker_task로 격리) — 이 타이머는 이제 SD를 전혀
 * 안 건드림(순수 렌더). 예전엔 여기서 SD fopen/fread/fclose가 직접 일어나서 SD 장애 시
 * 몇 초씩 LVGL 태스크를 막아 태스크워치독까지 발동시켰음
 * ([[feedback_design_for_exceptions_not_just_fails]]) — 그 SD 접근은 전부 워커로 옮기고,
 * 여기는 워커가 미리 계산해둔 스냅샷을 읽어 화면만 그림. 타이밍 로그는 "렌더가 실제로
 * 빨라졌는지"를 원복 전/후 비교할 수 있게 그대로 유지 */
static void refresh_stats_page(lv_timer_t *t)
{
    (void)t;
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t t0 = lv_tick_get();
    bool ok = refresh_stats_overview_panel();
    uint32_t t1 = lv_tick_get();
    if (ok) ok = refresh_stats_table();
    uint32_t t2 = lv_tick_get();
    if (ok) ok = refresh_stats_graph();
    uint32_t t3 = lv_tick_get();
    if (!ok) report_sd_io_fail("stats tab query");
    if ((t3 - t0) > 50) {  /* 50ms 이상 걸린 사이클만 로그(매번 찍으면 스팸) */
        ESP_LOGW(TAG, "MEMDIAG refresh_stats_page timing: overview=%ums table=%ums graph=%ums total=%ums",
                 (unsigned)(t1 - t0), (unsigned)(t2 - t1), (unsigned)(t3 - t2), (unsigned)(t3 - t0));
    }
    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (before != after) {
        ESP_LOGW(TAG, "MEMDIAG refresh_stats_page: internal %u -> %u (delta=%d)",
                 (unsigned)before, (unsigned)after, (int)before - (int)after);
    }
}

static void stats_prev_page_cb(lv_event_t *e)
{
    (void)e;
    uint32_t total = stats_store_get_count();
    uint32_t total_pages = (total + STATS_STORE_PAGE_SIZE - 1) / STATS_STORE_PAGE_SIZE;
    if (total_pages == 0) total_pages = 1;
    if (s_stats_page_index + 1 < total_pages) s_stats_page_index++;
    refresh_stats_table();
}

static void stats_next_page_cb(lv_event_t *e)
{
    (void)e;
    if (s_stats_page_index > 0) s_stats_page_index--;
    refresh_stats_table();
}

/* 2026-09-07(사용자 지시 — "10개씩 이동 단추도 있으면") — 1칸 이동과 동일 원칙, 그냥
 * STATS_JUMP_PAGE_COUNT칸씩 클램프 */
static void stats_jump_prev_page_cb(lv_event_t *e)
{
    (void)e;
    uint32_t total = stats_store_get_count();
    uint32_t total_pages = (total + STATS_STORE_PAGE_SIZE - 1) / STATS_STORE_PAGE_SIZE;
    if (total_pages == 0) total_pages = 1;
    s_stats_page_index += STATS_JUMP_PAGE_COUNT;
    if (s_stats_page_index + 1 > total_pages) s_stats_page_index = total_pages - 1;
    refresh_stats_table();
}

static void stats_jump_next_page_cb(lv_event_t *e)
{
    (void)e;
    s_stats_page_index = (s_stats_page_index > STATS_JUMP_PAGE_COUNT) ? s_stats_page_index - STATS_JUMP_PAGE_COUNT : 0;
    refresh_stats_table();
}

/* 2026-09-07(사용자 지시 — "저장값 지우기 기능도", "지울때 확인 팝업도") — Yes/Cancel
 * 공용 확인팝업(show_confirm_popup) 재사용 */
static void cb_delete_stats_confirmed(void *ctx)
{
    (void)ctx;
    stats_store_delete_all();
    s_stats_page_index = 0;
    refresh_stats_table();
    refresh_stats_overview_panel();
}

static void cb_delete_stats_tap(lv_event_t *e)
{
    (void)e;
    show_confirm_popup(ui_str(STR_CONFIRM_DELETE_STATS), cb_delete_stats_confirmed, NULL);
}

/* 2026-09-07(사용자 설계 — "사용자는 그래프만 보거나 테이블만 보는 형태") — 한 번에 하나만
 * 보임(분할 아님). 스크롤/스냅 애니메이션 대신 단순 HIDDEN 토글 — 정확히 이 요구사항과
 * 일치하고, 제스처 처리도 훨씬 단순해짐 */
static void switch_to_graph_view(void)
{
    lv_obj_add_flag(s_stats_table_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_stats_graph_view, LV_OBJ_FLAG_HIDDEN);
    s_stats_graph_force_refresh = true;  /* 2026-09-11 — 숨겨져있던 동안 갱신 안 됐을 수
                                             있으니 보이자마자 바로 최신으로 */
    refresh_stats_graph();
}

static void switch_to_table_view(void)
{
    lv_obj_add_flag(s_stats_graph_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_stats_table_view, LV_OBJ_FLAG_HIDDEN);
}

static void cb_switch_to_graph_tap(lv_event_t *e) { (void)e; switch_to_graph_view(); }
static void cb_switch_to_table_tap(lv_event_t *e) { (void)e; switch_to_table_view(); }

/* 2026-09-07(사용자 설계) — 테이블: 좌측 스와이프하면 그래프로, 위/아래 스와이프하면
 * 페이지 이동(세로 스와이프로 하단 버튼줄 대체, 사용자 지시: "세로 스와이프로 페이지
 * 넘기기"). 테이블 자체의 CLICKABLE은 그대로 둬야 제스처 인식이 되므로(꺼버리면 눌림
 * 자체가 안 잡힘), 선택 비활성화는 위젯 생성부에서 눌림 상태 스타일을 투명 처리하는
 * 방식으로 별도 처리(cb_stats_table_gesture와는 무관) */
static void cb_stats_table_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) { switch_to_graph_view(); return; }
    if (dir == LV_DIR_TOP)    { stats_next_page_cb(NULL); return; }  /* 위로 스와이프 = 다음(최신) */
    if (dir == LV_DIR_BOTTOM) { stats_prev_page_cb(NULL); return; }  /* 아래로 스와이프 = 이전(과거) */
}

/* 2026-09-11(그래프 재설계 항목4, 사용자 확정 — "테이블 그래프 스와이프 안쓰는데?") — 기존
 * 좌우 스와이프로 테이블<->그래프 전환하던 건 실사용 안 하는 걸로 확인돼서, 좌우는 원래
 * 설계대로 시간 이동에 씀(테이블<->그래프 전환은 "<<"/">>" 버튼으로만). 왼쪽 스와이프=더
 * 과거로(오프셋 증가), 오른쪽 스와이프=더 최근으로(오프셋 감소, 0 미만/미래 없음) */
/* 2026-09-11(사용자 지시 — "기기 반응이 느린 편... 스와이프 인식됨을 알려야") — 힌트를
 * 보여준 프레임이 실제로 화면에 그려질 기회를 주기 위해, 무거운 작업(SD 재조회+다시
 * 그리기)은 이 프레임 안에서 동기 실행하지 않고 lv_async_call로 다음 루프로 미룸
 * (event-driven 패턴, [[feedback_event_driven_not_polling]]) */
static void stats_graph_swipe_async_refresh(void *user_data)
{
    (void)user_data;
    s_stats_graph_force_refresh = true;
    refresh_stats_graph();
    if (s_stats_graph_swipe_hint) lv_obj_add_flag(s_stats_graph_swipe_hint, LV_OBJ_FLAG_HIDDEN);
}

/* older=true: 과거로(offset++, "<<"), older=false: 현재쪽으로(offset--, ">>", 0에서 멈춤) */
static void stats_graph_pan(bool older)
{
    if (older) {
        s_stats_graph_offset++;
    } else {
        if (s_stats_graph_offset == 0) return;
        s_stats_graph_offset--;
    }
    if (s_stats_graph_swipe_hint) {
        lv_label_set_text(s_stats_graph_swipe_hint, older ? "<<" : ">>");
        lv_obj_remove_flag(s_stats_graph_swipe_hint, LV_OBJ_FLAG_HIDDEN);
    }
    lv_async_call(stats_graph_swipe_async_refresh, NULL);
}

static void cb_stats_graph_pan_left_tap(lv_event_t *e)
{
    (void)e;
    stats_graph_pan(true);
}

static void cb_stats_graph_pan_right_tap(lv_event_t *e)
{
    (void)e;
    stats_graph_pan(false);
}

/* 2026-09-04(사용자 지시 — 요약판넬 우측에 신호세기, 숫자보다 막대/흔한 와이파이 표시형태로,
 * 이후 "5개로 해") — 높이가 다른 막대 5개, RSSI 임계값에 따라 왼쪽부터 채움(폰 상태바와
 * 동일한 관례) */
#define SIGNAL_BAR_COUNT 5

static lv_obj_t *create_signal_widget(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 28, 16);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_column(box, 1, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    static const uint8_t bar_h[SIGNAL_BAR_COUNT] = { 4, 7, 10, 13, 16 };
    for (int i = 0; i < SIGNAL_BAR_COUNT; i++) {
        lv_obj_t *bar = lv_obj_create(box);
        lv_obj_set_size(bar, 3, bar_h[i]);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    }
    return box;
}

static void update_signal_widget(lv_obj_t *box, bool has_rssi, int8_t rssi)
{
    int filled;
    if      (!has_rssi)   filled = 0;
    else if (rssi >= -45) filled = 5;
    else if (rssi >= -55) filled = 4;
    else if (rssi >= -65) filled = 3;
    else if (rssi >= -75) filled = 2;
    else if (rssi >= -85) filled = 1;
    else                  filled = 0;

    for (int i = 0; i < SIGNAL_BAR_COUNT; i++) {
        lv_obj_t *bar = lv_obj_get_child(box, i);
        lv_obj_set_style_bg_color(bar, (i < filled) ? lv_palette_main(LV_PALETTE_BLUE)
                                                     : lv_palette_main(LV_PALETTE_GREY), 0);
    }
}

static void refresh_dashboard(lv_timer_t *t)
{
    (void)t;

    /* 2026-09-11(SD 신뢰성 재설계 항목4) — 쓰기경로(esp_now_hub.c recv_cb, LVGL 태스크 아님)가
     * SD I/O 실패를 만났으면 여기(LVGL 태스크, 매 틱)서 test-and-clear로 가져와 회로차단기를
     * 세움 — 읽기실패와 마찬가지로 사용자에게 알리고 재연결/포맷으로 대응하게 함 */
    if (stats_store_take_write_io_error()) {
        mark_sd_io_fail("measurement save (write)");
        refresh_storage_status_label();
    }

    /* 2026-08-21 — 웹 대시보드 URL(사용자 지시). IP는 WiFi 재연결 등으로 바뀔 수 있어서
     * 매 틱 다시 읽음(가벼운 문자열 비교라 비용 무시 가능) — 없으면(빈 문자열) 숨김 */
    const char *ip = esp_now_hub_get_own_ip_str();
    if (ip[0] != '\0') {
        lv_label_set_text_fmt(s_web_url_label, "http://%s:80", ip);
        lv_obj_remove_flag(s_web_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_web_row, LV_OBJ_FLAG_HIDDEN);
    }

    refresh_network_right_zone();  /* 2026-08-29 — 설정탭 네트워크 행의 우측(IP/SSID/찾기) */

    /* 2026-08-21 — 요약 둘째줄, 내부/PSRAM 여유메모리 상시 표시(사용자 지시) */
    char mem_i[16], mem_p[16];
    uint32_t free_internal_now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    format_bytes_human(free_internal_now, mem_i, sizeof(mem_i));
    format_bytes_human((uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), mem_p, sizeof(mem_p));
    /* 2026-09-09(사용자 지시 — "항목 표기에 콜론이 어떤건 붙고 어떤 건 안 붙어있어, 다
     * 붙여줘") — 콜론 앞 공백 제거, 다른 라벨들("%s: ...")과 통일 */
    lv_label_set_text_fmt(s_mem_status_label, "%s: I = %s / P = %s", ui_str(STR_LABEL_MEMORY), mem_i, mem_p);

    /* 2026-09-10(사용자 설계 — "CNTL 메모리 밑에 SD 용량도 표시... 9:1 비율... 90%가 될 때
     * 10%만큼 오래된 걸 지운다") — SD 원격 조회는 매 틱(1초)마다 하기엔 낭비라 5초마다만.
     * Picture는 캠 사진저장 자체가 아직 미구현(미정)이라 항상 0 사용(예산은 그대로 계산돼
     * 표시됨) */
    static int s_storage_check_tick = 0;
    if (++s_storage_check_tick >= 5) {
        s_storage_check_tick = 0;
        refresh_storage_status_label();  /* 2026-09-10 재설계 — 실제 계산/표시 로직은
            SD 상태/복구 섹션의 refresh_storage_status_label()로 이동(탭 팝업에서 재연결/
            포맷 직후에도 즉시 재사용해야 해서 공용 함수로 뺌, [[project_cntl_sd_reliability_redesign_2026_09_10]]) */
    }
    /* 2026-09-07(임시 진단 — 내부RAM 서서히 감소 원인 추적) — 10초마다(이 틱이 1초 주기라
     * 10번째마다) 전체 추이를 로그로 남김. stats_store_append() 안쪽 진단과 대조용 */
    static int s_mem_log_tick = 0;
    if (++s_mem_log_tick >= 10) {
        s_mem_log_tick = 0;
        ESP_LOGW(TAG, "MEMDIAG periodic internal free=%u", (unsigned)free_internal_now);
    }

    if (!s_dash_nodes || !s_dash_nodes_prev) return;  /* PSRAM 할당 실패 시(극히 드묾) */

    /* 판넬1: 요약 — 페어링된(연결된) 장치 전부(CAM+SENS), 정상 표시 */
    int total = esp_now_hub_get_nodes(HUB_NODE_KIND_UNKNOWN, s_dash_nodes, ESP_NOW_HUB_MAX_NODES);

    bool dash_changed = (total != s_dash_count_prev);
    for (int i = 0; !dash_changed && i < total; i++) {
        if (!node_display_equal(&s_dash_nodes[i], &s_dash_nodes_prev[i])) dash_changed = true;
        /* 2026-09-04 버그수정 — node_display_equal()은 mac/kind/ever_paired/name만 봐서
         * conn_state(끊기 시 PAIRED->ORPHAN)는 변화로 안 잡힘. 그래서 끊기 직후에도 이
         * 노드가 esp_now_hub_get_nodes()의 last_seen_ms 타임아웃(응답성*6)에 걸려 목록에서
         * 빠지기 전까지 요약판넬 행이 안 지워지고 남아있었음(실기 확인: 끊기 후에도
         * 한동안 "연결중"으로 보임) — conn_state 변화도 직접 비교해서 즉시 재생성 트리거 */
        else if (s_dash_nodes[i].conn_state != s_dash_nodes_prev[i].conn_state) dash_changed = true;
    }
    /* 2026-09-08(연결 기능 주화면 이관) — Summary의 장치별 행(구 s_summary_list)은 없앰,
     * Sensor/Camera 판넬로 이관. dash_changed/s_dash_nodes_prev/s_dash_count_prev 자체는
     * 아래 Sensor/Camera 대시 목록 재생성 판단에 계속 씀 */
    if (dash_changed) {
        memcpy(s_dash_nodes_prev, s_dash_nodes, sizeof(esp_now_hub_node_t) * total);
        s_dash_count_prev = total;
    }

    /* 판넬2: 측정기 — 2026-09-08(연결 기능 주화면 이관, 사용자 설계) — 값 표시(구
     * append_sensor_value_row/s_sensor_todo)는 없애고 Summary의 실시간 순시치 블록이
     * 대신 담당(아래 refresh_summary_live_values 참고). 여기는 카메라 판넬과 완전히 같은
     * "연결됨" 장치행 목록만 */
    esp_now_hub_node_t sens_nodes[ESP_NOW_HUB_MAX_NODES];
    uint8_t             sens_macs[ESP_NOW_HUB_MAX_NODES][6];
    int sens_count = 0;
    for (int i = 0; i < total; i++) {
        if (s_dash_nodes[i].kind != HUB_NODE_KIND_SENS) continue;
        if (esp_now_hub_get_conn_state(s_dash_nodes[i].mac) == HUB_CONN_STATE_WAITING) continue;
        sens_nodes[sens_count] = s_dash_nodes[i];
        memcpy(sens_macs[sens_count], s_dash_nodes[i].mac, 6);
        sens_count++;
    }

    if (dash_changed) {
        lv_indev_reset(NULL, s_sensor_dash_list);
        lv_obj_clean(s_sensor_dash_list);
        for (int i = 0; i < sens_count && i < ESP_NOW_HUB_MAX_NODES; i++) {
            lv_obj_t *row = lv_obj_create(s_sensor_dash_list);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            /* 2026-09-09(사용자 발견 — PRESSED 하이라이트를 넣고 보니 "탭 가능 영역이 딱
             * 글씨 높이만큼이었어") — 행 사이 간격(pad_row, 리스트 쪽)은 그대로 두고 행
             * 자신의 상하 패딩만 최대한 키워서 탭 영역을 넓힘("최대한 넓혀") */
            lv_obj_set_style_pad_ver(row, 16, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            /* 2026-09-09(사용자 재설계 — "전계강도를 가장 왼쪽으로... 전계강도+공백1칸+
             * 장치명...+> 표시는 우측 정렬") — SPACE_BETWEEN을 버리고 순서(signal->label->
             * chevron)+label의 flex_grow(1)로 배치: signal-label 사이는 pad_column의 좁은
             * 고정 간격, label이 남는 폭을 다 먹어서 chevron이 자동으로 행 오른쪽 끝에 붙음 */
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 8, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(row, (void *)(uintptr_t)i);
            lv_obj_add_event_cb(row, cb_sensor_dash_row_clicked, LV_EVENT_CLICKED, NULL);
            /* 2026-09-09(사용자 설계 — "탭 가능한건지, 눌리긴 했는지 확인할 방법") — 눌리는
             * 순간 배경 하이라이트(별도 이벤트 코드 없이 LVGL PRESSED 상태 스타일만) */
            lv_obj_set_style_bg_color(row, lv_palette_main(LV_PALETTE_GREY), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(row, LV_OPA_30, LV_STATE_PRESSED);

            lv_obj_t *signal = create_signal_widget(row);

            lv_obj_t *label = lv_label_create(row);
            lv_obj_set_flex_grow(label, 1);
            lv_obj_set_style_text_font(label, ui_font_get(UI_FONT_SIZE_18), 0);

            lv_obj_t *chevron = lv_label_create(row);
            lv_label_set_text(chevron, ">");
            lv_obj_set_style_text_font(chevron, ui_font_get(UI_FONT_SIZE_18), 0);
            lv_obj_add_style(chevron, &style_text_muted, 0);

            s_sensor_dash_row_objs[i] = label;
            s_sensor_dash_row_signal[i] = signal;
            memcpy(s_sensor_dash_row_macs[i], sens_macs[i], 6);
            /* 2026-09-09(사용자 설계 — "장치명은 접속해온 장치가 제시하는 이름... 코드
             * 내에서는 항상 장치명을 ID로 사용... Alias는 표기용... 지정돼 있으면 Alias를
             * 쓰고 지정 안 돼있으면 장치명을 씀") — name은 절대 안 건드리고 항상 진짜
             * 장치명, alias는 표시용으로만 쓰는 별도 필드(빈 문자열=미지정) */
            strncpy(s_sensor_dash_row_names[i], sens_nodes[i].name, ESP_NOW_LINK_NAME_LEN - 1);
            s_sensor_dash_row_names[i][ESP_NOW_LINK_NAME_LEN - 1] = '\0';
            strncpy(s_sensor_dash_row_alias[i], device_config_get_alias(sens_macs[i]),
                    sizeof(s_sensor_dash_row_alias[i]) - 1);
            s_sensor_dash_row_alias[i][sizeof(s_sensor_dash_row_alias[i]) - 1] = '\0';
            s_sensor_dash_row_last_text[i][0] = '\0';
        }
        s_sensor_dash_row_count = (sens_count < ESP_NOW_HUB_MAX_NODES) ? sens_count : ESP_NOW_HUB_MAX_NODES;
    }
    for (int i = 0; i < s_sensor_dash_row_count; i++) {
        hub_conn_state_t st = esp_now_hub_get_conn_state(s_sensor_dash_row_macs[i]);
        char buf[96];
        /* 표시용으로만 여기서 alias-or-name 선택(지역 변수) — name 필드 자체는 절대 안 바뀜 */
        const char *display_name = (s_sensor_dash_row_alias[i][0] != '\0')
                                    ? s_sensor_dash_row_alias[i] : s_sensor_dash_row_names[i];
        int n = snprintf(buf, sizeof(buf), "%s (%s)", display_name,
                 ui_str(st == HUB_CONN_STATE_ACTIVE ? STR_STATUS_ACTIVE : STR_STATUS_PAIRED));
        /* 개별설정값(측정주기) — 사용자 설계: "연결된 목록에는 측정 값이 아니라, 개별
         * 설정된 값이 보여야되". 2026-09-09(사용자 지적 — "그냥 10s로 나오고... Measure 10S
         * 형식이 좋고, 항목간 대시(-)보다 (/)가 좋아") — 라벨 접두 추가, 구분자 " / "로 통일 */
        uint32_t interval_sec = device_config_get_sens_sample_interval_sec(s_sensor_dash_row_macs[i]);
        if (interval_sec > 0 && n > 0 && (size_t)n < sizeof(buf)) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " / %s %us",
                          ui_str(STR_LABEL_MEASURE_SHORT), (unsigned)interval_sec);
        }
        for (int j = 0; j < sens_count; j++) {
            if (memcmp(sens_macs[j], s_sensor_dash_row_macs[i], 6) != 0) continue;
            if (sens_nodes[j].has_deepsleep_stats && n > 0 && (size_t)n < sizeof(buf)) {
                char batt[32];
                format_battery_display(batt, sizeof(batt), sens_nodes[j].battery_mv, sens_nodes[j].battery_pct);
                snprintf(buf + n, sizeof(buf) - (size_t)n, " / %s", batt);
            }
            update_signal_widget(s_sensor_dash_row_signal[i], sens_nodes[j].has_rssi, sens_nodes[j].rssi);
            break;
        }
        if (strcmp(s_sensor_dash_row_last_text[i], buf) != 0) {
            lv_label_set_text(s_sensor_dash_row_objs[i], buf);
            strncpy(s_sensor_dash_row_last_text[i], buf, sizeof(s_sensor_dash_row_last_text[i]) - 1);
            s_sensor_dash_row_last_text[i][sizeof(s_sensor_dash_row_last_text[i]) - 1] = '\0';
        }
    }

    bool sensor_connected = (sens_count > 0);
    if (sensor_connected) {
        lv_obj_add_flag(s_sensor_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_sensor_dash_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* 2026-09-09(사용자 지적 — "No xxx device는... 대기 중인 것도 없고 연결된 것도
         * 없을 때만 이게 보여") — 대기중 목록(s_sensor_row_count, refresh_sensor_list의
         * 자체 1초 타이머가 관리하는 전역 변수)까지 같이 봐서 정말 아무 것도 없을 때만
         * "없음" 문구를 보여줌 */
        if (s_sensor_row_count == 0) lv_obj_remove_flag(s_sensor_empty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_sensor_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sensor_dash_list, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_summary_live_values(s_dash_nodes, total);  /* 2026-09-08 — Summary 실시간 순시치 블록 */

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

    /* 2026-09-08(카메라 팝업 추출) — 주화면에 남는 "연결된 카메라" 목록, 요약판넬(s_summary_list)과
     * 완전히 같은 2단계 패턴: 구조 재생성은 dash_changed일 때만(위 요약판넬과 같은 조건 재사용),
     * 문구/신호세기는 매 틱 갱신하되 실제로 바뀔 때만 lv_label_set_text 호출 */
    if (dash_changed) {
        lv_indev_reset(NULL, s_camera_dash_list);
        lv_obj_clean(s_camera_dash_list);
        for (int i = 0; i < cam_count && i < ESP_NOW_HUB_MAX_NODES; i++) {
            lv_obj_t *row = lv_obj_create(s_camera_dash_list);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            /* 2026-09-09(사용자 발견 — "탭 가능 영역이 딱 글씨 높이만큼이었어") — 센서
             * 목록과 동일 이유로 행 자신의 상하 패딩만 최대한 키움 */
            lv_obj_set_style_pad_ver(row, 16, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            /* 2026-09-09(사용자 재설계 — 센서 목록과 동일 원칙, "전계강도를 가장 왼쪽으로") */
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 8, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            /* 2026-09-08(연결 기능 주화면 이관) — 탭하면 개별설정 팝업(Alias/연결끊기) */
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(row, (void *)(uintptr_t)i);
            lv_obj_add_event_cb(row, cb_camera_dash_row_clicked, LV_EVENT_CLICKED, NULL);
            /* 2026-09-09(사용자 설계 — "탭 가능한건지, 눌리긴 했는지 확인할 방법") */
            lv_obj_set_style_bg_color(row, lv_palette_main(LV_PALETTE_GREY), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(row, LV_OPA_30, LV_STATE_PRESSED);

            lv_obj_t *signal = create_signal_widget(row);

            lv_obj_t *label = lv_label_create(row);
            lv_obj_set_flex_grow(label, 1);
            lv_obj_set_style_text_font(label, ui_font_get(UI_FONT_SIZE_18), 0);

            lv_obj_t *chevron = lv_label_create(row);
            lv_label_set_text(chevron, ">");
            lv_obj_set_style_text_font(chevron, ui_font_get(UI_FONT_SIZE_18), 0);
            lv_obj_add_style(chevron, &style_text_muted, 0);

            s_camera_dash_row_objs[i] = label;
            s_camera_dash_row_signal[i] = signal;
            memcpy(s_camera_dash_row_macs[i], cam_macs[i], 6);
            /* 2026-09-09(사용자 설계 — 센서 목록과 동일 원칙) — name은 항상 진짜 장치명(ID),
             * alias는 표시용 별도 필드 */
            strncpy(s_camera_dash_row_names[i], cam_nodes[i].name, ESP_NOW_LINK_NAME_LEN - 1);
            s_camera_dash_row_names[i][ESP_NOW_LINK_NAME_LEN - 1] = '\0';
            strncpy(s_camera_dash_row_alias[i], device_config_get_alias(cam_macs[i]),
                    sizeof(s_camera_dash_row_alias[i]) - 1);
            s_camera_dash_row_alias[i][sizeof(s_camera_dash_row_alias[i]) - 1] = '\0';
            s_camera_dash_row_last_text[i][0] = '\0';
        }
        s_camera_dash_row_count = (cam_count < ESP_NOW_HUB_MAX_NODES) ? cam_count : ESP_NOW_HUB_MAX_NODES;
    }
    for (int i = 0; i < s_camera_dash_row_count; i++) {
        hub_conn_state_t st = esp_now_hub_get_conn_state(s_camera_dash_row_macs[i]);
        char buf[96];
        const char *display_name = (s_camera_dash_row_alias[i][0] != '\0')
                                    ? s_camera_dash_row_alias[i] : s_camera_dash_row_names[i];
        int n = snprintf(buf, sizeof(buf), "%s (%s)", display_name,
                 ui_str(st == HUB_CONN_STATE_ACTIVE ? STR_STATUS_ACTIVE : STR_STATUS_PAIRED));
        for (int j = 0; j < cam_count; j++) {
            if (memcmp(cam_macs[j], s_camera_dash_row_macs[i], 6) != 0) continue;
            /* 2026-09-08(사용자 설계 — "센서, 캠 연결을 주화면에서 하면... Summary에 있던
             * 배터리, 전계강도를 각 sensor, camera panel로 옮기고") */
            if (cam_nodes[j].has_deepsleep_stats && n > 0 && (size_t)n < sizeof(buf)) {
                char batt[32];
                format_battery_display(batt, sizeof(batt), cam_nodes[j].battery_mv, cam_nodes[j].battery_pct);
                snprintf(buf + n, sizeof(buf) - (size_t)n, " / %s", batt);
            }
            update_signal_widget(s_camera_dash_row_signal[i], cam_nodes[j].has_rssi, cam_nodes[j].rssi);
            break;
        }
        if (strcmp(s_camera_dash_row_last_text[i], buf) != 0) {
            lv_label_set_text(s_camera_dash_row_objs[i], buf);
            strncpy(s_camera_dash_row_last_text[i], buf, sizeof(s_camera_dash_row_last_text[i]) - 1);
            s_camera_dash_row_last_text[i][sizeof(s_camera_dash_row_last_text[i]) - 1] = '\0';
        }
    }

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
        lv_obj_remove_flag(s_camera_dash_list, LV_OBJ_FLAG_HIDDEN);
        /* 2026-09-08(카메라 팝업 추출) — toolbar/split_row는 팝업이 열려있을 때만 보여야 함
         * (지금 부모가 주화면 camera_box인지 s_camera_popup인지로 판단) — 팝업이 열려있으면
         * build_camera_tab()이 이미 hidden을 풀어뒀고, 닫혀있으면 팝업 쪽 표시는 의미가
         * 없으므로 여기서는 부모가 팝업일 때만 hidden을 갱신(주화면에 있을 땐 무조건 숨김) */
        if (s_camera_popup) {
            lv_obj_remove_flag(s_camera_content, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_camera_split_row, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        /* 2026-09-09(사용자 지적) — 센서 판넬과 동일 원칙: 대기중(s_camera_row_count)까지
         * 없을 때만 "없음" 표시 */
        if (s_camera_row_count == 0) lv_obj_remove_flag(s_camera_empty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_camera_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_camera_dash_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_camera_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_camera_split_row, LV_OBJ_FLAG_HIDDEN);
    }
    /* 2026-09-08(사용자 지시 — "한 개도 없으면 역상 속 글씨가 밝은 회색으로 안눌린다는
     * 표현") — 상태 바뀔 때만 갱신(매틱 재적용 방지) */
    if (camera_connected != s_camera_title_enabled_prev) {
        s_camera_title_enabled_prev = camera_connected;
        lv_obj_set_style_text_color(s_dash_title[2],
            camera_connected ? lv_color_white() : lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    }

    /* 2026-09-04 — 사진/목록 수신 완료 반응은 매틱 폴링 대신 이벤트(on_photo_result_event/
     * on_list_result_event, esp_now_photo_set_ready_cb 등록)로 옮김. 지금촬영/모두지우기
     * 팝업 쪽 목록 완료 처리는 각자의 진행 팝업 tick이 별도로 계속 담당(그동안 이 배경
     * 타이머 자체가 pause_bg_timers()로 멈춰있어서 이벤트와 안 겹침) */
}

/* 통계 탭 로그박스 갱신 — ui_log 모듈에 쌓인 스냅샷을 그대로 라벨에 채우고 항상 맨
 * 아래(최신)로 스크롤. 내용이 안 바뀌었으면 다시 안 그림(길이만 비교 — 완벽하진 않지만
 * 이 용도로는 충분) */
/* 통계 탭 로그 폭 초과 줄 처리 — LVGL의 LONG_DOT(라벨 전체 기준)이나 팝업 없이, 우리가
 * 직접 폰트 기준 픽셀폭을 재서 넘치면 끝을 잘라내고 "..."을 붙임(2026-08-22, 사용자 지시).
 * 로그가 전부 영문(ASCII)이라 UTF-8 디코딩 없이 바이트 단위로 처리 가능 — line[]은 이 파일
 * 전역에서 최대 160바이트라 cum[] 크기를 거기 맞춤 */
static void trim_to_width(char *text, const lv_font_t *font, int32_t max_width)
{
    size_t len = strlen(text);
    if (len == 0) return;

    int32_t cum[160];
    size_t n = (len < sizeof(cum) / sizeof(cum[0])) ? len : sizeof(cum) / sizeof(cum[0]) - 1;
    int32_t w = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t next = (i + 1 < n) ? (uint32_t)(unsigned char)text[i + 1] : '\0';
        w += lv_font_get_glyph_width(font, (uint32_t)(unsigned char)text[i], next);
        cum[i] = w;
    }
    if (n == len && w <= max_width) return;  /* 다 들어감 */

    int32_t dots_w = 3 * lv_font_get_glyph_width(font, '.', '.');
    size_t cut = n;
    while (cut > 0 && cum[cut - 1] + dots_w > max_width) cut--;
    text[cut] = '\0';
    strcat(text, "...");
}

/* src(줄바꿈으로 구분된 여러 줄)를 한 줄씩 trim_to_width에 넣어 dst에 재조립 — in-place로
 * 하면 줄이 짧아질 때 뒷부분을 밀어야 해서 오히려 복잡해지므로 별도 버퍼에 씀(원본
 * 누적버퍼는 안 건드림, 다음 tick에도 그대로 재사용 가능) */
static void trim_multiline_to_width(const char *src, char *dst, size_t dst_cap,
                                     const lv_font_t *font, int32_t max_width)
{
    size_t out_len = 0;
    const char *p = src;
    char line[160];
    while (*p && out_len + 1 < dst_cap) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        size_t copy_len = (line_len < sizeof(line) - 1) ? line_len : sizeof(line) - 1;
        memcpy(line, p, copy_len);
        line[copy_len] = '\0';

        trim_to_width(line, font, max_width);

        size_t line_out_len = strlen(line);
        size_t room = dst_cap - out_len - 1;
        if (line_out_len > room) line_out_len = room;
        memcpy(dst + out_len, line, line_out_len);
        out_len += line_out_len;
        if (out_len + 1 < dst_cap) dst[out_len++] = '\n';

        p = nl ? nl + 1 : p + strlen(p);
    }
    if (out_len > 0 && dst[out_len - 1] == '\n') out_len--;
    dst[out_len] = '\0';
}

#define LOG_BOX_SNAPSHOT_CAP 3072
static void refresh_log_box(lv_timer_t *t)
{
    (void)t;
    /* 2026-08-21 — 내부(비-PSRAM) DRAM이 httpd_start 실패(5005)를 겪을 만큼 빠듯했던 걸
     * 실기로 확인(free internal=1111~1419B) — 텍스트 표시용이라 빠른 접근이 필수가 아닌
     * 이 버퍼를 PSRAM으로 옮김(폰트버퍼 font_buf_malloc과 동일 원칙). 부팅 시 한 번만
     * 할당하고 계속 재사용(다른 고정버퍼들과 동일 원칙) */
    static char *snapshot = NULL;
    static char *trimmed = NULL;
    static size_t last_len = 0;
    if (!snapshot) {
        snapshot = heap_caps_malloc(LOG_BOX_SNAPSHOT_CAP, MALLOC_CAP_SPIRAM);
        trimmed  = heap_caps_malloc(LOG_BOX_SNAPSHOT_CAP, MALLOC_CAP_SPIRAM);
        if (!snapshot || !trimmed) return;
    }

    ui_log_get_snapshot(snapshot, LOG_BOX_SNAPSHOT_CAP);
    size_t len = strlen(snapshot);
    if (len == last_len) return;
    last_len = len;

    int32_t max_w = lv_obj_get_content_width(s_log_container);
    trim_multiline_to_width(snapshot, trimmed, LOG_BOX_SNAPSHOT_CAP, &lv_font_montserrat_18, max_w);

    lv_label_set_text(s_log_label, trimmed);
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
    if (!s_power_log_buf) return;  /* PSRAM 할당 실패 시(극히 드묾) */
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
                    break;
                }
            }
        }
        if (!tr) continue;  /* 자리 없음 — MAX_NODES 이상은 원래 못 옴 */

        /* 2026-08-25(CASK 재설계) — 예전엔 여기서 SLEEP_NOW 발신 시점을 별도 줄로 찍었는데
         * (sleep_now_send_count 기반), CNTL이 능동적으로 "언제 보낼지" 미리 판단해서 먼저
         * 쏘던 구조 자체가 없어짐 — SLEEP_NOW는 이제 매 WAKE_HELLO의 CASK 마지막 단계로
         * 결정적으로(항상) 나가므로, "보냈다"는 사실 자체가 더 이상 별도로 기록할 만한
         * 이벤트가 아님(항상 일어나는 일이라서) */

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
        /* 2026-08-10 — 이 줄이 실제로 몇 시(mm:ss, Cntl 부팅 후 경과) 찍혔는지 붙여서, 줄 사이
         * 실제 간격을 육안으로 바로 잴 수 있게 함(사용자 지시 — "20초마다 뜬다" 같은 관찰을
         * 스톱워치 없이 확인하기 위함). 2026-08-11 — 줄 끝(-mm:ss)에서 줄 맨 앞([mm:ss] )으로
         * 이동(사용자 지시) */
        char line[176];  /* 2026-08-22 — 배터리 진단정보 추가로 여유 늘림(원래 144) */
        int prefix_len = 0;
        ui_log_format_timestamp(line, sizeof(line));
        prefix_len = (int)strlen(line);
        lv_snprintf(line + prefix_len, sizeof(line) - prefix_len, ui_str(STR_DEEPSLEEP_LINE_FMT), nodes[i].name,
                    (unsigned long)nodes[i].ds_cycle_count, wake_str,
                    (unsigned long)nodes[i].ds_last_awake_uptime_ms,
                    (unsigned long)nodes[i].ds_last_sleep_interval_sec,
                    (unsigned long)nodes[i].ds_last_actual_sleep_sec,
                    (unsigned long)nodes[i].ds_rwdt_catch_count,
                    (unsigned)nodes[i].battery_mv, (unsigned)nodes[i].battery_pct,
                    (unsigned)nodes[i].battery_adc_raw);

        size_t cur_len  = strlen(s_power_log_buf);
        size_t line_len = strlen(line);
        if (cur_len + line_len + 2 > POWER_LOG_BUF_CAP) {
            size_t keep_from = POWER_LOG_BUF_CAP / 2;
            memmove(s_power_log_buf, s_power_log_buf + keep_from, cur_len - keep_from + 1);
            cur_len = strlen(s_power_log_buf);
        }
        strcat(s_power_log_buf, line);
        strcat(s_power_log_buf, "\n");
        appended = true;
    }

    if (appended) {
        static char *trimmed = NULL;
        if (!trimmed) trimmed = heap_caps_malloc(POWER_LOG_BUF_CAP, MALLOC_CAP_SPIRAM);
        if (trimmed) {
            int32_t max_w = lv_obj_get_content_width(s_power_list);
            trim_multiline_to_width(s_power_log_buf, trimmed, POWER_LOG_BUF_CAP, &lv_font_montserrat_18, max_w);
            lv_label_set_text(s_power_log_label, trimmed);
        } else {
            lv_label_set_text(s_power_log_label, s_power_log_buf);
        }
        lv_obj_scroll_to_y(s_power_list, LV_COORD_MAX, LV_ANIM_OFF);
    } else if (count == 0 && s_power_log_buf[0] == '\0') {
        /* 2026-09-07 버그수정(사용자 지적) — 이 라벨(s_power_log_label)은 성능 때문에
         * 나눔고딕 TTF가 아니라 lv_font_montserrat_18(한글 글리프 없음)을 씀 — 로그 자체가
         * 원래 항상 영문(feedback_cntl_stats_tab_log_perf 참고)인데 여기만 ui_str()로
         * 한국어 문구를 시도해서 한글모드에서 네모박스로 깨졌었음. 이 판넬 한정으로는
         * 언어 무관 항상 영문 고정이 맞음 */
        lv_label_set_text(s_power_log_label, "No camera device");
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

/* 2026-09-07(사용자 지시 — "레이블-값/드랍다운-버튼 형태를 레이블-공백-우정렬 값/드랍다운
 * 버튼 형식으로 통일") — SPACE_BETWEEN에 자식 3개(라벨/드롭다운/버튼)를 그대로 두면
 * 드롭다운이 가운데 애매한 자리에 뜸(개괄 판넬 헤더에서 먼저 발견된 문제와 동일). 드롭다운
 * (or 값 라벨)+버튼을 하나의 우측 묶음으로 만들어 라벨-공백-묶음 2분할이 되게 함 */
/* 2026-09-08(사용자 지시 — "지금 단추가 다 라운드스퀘어잖아... 직사각형 짙은 회색배경에
 * 흰글씨였어") — 다른 버튼(둥근모서리)과 구분되는 "역상" 눌림 표시. 시간/네트워크/센서·
 * 카메라판넬 제목 전부 이 스타일 공유 — CLICKABLE+event_cb는 호출부가 각자 따로 붙임 */
static void style_inverted_control(lv_obj_t *label)
{
    lv_obj_set_style_bg_color(label, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_radius(label, 0, 0);
    lv_obj_set_style_pad_hor(label, 8, 0);
    lv_obj_set_style_pad_ver(label, 4, 0);
}

static lv_obj_t *create_row_right_cluster(lv_obj_t *row)
{
    lv_obj_t *cluster = lv_obj_create(row);
    lv_obj_set_size(cluster, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cluster, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cluster, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cluster, 0, 0);
    lv_obj_set_style_pad_column(cluster, 8, 0);
    lv_obj_set_style_border_width(cluster, 0, 0);
    return cluster;
}

/* 2026-09-08(재설계 — 상단바 시각/네트워크 컨트롤 분리) — 예전엔 s_clock_label 하나에
 * "HH:MM:SS - CH3"처럼 합쳐서 찍었는데, 이제 시각 컨트롤과 네트워크 컨트롤이 각자 탭
 * 가능한 별도 위젯이라 텍스트도 분리 */
static void refresh_clock(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char time_buf[12];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm_buf);
    if (s_time_ctrl_label) lv_label_set_text(s_time_ctrl_label, time_buf);

    if (s_network_ctrl_label) {
        char net_buf[48];
        bool ap_mode = device_config_get_wifi_ap_mode();
        /* 2026-08-30(사용자 지시) — STA 모드에서 부팅 후 25초간 저장된 AP를 한 번도 못
         * 찾았으면, 계속 재시도 중임을 위장하지 말고 "AP 없음"을 명시. "찾기"로 수동
         * 연결하면 esp_now_hub_sta_boot_giveup()이 자동으로 false가 되어 원래 표시로 복귀 */
        if (!ap_mode && esp_now_hub_sta_boot_giveup()) {
            snprintf(net_buf, sizeof(net_buf), "STA - %s", ui_str(STR_STATUS_NO_AP));
        } else if (ap_mode) {
            /* 2026-09-08(사용자 지시 — "AP라면 SSID가 뭔지도 표기") */
            snprintf(net_buf, sizeof(net_buf), "AP - %s CH%u", esp_now_hub_get_ap_ssid(),
                     (unsigned)esp_now_hub_get_wifi_channel());
        } else {
            /* 2026-09-08(사용자 지시 — "자리가 충분하면 SSID도") — 채널도 계속 같이 표기
             * (2026-08-02 지시: 공유기 자동채널선택 변경을 알아채기 위함, 계속 유효) */
            snprintf(net_buf, sizeof(net_buf), "STA - %s CH%u", esp_now_hub_get_active_sta_ssid(),
                     (unsigned)esp_now_hub_get_wifi_channel());
        }
        lv_label_set_text(s_network_ctrl_label, net_buf);
    }

    /* 설정탭 시각설정 행의 현재값 표시 — 설정 팝업이 안 열려있으면 NULL */
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
static lv_obj_t *s_settime_popup = NULL;

/* 2026-09-08(사용자 지시 — "시간설정 팝업... 이것도 전화면으로") — create_modal() 대신
 * 전체화면 셸이라 부모 체인 워크(cb_modal_close)를 못 씀, 직접 삭제 */
static void cb_close_settime_popup(lv_event_t *e)
{
    (void)e;
    lv_obj_delete(s_settime_popup);
    s_settime_popup = NULL;
}

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
        ui_log_add_err(UI_ERR_RTC_SET_FAILED, "RTC time set failed: %s", esp_err_to_name(err));
    }
    refresh_clock(NULL);  /* 로고부제 + 이 행의 표시값을 새 시각으로 즉시 갱신(다음 1초 tick까지 안 기다림) */
    cb_close_settime_popup(e);
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
    /* 2026-09-07(사용자 지시 — "위아래 패딩을 반으로") — 기본테마 pad_small(14px 상하좌우)
     * 중 상하만 절반(7px)으로, 좌우는 그대로 */
    lv_obj_set_style_pad_ver(dd, 7, 0);
    return dd;
}

static void show_settime_popup(void)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    int cur_year = tm_buf.tm_year + 1900;

    if (s_settime_popup) return;  /* 이미 열려있음 */

    settime_popup_state_t *st = &s_settime_state;
    st->year_base = cur_year - SETTIME_YEAR_SPAN_BEFORE;
    int year_count = SETTIME_YEAR_SPAN_BEFORE + SETTIME_YEAR_SPAN_AFTER + 1;

    s_settime_popup = create_page_popup();
    add_page_popup_header(s_settime_popup, ui_str(STR_TITLE_SET_TIME), cb_close_settime_popup, NULL);

    /* 콘텐츠 영역 — 화면 중앙에 픽커+확인버튼 */
    lv_obj_t *content = lv_obj_create(s_settime_popup);
    lv_obj_set_size(content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, 20, 0);
    lv_obj_set_style_border_width(content, 0, 0);

    lv_obj_t *picker_row = lv_obj_create(content);
    lv_obj_set_size(picker_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(picker_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(picker_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(picker_row, 0, 0);
    lv_obj_set_style_pad_column(picker_row, 4, 0);

    /* 2026-09-08(사용자 지적 — "숫자와 열림 기호가 겹쳐있고") — 비트맵 폰트(Montserrat)
     * 숫자 폭이 TTF 때보다 넓어서 고정폭이 부족해 화살표와 겹침, 여유 있게 넓힘 */
    st->year_dd  = add_settime_dropdown(picker_row, st->year_base, year_count, "%04d",
                                         cur_year - st->year_base, 90);
    st->month_dd = add_settime_dropdown(picker_row, 1, 12, "%02d", tm_buf.tm_mon, 74);
    st->day_dd   = add_settime_dropdown(picker_row, 1, 31, "%02d", tm_buf.tm_mday - 1, 74);

    lv_obj_t *sep = lv_label_create(picker_row);
    lv_label_set_text(sep, " ");

    st->hour_dd  = add_settime_dropdown(picker_row, 0, 24, "%02d", tm_buf.tm_hour, 74);
    st->min_dd   = add_settime_dropdown(picker_row, 0, 60, "%02d", tm_buf.tm_min, 74);

    /* 확인 버튼 하나만 — 취소는 헤더의 닫기(X)가 대신함(통계/설정 팝업과 동일 패턴) */
    lv_obj_t *confirm_btn = lv_button_create(content);
    lv_obj_add_event_cb(confirm_btn, cb_settime_confirm, LV_EVENT_CLICKED, NULL);
    lv_obj_t *confirm_lbl = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_lbl, ui_str(STR_BTN_CONFIRM));
    lv_obj_set_style_text_font(confirm_lbl, ui_font_get(UI_FONT_SIZE_18), 0);
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

/* ════════════════════════════════════════════════════════════
 * 네트워크(WiFi) 설정 — 독립(AP)/종속(STA) 전환 + "찾기"(STA SSID 스캔/선택/비밀번호입력)
 * (2026-08-29) 모드 전환도, WiFi 연결정보 변경도 둘 다 부팅 시 한 번만 적용되는 구조
 * (esp_now_hub.c의 wifi_bringup 참고)라, 저장 후엔 항상 재시작 확인 팝업(show_confirm_popup
 * 재사용)으로 마무리 — 살아있는 상태에서 esp_wifi 모드를 핫스왑하는 위험/복잡도를 피함
 * ════════════════════════════════════════════════════════════ */
static void cb_network_mode_restart_confirmed(void *ctx)
{
    bool new_ap_mode = (bool)(uintptr_t)ctx;
    device_config_set_wifi_ap_mode(new_ap_mode);
    esp_restart();
}

/* 2026-08-29 버그수정(사용자 리포트) — 취소를 누르면 드롭다운 표시를 실제 저장된 값으로
 * 되돌려야 함. 공용 show_confirm_popup()은 취소 버튼에 cb_modal_close만 고정으로 붙어있어서
 * (다른 여러 확인팝업이 같이 쓰는 함수라 시그니처를 못 바꿈) 이 행만 전용 팝업을 따로 둠 */
static void cb_network_mode_cancel(lv_event_t *e)
{
    lv_dropdown_set_selected(s_network_mode_dd, device_config_get_wifi_ap_mode() ? 0 : 1);
    cb_modal_close(e);
}

static void show_network_mode_confirm_popup(bool new_ap_mode)
{
    lv_obj_t *box = create_modal();

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, ui_str(STR_MSG_NETWORK_MODE_RESTART_CONFIRM));
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(msg, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *btn_row = create_modal_btn_row(box);
    add_modal_button(btn_row, STR_BTN_YES, cb_confirm_yes_trampoline, &s_confirm_state);
    add_modal_button(btn_row, STR_BTN_CANCEL, cb_network_mode_cancel, NULL);

    s_confirm_state.fn  = cb_network_mode_restart_confirmed;
    s_confirm_state.ctx = (void *)(uintptr_t)new_ap_mode;
}

static void cb_network_mode_changed(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    bool new_ap_mode = (sel == 0);  /* 0=독립(AP), 1=종속(STA) — 드롭다운 옵션 순서와 일치 */
    bool cur_ap_mode = device_config_get_wifi_ap_mode();
    ui_log_add("net_mode_dd changed: sel=%u new_ap=%d cur_ap=%d", (unsigned)sel, (int)new_ap_mode, (int)cur_ap_mode);
    if (new_ap_mode == cur_ap_mode) return;  /* 실제로 바뀐 게 없으면 팝업 안 띄움 */
    show_network_mode_confirm_popup(new_ap_mode);
}

/* 2026-08-29 재설계(사용자 지시) — 스캔목록 팝업과 비밀번호입력 팝업을 하나의 박스 안에서
 * 내용만 바꾸던 방식에서, 목록 위에 비밀번호 팝업이 "추가로" 스택되는 2단 팝업 구조로 변경 */
static lv_obj_t *s_wifi_status_lbl  = NULL;  /* 스캔 팝업 상단 "연결됨: X" / "아직 없음" */
static lv_obj_t *s_wifi_list        = NULL;  /* 스캔 결과 리스트(스캔 팝업 소속) */
static lv_obj_t *s_wifi_scan_popup  = NULL;  /* 2026-09-08 — 전체화면 전환, 팝업 루트 직접 참조용 */
static lv_obj_t *s_wifi_keyboard    = NULL;
static lv_obj_t *s_wifi_password_ta = NULL;
static lv_obj_t *s_wifi_pw_box         = NULL;  /* 비밀번호 팝업 자체(비동기 콜백에서 닫을 때 필요) */
static lv_obj_t *s_wifi_connect_btn    = NULL;  /* "연결" 버튼(시도 중엔 비활성화) */
static char      s_wifi_test_password[65] = "";
#define WIFI_SCAN_MAX_RESULTS 16
/* 2026-08-29(사용자 지시: "PSRAM도 몰아 넣어") — 내부 RAM 12K->1.7K 급감 대응. 둘 다
 * 스캔 팝업이 열릴 때(cb_network_find_btn) 최초 1회 PSRAM에 할당, 이후 재사용 */
static char (*s_wifi_scan_ssids)[33] = NULL;
static wifi_ap_record_t *s_wifi_scan_records = NULL;
static int  s_wifi_scan_count = 0;
static char s_wifi_selected_ssid[33] = "";

/* 2026-09-09(사용자 지적) — refresh_network_right_zone()와 동일 이유로 device_config_get_sta_ssid()
 * 게이트 제거, 실제 연결 여부(ip 유무)만으로 판단 */
static void update_wifi_status_label(void)
{
    if (!s_wifi_status_lbl) return;
    const char *ip = esp_now_hub_get_own_ip_str();
    if (ip[0] != '\0') {
        lv_label_set_text_fmt(s_wifi_status_lbl, "%s: %s", ui_str(STR_STATUS_CONNECTED),
                               esp_now_hub_get_active_sta_ssid());
    } else {
        lv_label_set_text(s_wifi_status_lbl, ui_str(STR_STATUS_NOT_CONNECTED));
    }
}

static void cb_wifi_scan_popup_close(lv_event_t *e)
{
    (void)e;
    esp_now_hub_set_sta_reconnect_paused(false);
    s_wifi_status_lbl = NULL;
    s_wifi_list = NULL;
    lv_obj_delete(s_wifi_scan_popup);
    s_wifi_scan_popup = NULL;
}

/* 비밀번호 팝업(+ 그 아래 스캔목록 팝업까지) 정리 — 저장하든 취소든 성공/실패든 공통으로
 * 필요한 부분. s_wifi_pw_box를 직접 참조하므로 버튼/이벤트 없이도(비동기 콜백에서도) 호출
 * 가능(2026-08-29 — 실시간 접속 시도 결과가 esp_now_hub.c의 이벤트 콜백에서 비동기로
 * 오는데, 그땐 클릭 이벤트가 없어서 예전처럼 버튼에서 부모를 거슬러 올라갈 수 없었음) */
static void close_wifi_popups(void)
{
    esp_now_hub_set_sta_reconnect_paused(false);
    if (s_wifi_keyboard) { lv_obj_delete(s_wifi_keyboard); s_wifi_keyboard = NULL; }
    s_wifi_password_ta = NULL;

    if (s_wifi_pw_box) {
        lv_obj_delete(lv_obj_get_parent(s_wifi_pw_box));  /* box -> overlay */
        s_wifi_pw_box = NULL;
        s_wifi_connect_btn = NULL;
    }

    /* 그 아래 스캔목록 팝업도 같이 닫기 — 목적을 이뤘으니(연결 성공이든 취소든) 목록까지
     * 볼 이유가 없음 */
    if (s_wifi_list) {
        lv_obj_delete(s_wifi_scan_popup);
        s_wifi_scan_popup = NULL;
        s_wifi_list = NULL;
        s_wifi_status_lbl = NULL;
    }
    resume_bg_timers();  /* pause_bg_timers는 idempotent(lv_timer_pause 반복 호출 안전)라 1번이면 충분 */
}

static void cb_wifi_pw_popup_close(lv_event_t *e)
{
    (void)e;
    close_wifi_popups();
}

/* 2026-08-29(사용자 설계: "AP 찾고 선택하고 접속하는 과정은 재시작 안 함") — esp_now_hub.c의
 * esp_now_hub_test_sta_connect() 결과 콜백. WiFi 이벤트 태스크에서 비동기로 불리므로
 * LVGL 조작 전체를 esp_lv_adapter_lock()으로 감싸야 함(wifi_scan_event_handler와 같은
 * 이유로 겪었던 화면깨짐 버그를 여기서 처음부터 피함) */
static void wifi_test_result_async_cb(void *user_data)
{
    bool success = (bool)(uintptr_t)user_data;
    /* 2026-08-29 — lv_async_call()의 콜백은 LVGL 자신의 태스크에서 정상 처리 흐름의
     * 일부로 실행되므로(esp_lvgl_adapter가 내부적으로 lv_timer_handler()를 도는 그
     * 태스크) 여기선 esp_lv_adapter_lock() 불필요 — 아래 cb_wifi_test_connect_result의
     * 주석 참고 */
    if (success) {
        /* TEMP TEST 2026-08-29 — 비번 저장 복원, 크래시 재현 시 정확히 뭘 저장하려던
         * 순간이었는지 캡처에 남기기 위한 직전 로그 */
        ESP_LOGI(TAG, "비번 저장 시도: SSID=[%s] PW=[%s](len=%d)",
                 s_wifi_selected_ssid, s_wifi_test_password, (int)strlen(s_wifi_test_password));
        device_config_set_sta_credentials(s_wifi_selected_ssid, s_wifi_test_password);
        close_wifi_popups();
        refresh_network_right_zone();  /* 2026-08-29 버그수정(사용자 리포트: "연결됐는데 상단은
                                           계속 찾기로 표시") — 언어전환/모드전환/부팅 때만 갱신되고
                                           연결 성공 직후엔 안 불렸음 */
        show_toast(ui_str(STR_STATUS_CONNECTED), lv_palette_main(LV_PALETTE_GREEN));
    } else if (s_wifi_connect_btn) {  /* 팝업이 그새 닫혔으면(사용자가 취소) 무시 */
        lv_obj_clear_state(s_wifi_connect_btn, LV_STATE_DISABLED);
        lv_label_set_text(lv_obj_get_child(s_wifi_connect_btn, 0), ui_str(STR_BTN_CONNECT));
        show_toast(ui_str(STR_MSG_WIFI_CONNECT_FAILED), lv_palette_main(LV_PALETTE_RED));
    }
}

/* 2026-08-29(사용자 지시: "토스트로 뭐하는지 단계마다 나오게") — DISCONNECTING/CONNECTING
 * 단계 전환을 토스트로 보여줌. wifi_test_result_async_cb와 같은 이유로 lv_async_call() 필요 */
static void wifi_test_stage_async_cb(void *user_data)
{
    esp_now_hub_sta_test_stage_t stage = (esp_now_hub_sta_test_stage_t)(uintptr_t)user_data;
    if (stage == STA_TEST_STAGE_DISCONNECTING) {
        show_toast(ui_str(STR_MSG_WIFI_STAGE_DISCONNECTING), lv_palette_main(LV_PALETTE_BLUE));
    } else {
        show_toast(ui_str(STR_MSG_WIFI_STAGE_AUTHENTICATING), lv_palette_main(LV_PALETTE_BLUE));
    }
}

static void cb_wifi_test_connect_stage(esp_now_hub_sta_test_stage_t stage, void *ctx)
{
    (void)ctx;
    lv_async_call(wifi_test_stage_async_cb, (void *)(uintptr_t)stage);
}

static void cb_wifi_test_connect_result(bool success, void *ctx)
{
    (void)ctx;
    /* 2026-08-29 버그수정(사용자 리포트: 옳은 비번으로 바로 연결 성공 시 크래시+리셋) —
     * 실기 크래시 로그로 "sys_evt 태스크 스택오버플로우" 확인됨. 원인: 이 콜백이 WiFi
     * 이벤트 태스크(2304바이트, 좁음) 안에서 곧바로 device_config_save()(784바이트
     * 구조체를 스택에 만들고 파일 I/O까지 함) + 팝업 삭제(LVGL 오브젝트 트리 정리) +
     * 토스트 생성을 전부 동기 실행하고 있었음 — 그 좁은 스택엔 너무 많은 일.
     * lv_async_call()로 실제 작업을 LVGL 자신의 태스크(별도의, 더 넉넉한 스택)로 미루고
     * 여기선 예약만 하고 바로 리턴 — WiFi 이벤트 태스크 스택 사용량을 최소화함.
     * (sys_evt 태스크 스택 자체도 2304->6144로 확대했지만, 애초에 이 무거운 작업을
     * 그 태스크에서 안 하는 게 더 근본적인 수정) */
    lv_async_call(wifi_test_result_async_cb, (void *)(uintptr_t)success);
}

static void cb_wifi_connect_btn(lv_event_t *e)
{
    (void)e;
    strncpy(s_wifi_test_password, lv_textarea_get_text(s_wifi_password_ta), sizeof(s_wifi_test_password) - 1);
    s_wifi_test_password[sizeof(s_wifi_test_password) - 1] = '\0';
    /* TEMP TEST 2026-08-29 — 키보드로 입력한 값이 실제로 그대로 캡처되는지 비교용. 원복할 것 */
    ESP_LOGI(TAG, "WiFi 테스트 접속 캡처값: SSID=[%s] PW=[%s](len=%d)",
             s_wifi_selected_ssid, s_wifi_test_password, (int)strlen(s_wifi_test_password));

    /* 2026-08-29 버그수정(사용자 리포트: "이미 연결된 AP를 선택해도 재시작한다는데") —
     * 저장된 값과 완전히 동일한 SSID+비밀번호면 실제로 바뀐 게 없으니 접속 시도 없이
     * 그냥 팝업만 닫음 */
    if (strcmp(s_wifi_selected_ssid, device_config_get_sta_ssid()) == 0 &&
        strcmp(s_wifi_test_password, device_config_get_sta_password()) == 0) {
        close_wifi_popups();
        refresh_network_right_zone();  /* 2026-08-29 버그수정 — 이 단축 경로도 팝업을 닫으므로
                                           성공 경로와 동일하게 상단 표시를 갱신해야 함 */
        return;
    }

    /* 2026-08-29(사용자 지적: 별도 "text" 상태 레이블 대신 버튼 캡션 자체를 상태로 씀) */
    lv_obj_add_state(s_wifi_connect_btn, LV_STATE_DISABLED);
    lv_label_set_text(lv_obj_get_child(s_wifi_connect_btn, 0), ui_str(STR_MSG_WIFI_CONNECTING));

    esp_now_hub_test_sta_connect(s_wifi_selected_ssid, s_wifi_test_password,
                                  cb_wifi_test_connect_result, cb_wifi_test_connect_stage, NULL);
}

static void cb_wifi_keyboard_hide(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
}

/* 2026-08-29(사용자 지시: "비번 보기 기능이 있으면 좋겠음") */
static void cb_wifi_toggle_password_visibility(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    bool was_masked = lv_textarea_get_password_mode(s_wifi_password_ta);
    lv_textarea_set_password_mode(s_wifi_password_ta, !was_masked);
    lv_label_set_text(lv_obj_get_child(btn, 0), was_masked ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

/* 리스트에서 SSID 선택 시 "추가 팝업"으로 비밀번호 입력(사용자 지시) — 스캔목록 팝업 위에
 * 새로 스택되는 완전히 별개의 모달 */
static void cb_wifi_ssid_selected(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    strncpy(s_wifi_selected_ssid, ssid, sizeof(s_wifi_selected_ssid) - 1);
    s_wifi_selected_ssid[sizeof(s_wifi_selected_ssid) - 1] = '\0';

    lv_obj_t *box = create_modal();
    s_wifi_pw_box = box;
    /* 2026-08-29(사용자 지시: "화면의 2/3") — LV_PCT는 화면 해상도(1024x600/800x480 등
     * 보드별로 다름, waveshare_rgb_lcd_port.h 참고) 상관없이 항상 2/3이 되게 함 */
    lv_obj_set_width(box, LV_PCT(66));
    /* 2026-08-29 버그수정(사용자 리포트) — create_modal()은 화면 중앙정렬인데, 키보드는
     * 화면 하단에 도킹돼서 큰 면적을 차지함. 중앙에 있으면 팝업 하단(연결/취소 버튼)이
     * 키보드에 가려짐 — 위쪽으로 옮겨서 키보드와 안 겹치게 함 */
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 20);

    /* 2026-08-29(사용자 지적: "SSID-비번창 한 줄에 배치") — 세로로 4개 쌓이던 걸 한 줄로
     * 줄여서 키보드가 떠도 아래 연결/취소 버튼이 안 가리게 함 */
    lv_obj_t *pw_row = lv_obj_create(box);
    lv_obj_set_size(pw_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pw_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(pw_row, 0, 0);
    lv_obj_set_style_pad_all(pw_row, 0, 0);

    lv_obj_t *ssid_lbl = lv_label_create(pw_row);
    lv_label_set_text(ssid_lbl, s_wifi_selected_ssid);
    lv_obj_set_style_text_font(ssid_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 2026-08-29 버그수정(사용자 리포트: "비번보이기 단추 토글 안됨") — 원래 눈 아이콘
     * 버튼을 텍스트 입력창의 *자식*으로 넣었었는데, 입력창 자체가 터치를 먼저 가로채서
     * (커서 이동/포커스 처리) 버튼 클릭이 전달 안 됐던 것으로 보임. 입력창과 버튼을
     * 같은 부모(pw_ta_wrap)의 *형제*로 분리하고, 버튼은 그 부모 기준으로 절대배치만
     * 해서 시각적으로만 입력창 안쪽에 겹쳐 보이게 함 — 터치 히트테스트는 서로 안 겹침 */
    lv_obj_t *pw_ta_wrap = lv_obj_create(pw_row);
    lv_obj_remove_style_all(pw_ta_wrap);
    lv_obj_set_flex_grow(pw_ta_wrap, 1);
    lv_obj_set_height(pw_ta_wrap, LV_SIZE_CONTENT);

    /* 별도 "비밀번호" 레이블 대신 placeholder 텍스트로 대체(사용자 지적: 레이블 제거) */
    s_wifi_password_ta = lv_textarea_create(pw_ta_wrap);
    lv_textarea_set_one_line(s_wifi_password_ta, true);
    lv_textarea_set_password_mode(s_wifi_password_ta, true);
    lv_textarea_set_placeholder_text(s_wifi_password_ta, ui_str(STR_LABEL_WIFI_PASSWORD));
    lv_obj_set_width(s_wifi_password_ta, LV_PCT(100));
    lv_obj_set_style_text_font(s_wifi_password_ta, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_pad_right(s_wifi_password_ta, 36, 0);  /* 안쪽 눈 아이콘과 텍스트 안 겹치게 */

    /* 2026-08-29(사용자 지시: "요즘 유행은 이 단추가... 텍스트 입력창 우측 끝 안쪽에
     * 조그맣게") — pw_ta_wrap 기준으로 오른쪽에 절대배치, 입력창의 자식이 아니라 형제.
     * LV_SYMBOL_EYE_OPEN/CLOSE는 이 앱 커스텀 폰트(NanumGothic TTF, ui_font_get)엔 없는
     * 글리프라 폰트를 명시적으로 안 지정 — LVGL 기본 내장 폰트로 떨어지는데, 그게 이
     * 심볼들을 갖고 있는 걸 키보드의 Enter/OK 키에서 이미 실측 확인함(사용자 확인) */
    lv_obj_t *pw_show_btn = lv_button_create(pw_ta_wrap);
    lv_obj_remove_style_all(pw_show_btn);
    lv_obj_set_size(pw_show_btn, 28, 28);
    lv_obj_align(pw_show_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_flag(pw_show_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pw_show_btn, cb_wifi_toggle_password_visibility, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pw_show_lbl = lv_label_create(pw_show_btn);
    lv_label_set_text(pw_show_lbl, LV_SYMBOL_EYE_CLOSE);  /* 기본 상태=마스킹됨=감은눈 */
    lv_obj_center(pw_show_lbl);

    /* 2026-08-29(사용자 지시: "이미 연결 중인 AP를 선택하면 비번이 미리 채워져 있으면" +
     * "여러 개 비번 저장 가능하지?") — 활성 슬롯뿐 아니라 예전에 저장했던 어떤 SSID든
     * 다시 선택하면 비번을 미리 채움(device_config_find_sta_password가 전체 슬롯 탐색) */
    const char *saved_pw = device_config_find_sta_password(s_wifi_selected_ssid);
    if (saved_pw) {
        lv_textarea_set_text(s_wifi_password_ta, saved_pw);
    }

    lv_obj_t *btn_row = create_modal_btn_row(box);
    s_wifi_connect_btn = add_modal_button(btn_row, STR_BTN_CONNECT, cb_wifi_connect_btn, NULL);
    add_modal_button(btn_row, STR_BTN_CANCEL, cb_wifi_pw_popup_close, NULL);

    if (!s_wifi_keyboard) {
        s_wifi_keyboard = lv_keyboard_create(lv_screen_active());
        /* 2026-08-29 버그수정(사용자 지적: "키보드 어떻게 닫아?") — 키보드 자체의 "✕"
         * (LV_EVENT_CANCEL)와 체크/Enter(LV_EVENT_READY) 키에 아무 것도 안 붙어있어서
         * 눌러도 반응이 없었음. 둘 다 그냥 키보드만 숨김(입력값 제출은 아래 "연결" 버튼
         * 전용 — 눌러서 실수로 바로 연결/재시작되는 걸 방지) */
        lv_obj_add_event_cb(s_wifi_keyboard, cb_wifi_keyboard_hide, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_wifi_keyboard, cb_wifi_keyboard_hide, LV_EVENT_CANCEL, NULL);
    }
    lv_obj_remove_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);  /* 이전에 숨겨졌을 수 있음 */
    lv_keyboard_set_textarea(s_wifi_keyboard, s_wifi_password_ta);
}

/* 2026-08-29 — esp_wifi_scan_start(NULL, false)는 비동기라, WIFI_EVENT_SCAN_DONE으로 완료를
 * 받음. 팝업이 닫힌 뒤 스캔이 뒤늦게 끝나는 경우 s_wifi_list가 NULL이라 그냥 무시(존재하지
 * 않는 위젯에 그리지 않도록 방어) */
static void wifi_scan_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id != WIFI_EVENT_SCAN_DONE) return;
    if (!s_wifi_list) return;  /* 팝업이 이미 닫힘 */

    if (!s_wifi_scan_records || !s_wifi_scan_ssids) return;  /* 팝업 열 때 할당 안 됐으면 방어 */
    uint16_t num = WIFI_SCAN_MAX_RESULTS;
    esp_wifi_scan_get_ap_records(&num, s_wifi_scan_records);

    /* 2026-08-29 버그수정(사용자 리포트: 스캔 결과 도착 시점에 화면 전체가 깨지고 입력이
     * 먹통) — 이 핸들러는 esp_event 루프 태스크에서 도는데, LVGL은 스레드 안전하지 않아서
     * 그 오브젝트 트리를 LVGL 자신의 태스크가 렌더링하는 도중에 다른 태스크에서 동시에
     * lv_obj_clean/lv_list_add_button 등으로 건드리면 딱 이런 화면 깨짐+먹통이 남.
     * esp_lv_adapter_lock()으로 LVGL 조작 구간 전체를 감싸야 함 — 이 프로젝트의 다른 크로스
     * 태스크 LVGL 접근(main.c의 esp_lv_adapter_lock(-1) 사용)과 동일한 패턴 */
    if (esp_lv_adapter_lock(-1) != ESP_OK) return;

    lv_obj_clean(s_wifi_list);
    s_wifi_scan_count = 0;

    for (int i = 0; i < num && s_wifi_scan_count < WIFI_SCAN_MAX_RESULTS; i++) {
        const char *ssid = (const char *)s_wifi_scan_records[i].ssid;
        if (ssid[0] == '\0') continue;
        bool dup = false;
        for (int j = 0; j < s_wifi_scan_count; j++) {
            if (strcmp(s_wifi_scan_ssids[j], ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strncpy(s_wifi_scan_ssids[s_wifi_scan_count], ssid, sizeof(s_wifi_scan_ssids[0]) - 1);
        /* 2026-08-29 버그수정(사용자 리포트: "연결된 SSID가 별 표시도 없이 그대로") — 현재
         * 연결된 SSID를 리스트에서 구분 가능하게 표시. LV_SYMBOL_OK 같은 심볼은 이 프로젝트
         * 커스텀 TTF에 없는 글리프라(다른 곳에서 이미 겪은 문제) 일반 텍스트 표식으로 대체 */
        const char *active_ssid = device_config_get_sta_ssid();
        const char *own_ip = esp_now_hub_get_own_ip_str();
        bool is_connected = (active_ssid[0] != '\0' && own_ip[0] != '\0' &&
                              strcmp(active_ssid, ssid) == 0);
        char label_buf[64];
        snprintf(label_buf, sizeof(label_buf), "%s%s (%d dBm)",
                 is_connected ? ui_str(STR_TAG_WIFI_CONNECTED) : "", ssid, (int)s_wifi_scan_records[i].rssi);
        lv_obj_t *btn = lv_list_add_button(s_wifi_list, NULL, label_buf);
        lv_obj_set_style_text_font(btn, ui_font_get(UI_FONT_SIZE_18), 0);
        lv_obj_add_event_cb(btn, cb_wifi_ssid_selected, LV_EVENT_CLICKED,
                             s_wifi_scan_ssids[s_wifi_scan_count]);
        s_wifi_scan_count++;
    }

    if (s_wifi_scan_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_wifi_list);
        lv_label_set_text(lbl, ui_str(STR_MSG_WIFI_SCAN_EMPTY));
        lv_obj_set_style_text_font(lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    }

    esp_lv_adapter_unlock();
}

/* 2026-08-29 — cb_network_find_btn(최초 진입)과 cb_wifi_rescan_btn(팝업 안 "다시 찾기")이
 * 공유하는 실제 스캔 트리거. s_wifi_list가 이미 만들어져 있어야 함(팝업이 열린 상태) */
static void trigger_wifi_scan(void)
{
    lv_obj_clean(s_wifi_list);
    lv_obj_t *scanning_lbl = lv_label_create(s_wifi_list);
    lv_label_set_text(scanning_lbl, ui_str(STR_MSG_WIFI_SCANNING));
    lv_obj_set_style_text_font(scanning_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 2026-08-29 버그수정(사용자 리포트: "팝업 뜨면서 찾는 기능은 안됨, 다시찾기 눌러야
     * 시작") — esp_now_hub_set_sta_reconnect_paused(true)는 향후 재연결만 막지, 팝업이
     * 뜨는 바로 그 순간 이미 진행 중이던 연결 시도까지 취소하진 않음. 그 시도가 아직 안
     * 끝난 채로 esp_wifi_scan_start()를 부르면 ESP_ERR_WIFI_STATE로 실패(IDF 특성:
     * 연결 시도 중엔 스캔 거부) — 그래서 최초 1회만 실패하고 "다시 찾기"(그땐 이미 그
     * 시도가 끝나있음)부터 되는 증상이었음. 스캔 직전에 확실히 disconnect()로 정리 */
    /* 2026-08-29 버그수정(사용자 지시: "찾기 팝업 열 때 먼저 끊지 마, 비번창에서 연결 누를
     * 때 끊어야 돼") — 여기서 미리 disconnect()하면 스캔만 열어봐도 실제 WiFi 연결이
     * 끊어져버리고, "이미 같은 AP" 단축 경로는 재연결을 안 시켜서 그대로 끊긴 채 남는 문제가
     * 있었음. 실제 접속 전환은 esp_now_hub_test_sta_connect()가 접속 시도 시점에 자체적으로
     * disconnect-then-connect를 이미 처리하므로, 여기서는 더 이상 선제적으로 끊지 않음 */
    if (esp_wifi_scan_start(NULL, false) != ESP_OK) {
        lv_obj_clean(s_wifi_list);
        lv_obj_t *lbl = lv_label_create(s_wifi_list);
        lv_label_set_text(lbl, ui_str(STR_MSG_WIFI_SCAN_EMPTY));
        lv_obj_set_style_text_font(lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    }
}

static void cb_wifi_rescan_btn(lv_event_t *e)
{
    (void)e;
    trigger_wifi_scan();
}

static void cb_network_find_btn(lv_event_t *e)
{
    (void)e;
    /* 2026-08-29 버그수정 — 스캔이 되려면 STA 재연결 루프가 잠깐 쉬어야 함(위
     * esp_now_hub_set_sta_reconnect_paused 주석 참고). 팝업 닫힐 때(cb_wifi_scan_popup_close/
     * close_wifi_popups) 반드시 해제됨 */
    esp_now_hub_set_sta_reconnect_paused(true);

    /* 2026-08-29(사용자 지시: "PSRAM도 몰아 넣어") — 최초 1회만 할당, 이후 재사용(찾기 팝업
     * 열 때마다 다시 만들 필요 없음) */
    if (!s_wifi_scan_ssids) {
        s_wifi_scan_ssids = heap_caps_calloc(WIFI_SCAN_MAX_RESULTS, sizeof(s_wifi_scan_ssids[0]), MALLOC_CAP_SPIRAM);
    }
    if (!s_wifi_scan_records) {
        s_wifi_scan_records = heap_caps_calloc(WIFI_SCAN_MAX_RESULTS, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    }

    if (s_wifi_scan_popup) return;  /* 이미 열려있음 */

    /* 2026-09-08(사용자 지시 — "Wifi scan popup이 전화면이 아니고") — 전체화면 셸로 전환 */
    s_wifi_scan_popup = create_page_popup();
    add_page_popup_header(s_wifi_scan_popup, ui_str(STR_TITLE_WIFI_SCAN), cb_wifi_scan_popup_close, NULL);

    lv_obj_t *content = lv_obj_create(s_wifi_scan_popup);
    lv_obj_set_size(content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 12, 0);
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_set_style_border_width(content, 0, 0);

    /* 2026-08-29 사용자 지시 — 이미 연결된 네트워크가 있으면 표기, 없으면 "아직 없음" */
    s_wifi_status_lbl = lv_label_create(content);
    lv_obj_set_style_text_font(s_wifi_status_lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_style(s_wifi_status_lbl, &style_text_muted, 0);
    update_wifi_status_label();

    s_wifi_list = lv_list_create(content);
    lv_obj_set_width(s_wifi_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_wifi_list, 1);

    /* 2026-08-29 사용자 지시 — "다시 찾기"가 닫기 단추 왼쪽에 오도록 먼저 추가. 이 팝업은
     * 뭔가를 "취소"하는 게 아니라 그냥 닫는 거라 공용 STR_BTN_CANCEL 대신 전용
     * STR_BTN_CLOSE 사용(닫기는 이제 헤더의 X가 대신하므로 여기선 다시찾기만) */
    lv_obj_t *btn_row = lv_obj_create(content);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_t *rescan_btn = lv_button_create(btn_row);
    lv_obj_add_event_cb(rescan_btn, cb_wifi_rescan_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rescan_lbl = lv_label_create(rescan_btn);
    lv_label_set_text(rescan_lbl, ui_str(STR_BTN_RESCAN));
    lv_obj_set_style_text_font(rescan_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    trigger_wifi_scan();
}

static void refresh_network_right_zone(void)
{
    if (!s_network_right_label || !s_network_find_btn) return;  /* 행이 아직 안 만들어짐 */

    bool ap_mode = device_config_get_wifi_ap_mode();
    const char *ip = esp_now_hub_get_own_ip_str();

    if (ap_mode) {
        /* IP는 사용자가 바꿀 수 있는 값이 아니라 정보 표시일 뿐이라 평범한 라벨 */
        lv_label_set_text(s_network_right_label, ip);
        lv_obj_remove_flag(s_network_right_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_network_find_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* STA 모드는 항상 버튼 하나 — 연결 안 됐으면 캡션 "찾기", 연결됐으면 캡션이 SSID로
         * 바뀔 뿐 여전히 같은 버튼(눌러서 다른 AP로 재검색 가능, 2026-08-29 사용자 지적:
         * "연결된 AP가 있을 때 바꿀 방법이 없다" + "레이블이 단추 캡션이어야").
         * 2026-09-09(사용자 지적 — "상단바에는 연결된 AP SSID가 이미 보이고 있으니까
         * 내가 지적한 2군데는 버그야") — 2026-08-29엔 "찾기로 저장한 SSID 없으면 하드코딩
         * 폴백이어도 무조건 찾기로 표시"가 의도적 설계였지만, 이 설계 자체가 실제 연결
         * 정보가 있는데도 안 보여주는 버그로 재판정됨 — 상단바(esp_now_hub_get_active_sta_ssid
         * 그대로 사용)와 똑같이 "진짜 연결됐는지"(ip 유무)만으로 판단하도록 정정.
         * device_config_get_sta_ssid() 게이트 제거 */
        lv_obj_add_flag(s_network_right_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_network_find_btn, LV_OBJ_FLAG_HIDDEN);
        if (ip[0] != '\0') {
            lv_label_set_text(s_network_find_lbl, esp_now_hub_get_active_sta_ssid());
        } else {
            lv_label_set_text(s_network_find_lbl, ui_str(STR_BTN_FIND));
        }
    }
}

/* 설정탭 Apply 5단계 플로우(2026-08-08, 사용자 설계) — 공용 진행팝업(show_progress_popup)
 * 재사용, capture/response 둘 다 이 하나의 tick 함수로 처리(어느 쪽인지는
 * s_config_apply_target로 구분). 촬영주기/응답성 각각 독립된 Apply 버튼이라 동시에 둘 다
 * 누르는 경우는 없다고 가정(2단계 팝업이 모달이라 물리적으로도 막힘) */
typedef enum { CONFIG_APPLY_TARGET_CAPTURE, CONFIG_APPLY_TARGET_RESPONSE, CONFIG_APPLY_TARGET_XCLK } config_apply_target_t;
static config_apply_target_t s_config_apply_target;
static int                   s_config_apply_pending_idx;
static uint32_t              s_config_apply_start_ms;
static lv_obj_t              *s_config_apply_label;

/* 2026-08-23(사용자 지적) — 응답성을 "30초 -> Live(0초)"처럼 짧은 쪽으로 바꿀 때, CAM은
 * 아직 옛 값(30초)대로 자고 있는데 cam_response_timeout_ms()는 새로 저장된 값(0초, 3초
 * 예산)으로 계산돼서 CAM이 실제로 깨기도 전에 4005(무응답)로 오탐됨 — 결국 30초 근방에
 * 정상 적용되긴 하지만 그 사이 가짜 에러가 뜸. 0이면 기존 cam_response_timeout_ms() 그대로
 * 사용, 응답성 변경 시에만 old/new 중 큰 쪽으로 계산해서 여기 채움(cb_apply_response_interval) */
static uint32_t s_config_apply_timeout_ms_override = 0;

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

static void update_xclk_apply_enabled(void)
{
    bool changed = (lv_dropdown_get_selected(s_xclk_dd) != (uint16_t)s_xclk_applied_idx);
    if (changed) lv_obj_clear_state(s_xclk_apply_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(s_xclk_apply_btn, LV_STATE_DISABLED);
}

static void cb_xclk_changed(lv_event_t *e) { (void)e; update_xclk_apply_enabled(); }

/* 측정 주기 Apply 버튼 활성화 판정(2026-09-05) — 촬영주기와 동일 패턴. select_sensor()가
 * 선택이 바뀔 때마다 이것도 다시 불러서, 새로 선택된 센서 기준으로 판정을 갱신함 */
static void update_sens_measure_apply_enabled(void)
{
    bool changed = s_has_selected_sensor &&
                   (lv_dropdown_get_selected(s_sens_measure_dd) != (uint16_t)s_sens_measure_applied_idx);
    if (changed) lv_obj_clear_state(s_sens_measure_apply_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(s_sens_measure_apply_btn, LV_STATE_DISABLED);
}
static void cb_sens_measure_interval_changed(lv_event_t *e) { (void)e; update_sens_measure_apply_enabled(); }

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
        } else if (s_config_apply_target == CONFIG_APPLY_TARGET_RESPONSE) {
            s_response_interval_applied_idx = s_config_apply_pending_idx;
            update_response_apply_enabled();
        } else {
            s_xclk_applied_idx = s_config_apply_pending_idx;
            update_xclk_apply_enabled();
        }
        esp_now_hub_config_apply_stage_clear();
        return true;
    }
    uint32_t timeout_ms = s_config_apply_timeout_ms_override ? s_config_apply_timeout_ms_override
                                                              : cam_response_timeout_ms();
    if (lv_tick_elaps(s_config_apply_start_ms) > timeout_ms) {
        lv_label_set_text(s_config_apply_label, ui_str(STR_CONFIG_APPLY_STALLED));
        lv_obj_set_style_text_color(s_config_apply_label, lv_palette_main(LV_PALETTE_RED), 0);
        ui_log_add_err(UI_ERR_CONFIG_NORESPONSE, "Config apply request: no CAM response (timeout)");
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
        ui_log_add("Capture interval apply: no camera selected");
        return;
    }
    uint16_t idx = lv_dropdown_get_selected(s_capture_interval_dd);
    uint32_t sec = (idx < (sizeof(s_capture_interval_values) / sizeof(s_capture_interval_values[0])))
                   ? s_capture_interval_values[idx] : 0;
    s_config_apply_target = CONFIG_APPLY_TARGET_CAPTURE;
    s_config_apply_pending_idx = idx;
    s_config_apply_timeout_ms_override = 0;  /* 응답성 전용 보정값 — 이 요청엔 안 씀 */
    /* device_config에는 항상 저장되고, CAM은 매 웨이크(=매 접속)마다 무조건 최신값을
     * 다시 받아가므로(push_cam_config_to()가 PAIR_ACK 시점에도 자동 호출됨) WAITING이어도
     * "진짜 실패"가 아니라 "다음 접속에 반영될 정상 대기 상태"임 — 2026-08-10, 사용자
     * 지적으로 require_active_or_report()(2007 에러) 대신 응답성 적용과 동일하게 정보
     * 로그만 남기도록 수정(처음엔 실수로 2007과 "저장됨" 안내가 동시에 뜨는 모순이 있었음) */
    esp_now_hub_apply_cam_capture_interval_sec(s_selected_cam_mac, sec);
    if (esp_now_hub_get_conn_state(s_selected_cam_mac) == HUB_CONN_STATE_WAITING) {
        ui_log_add("Capture interval saved - applied automatically on CAM reconnect");
        return;
    }
    show_config_apply_popup();
}

/* 2026-09-05 — 센스는 콘 개입 없이 자기 주기대로 자율적으로 깨어(사용자 설계) 매 웨이크
 * SENS_CONFIG_SET을 다시 받아가므로, 촬영주기처럼 진행팝업으로 "적용 중" 대기를 보여줄
 * 이유가 약함(다음 깨어날 때 반영될 뿐, 그 시점을 콘이 능동적으로 기다릴 필요가 없음) —
 * AGC/AEC 스위치의 "즉시 저장" 패턴과 같은 이유로 팝업 없이 로그만 남김(단순화) */
static void cb_apply_sens_measure_interval(lv_event_t *e)
{
    (void)e;
    if (!s_has_selected_sensor) {
        ui_log_add("Measure period apply: no sensor selected");
        return;
    }
    uint16_t idx = lv_dropdown_get_selected(s_sens_measure_dd);
    uint32_t sec = (idx < (sizeof(s_sens_measure_interval_values) / sizeof(s_sens_measure_interval_values[0])))
                   ? s_sens_measure_interval_values[idx] : 15;
    esp_now_hub_apply_sens_sample_interval_sec(s_selected_sensor_mac, sec);
    s_sens_measure_applied_idx = idx;
    update_sens_measure_apply_enabled();
    ui_log_add("Measure period saved - applied on next Sens wake");
}

static void cb_apply_xclk(lv_event_t *e)
{
    (void)e;
    if (!s_has_selected_cam) {
        ui_log_add("XCLK apply: no camera selected");
        return;
    }
    uint16_t idx = lv_dropdown_get_selected(s_xclk_dd);
    uint8_t mhz = (idx < (sizeof(s_xclk_values) / sizeof(s_xclk_values[0])))
                  ? s_xclk_values[idx] : s_xclk_values[0];
    s_config_apply_target = CONFIG_APPLY_TARGET_XCLK;
    s_config_apply_pending_idx = idx;
    s_config_apply_timeout_ms_override = 0;  /* 응답성 전용 보정값 — 이 요청엔 안 씀 */
    esp_now_hub_apply_cam_xclk_mhz(s_selected_cam_mac, mhz);
    if (esp_now_hub_get_conn_state(s_selected_cam_mac) == HUB_CONN_STATE_WAITING) {
        ui_log_add("XCLK saved - applied automatically on CAM reconnect");
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
    /* 2026-08-23 — CAM은 새 값이 아니라 옛 값(지금 이 순간 저장돼있는 값)만큼 자고 있을 수
     * 있으므로(예: 30초->Live), old/new 중 큰 쪽 기준으로 이번 팝업만 타임아웃을 늘림 —
     * esp_now_hub_apply_response_interval_sec()가 저장값을 새 값으로 바로 덮어쓰기 전에
     * 옛 값을 먼저 읽어둬야 함 */
    uint32_t old_sec = device_config_get_response_interval_sec();
    uint32_t wait_sec = (old_sec > sec) ? old_sec : sec;
    if (wait_sec > 30U) wait_sec = 30U;
    s_config_apply_timeout_ms_override = wait_sec * 1000U + 3000U;
    /* 반환값으로 판단(2026-08-10) — 이 설정은 특정 CAM 하나가 아니라 "지금 ACTIVE한 CAM
     * 전부"가 대상이라 require_active_or_report()의 mac 하나 기준 검사가 안 맞음. 대상이
     * 하나도 없으면(전부 WAITING) 값은 저장됐지만 응답 대기 팝업은 안 띄움 — 다른 4개
     * 통신 기능과 동일 원칙 */
    if (!esp_now_hub_apply_response_interval_sec(sec)) {
        /* 2026-09-07 버그수정 — 페어링된 CAM이 하나도 없으면(예: 센스만 연결된 상태)
         * esp_now_hub_apply_response_interval_sec()가 false를 반환하는데(ACK 대기 대상이
         * CAM뿐이라, 센스는 매 사이클 자동으로 최신값을 받아가서 별도 ACK가 필요없음),
         * 여기서 그냥 return해버리면 값은 실제로 저장됐는데도 s_response_interval_applied_idx가
         * 안 갱신돼서 Apply 버튼이 영원히 활성 상태로 남았음(cb_apply_adaptive_response()의
         * 즉시적용 패턴과 동일하게 여기서도 바로 반영) */
        s_response_interval_applied_idx = idx;
        update_response_apply_enabled();
        ui_log_add("Response interval saved - applied automatically on CAM reconnect");
        return;
    }
    show_config_apply_popup();
}

/* 2026-08-21 — AGC/AEC 스위치. 값이 불리언 하나뿐이고 진단용이라, 촬영주기/응답성의
 * 5단계 팝업(드롭다운+Apply+ACK대기) 대신 토글 즉시 반영 — 스위치의 통상적인 UX와도
 * 맞음. 그래도 실제 전송은 reliable stack(esp_now_tx) 그대로라 유실 걱정은 없음, 화면에
 * 진행상태만 안 보여줄 뿐 */
static void cb_agc_switch_changed(lv_event_t *e)
{
    (void)e;
    if (!s_has_selected_cam) {
        ui_log_add("AGC apply: no camera selected");
        return;
    }
    bool enable = lv_obj_has_state(s_agc_switch, LV_STATE_CHECKED);
    esp_now_hub_apply_cam_agc_enable(s_selected_cam_mac, enable);
    if (esp_now_hub_get_conn_state(s_selected_cam_mac) == HUB_CONN_STATE_WAITING) {
        ui_log_add("AGC saved - applied automatically on CAM reconnect");
    }
}

static void cb_aec_switch_changed(lv_event_t *e)
{
    (void)e;
    if (!s_has_selected_cam) {
        ui_log_add("AEC apply: no camera selected");
        return;
    }
    bool enable = lv_obj_has_state(s_aec_switch, LV_STATE_CHECKED);
    esp_now_hub_apply_cam_aec_enable(s_selected_cam_mac, enable);
    if (esp_now_hub_get_conn_state(s_selected_cam_mac) == HUB_CONN_STATE_WAITING) {
        ui_log_add("AEC saved - applied automatically on CAM reconnect");
    }
}

static void cb_auto_connect_known_changed(lv_event_t *e)
{
    (void)e;
    device_config_set_auto_connect_known(lv_obj_has_state(s_auto_connect_known_switch, LV_STATE_CHECKED));
}

/* new가 켜지면 known을 강제로 켠 채 비활성화(사용자 지적: "2번이면 1번을 만족시키는
 * 조건이라 UI를 잘 만들어야지") — new가 꺼지면 known은 저장된 실제 값으로 되돌리고 다시
 * 조작 가능하게 함 */
static void cb_auto_connect_new_changed(lv_event_t *e)
{
    (void)e;
    bool enable = lv_obj_has_state(s_auto_connect_new_switch, LV_STATE_CHECKED);
    device_config_set_auto_connect_new(enable);
    if (enable) {
        lv_obj_add_state(s_auto_connect_known_switch, LV_STATE_CHECKED);
        lv_obj_add_state(s_auto_connect_known_switch, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_auto_connect_known_switch, LV_STATE_DISABLED);
        if (device_config_get_auto_connect_known()) {
            lv_obj_add_state(s_auto_connect_known_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_auto_connect_known_switch, LV_STATE_CHECKED);
        }
    }
}

/* 페이지콘트롤 — 페이지탭 3개(상황판/통계/설정). 로고 + 상황판/통계 탭 내용(원래 데모의
 * Profile/Analytics 위젯)은 고치기 전 상태 그대로 활용 — 설정 탭만 새로 만든 그룹박스로 교체 */
/* 2026-08-29 버그수정(사용자 리포트: "찾기" 팝업이 30초 넘게 "검색 중..."에 멈춤) — 원래
 * ui_init()이 이 등록을 직접 했는데, ui_init()은 esp_now_hub_init()보다 먼저 호출되고
 * (main.c app_main 참고) esp_event_loop_create_default()는 esp_now_hub_init() 내부에서
 * 호출됨 — 즉 등록 시점에 기본 이벤트루프가 아직 없어서 esp_event_handler_register()가
 * 조용히 실패하고 있었음(반환값 확인 안 해서 못 잡음). main.c의 ip_event_handler 등록과
 * 똑같은 이유로, main.c가 esp_now_hub_init() 호출 *이후*에 이 함수를 불러줘야 함 */
void ui_main_register_wifi_events(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &wifi_scan_event_handler, NULL));
}

/* ════════════════════════════════════════════════════════════
 * 2026-09-04(사용자 설계: "PC 원격제어처럼") — 웹 입력을 실제 탭/팝업/확인 시퀀스로 합성.
 * 웹(httpd 태스크)이 직접 esp_now_hub_request_pair() 등을 부르는 대신, 온디바이스 탭이
 * 눌렀을 그 위젯에 그대로 lv_event_send()를 보냄 — 그래야 로직이 갈라질 여지가 없음
 * (project_cntl_web_full_ui_injection_design_2026_09_04 메모리 참고).
 * ════════════════════════════════════════════════════════════ */

/* 모달의 자식들 중 특정 콜백이 등록된 위젯을 찾음(재귀) — 확인 버튼을 찾는 용도.
 * 팝업 생성엔 무선 왕복이 없어 동기적으로 이어지므로, 방금 탭 합성한 직후 바로 써도 됨 */
static lv_obj_t *find_widget_by_event_cb(lv_obj_t *root, lv_event_cb_t target_cb)
{
    if (!root) return NULL;
    uint32_t child_cnt = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_get_user_data(child) == (void *)target_cb) return child;
        lv_obj_t *found = find_widget_by_event_cb(child, target_cb);
        if (found) return found;
    }
    return NULL;
}

/* httpd 태스크에서 LVGL 태스크의 함수를 동기 호출하듯 실행 — lv_event_send()는 LVGL
 * 태스크에서만 안전해서 lv_async_call()로 미루되, "합성 자체가 성공했는지"(위젯을
 * 찾았는지)는 즉시(동기적으로) 알아야 함. 단일 슬롯 — "웹과 앱이 동시 작업하지 않는다"는
 * 전제(사용자 확인) 하에 안전함 */
static SemaphoreHandle_t s_inject_done_sem = NULL;
static bool (*s_inject_fn)(void *arg) = NULL;
static void  *s_inject_arg = NULL;
static bool   s_inject_result = false;

static void cb_async_inject_trampoline(void *user_data)
{
    (void)user_data;
    s_inject_result = s_inject_fn ? s_inject_fn(s_inject_arg) : false;
    xSemaphoreGive(s_inject_done_sem);
}

static bool run_on_lvgl_task(bool (*fn)(void *arg), void *arg, uint32_t timeout_ms)
{
    if (!s_inject_done_sem) s_inject_done_sem = xSemaphoreCreateBinary();
    s_inject_fn  = fn;
    s_inject_arg = arg;
    xSemaphoreTake(s_inject_done_sem, 0);  /* 이전에 남아있을 수 있는 신호 비움 */
    lv_async_call(cb_async_inject_trampoline, NULL);
    if (xSemaphoreTake(s_inject_done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    return s_inject_result;
}

static lv_obj_t *find_camera_row_by_mac(const uint8_t *mac)
{
    for (int i = 0; i < s_camera_row_count; i++) {
        if (memcmp(s_camera_row_macs[i], mac, 6) == 0) return s_camera_row_objs[i];
    }
    return NULL;
}

static bool inject_fn_connect(void *arg)
{
    const uint8_t *mac = (const uint8_t *)arg;
    /* 2026-09-07 버그수정(사용자 지적 — "앞으로 테스트 때도 문제가 될 것 같아서") — 이미
     * 연결된 상태에서 행을 탭하면 cb_camera_item_clicked()가 연결이 아니라 연결해제
     * 확인팝업을 띄움. 이 함수는 pair_confirm 버튼만 찾으므로 그 경우 못 찾고 false를
     * 반환하면서 연결해제 팝업만 화면에 덩그러니 남는 사고가 났음 — 이미 연결됐으면
     * 애초에 아무것도 안 건드리고 바로 성공 처리 */
    if (esp_now_hub_get_conn_state(mac) != HUB_CONN_STATE_WAITING) return true;
    lv_obj_t *row = find_camera_row_by_mac(mac);
    if (!row) return false;  /* 지금 목록에 없음 */
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);  /* -> cb_camera_item_clicked -> show_pair_confirm_popup */
    lv_obj_t *confirm = find_widget_by_event_cb(s_last_modal, cb_pair_confirm);
    if (!confirm) return false;
    lv_obj_send_event(confirm, LV_EVENT_CLICKED, NULL);  /* -> cb_pair_confirm -> esp_now_hub_request_pair() */
    return true;
}

bool ui_main_inject_connect(const uint8_t *mac)
{
    static uint8_t mac_copy[6];
    memcpy(mac_copy, mac, 6);
    return run_on_lvgl_task(inject_fn_connect, mac_copy, 1000);
}

/* 2026-09-06(사용자 지시 — 야간 자동 테스트용) — 센스 행은 카메라 행과 별도 리스트
 * (s_sensor_row_objs/macs)라서 find_camera_row_by_mac 재사용이 안 됨. 확인팝업 콜백은
 * cb_pair_confirm 그대로 공용(cb_sensor_item_clicked가 show_pair_confirm_popup을
 * CAM과 동일하게 씀) */
static lv_obj_t *find_sensor_row_by_mac(const uint8_t *mac)
{
    for (int i = 0; i < s_sensor_row_count; i++) {
        if (memcmp(s_sensor_row_macs[i], mac, 6) == 0) return s_sensor_row_objs[i];
    }
    return NULL;
}

static bool inject_fn_connect_sensor(void *arg)
{
    const uint8_t *mac = (const uint8_t *)arg;
    /* 2026-09-07 버그수정 — inject_fn_connect()와 동일 이유(사용자 지적: "앞으로 테스트
     * 때도 문제가 될 것 같아서") — 이미 연결됐으면 연결해제 확인팝업이 뜨는 걸 막음 */
    if (esp_now_hub_get_conn_state(mac) != HUB_CONN_STATE_WAITING) return true;
    lv_obj_t *row = find_sensor_row_by_mac(mac);
    if (!row) return false;
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);  /* -> cb_sensor_item_clicked -> show_pair_confirm_popup */
    lv_obj_t *confirm = find_widget_by_event_cb(s_last_modal, cb_pair_confirm);
    if (!confirm) return false;
    lv_obj_send_event(confirm, LV_EVENT_CLICKED, NULL);  /* -> cb_pair_confirm -> esp_now_hub_request_pair() */
    return true;
}

bool ui_main_inject_connect_sensor(const uint8_t *mac)
{
    static uint8_t mac_copy[6];
    memcpy(mac_copy, mac, 6);
    return run_on_lvgl_task(inject_fn_connect_sensor, mac_copy, 1000);
}

/* 2026-09-06(사용자 지시 — 야간 자동 테스트용) — 응답성(response_interval) 드롭다운 선택
 * + Apply 버튼 탭까지 그대로 합성. sec가 s_response_interval_values(0/3/10/30/60)에
 * 없으면 실패 */
static bool inject_fn_set_response_interval(void *arg)
{
    uint32_t sec = *(uint32_t *)arg;
    int idx = find_value_index(s_response_interval_values,
        sizeof(s_response_interval_values) / sizeof(s_response_interval_values[0]), sec);
    if (idx < 0 || !s_response_interval_dd || !s_response_apply_btn) return false;
    lv_dropdown_set_selected(s_response_interval_dd, (uint16_t)idx);
    lv_obj_send_event(s_response_interval_dd, LV_EVENT_VALUE_CHANGED, NULL);  /* -> cb_response_interval_changed */
    lv_obj_send_event(s_response_apply_btn, LV_EVENT_CLICKED, NULL);          /* -> cb_apply_response_interval */
    return true;
}

bool ui_main_inject_set_response_interval(uint32_t sec)
{
    static uint32_t sec_copy;
    sec_copy = sec;
    return run_on_lvgl_task(inject_fn_set_response_interval, &sec_copy, 1000);
}

/* 2026-09-08(연결 기능 주화면 이관 — 이 함수 안전성을 위한 수정) — 연결된 카메라는 이제
 * s_camera_row_*(대기중 전용)가 아니라 s_camera_dash_row_*(연결됨)에 있음. 행 라벨의
 * 부모가 클릭 이벤트 리스너가 걸린 행 컨테이너(cb_camera_dash_row_clicked 참고) */
static lv_obj_t *find_camera_dash_row_by_mac(const uint8_t *mac)
{
    for (int i = 0; i < s_camera_dash_row_count; i++) {
        if (memcmp(s_camera_dash_row_macs[i], mac, 6) == 0) return lv_obj_get_parent(s_camera_dash_row_objs[i]);
    }
    return NULL;
}

static bool inject_fn_disconnect(void *arg)
{
    const uint8_t *mac = (const uint8_t *)arg;
    lv_obj_t *row = find_camera_dash_row_by_mac(mac);
    if (!row) return false;
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);  /* -> cb_camera_dash_row_clicked -> build_device_popup */
    if (!s_device_disconnect_btn) return false;
    lv_obj_send_event(s_device_disconnect_btn, LV_EVENT_CLICKED, NULL);  /* -> cb_device_disconnect_clicked ->
                                                                             show_confirm_popup */
    lv_obj_t *confirm = find_widget_by_event_cb(s_last_modal, cb_confirm_yes_trampoline);
    if (!confirm) return false;
    lv_obj_send_event(confirm, LV_EVENT_CLICKED, NULL);  /* -> cb_confirm_yes_trampoline -> cb_device_disconnect_confirm */
    return true;
}

bool ui_main_inject_disconnect(const uint8_t *mac)
{
    static uint8_t mac_copy[6];
    memcpy(mac_copy, mac, 6);
    return run_on_lvgl_task(inject_fn_disconnect, mac_copy, 1000);
}

/* 2026-09-07(사용자 지시 — 어젯밤 이미 쌓인 이산화탄소 0 레코드 정리용) — 삭제 버튼+확인
 * 팝업 둘 다 탭 합성, 다른 확인팝업류(unpair 등)와 동일 패턴 */
static bool inject_fn_delete_stats(void *arg)
{
    (void)arg;
    if (!s_stats_delete_btn) return false;
    lv_obj_send_event(s_stats_delete_btn, LV_EVENT_CLICKED, NULL);  /* -> cb_delete_stats_tap -> show_confirm_popup */
    lv_obj_t *confirm = find_widget_by_event_cb(s_last_modal, cb_confirm_yes_trampoline);
    if (!confirm) return false;
    lv_obj_send_event(confirm, LV_EVENT_CLICKED, NULL);  /* -> cb_confirm_yes_trampoline -> cb_delete_stats_confirmed */
    return true;
}

bool ui_main_inject_delete_stats(void)
{
    return run_on_lvgl_task(inject_fn_delete_stats, NULL, 1000);
}

static bool inject_fn_list_refresh(void *arg)
{
    (void)arg;
    if (!s_camera_renew_btn) return false;
    uint32_t before = esp_now_photo_list_get_current_generation();
    lv_obj_send_event(s_camera_renew_btn, LV_EVENT_CLICKED, NULL);  /* -> cb_renew_list */
    uint32_t after = esp_now_photo_list_get_current_generation();
    return after != before;  /* 눌렸지만 cb_renew_list의 가드(선택된 CAM 없음 등)에 걸리면
                                 세대가 그대로라 실패로 판정됨 */
}

/* 성공 시 실제로 새로 생긴 세대번호(main.c가 esp_now_photo_list_wait_result()에 넘길 것) */
uint32_t ui_main_inject_list_refresh(bool *out_ok)
{
    *out_ok = run_on_lvgl_task(inject_fn_list_refresh, NULL, 1000);
    return esp_now_photo_list_get_current_generation();
}

static bool inject_fn_photo_select(void *arg)
{
    uint32_t file_id = (uint32_t)(uintptr_t)arg;
    uint32_t child_cnt = lv_obj_get_child_cnt(s_photo_list);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *row = lv_obj_get_child(s_photo_list, i);
        /* 2026-09-04(웹 합성용) — 행 생성 시 lv_obj_set_user_data()로 file_id를 매달아둠
         * (refresh_photo_list_ui() 참고) — 이벤트 콜백 user_data(공개 API로 못 읽음)와는
         * 별개인 위젯 자체의 범용 슬롯 */
        if ((uint32_t)(uintptr_t)lv_obj_get_user_data(row) == file_id) {
            lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);  /* -> cb_photo_row_select */
            return true;
        }
    }
    return false;  /* 지금 목록에 없는 file_id(웹이 가져간 목록이 낡았을 수 있음) */
}

bool ui_main_inject_photo_select(uint32_t file_id)
{
    return run_on_lvgl_task(inject_fn_photo_select, (void *)(uintptr_t)file_id, 1000);
}

/* 2026-09-07(임시 진단 — 사용자 실측 리포트: "탭 선택 몇 번 하고 나면 3K가 줄어드는데") —
 * 탭 전환 자체는 지금 LVGL 콘텐츠 스크롤일 뿐 새 위젯 할당 이유가 없어야 하는데, 실측으로
 * 반복 드랍이 확인돼서 전환마다 직접 찍어봄 */
/* 2026-09-07 — 통계/설정/로그 탭 콘텐츠는 이제 실제 탭이 아니라 팝업(open_*_popup)이라,
 * 탭바 클릭으로 인덱스가 바뀌어도 즉시 0(상황판)으로 되돌리고 대신 팝업을 염. 같은 이벤트
 * 사이클 안에서 set_active(0)을 호출해 시각적 깜빡임 없이 처리됨(LVGL이 다음 프레임에
 * 한번에 그림) */
/* 2026-09-08(사용자 재설계) — 진짜 탭전환: 떠나는 탭의 콘텐츠는 지우고(teardown_*_tab),
 * 들어가는 탭의 콘텐츠를 그 자리에 지음(build_*_tab). 상황판(인덱스 0)은 항상 상주라
 * 대상에서 제외 */
/* 2026-09-08(사용자 지시 — "네트워크는 누르면 STA인 경우 wiFi scan이 나오면 좋겠고") —
 * STA 모드면 WiFi 스캔 팝업을 바로 열고(cb_network_find_btn 재사용), AP 모드면 스캔할
 * 대상이 없으므로 설정 팝업(네트워크 행)을 염 */
static void cb_network_ctrl_tap(lv_event_t *e)
{
    if (!device_config_get_wifi_ap_mode()) {
        cb_network_find_btn(e);
        return;
    }
    cb_settings_btn_tap(e);
}

void ui_init(void)
{
    lv_demo_widgets_components_init();  /* profile/analytics가 쓰는 공용 스타일/폰트 초기화 */

    /* 판넬 JPEG 디코드 버퍼 — 부팅 시 한 번만 잡고 계속 재사용(위 s_photo_jpeg_buf
     * 선언부 주석 참고) */
    ui_log_add("INIT free PSRAM(after font load)=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    s_photo_jpeg_buf = jpeg_calloc_align(PHOTO_PANEL_BUF_CAP, 16);
    if (s_photo_jpeg_buf) {
        ui_log_add("INIT panel_buf=OK free PSRAM=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        ESP_LOGE(TAG, "판넬 디코드 버퍼 할당 실패(%u bytes)", (unsigned)PHOTO_PANEL_BUF_CAP);
        ui_log_add_err(UI_ERR_PANEL_BUF_ALLOC, "Panel buffer alloc failed - photo preview unavailable");
    }

    s_power_log_buf = heap_caps_malloc(POWER_LOG_BUF_CAP, MALLOC_CAP_SPIRAM);
    if (s_power_log_buf) {
        s_power_log_buf[0] = '\0';
    } else {
        ESP_LOGE(TAG, "전력로그 버퍼 할당 실패(%u bytes)", (unsigned)POWER_LOG_BUF_CAP);
    }

    s_current_list = heap_caps_malloc(sizeof(esp_now_photo_list_view_item_t) * ESP_NOW_PHOTO_LIST_MAX,
                                       MALLOC_CAP_SPIRAM);
    if (!s_current_list) {
        ESP_LOGE(TAG, "사진목록 버퍼 할당 실패 — 목록 표시 불가");
    }

    s_dash_nodes      = heap_caps_malloc(sizeof(esp_now_hub_node_t) * ESP_NOW_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    s_dash_nodes_prev = heap_caps_malloc(sizeof(esp_now_hub_node_t) * ESP_NOW_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    s_camera_nodes      = heap_caps_malloc(sizeof(esp_now_hub_node_t) * ESP_NOW_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    s_camera_nodes_prev = heap_caps_malloc(sizeof(esp_now_hub_node_t) * ESP_NOW_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    s_sensor_nodes      = heap_caps_malloc(sizeof(esp_now_hub_node_t) * ESP_NOW_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    s_sensor_nodes_prev = heap_caps_malloc(sizeof(esp_now_hub_node_t) * ESP_NOW_HUB_MAX_NODES, MALLOC_CAP_SPIRAM);
    if (!s_dash_nodes || !s_dash_nodes_prev || !s_camera_nodes || !s_camera_nodes_prev ||
        !s_sensor_nodes || !s_sensor_nodes_prev) {
        ESP_LOGE(TAG, "노드 추적 버퍼 할당 실패 — 상황판/카메라/측정기 목록 표시 불가");
    }

    /* 2026-09-08(사용자 재설계 — "UI를 완전히 바꾸려고 해... 단일 화면") — lv_tabview 자체를
     * 없애고 주화면(구 상황판) 하나만 상주하는 화면 구조로 전환. 통계/설정은 상단바의
     * 버튼으로 여는 전체화면 팝업이 됨(아래 build_stats_popup/build_option_popup) */
    s_main_screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_main_screen);
    lv_obj_set_size(s_main_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_main_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_main_screen, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_bg_opa(s_main_screen, LV_OPA_COVER, 0);

    /* 상단바 — 구 tab_bar 자리(로고/시계) 재구성. 높이는 버튼/아이콘 크기에 맞춰 자연스럽게
     * 정해짐(구 75px 고정보다 낮아짐, 로고 이미지가 빠지고 아이콘들이 표준 버튼 높이로
     * 통일되므로) */
    s_top_bar = lv_obj_create(s_main_screen);
    lv_obj_set_size(s_top_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_top_bar, 12, 0);
    lv_obj_set_style_pad_ver(s_top_bar, 6, 0);
    lv_obj_set_style_border_width(s_top_bar, 0, 0);
    lv_obj_set_style_radius(s_top_bar, 0, 0);

    /* 2026-09-08(사용자 재수정 지시 — "상단바 순서를 로고 - 공백 - 시간-네트워크-상태-
     * Settings로") — 로고만 단독 좌측, 나머지는 전부 우측 클러스터 */
    s_logo_title = lv_label_create(s_top_bar);
    lv_label_set_text(s_logo_title, ui_str(STR_LOGO_TITLE));
    /* 2026-09-08(사용자 지시 — "폰트는 볼드(가능하면)에 크기가 좀 더 컸으면", "메모리
     * 더먹으면 안먹는 쪽으로") — 볼드 폰트 테이블을 새로 켜면 플래시가 더 드니 크기만 키움 */
    lv_obj_set_style_text_font(s_logo_title, ui_font_get(UI_FONT_SIZE_24), 0);

    lv_obj_t *top_bar_right = lv_obj_create(s_top_bar);
    lv_obj_set_size(top_bar_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_bar_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(top_bar_right, 0, 0);
    lv_obj_set_style_pad_column(top_bar_right, 10, 0);
    lv_obj_set_style_border_width(top_bar_right, 0, 0);

    /* 2026-09-08(사용자 수정 지시 — "지금 단추가 다 라운드스퀘어잖아... 직사각형 짙은
     * 회색배경에 흰글씨였어") — 다른 버튼들(둥근모서리)과 구분되는 눌림 표시. 시간/네트워크
     * 둘 다 같은 헬퍼로 만듦 */
    s_time_ctrl_label = lv_label_create(top_bar_right);
    style_inverted_control(s_time_ctrl_label);
    lv_obj_add_flag(s_time_ctrl_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_time_ctrl_label, cb_settime_btn, LV_EVENT_CLICKED, NULL);
    /* 2026-09-09(사용자 지적 — "시간표시 역상 크기가 시간 숫자에 따라 크기가 변하지
     * 않게") — Montserrat 숫자는 자간이 고정폭이 아니라("1"이 "8"보다 좁음), 매초 자릿수
     * 조합이 바뀌면서 라벨(자동폭)이 미세하게 늘었다 줄었다 함. "8"이 숫자 중 가장 넓은
     * 편이라("88:88:88") 그걸로 실측해서 그 폭으로 고정 + 가운데 정렬(실제 표시는 다음
     * refresh_clock() 틱에서 바로 진짜 시각으로 덮어써짐) */
    lv_label_set_text(s_time_ctrl_label, "88:88:88");
    lv_obj_update_layout(s_time_ctrl_label);
    /* 2026-09-09(사용자 지적 — "22:45:00 쯤이 되면 폭을 넘어서 가끔 두줄로 나옴") — 실측폭
     * 그대로 쓰면 특정 자릿수 조합에서 여유가 없어 순간적으로 줄바꿈됨. 여유값 추가 */
    lv_obj_set_width(s_time_ctrl_label, lv_obj_get_width(s_time_ctrl_label) + 12);
    lv_obj_set_style_text_align(s_time_ctrl_label, LV_TEXT_ALIGN_CENTER, 0);

    /* 네트워크 컨트롤 — AP/STA + SSID(가능하면), 탭하면: STA=WiFi 스캔 직접, AP=설정 팝업.
     * 2026-09-08(사용자 지시 — 폭 부족 대비) — 긴 SSID는 말줄임 */
    s_network_ctrl_label = lv_label_create(top_bar_right);
    style_inverted_control(s_network_ctrl_label);
    lv_label_set_long_mode(s_network_ctrl_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_network_ctrl_label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_network_ctrl_label, 220, 0);
    lv_obj_add_flag(s_network_ctrl_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_network_ctrl_label, cb_network_ctrl_tap, LV_EVENT_CLICKED, NULL);

    refresh_clock(NULL);  /* 첫 타이머 tick 전까지 빈 채로 안 보이게 즉시 한 번 채움 */
    lv_timer_create(refresh_clock, 1000, NULL);

    /* 2026-09-08(사용자 지시 — "상단바 통계 버튼을 없애고, 센서 판넬 Sensor 역상을 누르면
     * 열리게", "순서를... 시간-네트워크-상태-Settings로") — 통계 버튼 제거(빈 자리는 그냥
     * 둠), 상태아이콘이 Settings보다 먼저 오도록 순서 유지 */
    /* 상태 아이콘(정상/경고/에러) — 3개 다 만들어두고 상태에 맞는 것만 보임(구 로고/경고
     * 아이콘 이진 토글과 같은 패턴, 3단계로 확장). 전부 같은 크기(버튼 높이 기준) */
    lv_obj_t *status_icon_box = lv_obj_create(top_bar_right);
    lv_obj_remove_style_all(status_icon_box);
    lv_obj_set_size(status_icon_box, 32, 32);
    lv_obj_add_flag(status_icon_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(status_icon_box, cb_logo_warning_tap, LV_EVENT_CLICKED, NULL);

    /* 2026-09-08(사용자 지시 — "내가 생각한건 리프레시 모양의 회전하는 화살표 모양이었는데"
     * → 정지 상태로, 크기는 경고/에러 아이콘과 동일하게) */
    s_status_normal = lv_label_create(status_icon_box);
    lv_obj_center(s_status_normal);
    lv_label_set_text(s_status_normal, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(s_status_normal, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_status_normal, lv_palette_main(LV_PALETTE_GREEN), 0);

    s_status_warning = lv_label_create(status_icon_box);
    lv_obj_center(s_status_warning);
    lv_obj_add_flag(s_status_warning, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_status_warning, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(s_status_warning, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_status_warning, lv_color_hex(0xFFCC00), 0);

    /* 2026-09-09(사용자 지적 — "아이콘이 왜 이리 작아") — 원래 의도("크기는 경고/에러
     * 아이콘과 동일하게", 위 주석)와 다르게 원(24x24)과 안쪽 X(14pt)가 둘 다 Normal/Warning
     * 글리프(24pt, 32x32 박스)보다 작았음 — 박스/글리프 크기를 맞춤 */
    s_status_error = lv_obj_create(status_icon_box);
    lv_obj_remove_style_all(s_status_error);
    /* 2026-09-09(사용자 지적 — "에러 상태에서 에러 단추 안눌려") — lv_obj_create()는 기본
     * CLICKABLE이라 여기서 터치를 가로챈 뒤(LVGL은 기본적으로 이벤트 버블링을 안 함)
     * status_icon_box에 걸린 cb_logo_warning_tap까지 안 올라갔음. Normal/Warning은
     * 라벨(기본 비클릭)이라 자연스럽게 부모가 처리했던 것과 대비됨 — 이 원도 비클릭으로
     * 만들어 나머지 둘과 동일하게 부모가 처리하게 함 */
    lv_obj_remove_flag(s_status_error, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_status_error, 32, 32);
    lv_obj_center(s_status_error);
    lv_obj_add_flag(s_status_error, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(s_status_error, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_error, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(s_status_error, LV_OPA_COVER, 0);
    lv_obj_t *status_error_lbl = lv_label_create(s_status_error);
    lv_obj_center(status_error_lbl);
    lv_label_set_text(status_error_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(status_error_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(status_error_lbl, lv_color_white(), 0);

    s_settings_btn = lv_button_create(top_bar_right);
    lv_obj_add_event_cb(s_settings_btn, cb_settings_btn_tap, LV_EVENT_CLICKED, NULL);
    s_settings_btn_lbl = lv_label_create(s_settings_btn);
    lv_label_set_text(s_settings_btn_lbl, ui_str(STR_TAB_OPTION));
    lv_obj_set_style_text_font(s_settings_btn_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_timer_create(error_poll_tick, 200, NULL);

    /* 주화면 — 판넬 3개(요약/측정기/카메라), 1초마다 연결 상태 반영 */
    lv_obj_t *dashboard_page = lv_obj_create(s_main_screen);
    lv_obj_set_flex_grow(dashboard_page, 1);
    lv_obj_set_width(dashboard_page, LV_PCT(100));
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
    /* 2026-08-21 — 요약 맨 윗줄에 웹 대시보드 접속 URL(사용자 지시). IP를 아직 못 받았으면
     * refresh_dashboard()가 숨김 처리함(빈 문자열 반환 시).
     * 2026-09-09(사용자 지적 — "Web : 까지는 밑줄 치지 마") — "Web " 접두문구와 URL을
     * 별도 라벨로 분리, 접두문구는 평범한 텍스트로 두고 URL 라벨에만 파란색+밑줄+클릭 적용 */
    /* 2026-09-09(사용자 지시 — "웹과 메모리도 한 줄로 넣을 수 있으면") — Web(조건부 숨김)과
     * Memory(항상 표시)를 한 줄에 나란히. Web 부분만 숨겨야 하므로 s_web_row는 그대로
     * 유지하고, 이걸 감싸는 상위 row(summary_top_row)에 Memory 라벨을 형제로 추가 —
     * s_web_row가 숨겨져도(IP 없음) flex가 그 공간을 안 차지해서 Memory만 남음 */
    /* 2026-09-10(사용자 지시 — "웹, 스토리지를 하나의 박스로 감싸는 게 낫겠다" ->
     * "웹 메모리(1줄) 스토리지(1줄)") — Web/Memory 행과 Storage 행을 한 박스(세로 2줄)로
     * 묶음. pad_all(10)을 이 바깥 박스로 옮기고, 안쪽 summary_top_row는 pad_all(0)+
     * pad_column(12)만 유지 — Memory/humidity 정렬 계산(바깥+안쪽 pad_all 합 = 기존 10)은
     * 그대로 보존됨 */
    lv_obj_t *summary_top_box = lv_obj_create(summary_box);
    lv_obj_set_size(summary_top_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(summary_top_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(summary_top_box, 0, 0);
    lv_obj_set_style_pad_all(summary_top_box, 10, 0);

    lv_obj_t *summary_top_row = lv_obj_create(summary_top_box);
    lv_obj_set_size(summary_top_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(summary_top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_width(summary_top_row, 0, 0);
    lv_obj_set_style_pad_all(summary_top_row, 0, 0);
    lv_obj_set_style_pad_column(summary_top_row, 12, 0);

    /* 2026-09-09 — 예전엔 Web 텍스트 길이와 무관하게 Memory가 절반 지점부터 시작하도록 고정
     * 50% 폭을 썼는데, summary_sub_row의 두 박스는 flex_grow(1)이라 폭 계산 방식이 달라서
     * (flex_grow는 간격을 뺀 나머지를 반으로 나눔, 고정 50%는 간격을 안 뺌) 미세하게
     * 어긋났음. summary_sub_row와 동일하게 flex_grow(1)로 바꿔 정렬 기준을 통일 */
    s_web_row = lv_obj_create(summary_top_row);
    lv_obj_set_height(s_web_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_web_row, 1);
    lv_obj_set_flex_flow(s_web_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_width(s_web_row, 0, 0);
    lv_obj_set_style_pad_all(s_web_row, 0, 0);
    lv_obj_add_flag(s_web_row, LV_OBJ_FLAG_HIDDEN);

    s_web_prefix_label = lv_label_create(s_web_row);
    lv_obj_set_style_text_font(s_web_prefix_label, ui_font_get(UI_FONT_SIZE_18), 0);
    /* 2026-09-09(사용자 지적 — "Web에는 콜론이 빠졌어") — 여기(부팅 시 실제로 그려지는
     * 곳)를 안 고치고 refresh_lang_texts() 쪽만 고쳤던 실수. 두 곳 다 동일 포맷이어야 함 */
    lv_label_set_text_fmt(s_web_prefix_label, "%s: ", ui_str(STR_LABEL_WEB));

    /* 2026-09-08(사용자 재설계 — "요약의 웹 주소를 링크표시(파란색, 밑줄)로 바꿔서 클릭하면
     * 뜨게 해") — 로고가 없어지면서 QR 팝업 트리거를 여기로 옮김(cb_logo_title_tap 재사용) */
    s_web_url_label = lv_label_create(s_web_row);
    lv_obj_set_style_text_font(s_web_url_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_color(s_web_url_label, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_decor(s_web_url_label, LV_TEXT_DECOR_UNDERLINE, 0);
    lv_obj_add_flag(s_web_url_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_web_url_label, cb_logo_title_tap, LV_EVENT_CLICKED, NULL);

    /* 2026-08-21 — 여유 메모리 상시 표시(사용자 지시) — refresh_dashboard()가 매 틱 텍스트를
     * 채움, 웹 URL과 달리 항상 값이 있어서 숨김 처리 없음. 2026-09-09부터 Web과 한 줄
     * (summary_top_row의 형제) */
    s_mem_status_label = lv_label_create(summary_top_row);
    lv_obj_set_flex_grow(s_mem_status_label, 1);
    lv_obj_set_style_text_font(s_mem_status_label, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 2026-09-10(사용자 설계 — "CNTL 메모리 밑에 SD 용량도 표시... Storage[%(Remain MB)]:
     * Picture xx(yy) / Measure zz(kk) / Total aa(bb)") — Memory 바로 아래 새 줄. 단위(%,
     * MB)는 앞 괄호 라벨 한 번만 쓰고 값 뒤에는 반복 안 함(사용자 지시).
     * 2026-09-10(사용자 지시 — "웹 메모리(1줄) 스토리지(1줄)") — summary_top_box의 둘째 줄
     * (summary_top_row의 형제) */
    s_storage_status_label = lv_label_create(summary_top_box);
    lv_obj_set_style_text_font(s_storage_status_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_text(s_storage_status_label, "");
    /* 2026-09-11(사용자 지시 — "상태버튼을 눌러서 조치하라고 문구 표시") — 조치는 상태
     * 아이콘(cb_logo_warning_tap) 쪽으로 일원화, 이 라벨 자체는 더 이상 탭 대상 아님.
     * 2026-09-11(사용자 지시 — "SD: 빼고만 빨간색으로 해") — #RRGGBB 텍스트# 인라인
     * 문법을 쓰려면 반드시 이걸 켜야 함(전에 이걸 빠뜨려서 "#ff0000 ..." 이 그대로
     * 문자로 찍혔던 버그가 있었음, 이번엔 잊지 않음) */
    lv_label_set_recolor(s_storage_status_label, true);

    /* 2026-09-08(연결 기능 주화면 이관, 사용자 설계) — 장치별 행은 전부 Sensor/Camera
     * 판넬로 이관, Summary에는 대신 실시간 순시치(온도/습도/CO2/암모니아)만 —
     * "Summary는 시스템이 잘 돌고 있는지 보여주려는 의도". 2026-09-09(사용자 지시 —
     * "통계처럼 보이길 바래 (두 줄로)") — 통계 Overview 판넬과 동일한 좌우 2열 구조(좌=온도/
     * CO2, 우=습도/암모니아)로, 4줄 세로나열 대신 2줄로 */
    lv_obj_t *summary_sub_row = lv_obj_create(summary_box);
    lv_obj_set_size(summary_sub_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(summary_sub_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_width(summary_sub_row, 0, 0);
    /* 2026-09-09(사용자 지시 — "4개 항목을 한 박스 안에 보이게") — 온도/습도/CO2/암모니아를
     * 감싸는 이 컨테이너 자체에 연한 회색 배경+둥근모서리+패딩("좋아", 사용자 승인) */
    lv_obj_set_style_bg_color(summary_sub_row, lv_palette_lighten(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_bg_opa(summary_sub_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(summary_sub_row, 8, 0);
    lv_obj_set_style_pad_all(summary_sub_row, 10, 0);
    lv_obj_set_style_pad_column(summary_sub_row, 12, 0);

    lv_obj_t *summary_left_box = lv_obj_create(summary_sub_row);
    lv_obj_set_flex_grow(summary_left_box, 1);
    lv_obj_set_height(summary_left_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(summary_left_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(summary_left_box, 0, 0);
    lv_obj_set_style_pad_all(summary_left_box, 0, 0);
    /* 2026-09-09(사용자 지적 — "글씨배경은 여전히 흰색") — 기본 테마가 이 박스에 불투명
     * 흰색을 칠해서 부모(summary_sub_row)의 회색이 안 보였음 */
    lv_obj_set_style_bg_opa(summary_left_box, LV_OPA_TRANSP, 0);

    s_summary_live_temp_label = lv_label_create(summary_left_box);
    lv_obj_set_style_text_font(s_summary_live_temp_label, ui_font_get(UI_FONT_SIZE_18), 0);
    s_summary_live_co2_label = lv_label_create(summary_left_box);
    lv_obj_set_style_text_font(s_summary_live_co2_label, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *summary_right_box = lv_obj_create(summary_sub_row);
    lv_obj_set_flex_grow(summary_right_box, 1);
    lv_obj_set_height(summary_right_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(summary_right_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(summary_right_box, 0, 0);
    lv_obj_set_style_pad_all(summary_right_box, 0, 0);
    lv_obj_set_style_bg_opa(summary_right_box, LV_OPA_TRANSP, 0);

    s_summary_live_humi_label = lv_label_create(summary_right_box);
    lv_obj_set_style_text_font(s_summary_live_humi_label, ui_font_get(UI_FONT_SIZE_18), 0);
    s_summary_live_nh3_label = lv_label_create(summary_right_box);
    lv_obj_set_style_text_font(s_summary_live_nh3_label, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *sensor_box = create_dashboard_panel(dashboard_page, STR_GROUP_SENSOR, 1);
    /* 2026-09-08(사용자 지시 — "센서 판넬 제목 Sensor를... 역상으로", "센서 판넬 Sensor
     * 역상을 누르면 열리게") — 통계는 전부 센서 데이터라 진입점을 여기로 옮김 */
    style_inverted_control(s_dash_title[1]);
    lv_obj_add_flag(s_dash_title[1], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_dash_title[1], cb_stats_btn_tap, LV_EVENT_CLICKED, NULL);
    s_sensor_empty = lv_label_create(sensor_box);
    lv_label_set_text(s_sensor_empty, ui_str(STR_PANEL_NO_SENSOR));
    lv_obj_set_style_text_font(s_sensor_empty, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 2026-09-08(연결 기능 주화면 이관) — "연결됨" 목록(s_summary_list와 동일 스타일).
     * 2026-09-09(사용자 지시 — "connected 표기는 필요 없어 보여") — 표제 라벨 제거, 목록
     * 자체(이름+상태+신호+배터리 행)만으로 충분 */
    s_sensor_dash_list = lv_obj_create(sensor_box);
    lv_obj_set_size(s_sensor_dash_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_sensor_dash_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(s_sensor_dash_list, 0, 0);
    lv_obj_set_style_bg_opa(s_sensor_dash_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_sensor_dash_list, 0, 0);
    lv_obj_set_style_pad_row(s_sensor_dash_list, 10, 0);
    lv_obj_add_flag(s_sensor_dash_list, LV_OBJ_FLAG_HIDDEN);  /* 초기값: 연결된 센서 없음 */

    /* "대기중" 목록 — 설정탭 sensor_group_box에 있던 것을 그대로 이관(위젯/타이머/새로고침
     * 함수 재사용, retarget만) */
    lv_obj_t *sensor_pending_row = lv_obj_create(sensor_box);
    lv_obj_set_size(sensor_pending_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(sensor_pending_row, 0, 0);
    lv_obj_set_style_pad_all(sensor_pending_row, 0, 0);
    s_sensor_pending_lbl = lv_label_create(sensor_pending_row);
    lv_label_set_text(s_sensor_pending_lbl, ui_str(STR_LABEL_PENDING));
    lv_obj_set_style_text_font(s_sensor_pending_lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_style(s_sensor_pending_lbl, &style_text_muted, 0);
    lv_obj_add_flag(s_sensor_pending_lbl, LV_OBJ_FLAG_HIDDEN);  /* 초기값: 대기중인 센서 없음 */
    s_sensor_list = lv_list_create(sensor_box);
    lv_obj_set_size(s_sensor_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_add_flag(s_sensor_list, LV_OBJ_FLAG_HIDDEN);
    s_sensor_list_timer = lv_timer_create(refresh_sensor_list, 1000, NULL);

    lv_obj_t *camera_box = create_dashboard_panel(dashboard_page, STR_GROUP_CAMERA, 2);
    s_camera_box = camera_box;
    /* 2026-09-08(사용자 지시 — "카메라 판넬 제목 Camera도" 역상, "한 개도 없으면 역상 속
     * 글씨가 밝은 회색으로 안눌린다는 표현") — 초기값은 0대 상태(회색, 클릭 비활성) —
     * refresh_dashboard()가 cam_count 바뀔 때마다 다시 계산(아래 참고). 2026-09-08(카메라
     * 팝업 추출) — 센서판넬 제목과 동일 원칙으로 클릭 가능하게 만들되, cb_camera_btn_tap이
     * s_camera_title_enabled_prev(회색/흰색 상태)를 직접 봐서 0대일 때는 탭이 안 먹게 막음 */
    style_inverted_control(s_dash_title[2]);
    lv_obj_set_style_text_color(s_dash_title[2], lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_add_flag(s_dash_title[2], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_dash_title[2], cb_camera_btn_tap, LV_EVENT_CLICKED, NULL);
    s_camera_empty = lv_label_create(camera_box);
    lv_label_set_text(s_camera_empty, ui_str(STR_PANEL_NO_CAMERA));
    lv_obj_set_style_text_font(s_camera_empty, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 2026-09-08(카메라 팝업 추출) — 팝업이 닫혀있는 평상시 주화면에 남는 "연결된 카메라"
     * 목록. s_summary_list와 완전히 같은 스타일(요약판넬과 통일).
     * 2026-09-09(사용자 지시 — "connected 표기는 필요 없어 보여") — 표제 라벨 제거 */
    s_camera_dash_list = lv_obj_create(camera_box);
    lv_obj_set_size(s_camera_dash_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_camera_dash_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(s_camera_dash_list, 0, 0);
    lv_obj_set_style_bg_opa(s_camera_dash_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_camera_dash_list, 0, 0);
    lv_obj_set_style_pad_row(s_camera_dash_list, 10, 0);
    lv_obj_add_flag(s_camera_dash_list, LV_OBJ_FLAG_HIDDEN);  /* 초기값: 연결된 CAM 없음 */

    /* 2026-09-08(연결 기능 주화면 이관) — "대기중" 목록, 설정탭 camera_group_box에 있던
     * 것을 그대로 이관(위젯/타이머/새로고침 함수 재사용, retarget만) */
    lv_obj_t *camera_pending_row = lv_obj_create(camera_box);
    lv_obj_set_size(camera_pending_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(camera_pending_row, 0, 0);
    lv_obj_set_style_pad_all(camera_pending_row, 0, 0);
    s_camera_pending_lbl = lv_label_create(camera_pending_row);
    lv_label_set_text(s_camera_pending_lbl, ui_str(STR_LABEL_PENDING));
    lv_obj_set_style_text_font(s_camera_pending_lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_style(s_camera_pending_lbl, &style_text_muted, 0);
    lv_obj_add_flag(s_camera_pending_lbl, LV_OBJ_FLAG_HIDDEN);  /* 초기값: 대기중인 CAM 없음 */
    s_camera_list = lv_list_create(camera_box);
    lv_obj_set_size(s_camera_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_add_flag(s_camera_list, LV_OBJ_FLAG_HIDDEN);
    s_camera_list_timer = lv_timer_create(refresh_camera_list, 1000, NULL);

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
    lv_obj_set_style_pad_ver(s_camera_select_dd, 7, 0);  /* 2026-09-07 — 위아래 패딩 절반 */
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

    s_camera_renew_btn = lv_button_create(camera_btn_group);
    lv_obj_add_event_cb(s_camera_renew_btn, cb_renew_list, LV_EVENT_CLICKED, NULL);
    s_camera_renew_lbl = lv_label_create(s_camera_renew_btn);
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

    /* 버튼 폭 통일(2026-08-09, 사용자 지시) — 지금까지 만든 메인 화면(상황판) 버튼들의 실측
     * 자연폭 중 최댓값을 기준폭으로 잡아 적용. 팝업 버튼(add_modal_button)은 s_action_btn_width가
     * 여기서 설정된 뒤부터 뜨므로 자동으로 같은 폭을 받음. 사진목록의 del_btn(휴지통 아이콘,
     * 반복되는 작은 버튼)은 성격이 달라 제외.
     * 2026-09-08(탭→진짜 탭전환) — 설정탭(재시작/각 Apply/시각설정) 버튼들은 이제 부팅
     * 시점에 존재하지 않아(그 탭이 선택돼야 생김) 이 배열에서 뺐음 — 그 버튼들은
     * build_option_tab() 안에서 이미 정해진 s_action_btn_width를 그대로 적용만 함(재계산 없음) */
    lv_obj_t *action_buttons[] = { btn_capture, s_camera_renew_btn, btn_delete_all };
    lv_obj_update_layout(lv_screen_active());
    for (size_t i = 0; i < sizeof(action_buttons) / sizeof(action_buttons[0]); i++) {
        lv_coord_t w = lv_obj_get_width(action_buttons[i]);
        if (w > s_action_btn_width) s_action_btn_width = w;
    }
    for (size_t i = 0; i < sizeof(action_buttons) / sizeof(action_buttons[0]); i++) {
        lv_obj_set_width(action_buttons[i], s_action_btn_width);
        lv_obj_center(lv_obj_get_child(action_buttons[i], 0));  /* 레이블 중앙정렬(2026-08-09) */
    }

    /* 2026-09-04(사용자 설계: "이벤트로 처리해") — 사진/목록/연결 완료 이벤트에 앱 쪽 반응을
     * 등록. 매틱 폴링하던 refresh_dashboard()의 해당 부분은 제거하고 여기로 옮김 */
    esp_now_photo_set_ready_cb(on_photo_result_event);
    esp_now_photo_list_set_ready_cb(on_list_result_event);
    esp_now_hub_set_connect_event_cb(on_connect_result_event);
}

/* 2026-09-08(재설계) — 통계 팝업 닫기: 타이머 삭제 + 팝업 전체 삭제(lv_obj_delete — 이제
 * 실제 탭이 아니라 진짜 팝업이라 닫으면 완전히 없어짐) + 위젯 핸들 NULL */
static void cb_close_stats_popup(lv_event_t *e)
{
    (void)e;
    teardown_stats_tab();
}

static void teardown_stats_tab(void)
{
    size_t heap_before_close = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (s_stats_page_timer) { lv_timer_delete(s_stats_page_timer); s_stats_page_timer = NULL; }
    lv_obj_delete(s_stats_popup);
    s_stats_popup = NULL;
    s_stats_popup_title = NULL;
    s_stats_tab_built = false;
    size_t heap_after_close = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "MEMDIAG 통계팝업 닫기: internal %u -> %u (회수 %d bytes)",
             (unsigned)heap_before_close, (unsigned)heap_after_close,
             (int)heap_after_close - (int)heap_before_close);
    s_stats_overview_title = NULL;
    s_stats_scale_dd = NULL;
    s_overview_temp_label = NULL;
    s_overview_humi_label = NULL;
    s_overview_co2_label = NULL;
    s_overview_nh3_label = NULL;
    s_stats_pager = NULL;
    s_stats_table_view = NULL;
    s_stats_graph_view = NULL;
    s_stats_delete_btn = NULL;
    s_stats_delete_lbl = NULL;
    s_stats_table_header_lbl = NULL;
    s_stats_jump_prev_btn = NULL;
    s_stats_jump_prev_lbl = NULL;
    s_stats_prev_btn = NULL;
    s_stats_prev_lbl = NULL;
    s_stats_page_label = NULL;
    s_stats_next_btn = NULL;
    s_stats_next_lbl = NULL;
    s_stats_jump_next_btn = NULL;
    s_stats_jump_next_lbl = NULL;
    s_stats_table = NULL;
    /* 2026-09-11 — 스와이프 직후 팝업이 바로 닫히면 lv_async_call이 다음 루프에서 이미
     * 삭제된 위젯을 건드릴 수 있어 취소(use-after-free 방지) */
    lv_async_call_cancel(stats_graph_swipe_async_refresh, NULL);
    s_stats_gap_chart = NULL;
    s_stats_graph_swipe_hint = NULL;
    s_stats_chart_tap_label = NULL;
    s_stats_chart_tap_shown_series = -1;
    s_stats_graph_xaxis_row = NULL;
    for (int i = 0; i < STATS_GRAPH_X_LABEL_MAX; i++) s_stats_graph_x_labels[i] = NULL;
    s_stats_graph_max_row = NULL;
    s_stats_graph_min_row = NULL;
    for (int i = 0; i < STATS_GRAPH_SERIES_COUNT; i++) {
        s_stats_graph_max_label[i] = NULL;
        s_stats_graph_min_label[i] = NULL;
    }
    s_stats_graph_offset = 0;  /* 다음에 다시 열 땐 "지금" 창부터 */
}

/* 2026-09-11(사용자 지시 — "스크롤 바와 스크롤러블을 이 화면내에서 다 없애고 진행해") —
 * 개별 조상마다 하나씩 SCROLLABLE을 빼다가 chart_checkbox_row처럼 빠뜨리는 경우가 생김
 * (버튼 2개 추가로 그 줄이 넘쳐서 가로 스크롤바 발생, 사용자가 직접 목격). 통계 팝업 전체
 * 서브트리를 한 번에 순회하며 SCROLLABLE 플래그 + 스크롤바 자체를 다 꺼서 빠짐없이 처리 */
static void disable_scroll_recursive(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        disable_scroll_recursive(lv_obj_get_child(obj, i));
    }
}

/* 2026-09-08(재설계 — 상단바 통계 버튼이 여는 전체화면 팝업) */
static void build_stats_tab(void)
{
    if (s_stats_tab_built) return;  /* 이미 열려있음 */
    s_stats_tab_built = true;

    lv_obj_t *stats_page = create_page_popup();
    s_stats_popup = stats_page;
    add_page_popup_header(stats_page, ui_str(STR_TAB_STATISTICS), cb_close_stats_popup, &s_stats_popup_title);
    lv_obj_set_style_pad_hor(stats_page, 4, 0);
    lv_obj_set_style_pad_bottom(stats_page, 4, 0);
    lv_obj_set_style_pad_row(stats_page, 4, 0);
    lv_obj_set_style_bg_color(stats_page, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_bg_opa(stats_page, LV_OPA_COVER, 0);

    /* 2026-09-07(임시 진단 — 사용자 지시: "통계탭에서 소모되는 메모리들을 측정해") — 어젯밤
     * 부팅단계별 프로파일링과 동일 기법, 이번엔 통계탭 위젯 생성 구간만 잘라서 측정 */
    size_t heap_before_stats_tab = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    /* 2026-09-06(사용자 지시) — 일반로그/전력로그는 새 "로그" 탭(4번째)으로 이동함(아래
     * log_page 생성부 참고, 위젯/변수는 그대로 재사용). 이 탭은 이제 실제 시계열 통계 —
     * 그래프(Y=값, X=시간, 1h/12h/24h/1주일)는 다음 단계, 이번엔 최대/최소 판넬 +
     * 페이지네이션 값 테이블만 구현 */
    /* 개괄 판넬(2026-09-07 재설계, 구 "최대/최소 판넬") — 제목+Scale은 서브판넬 바깥
     * (전체폭), 그 아래 좌=온도/이산화탄소, 우=습도/암모니아 서브판넬(사용자 확정) */
    lv_obj_t *overview_box = lv_obj_create(stats_page);
    lv_obj_set_size(overview_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(overview_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(overview_box, 6, 0);

    lv_obj_t *overview_header_row = lv_obj_create(overview_box);
    lv_obj_set_size(overview_header_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(overview_header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(overview_header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(overview_header_row, 0, 0);
    lv_obj_set_style_pad_all(overview_header_row, 0, 0);

    s_stats_overview_title = lv_label_create(overview_header_row);
    lv_label_set_text(s_stats_overview_title, ui_str(STR_PANEL_STATS_OVERVIEW));
    lv_obj_set_style_text_font(s_stats_overview_title, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 2026-09-07(사용자 지시 — "Overview(좌정렬) - 공간 - 우정렬 드랍다운, 모두지우기") —
     * 3개를 그냥 SPACE_BETWEEN에 나란히 두면 Scale이 가운데 어중간한 자리에 뜸. Scale+삭제를
     * 하나의 묶음으로 만들어서 그 묶음 자체를 오른쪽 끝에 붙임(제목은 왼쪽 끝 그대로) */
    lv_obj_t *overview_header_right = lv_obj_create(overview_header_row);
    lv_obj_set_size(overview_header_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(overview_header_right, LV_FLEX_FLOW_ROW);
    /* 2026-09-07 — stats_nav_cluster와 동일 이유로 세로 CENTER 명시(안 그러면 기본값 TOP) */
    lv_obj_set_flex_align(overview_header_right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(overview_header_right, 0, 0);
    lv_obj_set_style_pad_column(overview_header_right, 8, 0);
    lv_obj_set_style_border_width(overview_header_right, 0, 0);

    s_stats_scale_dd = lv_dropdown_create(overview_header_right);
    lv_dropdown_set_options(s_stats_scale_dd, ui_str(STR_STATS_SCALE_OPTIONS));
    lv_dropdown_set_selected(s_stats_scale_dd, 0);
    lv_obj_set_style_pad_ver(s_stats_scale_dd, 7, 0);  /* 2026-09-07 — 위아래 패딩 절반 */
    lv_obj_set_style_text_font(s_stats_scale_dd, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_stats_scale_dd), ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_event_cb(s_stats_scale_dd, cb_stats_scale_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_stats_delete_btn = lv_button_create(overview_header_right);
    lv_obj_add_event_cb(s_stats_delete_btn, cb_delete_stats_tap, LV_EVENT_CLICKED, NULL);
    s_stats_delete_lbl = lv_label_create(s_stats_delete_btn);
    lv_label_set_text(s_stats_delete_lbl, ui_str(STR_BTN_DELETE_STATS));
    /* 2026-09-08(사용자 지시 — "통계의 버튼들은 다 높이가 작네... 폰트도 표준 폰트로") */
    lv_obj_set_style_text_font(s_stats_delete_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *overview_sub_row = lv_obj_create(overview_box);
    lv_obj_set_size(overview_sub_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(overview_sub_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_width(overview_sub_row, 0, 0);
    lv_obj_set_style_pad_all(overview_sub_row, 0, 0);
    lv_obj_set_style_pad_column(overview_sub_row, 12, 0);

    lv_obj_t *overview_left_box = lv_obj_create(overview_sub_row);
    lv_obj_set_flex_grow(overview_left_box, 1);
    lv_obj_set_height(overview_left_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(overview_left_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(overview_left_box, 0, 0);
    lv_obj_set_style_pad_all(overview_left_box, 0, 0);

    s_overview_temp_label = lv_label_create(overview_left_box);
    lv_obj_set_style_text_font(s_overview_temp_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_text(s_overview_temp_label, "");

    s_overview_co2_label = lv_label_create(overview_left_box);
    lv_obj_set_style_text_font(s_overview_co2_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_text(s_overview_co2_label, "");

    lv_obj_t *overview_right_box = lv_obj_create(overview_sub_row);
    lv_obj_set_flex_grow(overview_right_box, 1);
    lv_obj_set_height(overview_right_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(overview_right_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(overview_right_box, 0, 0);
    lv_obj_set_style_pad_all(overview_right_box, 0, 0);

    s_overview_humi_label = lv_label_create(overview_right_box);
    lv_obj_set_style_text_font(s_overview_humi_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_text(s_overview_humi_label, "");

    s_overview_nh3_label = lv_label_create(overview_right_box);
    lv_obj_set_style_text_font(s_overview_nh3_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_text(s_overview_nh3_label, "");

    /* 테이블<->그래프 스와이프 전환(2026-09-07, 사용자 설계 — "사용자는 그래프만 보거나
     * 테이블만 보는 형태"). 그래프 자체는 다음 단계, 지금은 뼈대(전환+자리)만 */
    s_stats_pager = lv_obj_create(stats_page);
    lv_obj_set_size(s_stats_pager, LV_PCT(100), 0);
    lv_obj_set_flex_grow(s_stats_pager, 1);
    lv_obj_set_style_pad_all(s_stats_pager, 0, 0);
    lv_obj_set_style_border_width(s_stats_pager, 0, 0);
    /* 2026-09-11(그래프 스와이프 진단 — 코드+LVGL 공식 문서 확인: "Gestures are not triggered
     * if a widget is being scrolled") — 차트/그래프뷰 자체는 이미 스크롤 꺼놨지만, 터치가
     * 처음 눌리는 지점부터 화면까지 이어지는 조상 체인 중 스크롤 가능한 게 하나라도 남아
     * 있으면 거기서 드래그를 스크롤로 먼저 채가서 제스처 자체가 안 생김 — 이 팝업은 내용이
     * 화면에 정확히 맞게 설계돼 있어 스크롤할 이유가 없으므로, 체인 전체(페이저+팝업
     * 오버레이)에서 스크롤을 꺼서 원천 차단 */
    lv_obj_remove_flag(s_stats_pager, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(stats_page, LV_OBJ_FLAG_SCROLLABLE);

    s_stats_table_view = lv_obj_create(s_stats_pager);
    lv_obj_set_size(s_stats_table_view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_stats_table_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_stats_table_view, 6, 0);
    lv_obj_set_style_border_width(s_stats_table_view, 0, 0);

    /* 헤더(항목/값/시간) — 테이블 밖으로 분리해서 스크롤 안 되게 고정(사용자 지시:
     * "스크롤 안되야되. 이게 어려우면 테이블 밖에 둬도 되"), 우측 끝에 현재/전체 페이지도
     * 같이 표시(사용자 지시: "그 헤더 줄에 같이 넣으면") */
    lv_obj_t *stats_table_header_row = lv_obj_create(s_stats_table_view);
    lv_obj_set_size(stats_table_header_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stats_table_header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_table_header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(stats_table_header_row, 0, 0);
    lv_obj_set_style_pad_all(stats_table_header_row, 4, 0);

    s_stats_table_header_lbl = lv_label_create(stats_table_header_row);
    lv_obj_set_style_text_font(s_stats_table_header_lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_text_fmt(s_stats_table_header_lbl, "%s / %s / %s", ui_str(STR_STATS_TABLE_HEADER_ITEM),
                           ui_str(STR_STATS_TABLE_HEADER_VALUE), ui_str(STR_STATS_TABLE_HEADER_TIME));

    /* 2026-09-07(사용자 지시 — "테이블 이동 단추 4개는 제목줄로 옮겨") — ±1/±10 버튼과
     * 페이지표시를 전부 한 묶음으로 헤더 우측에 배치(구 하단 버튼줄/우하단 오버레이 제거) */
    lv_obj_t *stats_nav_cluster = lv_obj_create(stats_table_header_row);
    lv_obj_set_size(stats_nav_cluster, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stats_nav_cluster, LV_FLEX_FLOW_ROW);
    /* 2026-09-07 버그수정 — flex_align을 안 줘서 기본값(START/START)으로 자식들이 위쪽
     * 정렬되고 있었음(사용자 지적: "페이지 레이블... TOP으로 되있는 것 같아"). 세로는
     * CENTER로 버튼들과 나란히 맞춤 */
    lv_obj_set_flex_align(stats_nav_cluster, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(stats_nav_cluster, 0, 0);
    lv_obj_set_style_pad_column(stats_nav_cluster, 4, 0);
    lv_obj_set_style_border_width(stats_nav_cluster, 0, 0);

    /* 2026-09-08(사용자 지시 — "역상처리로 바꿔. Disable도 카메라와 같은 convention으로") —
     * style_inverted_control()이 흰 글씨를 buttons에 지정하면 자식 라벨로 상속(LVGL
     * text_color는 상속 속성) — LV_STATE_DISABLED 오버라이드만 따로 얹으면 자동 전환됨
     * (Camera 판넬 제목처럼 수동 추적 불필요, LVGL 상태 시스템이 대신 처리) */
    s_stats_jump_prev_btn = lv_button_create(stats_nav_cluster);
    style_inverted_control(s_stats_jump_prev_btn);
    lv_obj_set_style_text_color(s_stats_jump_prev_btn, lv_palette_lighten(LV_PALETTE_GREY, 1), LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_stats_jump_prev_btn, stats_jump_prev_page_cb, LV_EVENT_CLICKED, NULL);
    s_stats_jump_prev_lbl = lv_label_create(s_stats_jump_prev_btn);
    lv_label_set_text(s_stats_jump_prev_lbl, ui_str(STR_BTN_JUMP_PREV10));
    lv_obj_set_style_text_font(s_stats_jump_prev_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    s_stats_prev_btn = lv_button_create(stats_nav_cluster);
    style_inverted_control(s_stats_prev_btn);
    lv_obj_set_style_text_color(s_stats_prev_btn, lv_palette_lighten(LV_PALETTE_GREY, 1), LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_stats_prev_btn, stats_prev_page_cb, LV_EVENT_CLICKED, NULL);
    s_stats_prev_lbl = lv_label_create(s_stats_prev_btn);
    lv_label_set_text(s_stats_prev_lbl, ui_str(STR_BTN_PREV_PAGE));
    lv_obj_set_style_text_font(s_stats_prev_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    s_stats_page_label = lv_label_create(stats_nav_cluster);
    lv_obj_set_style_text_font(s_stats_page_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_label_set_text(s_stats_page_label, "");

    s_stats_next_btn = lv_button_create(stats_nav_cluster);
    style_inverted_control(s_stats_next_btn);
    lv_obj_set_style_text_color(s_stats_next_btn, lv_palette_lighten(LV_PALETTE_GREY, 1), LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_stats_next_btn, stats_next_page_cb, LV_EVENT_CLICKED, NULL);
    s_stats_next_lbl = lv_label_create(s_stats_next_btn);
    lv_label_set_text(s_stats_next_lbl, ui_str(STR_BTN_NEXT_PAGE));
    lv_obj_set_style_text_font(s_stats_next_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    s_stats_jump_next_btn = lv_button_create(stats_nav_cluster);
    style_inverted_control(s_stats_jump_next_btn);
    lv_obj_set_style_text_color(s_stats_jump_next_btn, lv_palette_lighten(LV_PALETTE_GREY, 1), LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_stats_jump_next_btn, stats_jump_next_page_cb, LV_EVENT_CLICKED, NULL);
    s_stats_jump_next_lbl = lv_label_create(s_stats_jump_next_btn);
    lv_label_set_text(s_stats_jump_next_lbl, ui_str(STR_BTN_JUMP_NEXT10));
    lv_obj_set_style_text_font(s_stats_jump_next_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    s_stats_table = lv_table_create(s_stats_table_view);
    lv_obj_set_width(s_stats_table, LV_PCT(100));
    lv_obj_set_flex_grow(s_stats_table, 1);
    lv_table_set_column_count(s_stats_table, 3);
    lv_table_set_column_width(s_stats_table, 0, 300);
    lv_table_set_column_width(s_stats_table, 1, 180);
    lv_table_set_column_width(s_stats_table, 2, 150);
    lv_obj_set_style_text_font(s_stats_table, ui_font_get(UI_FONT_SIZE_18), 0);
    /* 줄간격 절반으로 축소(사용자 지시: "줄 간격이 너무 넓어. 반으로 줄여봐") */
    lv_obj_set_style_pad_ver(s_stats_table, 2, LV_PART_ITEMS);
    /* 선택(탭 시 셀 하이라이트) 비활성화(사용자 지시: "아예 선택이 안되야되") — CLICKABLE
     * 자체를 끄면 제스처 인식(아래)까지 같이 죽으므로, 눌림 상태 배경만 투명 처리해서
     * 시각적으로만 무효화 */
    lv_obj_set_style_bg_opa(s_stats_table, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
    /* 세로 스와이프로 페이지 이동, 좌측 스와이프로 그래프 전환(사용자 설계) */
    lv_obj_add_event_cb(s_stats_table, cb_stats_table_gesture, LV_EVENT_GESTURE, NULL);

    /* "<<" 오버레이(좌측 끝, 사용자 설계: 테이블에서 좌측 스와이프/이 버튼 둘 다로 그래프 전환) */
    lv_obj_t *table_to_graph_btn = lv_button_create(s_stats_table_view);
    lv_obj_add_flag(table_to_graph_btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(table_to_graph_btn, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_add_event_cb(table_to_graph_btn, cb_switch_to_graph_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_t *table_to_graph_lbl = lv_label_create(table_to_graph_btn);
    lv_label_set_text(table_to_graph_lbl, "<<");
    lv_obj_set_style_text_font(table_to_graph_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 2026-09-10(사용자 설계 — "라인+도트", "계열 4개 선택 표시", "탭하면 값") — 실제
     * lv_chart. 계열 순서 고정: 0=온도(빨강) 1=습도(파랑) 2=CO2(까망) 3=암모니아(청록) */
    s_stats_graph_view = lv_obj_create(s_stats_pager);
    lv_obj_set_size(s_stats_graph_view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_stats_graph_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(s_stats_graph_view, 0, 0);
    lv_obj_set_style_pad_all(s_stats_graph_view, 4, 0);
    lv_obj_set_style_pad_row(s_stats_graph_view, 4, 0);
    lv_obj_add_flag(s_stats_graph_view, LV_OBJ_FLAG_HIDDEN);

    lv_color_t chart_colors[STATS_GRAPH_SERIES_COUNT] = {
        lv_palette_main(LV_PALETTE_RED), lv_palette_main(LV_PALETTE_BLUE),
        lv_color_black(), lv_palette_darken(LV_PALETTE_YELLOW, 2)  /* 짙은 노랑, 2026-09-10 사용자 지시로 청록에서 변경 */
    };
    ui_str_id_t series_label_ids[STATS_GRAPH_SERIES_COUNT] = {
        STR_CHAN_LABEL_TEMP_C, STR_CHAN_LABEL_HUMI_PCT, STR_CHAN_LABEL_CO2_PPM, STR_CHAN_LABEL_NH3_PPM
    };

    lv_obj_t *chart_checkbox_row = lv_obj_create(s_stats_graph_view);
    lv_obj_set_size(chart_checkbox_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chart_checkbox_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_width(chart_checkbox_row, 0, 0);
    lv_obj_set_style_pad_all(chart_checkbox_row, 0, 0);
    lv_obj_set_style_pad_column(chart_checkbox_row, 8, 0);

    for (int s = 0; s < STATS_GRAPH_SERIES_COUNT; s++) {
        s_stats_chart_checkbox[s] = lv_checkbox_create(chart_checkbox_row);
        lv_checkbox_set_text(s_stats_chart_checkbox[s], ui_str(series_label_ids[s]));
        lv_obj_add_state(s_stats_chart_checkbox[s], LV_STATE_CHECKED);
        lv_obj_set_style_text_font(s_stats_chart_checkbox[s], ui_font_get(UI_FONT_SIZE_18), 0);
        lv_obj_set_style_text_color(s_stats_chart_checkbox[s], chart_colors[s], LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_stats_chart_checkbox[s], chart_colors[s], LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_stats_chart_checkbox[s], cb_stats_chart_series_toggle, LV_EVENT_VALUE_CHANGED,
                             (void *)(intptr_t)s);
    }

    /* 2026-09-11(사용자 지시 — "스와이프, 더블탭 다 없애고 그냥 버튼으로 해야겠다") — 제스처/
     * 더블탭 둘 다 실기에서 인식이 안 되거나 불안정해서 완전히 버리고, 가장 단순/확실한
     * LV_EVENT_CLICKED 버튼만 남김. "우측 끝 정렬"(사용자 지시) — 체크박스는 왼쪽에 그대로 두고
     * 사이에 flex_grow 스페이서를 둬서 버튼 두 개만 행의 오른쪽 끝으로 밀어냄 */
    lv_obj_t *pan_btn_spacer = lv_obj_create(chart_checkbox_row);
    lv_obj_remove_style_all(pan_btn_spacer);
    lv_obj_set_size(pan_btn_spacer, 1, 1);
    lv_obj_set_flex_grow(pan_btn_spacer, 1);

    lv_obj_t *pan_left_btn = lv_button_create(chart_checkbox_row);
    lv_obj_t *pan_left_lbl = lv_label_create(pan_left_btn);
    lv_label_set_text(pan_left_lbl, "<<");
    lv_obj_add_event_cb(pan_left_btn, cb_stats_graph_pan_left_tap, LV_EVENT_CLICKED, NULL);

    lv_obj_t *pan_right_btn = lv_button_create(chart_checkbox_row);
    lv_obj_t *pan_right_lbl = lv_label_create(pan_right_btn);
    lv_label_set_text(pan_right_lbl, ">>");
    lv_obj_add_event_cb(pan_right_btn, cb_stats_graph_pan_right_tap, LV_EVENT_CLICKED, NULL);

    s_stats_chart = lv_chart_create(s_stats_graph_view);
    lv_obj_set_size(s_stats_chart, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_stats_chart, 1);
    lv_chart_set_type(s_stats_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_stats_chart, STATS_GRAPH_POINT_COUNT);
    lv_chart_set_axis_range(s_stats_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(s_stats_chart, 3, 0);
    lv_obj_add_event_cb(s_stats_chart, cb_stats_chart_tap, LV_EVENT_CLICKED, NULL);
    for (int s = 0; s < STATS_GRAPH_SERIES_COUNT; s++) {
        s_stats_chart_series[s] = lv_chart_add_series(s_stats_chart, chart_colors[s], LV_CHART_AXIS_PRIMARY_Y);
    }
    /* 2026-09-11(사용자 지적 — "상단에 4계열 최대값, 하단에 4계열 최소값") — 정규화상
     * 100=그 계열의 실제 최대, 0=실제 최소라서 차트 맨 위/맨 아래와 항상 일치. 표시여부/
     * 값은 매 갱신마다 refresh_stats_graph()가 정함 */
    s_stats_graph_max_row = lv_obj_create(s_stats_chart);
    lv_obj_remove_style_all(s_stats_graph_max_row);
    lv_obj_add_flag(s_stats_graph_max_row, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_stats_graph_max_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_stats_graph_max_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_stats_graph_max_row, 6, 0);
    lv_obj_align(s_stats_graph_max_row, LV_ALIGN_TOP_MID, 0, 0);

    s_stats_graph_min_row = lv_obj_create(s_stats_chart);
    lv_obj_remove_style_all(s_stats_graph_min_row);
    lv_obj_add_flag(s_stats_graph_min_row, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_stats_graph_min_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_stats_graph_min_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_stats_graph_min_row, 6, 0);
    lv_obj_align(s_stats_graph_min_row, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* 2026-09-12(사용자 지시 — "최대/최소값을 현재 탭하면 값 보여주는 것처럼 하얀 박스 위에
     * 글씨를 써") — 방금 지운 탭-값 박스와 동일한 스타일(흰 배경 80% 불투명, 패딩4, radius4),
     * 글자색만 그 계열 색 그대로 유지 */
    for (int s = 0; s < STATS_GRAPH_SERIES_COUNT; s++) {
        s_stats_graph_max_label[s] = lv_label_create(s_stats_graph_max_row);
        lv_obj_add_flag(s_stats_graph_max_label[s], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(s_stats_graph_max_label[s], ui_font_get(UI_FONT_SIZE_12), 0);
        lv_obj_set_style_text_color(s_stats_graph_max_label[s], chart_colors[s], 0);
        lv_obj_set_style_bg_color(s_stats_graph_max_label[s], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_stats_graph_max_label[s], LV_OPA_80, 0);
        lv_obj_set_style_pad_all(s_stats_graph_max_label[s], 4, 0);
        lv_obj_set_style_radius(s_stats_graph_max_label[s], 4, 0);
        /* 2026-09-12(사용자 지시 — "암모니아는 잘 안보일 수 있잖아. 글씨에 까만 테두리
         * 둘 수 있어?") — LVGL 9 텍스트 외곽선 스타일, 모든 계열에 공통 적용 */
        lv_obj_set_style_text_outline_stroke_color(s_stats_graph_max_label[s], lv_color_black(), 0);
        lv_obj_set_style_text_outline_stroke_width(s_stats_graph_max_label[s], 1, 0);
        lv_obj_set_style_text_outline_stroke_opa(s_stats_graph_max_label[s], LV_OPA_COVER, 0);
        lv_label_set_text(s_stats_graph_max_label[s], "");

        s_stats_graph_min_label[s] = lv_label_create(s_stats_graph_min_row);
        lv_obj_add_flag(s_stats_graph_min_label[s], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(s_stats_graph_min_label[s], ui_font_get(UI_FONT_SIZE_12), 0);
        lv_obj_set_style_text_color(s_stats_graph_min_label[s], chart_colors[s], 0);
        lv_obj_set_style_bg_color(s_stats_graph_min_label[s], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_stats_graph_min_label[s], LV_OPA_80, 0);
        lv_obj_set_style_pad_all(s_stats_graph_min_label[s], 4, 0);
        lv_obj_set_style_radius(s_stats_graph_min_label[s], 4, 0);
        lv_obj_set_style_text_outline_stroke_color(s_stats_graph_min_label[s], lv_color_black(), 0);
        lv_obj_set_style_text_outline_stroke_width(s_stats_graph_min_label[s], 1, 0);
        lv_obj_set_style_text_outline_stroke_opa(s_stats_graph_min_label[s], LV_OPA_COVER, 0);
        lv_label_set_text(s_stats_graph_min_label[s], "");
    }
    /* 2026-09-11(그래프 재설계 — "라인+도트인데 라인만 그리는 걸로") — 점마커(LV_PART_INDICATOR)
     * 크기를 0으로 — 선만 남음 */
    lv_obj_set_style_width(s_stats_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_stats_chart, 0, LV_PART_INDICATOR);

    /* 2026-09-11(그래프 재설계 — "값이 없는 슬롯마다 그 자리에 점만") — 실데이터 차트와
     * 정확히 같은 위치/크기로 겹쳐그리는 두 번째 차트. LVGL은 자식을 부모 위에 그리므로
     * s_stats_chart의 자식으로 만들면 따로 정렬 계산 없이 자동으로 겹침. 선은 숨기고
     * (LV_PART_ITEMS 폭 0) 점마커만 보이게 — 위 실데이터 차트와 반대 스타일 */
    s_stats_gap_chart = lv_chart_create(s_stats_chart);
    lv_obj_set_size(s_stats_gap_chart, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_stats_gap_chart, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(s_stats_gap_chart, 0, 0);
    lv_obj_remove_flag(s_stats_gap_chart, LV_OBJ_FLAG_CLICKABLE);  /* 탭이 밑 실데이터 차트로 전달되게 */
    lv_obj_set_style_bg_opa(s_stats_gap_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_stats_gap_chart, 0, 0);
    lv_chart_set_type(s_stats_gap_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_stats_gap_chart, STATS_GRAPH_POINT_COUNT);
    lv_chart_set_axis_range(s_stats_gap_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(s_stats_gap_chart, 0, 0);
    lv_obj_set_style_line_width(s_stats_gap_chart, 0, LV_PART_ITEMS);  /* 선 숨김 */
    lv_obj_set_style_width(s_stats_gap_chart, 2, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_stats_gap_chart, 2, LV_PART_INDICATOR);
    for (int s = 0; s < STATS_GRAPH_SERIES_COUNT; s++) {
        s_stats_gap_series[s] = lv_chart_add_series(s_stats_gap_chart, chart_colors[s], LV_CHART_AXIS_PRIMARY_Y);
    }

    /* 2026-09-12(사용자 지시 — 좌상단 고정 박스는 지우고 탭 위치에 다시 만듦, "연한 노란색은
     * 눌러서 나온 경험을 제공하려는 거야" — 즉 상시 표시되는 min/max 범례(흰색)와 의도적으로
     * 다른 색으로 구분. "암모니아는 잘 안보일 수 있잖아" → 검정 글자 외곽선 추가) — 위치는
     * 매 탭마다 cb_stats_chart_tap()이 실제 탭 지점으로 재배치, CLICKABLE을 꺼서 다음 탭이
     * 이 박스가 아니라 밑 차트로 그대로 전달되게 함(원 설계: "must NOT intercept taps") */
    s_stats_chart_tap_label = lv_label_create(s_stats_chart);
    lv_obj_add_flag(s_stats_chart_tap_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_stats_chart_tap_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_stats_chart_tap_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(s_stats_chart_tap_label, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_bg_color(s_stats_chart_tap_label, lv_palette_lighten(LV_PALETTE_YELLOW, 4), 0);
    lv_obj_set_style_bg_opa(s_stats_chart_tap_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_stats_chart_tap_label, 4, 0);
    lv_obj_set_style_radius(s_stats_chart_tap_label, 4, 0);
    lv_obj_set_style_text_outline_stroke_color(s_stats_chart_tap_label, lv_color_black(), 0);
    lv_obj_set_style_text_outline_stroke_width(s_stats_chart_tap_label, 1, 0);
    lv_obj_set_style_text_outline_stroke_opa(s_stats_chart_tap_label, LV_OPA_COVER, 0);
    lv_label_set_text(s_stats_chart_tap_label, "");

    /* 2026-09-11(사용자 지시 — "스와이프, 더블탭 다 없애고 그냥 버튼으로") — 버튼 클릭 시에도
     * 잠깐 보였다가 실제 갱신 끝나면 숨겨짐(stats_graph_pan/stats_graph_swipe_async_refresh) */
    s_stats_graph_swipe_hint = lv_label_create(s_stats_chart);
    lv_obj_add_flag(s_stats_graph_swipe_hint, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_stats_graph_swipe_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_stats_graph_swipe_hint, ui_font_get(UI_FONT_SIZE_30), 0);
    lv_obj_set_style_bg_color(s_stats_graph_swipe_hint, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_stats_graph_swipe_hint, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(s_stats_graph_swipe_hint, 8, 0);
    lv_obj_set_style_radius(s_stats_graph_swipe_hint, 6, 0);
    lv_obj_align(s_stats_graph_swipe_hint, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_stats_graph_swipe_hint, "");

    /* 2026-09-11(그래프 재설계 항목5) — 상대 X축 라벨 줄(차트 바로 아래, 고정 높이).
     * 최대 개수만큼 미리 만들어두고 매 갱신 때 그 스케일에 필요한 개수만 보이게 함 */
    s_stats_graph_xaxis_row = lv_obj_create(s_stats_graph_view);
    lv_obj_set_size(s_stats_graph_xaxis_row, LV_PCT(100), 18);
    lv_obj_set_style_border_width(s_stats_graph_xaxis_row, 0, 0);
    lv_obj_set_style_pad_all(s_stats_graph_xaxis_row, 0, 0);
    for (int i = 0; i < STATS_GRAPH_X_LABEL_MAX; i++) {
        s_stats_graph_x_labels[i] = lv_label_create(s_stats_graph_xaxis_row);
        lv_obj_add_flag(s_stats_graph_x_labels[i], LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_style_text_font(s_stats_graph_x_labels[i], ui_font_get(UI_FONT_SIZE_12), 0);
        lv_label_set_text(s_stats_graph_x_labels[i], "");
    }

    lv_obj_t *graph_to_table_btn = lv_button_create(s_stats_graph_view);
    /* 2026-09-10(사용자 지적 — "그래프 아래 좌측에 있어... 우측 가운데로 옮겨") — 그래프뷰가
     * 체크박스행+차트+탭라벨 때문에 COLUMN flex가 됐는데, 이 버튼은 IGNORE_LAYOUT 없이
     * lv_obj_align()만 줘서 flex가 정렬을 덮어씀(테이블 쪽 "<<" 버튼과 동일하게 고쳐야 함) */
    lv_obj_add_flag(graph_to_table_btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(graph_to_table_btn, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_add_event_cb(graph_to_table_btn, cb_switch_to_table_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_t *graph_to_table_lbl = lv_label_create(graph_to_table_btn);
    lv_label_set_text(graph_to_table_lbl, ">>");
    lv_obj_set_style_text_font(graph_to_table_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    s_stats_page_timer = lv_timer_create(refresh_stats_page, 2000, NULL);

    /* 이 시점까지 만들어진 통계 팝업 전체(테이블뷰+그래프뷰 포함)를 순회하며 스크롤 완전 차단.
     * 테이블 행(refresh_stats_table)은 이후 주기적으로 새로 생성되지만 그쪽은 생성 시점에
     * 개별적으로 이미 SCROLLABLE을 빼고 있음(기존 코드) */
    disable_scroll_recursive(stats_page);

    size_t heap_after_stats_tab = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "MEMDIAG 통계탭 위젯 생성 비용: internal %u -> %u (소모 %d bytes)",
             (unsigned)heap_before_stats_tab, (unsigned)heap_after_stats_tab,
             (int)heap_before_stats_tab - (int)heap_after_stats_tab);
}

static void cb_stats_btn_tap(lv_event_t *e)
{
    (void)e;
    /* 2026-09-11(사용자 지시 — "조치를 취하지 않고... 통계 팝업을 열려고 시도할 때, SD
     * 불량이라 통계 팝업을 열 수 없다고 알려야되") — SD I/O 에러 활성 중엔 팝업 자체를
     * 안 열고 안내만 표시 */
    if (s_sd_io_fail_active) {
        show_alert_popup(ui_str(STR_MSG_STATS_BLOCKED_SD_FAIL));
        return;
    }
    build_stats_tab();
}

/* 2026-09-08(카메라 팝업 추출) — 카메라판넬 제목 탭. s_camera_title_enabled_prev가 false면
 * (연결된 CAM 0대 — 회색 상태) 아무 것도 안 함, 센서와 달리 카메라는 "탭 불가" 표현이
 * 이미 회색으로 나가 있으므로 그 약속을 실제 동작에서도 지킴(사용자 설계: "제목
 * (탭가능/불가능)") */
static void cb_camera_btn_tap(lv_event_t *e)
{
    (void)e;
    if (!s_camera_title_enabled_prev) return;
    build_camera_tab();
}

static void cb_close_camera_popup(lv_event_t *e)
{
    (void)e;
    teardown_camera_tab();
}

/* 2026-09-08(카메라 팝업 추출) — 통계/설정과 달리 콘텐츠를 새로 안 짓고 기존
 * toolbar(s_camera_content)/split_row(s_camera_split_row)를 이 팝업으로 재부모화만 함.
 * 그 안의 드롭다운/목록/미리보기에 걸린 기존 이벤트 콜백·비동기 이벤트(on_photo_result_event
 * 등)는 위젯이 죽지 않으므로 손댈 필요가 없음 — refresh_dashboard()가 이미 매 틱 그
 * 내용을 최신으로 유지해줌(부모가 어디든 무관) */
static void build_camera_tab(void)
{
    if (s_camera_popup) return;  /* 이미 열려있음 */

    lv_obj_t *popup = create_page_popup();
    s_camera_popup = popup;
    add_page_popup_header(popup, ui_str(STR_GROUP_CAMERA), cb_close_camera_popup, &s_camera_popup_title);
    lv_obj_set_style_pad_hor(popup, 4, 0);

    lv_obj_set_parent(s_camera_content, popup);
    lv_obj_set_parent(s_camera_split_row, popup);
    /* 팝업은 카메라판넬 제목이 클릭 가능(=연결된 CAM 1대 이상)할 때만 열리므로 항상
     * "연결됨" 상태 — 다음 refresh_dashboard() 틱까지 기다리지 않고 즉시 보이게 함 */
    lv_obj_remove_flag(s_camera_content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_camera_split_row, LV_OBJ_FLAG_HIDDEN);
}

static void teardown_camera_tab(void)
{
    if (!s_camera_popup) return;
    size_t heap_before_close = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    lv_obj_set_parent(s_camera_content, s_camera_box);
    lv_obj_set_parent(s_camera_split_row, s_camera_box);
    /* 주화면으로 돌아온 toolbar/split_row는 무조건 숨김(주화면은 s_camera_dash_list만
     * 보여줌) — 다음 틱의 camera_connected 재계산을 기다리면 그 사이 한 틱 동안 주화면에
     * 툴바가 보이는 깜빡임이 생길 수 있어 여기서 바로 정리 */
    lv_obj_add_flag(s_camera_content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_camera_split_row, LV_OBJ_FLAG_HIDDEN);

    lv_obj_delete(s_camera_popup);
    s_camera_popup = NULL;
    s_camera_popup_title = NULL;

    size_t heap_after_close = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "MEMDIAG 카메라팝업 닫기: internal %u -> %u (회수 %d bytes)",
             (unsigned)heap_before_close, (unsigned)heap_after_close,
             (int)heap_after_close - (int)heap_before_close);
}

/* ════════════════════════════════════════════════════════════
 * 개별설정 팝업(2026-09-08, 사용자 설계 — 연결 기능 주화면 이관) — 연결된 장치 행 탭.
 * Alias 편집 + [센서만]측정주기 편집 + 연결끊기. 측정주기 위젯/로직(select_sensor,
 * s_sens_measure_dd, cb_apply_sens_measure_interval 등)은 예전 설정탭 것을 그대로 재사용 —
 * 여기로 옮겨 지어질 뿐 아무 로직도 안 바뀜.
 * ════════════════════════════════════════════════════════════ */
static void cb_device_keyboard_hide(lv_event_t *e)
{
    (void)e;
    if (s_device_keyboard) lv_obj_add_flag(s_device_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void cb_device_alias_ta_focused(lv_event_t *e)
{
    (void)e;
    if (!s_device_keyboard) return;
    lv_obj_remove_flag(s_device_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_device_keyboard, s_device_alias_ta);
}

/* 2026-09-09(사용자 최종 설계 — "Alias도 Apply 버튼 넣고, 눌렀을 때만 적용, 안 누르고
 * 닫으면 적용 안 되게") — 측정주기/촬영주기와 동일한 [값][Apply] 패턴으로 정정. 이전의
 * "포커스 해제/닫기 시 자동저장" 방식은 전부 제거 — Apply를 누른 값만 저장됨 */
static void update_device_alias_apply_enabled(void)
{
    if (!s_device_alias_ta || !s_device_alias_apply_btn) return;
    bool changed = (strcmp(lv_textarea_get_text(s_device_alias_ta), s_device_alias_applied_text) != 0);
    if (changed) lv_obj_clear_state(s_device_alias_apply_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(s_device_alias_apply_btn, LV_STATE_DISABLED);
}

static void cb_device_alias_ta_changed(lv_event_t *e)
{
    (void)e;
    update_device_alias_apply_enabled();
}

/* 2026-09-09(사용자 지적 — "주화면에는 표시가 안되고") — Apply된 순간이 유일한 커밋
 * 지점이므로, 여기서 저장과 동시에 주화면 목록도 갱신되게 강제(다음 refresh_dashboard()
 * 틱에서 dash_changed로 처리되어 Summary/Sensor/Camera 대시 목록이 전부 다시 그려짐 —
 * Alias 저장처럼 드문 조작이라 비용 무시 가능) */
static void cb_device_alias_apply_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_device_alias_ta) return;
    const char *text = lv_textarea_get_text(s_device_alias_ta);
    device_config_set_alias(s_device_popup_node.mac, text);
    strncpy(s_device_alias_applied_text, text, sizeof(s_device_alias_applied_text) - 1);
    s_device_alias_applied_text[sizeof(s_device_alias_applied_text) - 1] = '\0';
    update_device_alias_apply_enabled();
    s_dash_count_prev = -1;  /* 강제 재생성 — force_camera/sensor_list_redraw()와 동일 원칙 */
}

static void cb_device_disconnect_confirm(void *ctx)
{
    esp_now_hub_node_t *node = (esp_now_hub_node_t *)ctx;
    esp_now_hub_unpair(node->mac);
    teardown_device_popup();
}

static void cb_device_disconnect_clicked(lv_event_t *e)
{
    (void)e;
    char msg[64];
    snprintf(msg, sizeof(msg), "%s\n%s", s_device_popup_node.name, ui_str(STR_MSG_UNPAIR_CONFIRM));
    show_confirm_popup(msg, cb_device_disconnect_confirm, &s_device_popup_node);
}

static void cb_close_device_popup(lv_event_t *e)
{
    (void)e;
    ESP_LOGW(TAG, "MEMDIAG 개별설정 팝업 X 탭 수신 t=%u", (unsigned)lv_tick_get());
    teardown_device_popup();
}

/* 2026-09-08(연결 기능 주화면 이관) — "연결됨" 대시 목록 행 탭 -> 개별설정 팝업. user_data는
 * 그 rebuild 세대의 행 인덱스(refresh_dashboard 참고) — s_*_dash_row_count 범위 밖이면
 * 이미 재생성된 뒤라 무시(방어) */
/* 2026-09-09(사용자 지시 — "팝업이 늦게 열리는 건지, 탭 이벤트를 씹었는지 구분이 안되니
 * 모니터링 할 필요가 있어") — 탭 수신 시각을 여기서, 팝업 완성 시각을 build_device_popup()
 * 끝에서 각각 로그로 남김. 이 로그가 아예 안 뜨면 탭 자체가 안 먹은 것, 이 로그는 뜨는데
 * 팝업 완성 로그와 시간차가 크면 진짜 느린 것 — 시리얼 캡처로 구분 가능해짐 */
static void cb_camera_dash_row_clicked(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    uintptr_t idx = (uintptr_t)lv_obj_get_user_data(row);
    ESP_LOGW(TAG, "MEMDIAG 카메라 행 탭 수신 t=%u idx=%u/%d", (unsigned)lv_tick_get(), (unsigned)idx, s_camera_dash_row_count);
    if ((int)idx >= s_camera_dash_row_count) return;
    build_device_popup(s_camera_dash_row_macs[idx], s_camera_dash_row_names[idx], false);
}

static void cb_sensor_dash_row_clicked(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    uintptr_t idx = (uintptr_t)lv_obj_get_user_data(row);
    ESP_LOGW(TAG, "MEMDIAG 센서 행 탭 수신 t=%u idx=%u/%d", (unsigned)lv_tick_get(), (unsigned)idx, s_sensor_dash_row_count);
    if ((int)idx >= s_sensor_dash_row_count) return;
    build_device_popup(s_sensor_dash_row_macs[idx], s_sensor_dash_row_names[idx], true);
}

static void teardown_device_popup(void)
{
    if (!s_device_popup) return;
    /* 2026-09-09(사용자 최종 설계 — "안 누르고 닫으면 적용시키지 않도록") — 자동저장 없음,
     * Apply 안 누른 편집 내용은 그냥 버려짐(측정주기/촬영주기와 동일 원칙) */
    /* 2026-09-09(사용자 지적 — "X로 닫았을 때 키보드 안닫혀", 이어서 "탭해도 키보드 안떠") —
     * s_device_keyboard는 s_device_popup의 자식이 아니라 화면(lv_screen_active()) 직속
     * 형제라 팝업만 지우면 안 지워지는 것도 문제였지만, 숨기기만 하고 재사용하면 그
     * 키보드는 화면의 "오래된" 자식으로 남아서 다음에 새로 만들어지는 팝업(더 나중에 추가된
     * 자식이라 위에 그려짐)에 가려짐 — 숨김을 풀어도 화면상 안 보임. WiFi 비번 입력창의
     * s_wifi_keyboard와 동일하게 완전히 삭제(다음 사용 때 새로 만들어져 항상 최상단) */
    if (s_device_keyboard) { lv_obj_delete(s_device_keyboard); s_device_keyboard = NULL; }
    lv_obj_delete(s_device_popup);
    s_device_popup = NULL;
    s_device_popup_title = NULL;
    s_device_alias_ta = NULL;
    s_device_alias_apply_btn = NULL;
    /* 2026-09-08 — 측정주기 위젯도 이 팝업 자식이라 팝업과 함께 사라짐, 핸들 NULL로
     * 정리(build_option_tab 등 다른 곳의 기존 teardown 패턴과 동일) */
    s_sens_measure_dd = NULL;
    s_sens_measure_apply_btn = NULL;
    s_sens_measure_label = NULL;
    s_sens_measure_apply_lbl = NULL;
    s_sens_measure_applied_idx = -1;
    s_device_disconnect_btn = NULL;
    /* 다음에 팝업이 다시 열릴 때(같은 mac이라도) select_sensor()가 무조건 새 위젯을
     * 다시 동기화하도록 강제 — 아래 build_device_popup() 참고 */
    s_has_selected_sensor = false;

    ESP_LOGW(TAG, "MEMDIAG 개별설정 팝업 닫기 완료 t=%u", (unsigned)lv_tick_get());
}

static void build_device_popup(const uint8_t *mac, const char *name, bool is_sensor)
{
    if (s_device_popup) return;  /* 이미 열려있음 */

    memcpy(s_device_popup_node.mac, mac, 6);
    strncpy(s_device_popup_node.name, name ? name : "", sizeof(s_device_popup_node.name) - 1);
    s_device_popup_node.name[sizeof(s_device_popup_node.name) - 1] = '\0';
    s_device_popup_is_sensor = is_sensor;

    lv_obj_t *popup = create_page_popup();
    s_device_popup = popup;
    const char *alias = device_config_get_alias(mac);
    /* 2026-09-09(사용자 지시 — "팝업 제목은 Sensor/Camera로 고정") — 어떤 장치를 열든
     * 제목은 판넬 종류로 고정, Alias/장치명은 본문에만 표시 */
    add_page_popup_header(popup, ui_str(is_sensor ? STR_GROUP_SENSOR : STR_GROUP_CAMERA),
                           cb_close_device_popup, &s_device_popup_title);
    lv_obj_set_style_pad_hor(popup, 12, 0);

    /* 2026-09-09(사용자 설계 — "장치명 - 공백 - Alias: 텍스트 입력창으로 바꿔") — 한 줄:
     * [장치명] ... [Alias: 입력창] */
    lv_obj_t *alias_row = lv_obj_create(popup);
    lv_obj_set_size(alias_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(alias_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(alias_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(alias_row, 0, 0);
    lv_obj_set_style_pad_all(alias_row, 0, 0);
    lv_obj_set_style_pad_column(alias_row, 12, 0);

    lv_obj_t *orig_name_lbl = lv_label_create(alias_row);
    lv_label_set_text(orig_name_lbl, s_device_popup_node.name);
    lv_obj_set_style_text_font(orig_name_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *alias_cluster = lv_obj_create(alias_row);
    lv_obj_set_flex_grow(alias_cluster, 1);
    lv_obj_set_height(alias_cluster, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(alias_cluster, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(alias_cluster, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(alias_cluster, 0, 0);
    lv_obj_set_style_pad_all(alias_cluster, 0, 0);
    lv_obj_set_style_pad_column(alias_cluster, 6, 0);

    lv_obj_t *alias_lbl = lv_label_create(alias_cluster);
    lv_label_set_text_fmt(alias_lbl, "%s:", ui_str(STR_LABEL_ALIAS));
    lv_obj_set_style_text_font(alias_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    s_device_alias_ta = lv_textarea_create(alias_cluster);
    lv_textarea_set_one_line(s_device_alias_ta, true);
    lv_textarea_set_max_length(s_device_alias_ta, DEVICE_CONFIG_ALIAS_MAX_LEN - 1);
    lv_textarea_set_placeholder_text(s_device_alias_ta, s_device_popup_node.name);
    if (alias[0] != '\0') lv_textarea_set_text(s_device_alias_ta, alias);
    lv_obj_set_flex_grow(s_device_alias_ta, 1);
    lv_obj_set_style_text_font(s_device_alias_ta, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_event_cb(s_device_alias_ta, cb_device_alias_ta_focused, LV_EVENT_FOCUSED, NULL);
    /* 2026-09-09(사용자 지적 — "키보드 감춤 후 텍스트창 탭해도 다시 안 나와") — 이미
     * 포커스된 상태로 키보드만 숨긴 경우, 다시 탭해도 FOCUSED가 재발화 안 됨(이미
     * 포커스 상태라). CLICKED는 포커스 여부와 무관하게 탭마다 뜨므로 같이 등록 —
     * cb_device_alias_ta_focused 자체는 이벤트 종류를 안 가리므로 그대로 재사용 가능 */
    lv_obj_add_event_cb(s_device_alias_ta, cb_device_alias_ta_focused, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_device_alias_ta, cb_device_alias_ta_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* 2026-09-09(사용자 최종 설계) — 측정주기/촬영주기와 동일한 Apply 버튼. applied_text를
     * 현재 저장값으로 초기화해두면 이 시점엔 텍스트와 같으니 자동으로 비활성 상태로 시작 */
    strncpy(s_device_alias_applied_text, alias, sizeof(s_device_alias_applied_text) - 1);
    s_device_alias_applied_text[sizeof(s_device_alias_applied_text) - 1] = '\0';
    s_device_alias_apply_btn = lv_button_create(alias_cluster);
    lv_obj_add_event_cb(s_device_alias_apply_btn, cb_device_alias_apply_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_device_alias_apply_btn, LV_STATE_DISABLED);
    lv_obj_t *alias_apply_lbl = lv_label_create(s_device_alias_apply_btn);
    lv_label_set_text(alias_apply_lbl, ui_str(STR_BTN_APPLY));
    lv_obj_set_style_text_font(alias_apply_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    if (!s_device_keyboard) {
        s_device_keyboard = lv_keyboard_create(lv_screen_active());
        lv_obj_add_event_cb(s_device_keyboard, cb_device_keyboard_hide, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_device_keyboard, cb_device_keyboard_hide, LV_EVENT_CANCEL, NULL);
    }
    lv_obj_add_flag(s_device_keyboard, LV_OBJ_FLAG_HIDDEN);  /* 텍스트박스 포커스 전까진 숨김 */

    /* 측정주기 행(센서만) — 예전 설정탭 sens_measure_row와 완전히 동일한 위젯 구성/이벤트,
     * 자리만 이 팝업 안으로 옮김 */
    if (is_sensor) {
        lv_obj_t *sens_measure_row = lv_obj_create(popup);
        lv_obj_set_size(sens_measure_row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(sens_measure_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sens_measure_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_border_width(sens_measure_row, 0, 0);
        lv_obj_set_style_pad_all(sens_measure_row, 0, 0);

        s_sens_measure_label = lv_label_create(sens_measure_row);
        lv_label_set_text(s_sens_measure_label, ui_str(STR_LABEL_SENS_MEASURE_INTERVAL));
        lv_obj_set_style_text_font(s_sens_measure_label, ui_font_get(UI_FONT_SIZE_18), 0);

        lv_obj_t *sens_measure_right = create_row_right_cluster(sens_measure_row);

        s_sens_measure_dd = lv_dropdown_create(sens_measure_right);
        lv_obj_set_style_pad_ver(s_sens_measure_dd, 7, 0);
        lv_dropdown_set_options(s_sens_measure_dd, ui_str(STR_OPT_SENS_MEASURE_INTERVAL_LIST));
        lv_obj_set_style_text_font(s_sens_measure_dd, ui_font_get(UI_FONT_SIZE_18), 0);
        lv_obj_set_style_text_font(lv_dropdown_get_list(s_sens_measure_dd), ui_font_get(UI_FONT_SIZE_18), 0);
        lv_obj_add_event_cb(s_sens_measure_dd, cb_sens_measure_interval_changed, LV_EVENT_VALUE_CHANGED, NULL);

        s_sens_measure_apply_btn = lv_button_create(sens_measure_right);
        lv_obj_add_event_cb(s_sens_measure_apply_btn, cb_apply_sens_measure_interval, LV_EVENT_CLICKED, NULL);
        lv_obj_add_state(s_sens_measure_apply_btn, LV_STATE_DISABLED);
        s_sens_measure_apply_lbl = lv_label_create(s_sens_measure_apply_btn);
        lv_label_set_text(s_sens_measure_apply_lbl, ui_str(STR_BTN_APPLY));
        lv_obj_set_style_text_font(s_sens_measure_apply_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

        select_sensor(mac);  /* s_has_selected_sensor가 teardown에서 false로 리셋돼있어 항상 재동기화 */
    }

    /* 연결끊기 버튼 — 맨 아래, 위험한 조작이라 확인팝업 거침(cb_device_disconnect_clicked) */
    lv_obj_t *disconnect_btn = lv_button_create(popup);
    s_device_disconnect_btn = disconnect_btn;
    lv_obj_set_style_bg_color(disconnect_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_add_event_cb(disconnect_btn, cb_device_disconnect_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *disconnect_lbl = lv_label_create(disconnect_btn);
    lv_label_set_text(disconnect_lbl, ui_str(STR_BTN_DISCONNECT));
    lv_obj_set_style_text_font(disconnect_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    ESP_LOGW(TAG, "MEMDIAG 개별설정 팝업 완성 t=%u", (unsigned)lv_tick_get());
}

/* 2026-09-08(재설계) — 설정 콘텐츠만 지움(s_option_content 안 자식들, lv_obj_clean) — 팝업
 * 자체(s_option_popup)는 안 건드림(로그로 바꿔치기할 때도 씀). 카메라/센서 연결목록의
 * 캐시(행 개수/오브젝트배열)도 같이 리셋 — 안 그러면 웹 인젝션(find_camera_row_by_mac 등)이
 * 이미 지워진 행을 계속 가리킬 수 있음 */
static void teardown_option_tab(void)
{
    size_t heap_before_close = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    /* 2026-09-08(연결 기능 주화면 이관) — s_camera_list_timer/s_sensor_list_timer와 그
     * 대상(s_camera_list/s_sensor_list, 이제 "대기중" 목록)은 더 이상 이 팝업 소유가
     * 아니라 주화면에 상주(ui_init()에서 한 번만 생성, 여기서 손 안 댐) */
    lv_obj_clean(s_option_content);
    size_t heap_after_close = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "MEMDIAG 설정 콘텐츠 지움: internal %u -> %u (회수 %d bytes)",
             (unsigned)heap_before_close, (unsigned)heap_after_close,
             (int)heap_after_close - (int)heap_before_close);
    memset(s_group_title, 0, sizeof(s_group_title));  /* 제어기/측정기/영상/시스템 그룹박스 제목 4개 */
    s_lang_label = NULL;
    s_btn_ko = NULL;
    s_btn_en = NULL;
    s_capture_interval_label = NULL;
    s_capture_interval_dd = NULL;
    s_capture_apply_btn = NULL;
    s_capture_apply_lbl = NULL;
    s_agc_label = NULL;
    s_agc_switch = NULL;
    s_aec_label = NULL;
    s_aec_switch = NULL;
    s_xclk_label = NULL;
    s_xclk_dd = NULL;
    s_xclk_apply_btn = NULL;
    s_xclk_apply_lbl = NULL;
    s_response_interval_label = NULL;
    s_response_interval_dd = NULL;
    s_response_apply_btn = NULL;
    s_response_apply_lbl = NULL;
    s_response_help_label = NULL;
    s_adaptive_response_label = NULL;
    s_adaptive_response_dd = NULL;
    s_adaptive_apply_btn = NULL;
    s_adaptive_apply_lbl = NULL;
    s_adaptive_help_label = NULL;
    s_restart_label = NULL;
    s_restart_btn_lbl = NULL;
    s_time_label = NULL;
    s_time_value_label = NULL;
    s_time_set_btn_lbl = NULL;
    s_network_label = NULL;
    s_network_mode_dd = NULL;
    s_network_right_label = NULL;
    s_network_find_btn = NULL;
    s_network_find_lbl = NULL;
    s_auto_connect_known_switch = NULL;
    s_auto_connect_known_label = NULL;
    s_auto_connect_new_switch = NULL;
    s_auto_connect_new_label = NULL;
}

/* 2026-09-08(재설계) — s_option_content(설정 팝업의 헤더 아래 콘텐츠 영역) 안에 설정
 * 내용을 지음. 팝업 자체가 이미 열려있다는 전제(cb_settings_btn_tap/로그↔설정 바꿔치기가
 * 호출) — 여기선 이미 지어져있는지 체크 안 함(호출자가 teardown 먼저 보장) */
static void build_option_tab(void)
{
    /* 2026-09-07(임시 진단 — 사용자 지시: "메모리가 더 줄어든 것 같아") — 통계탭과 동일 기법,
     * 설정탭(카메라/센서/시스템 그룹박스 전체) 위젯 생성 구간만 잘라서 측정 */
    size_t heap_before_option_tab = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    lv_obj_t *option_page = s_option_content;
    lv_obj_set_style_pad_hor(option_page, 5, 0);
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
    /* 2026-09-08(사용자 지시 — TTF/한글 임시 비활성 실험) — 라벨을 영문("Korean")으로
     * 바꾸고 비활성화. 한글 관련 코드/데이터는 안 지움(나중에 되살릴 수도 있다고 하심) —
     * 그냥 지금은 못 누르게만 함 */
    s_btn_ko = lv_checkbox_create(btn_group);
    lv_checkbox_set_text(s_btn_ko, "Korean");
    lv_obj_set_style_text_font(s_btn_ko, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_radius(s_btn_ko, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_btn_ko, cb_lang_ko, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_btn_ko, LV_STATE_DISABLED);

    s_btn_en = lv_checkbox_create(btn_group);
    lv_checkbox_set_text(s_btn_en, "English");
    lv_obj_set_style_radius(s_btn_en, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_btn_en, cb_lang_en, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_btn_en, LV_STATE_DISABLED);

    update_lang_buttons();  /* 초기 선택 상태(기본 UI_LANG_KO) 반영 */

    /* 자동연결 스위치 2개(2026-09-08, 사용자 설계 — 연결 기능 주화면 이관).
     * 2026-09-09(사용자 지시 — "이 두 줄은 System이 아니고 CNTL로 옮겨") — 시스템
     * 그룹박스에서 CNTL 그룹박스로 이동. "신규 접속 장치"가 "이전 연결 장치"를 사실상
     * 포함하는 더 넓은 옵션이라(사용자 설계) 위쪽에 둠(순서 반대로) */
    lv_obj_t *auto_new_row = lv_obj_create(cntl_box);
    lv_obj_set_size(auto_new_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(auto_new_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(auto_new_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(auto_new_row, 0, 0);
    lv_obj_set_style_pad_hor(auto_new_row, 12, 0);
    lv_obj_set_style_pad_ver(auto_new_row, 0, 0);

    s_auto_connect_new_label = lv_label_create(auto_new_row);
    lv_label_set_text(s_auto_connect_new_label, ui_str(STR_LABEL_AUTO_CONNECT_NEW));
    lv_obj_set_style_text_font(s_auto_connect_new_label, ui_font_get(UI_FONT_SIZE_18), 0);

    s_auto_connect_new_switch = lv_switch_create(auto_new_row);
    if (device_config_get_auto_connect_new()) lv_obj_add_state(s_auto_connect_new_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_auto_connect_new_switch, cb_auto_connect_new_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *auto_known_row = lv_obj_create(cntl_box);
    lv_obj_set_size(auto_known_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(auto_known_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(auto_known_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(auto_known_row, 0, 0);
    lv_obj_set_style_pad_hor(auto_known_row, 12, 0);
    lv_obj_set_style_pad_ver(auto_known_row, 0, 0);

    s_auto_connect_known_label = lv_label_create(auto_known_row);
    lv_label_set_text(s_auto_connect_known_label, ui_str(STR_LABEL_AUTO_CONNECT_KNOWN));
    lv_obj_set_style_text_font(s_auto_connect_known_label, ui_font_get(UI_FONT_SIZE_18), 0);

    s_auto_connect_known_switch = lv_switch_create(auto_known_row);
    if (device_config_get_auto_connect_known()) lv_obj_add_state(s_auto_connect_known_switch, LV_STATE_CHECKED);
    if (device_config_get_auto_connect_new()) {
        lv_obj_add_state(s_auto_connect_known_switch, LV_STATE_CHECKED);
        lv_obj_add_state(s_auto_connect_known_switch, LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(s_auto_connect_known_switch, cb_auto_connect_known_changed, LV_EVENT_VALUE_CHANGED, NULL);

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

    /* 네트워크 행(2026-08-29 설계, 2026-09-07 재정렬 — 사용자 지시: "레이블-공백-우정렬
     * 값/드랍다운 버튼 형식으로 통일") — [라벨:좌][공백][독립/종속 드롭다운 + IP/SSID/찾기
     * 버튼:우 묶음]. 다른 설정행들과 동일하게 create_row_right_cluster로 묶어 라벨과
     * 2분할되게 함(구 방식: 좌/우 flex_grow(1) 3분할 — 드롭다운이 가운데 애매하게 떴었음) */
    lv_obj_t *network_row = lv_obj_create(cntl_box);
    lv_obj_set_size(network_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(network_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(network_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(network_row, 0, 0);
    lv_obj_set_style_pad_hor(network_row, 12, 0);
    lv_obj_set_style_pad_ver(network_row, 0, 0);

    s_network_label = lv_label_create(network_row);
    lv_label_set_text(s_network_label, ui_str(STR_LABEL_NETWORK));
    lv_obj_set_style_text_font(s_network_label, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *network_cluster = create_row_right_cluster(network_row);

    s_network_mode_dd = lv_dropdown_create(network_cluster);
    lv_obj_set_style_pad_ver(s_network_mode_dd, 7, 0);  /* 2026-09-07 — 위아래 패딩 절반 */
    {
        char opts[64];
        snprintf(opts, sizeof(opts), "%s\n%s", ui_str(STR_NETWORK_MODE_AP), ui_str(STR_NETWORK_MODE_STA));
        lv_dropdown_set_options(s_network_mode_dd, opts);
    }
    lv_dropdown_set_selected(s_network_mode_dd, device_config_get_wifi_ap_mode() ? 0 : 1);
    lv_obj_set_style_text_font(s_network_mode_dd, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_network_mode_dd), ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_event_cb(s_network_mode_dd, cb_network_mode_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *network_right = lv_obj_create(network_cluster);
    lv_obj_set_size(network_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(network_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(network_right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(network_right, 0, 0);
    lv_obj_set_style_pad_all(network_right, 0, 0);

    s_network_right_label = lv_label_create(network_right);
    lv_obj_set_style_text_font(s_network_right_label, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 2026-08-29(사용자 지시) — 이 행만 특수해서(값 표시 겸 변경 진입점) 다른 화면의
     * 꽉 찬 버튼과 다르게, 아이폰 설정 스타일(값 라벨 + 옆에 눌리는 화살표)로 만듦. 순수
     * lv_obj 컨테이너를 클릭 가능하게 만들어서 "버튼처럼 안 보이지만 눌리는" 형태 —
     * Sens 대시보드의 리스트타일+화살표 전례와 같은 계열이지만 그건 너무 작았다고 해서
     * 폭은 표준 액션버튼의 2배(아래 s_action_btn_width*2), 폰트도 본문과 동일 18pt로 키움 */
    s_network_find_btn = lv_obj_create(network_right);
    lv_obj_remove_style_all(s_network_find_btn);
    lv_obj_set_height(s_network_find_btn, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_network_find_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_network_find_btn, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_network_find_btn, 6, 0);  /* 터치 타겟 여유 */
    lv_obj_add_flag(s_network_find_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_network_find_btn, cb_network_find_btn, LV_EVENT_CLICKED, NULL);

    s_network_find_lbl = lv_label_create(s_network_find_btn);
    lv_label_set_text(s_network_find_lbl, ui_str(STR_BTN_FIND));
    lv_obj_set_style_text_font(s_network_find_lbl, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_style(s_network_find_lbl, &style_text_muted, 0);  /* 아이폰 설정처럼 값은 톤 낮춤 */
    lv_obj_set_flex_grow(s_network_find_lbl, 1);
    lv_label_set_long_mode(s_network_find_lbl, LV_LABEL_LONG_DOT);  /* 긴 SSID는 말줄임 */

    lv_obj_t *network_chevron = lv_label_create(s_network_find_btn);
    lv_label_set_text(network_chevron, ">");  /* 유니코드 화살표(›) 대신 순수 ASCII —
                                                  글씨체 글리프 누락 위험 회피(RS485 프로젝트
                                                  한글 미지원 폰트로 겪은 것과 같은 함정) */
    lv_obj_set_style_text_font(network_chevron, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_style(network_chevron, &style_text_muted, 0);

    refresh_network_right_zone();  /* 부팅 직후 현재 상태 즉시 반영(빈 채로 안 보이게) */

    /* 2026-09-08(연결 기능 주화면 이관) — 측정기(Sensor) 그룹박스 자체를 제거함. 대기중/
     * 연결됨 리스트와 측정주기 행 둘 다 주화면(Sensor 판넬)+개별설정 팝업으로 이관돼서 이
     * 그룹박스엔 남는 내용이 없었음 */

    /* 영상(Camera) 그룹박스 — 대기중/연결됨 리스트는 주화면으로 이관, 여기는 공통설정만
     * 유지(촬영주기도 공통 — 사용자 지시: "카메라는 촬영 주기도 공통이야") */
    lv_obj_t *camera_group_box = create_group_box(option_page, STR_GROUP_CAMERA);

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

    lv_obj_t *capture_right = create_row_right_cluster(capture_row);

    s_capture_interval_dd = lv_dropdown_create(capture_right);
    lv_obj_set_style_pad_ver(s_capture_interval_dd, 7, 0);  /* 2026-09-07 — 위아래 패딩 절반 */
    lv_dropdown_set_options(s_capture_interval_dd, ui_str(STR_OPT_CAPTURE_INTERVAL_LIST));
    s_capture_interval_applied_idx = find_value_index(s_capture_interval_values,
        sizeof(s_capture_interval_values) / sizeof(s_capture_interval_values[0]),
        device_config_get_cam_capture_interval_sec());
    /* find_value_index가 -1(못 찾음)을 반환할 수 있음(2026-08-11) — 그대로 두면 applied_idx
     * 비교 로직(Apply 버튼 활성화 판단)에 -1이 정확히 필요하지만, 드롭다운 표시용 인덱스는
     * 항상 유효한 범위여야 하므로 여기서만 0으로 방어 */
    lv_dropdown_set_selected(s_capture_interval_dd,
        (uint16_t)(s_capture_interval_applied_idx >= 0 ? s_capture_interval_applied_idx : 0));
    /* 드롭다운 기본폰트는 한글 글리프가 없는 LVGL 내장 폰트 — 닫힌 상태 표시(MAIN)와 펼친
     * 목록(lv_dropdown_get_list) 둘 다 커스텀 TTF로 따로 지정해야 함(안 하면 깨져 보임,
     * 2026-08-08 실기에서 확인 — cntl_row의 s_btn_ko 체크박스와 같은 이유) */
    lv_obj_set_style_text_font(s_capture_interval_dd, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_capture_interval_dd), ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_event_cb(s_capture_interval_dd, cb_capture_interval_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_capture_apply_btn = lv_button_create(capture_right);
    lv_obj_add_event_cb(s_capture_apply_btn, cb_apply_capture_interval, LV_EVENT_CLICKED, NULL);
    /* 2026-08-11 버그수정 — "부팅 직후엔 표시값==저장값"이라는 가정이 항상 맞지는 않음(저장된
     * 값이 지금 프리셋 목록에 없으면 applied_idx=-1이라 드롭다운 표시와 실제 적용값이 다를 수
     * 있음, device_config 버전불일치 폴백 사고로 실사용 중 발견) — 무조건 비활성화하지 말고
     * 실제 일치 여부로 판단 */
    update_capture_apply_enabled();
    s_capture_apply_lbl = lv_label_create(s_capture_apply_btn);  /* 전역: refresh_lang_texts에서 갱신 */
    lv_label_set_text(s_capture_apply_lbl, ui_str(STR_BTN_APPLY));
    lv_obj_set_style_text_font(s_capture_apply_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* AGC/AEC 행(2026-08-21, 세로줄 노이즈 진단용) — [라벨][스위치] 한 줄씩, 토글 즉시
     * 반영(위 cb_agc_switch_changed 주석 참고). 촬영주기와 같은 그룹박스(영상, 카메라별
     * 설정) */
    lv_obj_t *agc_row = lv_obj_create(camera_group_box);
    lv_obj_set_size(agc_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(agc_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(agc_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(agc_row, 0, 0);
    lv_obj_set_style_pad_hor(agc_row, 12, 0);
    lv_obj_set_style_pad_ver(agc_row, 0, 0);

    s_agc_label = lv_label_create(agc_row);
    lv_label_set_text(s_agc_label, ui_str(STR_LABEL_AGC));
    lv_obj_set_style_text_font(s_agc_label, ui_font_get(UI_FONT_SIZE_18), 0);

    s_agc_switch = lv_switch_create(agc_row);
    if (device_config_get_agc_enable()) lv_obj_add_state(s_agc_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_agc_switch, cb_agc_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *aec_row = lv_obj_create(camera_group_box);
    lv_obj_set_size(aec_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(aec_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(aec_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(aec_row, 0, 0);
    lv_obj_set_style_pad_hor(aec_row, 12, 0);
    lv_obj_set_style_pad_ver(aec_row, 0, 0);

    s_aec_label = lv_label_create(aec_row);
    lv_label_set_text(s_aec_label, ui_str(STR_LABEL_AEC));
    lv_obj_set_style_text_font(s_aec_label, ui_font_get(UI_FONT_SIZE_18), 0);

    s_aec_switch = lv_switch_create(aec_row);
    if (device_config_get_aec_enable()) lv_obj_add_state(s_aec_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_aec_switch, cb_aec_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* XCLK 행(2026-08-21, 화질/노이즈 진단용) — 촬영주기와 같은 [라벨][드롭다운][Apply]
     * 구조(즉시적용 스위치가 아니라 확인응답 팝업 있는 Apply 패턴, 사용자 지시) */
    lv_obj_t *xclk_row = lv_obj_create(camera_group_box);
    lv_obj_set_size(xclk_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(xclk_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(xclk_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(xclk_row, 0, 0);
    lv_obj_set_style_pad_hor(xclk_row, 12, 0);
    lv_obj_set_style_pad_ver(xclk_row, 0, 0);

    s_xclk_label = lv_label_create(xclk_row);
    lv_label_set_text(s_xclk_label, ui_str(STR_LABEL_XCLK));
    lv_obj_set_style_text_font(s_xclk_label, ui_font_get(UI_FONT_SIZE_18), 0);

    lv_obj_t *xclk_right = create_row_right_cluster(xclk_row);

    s_xclk_dd = lv_dropdown_create(xclk_right);
    lv_obj_set_style_pad_ver(s_xclk_dd, 7, 0);  /* 2026-09-07 — 위아래 패딩 절반 */
    lv_dropdown_set_options(s_xclk_dd, ui_str(STR_OPT_XCLK_LIST));
    s_xclk_applied_idx = find_value_index(s_xclk_values,
        sizeof(s_xclk_values) / sizeof(s_xclk_values[0]),
        device_config_get_xclk_mhz());
    lv_dropdown_set_selected(s_xclk_dd,
        (uint16_t)(s_xclk_applied_idx >= 0 ? s_xclk_applied_idx : 0));
    lv_obj_set_style_text_font(s_xclk_dd, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_xclk_dd), ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_event_cb(s_xclk_dd, cb_xclk_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_xclk_apply_btn = lv_button_create(xclk_right);
    lv_obj_add_event_cb(s_xclk_apply_btn, cb_apply_xclk, LV_EVENT_CLICKED, NULL);
    update_xclk_apply_enabled();
    s_xclk_apply_lbl = lv_label_create(s_xclk_apply_btn);
    lv_label_set_text(s_xclk_apply_lbl, ui_str(STR_BTN_APPLY));
    lv_obj_set_style_text_font(s_xclk_apply_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

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

    lv_obj_t *response_right = create_row_right_cluster(response_row);

    s_response_interval_dd = lv_dropdown_create(response_right);
    lv_obj_set_style_pad_ver(s_response_interval_dd, 7, 0);  /* 2026-09-07 — 위아래 패딩 절반 */
    lv_dropdown_set_options(s_response_interval_dd, ui_str(STR_OPT_RESPONSE_INTERVAL_LIST));
    s_response_interval_applied_idx = find_value_index(s_response_interval_values,
        sizeof(s_response_interval_values) / sizeof(s_response_interval_values[0]),
        device_config_get_response_interval_sec());
    lv_dropdown_set_selected(s_response_interval_dd,
        (uint16_t)(s_response_interval_applied_idx >= 0 ? s_response_interval_applied_idx : 0));
    lv_obj_set_style_text_font(s_response_interval_dd, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_response_interval_dd), ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_event_cb(s_response_interval_dd, cb_response_interval_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_response_apply_btn = lv_button_create(response_right);
    lv_obj_add_event_cb(s_response_apply_btn, cb_apply_response_interval, LV_EVENT_CLICKED, NULL);
    update_response_apply_enabled();  /* 2026-08-11 버그수정 — capture_interval과 동일 이유 */
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
     * CAM에 안 보내는 Cntl 내부값(esp_now_hub.c의 esp_now_hub_note_user_action 참고) */
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

    lv_obj_t *adaptive_right = create_row_right_cluster(adaptive_row);

    s_adaptive_response_dd = lv_dropdown_create(adaptive_right);
    lv_obj_set_style_pad_ver(s_adaptive_response_dd, 7, 0);  /* 2026-09-07 — 위아래 패딩 절반 */
    lv_dropdown_set_options(s_adaptive_response_dd, ui_str(STR_OPT_ADAPTIVE_RESPONSE_LIST));
    s_adaptive_response_applied_idx = find_value_index(s_adaptive_response_values,
        sizeof(s_adaptive_response_values) / sizeof(s_adaptive_response_values[0]),
        device_config_get_adaptive_response_sec());
    lv_dropdown_set_selected(s_adaptive_response_dd,
        (uint16_t)(s_adaptive_response_applied_idx >= 0 ? s_adaptive_response_applied_idx : 0));
    lv_obj_set_style_text_font(s_adaptive_response_dd, ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_adaptive_response_dd), ui_font_get(UI_FONT_SIZE_18), 0);
    lv_obj_add_event_cb(s_adaptive_response_dd, cb_adaptive_response_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_adaptive_apply_btn = lv_button_create(adaptive_right);
    lv_obj_add_event_cb(s_adaptive_apply_btn, cb_apply_adaptive_response, LV_EVENT_CLICKED, NULL);
    update_adaptive_apply_enabled();  /* 2026-08-11 버그수정 — capture_interval과 동일 이유 */
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

    lv_obj_t *time_right = create_row_right_cluster(time_row);

    s_time_value_label = lv_label_create(time_right);
    lv_obj_add_style(s_time_value_label, &style_text_muted, 0);
    lv_obj_set_style_text_font(s_time_value_label, ui_font_get(UI_FONT_SIZE_18), 0);
    refresh_clock(NULL);  /* 다음 1초 tick 전까지 빈 채로 안 보이게 즉시 한 번 채움(로고부제와 동일 이유) */

    lv_obj_t *time_set_btn = lv_button_create(time_right);
    lv_obj_add_event_cb(time_set_btn, cb_settime_btn, LV_EVENT_CLICKED, NULL);
    s_time_set_btn_lbl = lv_label_create(time_set_btn);
    lv_label_set_text(s_time_set_btn_lbl, ui_str(STR_BTN_SET_TIME));
    lv_obj_set_style_text_font(s_time_set_btn_lbl, ui_font_get(UI_FONT_SIZE_18), 0);

    /* 2026-09-07(탭→팝업 전환) — 설정탭 자신의 버튼들 폭 통일. s_action_btn_width는 이미
     * ui_init()에서 상황판 버튼 기준으로 정해져있음(재계산 없음, 그냥 적용만).
     * 2026-09-09(사용자 지적 — "세팅 단추를 누르면 콘이 죽어") — s_sens_measure_apply_btn은
     * 측정주기 위젯이 개별설정 팝업으로 옮겨가면서 여기서 더 이상 안 만들어짐(NULL) —
     * lv_obj_set_width(NULL, ...)가 태스크워치독 타임아웃(lvgl 태스크 무한루프로 보임)을
     * 일으켰음. 배열에서 제거 — 그 버튼은 이제 개별설정 팝업 안에서 다른 버튼들처럼
     * 자연폭(LV_SIZE_CONTENT)으로 그려짐(공용 s_action_btn_width 미적용, 시각적 차이는
     * 미미해서 지금은 그대로 둠) */
    lv_obj_t *option_action_buttons[] = {
        restart_btn, s_capture_apply_btn, s_response_apply_btn, s_adaptive_apply_btn,
        time_set_btn, s_xclk_apply_btn,
    };
    lv_obj_update_layout(lv_screen_active());
    for (size_t i = 0; i < sizeof(option_action_buttons) / sizeof(option_action_buttons[0]); i++) {
        lv_obj_set_width(option_action_buttons[i], s_action_btn_width);
        lv_obj_center(lv_obj_get_child(option_action_buttons[i], 0));
    }

    /* 네트워크 행의 값+화살표 셀렉터(2026-08-29 설계, 2026-09-07 수정 — 사용자 지적:
     * "SSID> 표시가 버튼보다 커서 종속 드랍다운 위치가 이상해져") — 표준폭의 2배로 뒀던 걸
     * 다른 버튼들과 같은 표준폭 하나로 줄임 */
    lv_obj_set_width(s_network_find_btn, s_action_btn_width);

    /* 2026-09-08(사용자 재설계 — "로그는... 설정 팝업 안에 있는 별도 버튼", 일반 사용자는
     * 안 볼 진단용이라 일부러 눈에 덜 띄는 자리) — 콘텐츠 맨 끝에 작은 버튼 하나. 누르면
     * 이 팝업 안에서 설정 콘텐츠를 로그 콘텐츠로 바꿔치기(팝업을 새로 안 열음) */
    lv_obj_t *log_entry_row = lv_obj_create(option_page);
    lv_obj_set_size(log_entry_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(log_entry_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(log_entry_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(log_entry_row, 0, 0);
    lv_obj_set_style_pad_hor(log_entry_row, 12, 0);
    lv_obj_set_style_pad_ver(log_entry_row, 6, 0);
    lv_obj_t *log_entry_btn = lv_button_create(log_entry_row);
    lv_obj_add_event_cb(log_entry_btn, cb_option_log_btn_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_t *log_entry_lbl = lv_label_create(log_entry_btn);
    lv_label_set_text(log_entry_lbl, ui_str(STR_TAB_LOG));
    lv_obj_set_style_text_font(log_entry_lbl, ui_font_get(UI_FONT_SIZE_12), 0);

    size_t heap_after_option_tab = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "MEMDIAG 설정탭 위젯 생성 비용(순수): internal %u -> %u (소모 %d bytes)",
             (unsigned)heap_before_option_tab, (unsigned)heap_after_option_tab,
             (int)heap_before_option_tab - (int)heap_after_option_tab);
}

static void cb_close_option_popup(lv_event_t *e)
{
    (void)e;
    if (s_log_tab_built) {
        /* 로그를 보고 있었으면 "뒤로가기"처럼 설정 콘텐츠로 되돌림 — 팝업 자체는 안 닫음 */
        teardown_log_tab();
        build_option_tab();
        lv_label_set_text(s_option_popup_title, ui_str(STR_TAB_OPTION));
        return;
    }
    teardown_option_tab();
    lv_obj_delete(s_option_popup);
    s_option_popup = NULL;
    s_option_content = NULL;
    s_option_popup_title = NULL;
    s_option_tab_built = false;
}

static void cb_settings_btn_tap(lv_event_t *e)
{
    (void)e;
    if (s_option_tab_built) return;  /* 이미 열려있음 */
    s_option_tab_built = true;

    s_option_popup = create_page_popup();
    add_page_popup_header(s_option_popup, ui_str(STR_TAB_OPTION), cb_close_option_popup, &s_option_popup_title);
    s_option_content = lv_obj_create(s_option_popup);
    lv_obj_set_size(s_option_content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_option_content, 1);
    lv_obj_set_flex_flow(s_option_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_option_content, 0, 0);
    lv_obj_set_style_border_width(s_option_content, 0, 0);
    build_option_tab();
}

/* 설정 팝업 안 "로그" 버튼 — 같은 팝업 안에서 콘텐츠만 바꿔치기(설정 지우고 로그 지음) */
static void cb_option_log_btn_tap(lv_event_t *e)
{
    (void)e;
    teardown_option_tab();
    build_log_tab();
    lv_label_set_text(s_option_popup_title, ui_str(STR_TAB_LOG));
}

/* 2026-09-08(재설계) — 로그 콘텐츠 지움(s_option_content 안 자식들 — 설정 팝업과 같은
 * 컨테이너를 공유하므로 팝업 자체는 안 건드림) */
static void teardown_log_tab(void)
{
    size_t heap_before_close = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (s_power_panel_timer) { lv_timer_delete(s_power_panel_timer); s_power_panel_timer = NULL; }
    if (s_log_box_timer) { lv_timer_delete(s_log_box_timer); s_log_box_timer = NULL; }
    lv_obj_clean(s_option_content);
    s_log_tab_built = false;
    size_t heap_after_close = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "MEMDIAG 로그탭 이탈: internal %u -> %u (회수 %d bytes)",
             (unsigned)heap_before_close, (unsigned)heap_after_close,
             (int)heap_after_close - (int)heap_before_close);
    s_log_panel_title = NULL;
    s_log_container = NULL;
    s_log_label = NULL;
    s_power_panel_title = NULL;
    s_power_log_pause_btn = NULL;
    s_power_log_pause_lbl = NULL;
    s_power_list = NULL;
    s_power_log_label = NULL;
}

/* 2026-09-08(사용자 재설계) — s_log_tab_page(부팅 시 이미 만들어진 빈 탭) 안에 내용을 지음 */
static void build_log_tab(void)
{
    s_log_tab_built = true;

    size_t heap_before_log_tab = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    /* 2026-09-08(재설계) — 설정 팝업 안 콘텐츠 바꿔치기로 진입(cb_option_log_btn_tap).
     * 기존 통계탭에 있던 일반로그+전력로그 판넬을 그대로 옮김(위젯 생성 코드 자체는
     * 무변경, 부모만 s_option_content로 교체) */
    lv_obj_t *log_page = s_option_content;
    lv_obj_set_style_pad_hor(log_page, 4, 0);
    lv_obj_set_style_pad_bottom(log_page, 4, 0);
    lv_obj_set_style_pad_row(log_page, 4, 0);
    lv_obj_set_style_bg_color(log_page, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_bg_opa(log_page, LV_OPA_COVER, 0);

    /* 2026-08-11, 사용자 지시 — 일반로그/전력로그 위아래 순서 맞바꿈(일반로그가 위, 전력로그가
     * 아래). 두 블록 내용 자체는 그대로, log_page에 자식으로 추가되는 순서만 바뀜(LVGL
     * flex-column은 생성 순서대로 위→아래 배치) */
    lv_obj_t *log_box = lv_obj_create(log_page);
    /* 2026-08-11, 사용자 지시 — "전력, 일반 모두 320 픽셀로 맞춰": power_box와 동일하게
     * 고정 320px(전에 flex_grow로 남는 ~73px만 나눠 갖던 걸 여기서 되돌림). power_box(320)
     * + log_box(320) 합이 실제 콘텐츠 영역(~393px)보다 커지므로 log_page 자체가 정상적으로
     * overflow해서 스크롤이 필요해짐 — 이게 의도(사용자: "스크롤 하겠다는 거니까, 화면
     * 크기에 맞추면 안되지") — power_title_row/log_title_row가 그 바깥 스크롤을 잡는
     * 고정 영역 역할 */
    lv_obj_set_size(log_box, LV_PCT(100), 320);
    lv_obj_set_flex_flow(log_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(log_box, 6, 0);

    lv_obj_t *log_title_row = lv_obj_create(log_box);
    lv_obj_set_size(log_title_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(log_title_row, 0, 0);
    lv_obj_set_style_pad_all(log_title_row, 0, 0);
    lv_obj_set_flex_flow(log_title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(log_title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_log_panel_title = lv_label_create(log_title_row);
    lv_label_set_text(s_log_panel_title, ui_str(STR_PANEL_GENERAL_LOG));
    lv_obj_set_style_text_font(s_log_panel_title, ui_font_get(UI_FONT_SIZE_18), 0);

    s_log_container = lv_obj_create(log_box);
    lv_obj_set_size(s_log_container, LV_PCT(100), 0);
    lv_obj_set_flex_grow(s_log_container, 1);
    lv_obj_set_scroll_dir(s_log_container, LV_DIR_VER);
    lv_obj_set_style_border_width(s_log_container, 0, 0);
    lv_obj_set_style_pad_all(s_log_container, 6, 0);

    s_log_label = lv_label_create(s_log_container);
    /* 2026-08-22, 사용자 지시 — 로그를 전부 영문으로 바꾸고 폭 넘는 줄은 우리가 직접
     * "..."로 잘라서 넣으므로(trim_multiline_to_width) WRAP의 비싼 매 렌더 재계산이
     * 필요 없음 → CLIP(단순 커팅, 실제로 걸릴 일은 없음). 폰트도 커스텀 TTF 대신
     * 빠른 내장 비트맵 폰트(Montserrat 18pt, 전력로그와 통일 — 20pt는 한 줄에 너무 적게
     * 들어가 트림 "..."이 자주 남아서 18pt로 축소, 2026-08-22 사용자 지시)로 교체 */
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_log_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_log_label, &lv_font_montserrat_18, 0);
    lv_label_set_text(s_log_label, "");

    s_log_box_timer = lv_timer_create(refresh_log_box, 500, NULL);

    lv_obj_t *power_box = lv_obj_create(log_page);
    lv_obj_set_size(power_box, LV_PCT(100), 320);  /* 2026-08-10, 사용자 지시 — 고정 320px */
    lv_obj_set_flex_flow(power_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(power_box, 6, 0);

    /* 제목 + 일시멈춤 단추를 한 행에(2026-08-10, 사용자 지시 — 값 읽는 동안 로그가 계속
     * 밀리지 않게 멈출 수 있게). 2026-08-11 — 이 행은 스크롤 컨테이너(s_power_list) 밖의
     * 고정 영역이라, 여기를 탭+드래그하면 안쪽 리스트가 가로채지 않고 바깥 log_page가
     * 스크롤됨(사용자 지시 — 로그 판넬이 전체 다 내부 스크롤 입력을 받아서 페이지 스크롤을
     * 잡을 영역이 없었던 문제의 해결책) */
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
    /* 2026-08-22 — 일반로그와 동일 이유로 CLIP + 18pt 비트맵 폰트로 통일(사용자 지시) */
    lv_label_set_long_mode(s_power_log_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_power_log_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_power_log_label, &lv_font_montserrat_18, 0);
    /* 2026-08-10 — C/S 줄을 구분하려고 recolor(#RRGGBB text#) 켰었으나, 2026-08-11에 3가지
     * 형태 다 실기에서 깨지는 걸 확인하고 recolor 자체를 포기(순수 텍스트 ">>> " 마커로
     * 대체, refresh_power_panel 참고) — 더 이상 안 쓰므로 켜두지 않음 */
    lv_label_set_text(s_power_log_label, "");

    s_power_panel_timer = lv_timer_create(refresh_power_panel, 2000, NULL);

    size_t heap_after_log_tab = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "MEMDIAG 로그탭 위젯 생성 비용(순수): internal %u -> %u (소모 %d bytes)",
             (unsigned)heap_before_log_tab, (unsigned)heap_after_log_tab,
             (int)heap_before_log_tab - (int)heap_after_log_tab);
}
