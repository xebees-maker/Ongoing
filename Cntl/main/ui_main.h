#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "power_relay.h"
#include "photo_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);

/* 2026-09-16 — SR(Power Control) 판정 루프(power_relay.c)가 매 주기 호출. cfg가 가리키는
 * 소스(그룹 통계 또는 개별 장치)의 현재 값을 ui_main.c의 기존 분류/집계 로직(stats_classify,
 * stats_collect_group_macs, stats_blend_macs)으로 구해 돌려줌 — 그 로직 자체는 static이라
 * power_relay.c가 직접 못 쓰므로 이 함수 하나로만 연결 */
bool ui_main_query_power_source_value(const power_relay_config_t *cfg, float *out_value);

/* 2026-08-29 — WIFI_EVENT_SCAN_DONE 핸들러 등록. 기본 이벤트루프가 생긴 뒤(node_hub_init()
 * 호출 이후)에 app_main()에서 불러야 함 — ui_init()보다 먼저는 절대 안 됨 */
void ui_main_register_wifi_events(void);

/* 2026-10-03(할 일 AD) — 웹 세션 잠금 상태가 바뀔 때(web_session 콜백, 어느 태스크에서든) — 콘 잠금 화면을 띄우거나 치움 */
void ui_main_on_web_session_lock(bool locked);

/* 2026-10-03(할 일 AD — 웹 주화면) — 상단바 상태 아이콘과 같은 판정의 재료(에러/경고/SD I/O 실패). 웹서버 태스크에서
 * 읽음 — bool 읽기라 잠금 없이 */
void ui_main_get_status_flags(bool *error, bool *warn, bool *sd_io_fail);

/* 2026-10-03(할 일 AD — 웹 장치 팝업) — 웹의 설정 변경을 LVGL 태스크에서 콘 화면과 같은 모델 함수로 실행.
 * device_config 등은 잠금 없이 LVGL 태스크에서만 쓰는 전제라 웹서버 태스크가 직접 쓰지 않음. op는 복사되므로 시간이 넘어도
 * 안전(작업은 자기 복사본으로 끝까지 감). 값은 콘의 선택지 목록에 있는 것만 받음 — 아니면 false */
typedef enum {
    UI_WEB_OP_ALIAS = 1,       /* text */
    UI_WEB_OP_SENS_INTERVAL,   /* value = 초 */
    UI_WEB_OP_CAM_CAPTURE,     /* value = 초 */
    UI_WEB_OP_CAM_AGC,         /* value = 0/1 */
    UI_WEB_OP_CAM_AEC,         /* value = 0/1 */
    UI_WEB_OP_CAM_XCLK,        /* value = MHz */
    UI_WEB_OP_CAM_LIGHT,       /* value = 0/1 — 캠 조명 시험(콘 Light Test 스위치와 같은 명령) */
    UI_WEB_OP_UNPAIR,          /* 연결 해제 */
    UI_WEB_OP_RELAY_ALIAS,     /* idx, text — 콘 릴레이 팝업 별명 Apply와 같음 */
    UI_WEB_OP_RELAY_APPLY,     /* idx, relay — 콘 릴레이 팝업 본 Apply와 같음(선택값 → 설정 계산은 콘과 같은 함수) */
    UI_WEB_OP_RELAY_OVERRIDE,  /* idx, value = 0/1 — 수동 조작(전원 아이콘 확인 팝업 Yes와 같음) */
} ui_web_op_type_t;

/* 릴레이 팝업의 사용자 선택값(화면 위젯이 나타내는 값 그대로). relay_apply_choices()가 이걸 설정 구조체로 바꿈 —
 * 콘 화면과 웹이 같은 계산을 씀 */
typedef struct {
    uint8_t  chan_type;        /* SENSOR_CHAN_TEMP_C / SENSOR_CHAN_CO2_PPM */
    bool     y_rises;          /* 도달(Up to)=true, 미만(Below)=false */
    bool     z_turns_on;       /* 켜짐=true, 꺼짐=false */
    float    center;           /* 값 */
    float    margin;           /* ± 오차 */
    bool     ai_mode;
    bool     manual_override;  /* 끄는 것만 여기서(켜기는 RELAY_OVERRIDE로 방향과 함께) */
    uint8_t  source_kind;      /* power_source_kind_t */
    uint8_t  group_choice;     /* 온도+그룹일 때: 0 Air 기본, 1 Air 정밀, 2 Agar */
    uint8_t  device_mac[6];
    bool     device_valid;     /* 장치 기준인데 고를 장치가 있었는지 */
    uint8_t  stat;             /* power_source_stat_t */
    bool     trend_enable;
    uint32_t trend_samples;    /* 선택지 값(3/5/10) */
    uint32_t min_hold_sec;     /* 선택지 값(30/60/180/300) */
} ui_web_relay_choices_t;

typedef struct {
    ui_web_op_type_t type;
    uint8_t  mac[6];
    uint32_t value;
    char     text[32];
    int      idx;                      /* 릴레이 번호(0..) */
    ui_web_relay_choices_t relay;
} ui_web_op_t;

bool ui_main_run_web_op(const ui_web_op_t *op, uint32_t timeout_ms);

/* 콘 화면 선택지 값 목록(웹이 같은 선택지를 보여 주려고) — which: 0 측정 주기(초), 1 촬영 주기(초), 2 XCLK(MHz). 개수 반환 */
int ui_main_get_option_values(int which, const uint32_t **out);  /* 3 추세 샘플 수, 4 최소 유지(초) */

/* 릴레이 팝업 기본값·값 범위(콘 화면과 같은 표) — 웹 릴레이 팝업용 */
void ui_main_relay_defaults(uint8_t chan, float *center, float *margin);
void ui_main_relay_spec(uint8_t chan, bool precise, float *min_v, float *max_v, float *step, int *decimals, float *margin_max);
/* 릴레이 팝업 채널 선택지(SENSOR_CHAN_*) — 개수 반환 */
int ui_main_relay_chan_list(uint8_t *out, int cap);

/* 2026-08-30(사용자 설계: "웹에 입력이 있으면, CNTL의 탭과 같은 입력 처리 과정을 거쳐야") —
 * 웹의 사진 가져오기도 온디바이스 탭과 같은 모델(s_selected_file_id)을 거치게 해서,
 * consume_ready_photo_if_current()의 "지금 기다리는 것과 다름" 오판(3008,
 * UI_ERR_PHOTO_SELECTION_STALE)을 막음 — main.c의 웹 API 핸들러가 photo_rx_fetch_by_id()를
 * 부르기 직전에 이것부터 호출 */
/* 2026-09-04(사용자 설계: "PC 원격제어처럼") — 웹 입력을 실제 탭/팝업/확인 시퀀스로 합성.
 * 전부 LVGL 태스크에서 동기적으로 실행되고(httpd 태스크는 완료까지 블로킹), 대상 위젯을
 * 못 찾으면(지금 화면/목록에 없음) false — main.c가 이걸로 "합성 자체의 실패"를 즉시 판정 */
bool ui_main_inject_connect(const uint8_t *mac);
/* 2026-09-27(3002 조사) — 카메라 선택 + "지금 촬영" 탭 합성 */
bool ui_main_inject_capture_now(const uint8_t *mac);
bool ui_main_inject_disconnect(const uint8_t *mac);
/* 2026-09-06(야간 자동 테스트용) — 센스 행은 카메라 행과 별도 리스트라 연결 합성도 별도 */
bool ui_main_inject_connect_sensor(const uint8_t *mac);
/* 응답성(response_interval) 드롭다운+Apply 합성 — sec는 0/3/10/30/60만 유효 */
bool ui_main_inject_set_response_interval(uint32_t sec);
/* 통계 전체 삭제 버튼+확인팝업 합성(2026-09-07, 어젯밤 쌓인 이산화탄소 0 레코드 정리용) */
bool ui_main_inject_delete_stats(void);
/* 2026-09-19(SD 제거 재설계 — 사진목록 UI 로컬화) — 목록갱신/사진선택 모두 이제 콘 SD를
 * 읽는 동기 로컬 동작이라(더 이상 CAM 응답을 기다리는 ESP-NOW 왕복이 아님) 예전의 세대번호/
 * 비파괴적 완료-확인 채널이 필요 없어짐. 반환 시점에 이미 결과가 반영돼 있음 */
bool ui_main_inject_list_refresh(void);
/* 새로고침된 목록을 그대로 복사(웹이 독자적으로 photo_storage를 다시 읽지 않고, 콘 화면이
 * 지금 보여주는 바로 그 배열을 읽어감 — "웹기생" 원칙) */
int ui_main_get_photo_list(photo_storage_item_t *out, int out_cap);
bool ui_main_inject_photo_select(uint8_t kind, uint32_t seq);
/* 선택된 사진의 원본(압축 해제 전) JPEG 바이트 — 웹의 "원본 그대로 보기"가 씀 */
bool ui_main_get_selected_photo_raw(const uint8_t **out_data, size_t *out_len);
/* 지금 선택된 사진의 (kind,seq) — 웹이 자신의 요청과 "지금 화면이 보여주는 것"이 같은지
 * 확인하는 용도(/photo 핸들러 참고) */
bool ui_main_get_selected_photo_id(uint8_t *out_kind, uint32_t *out_seq);

#ifdef __cplusplus
}
#endif
