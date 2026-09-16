#pragma once

/**
 * @file    power_relay.h
 * @brief   SR(솔리드스테이트 릴레이) 제어 — 2026-09-16 설계(순수 대화로 정리, project_
 *          cntl_sr_power_control_plan_2026_09_15 메모리 참고).
 *
 *          Sens/CAM과 달리 페어링되는 장치가 아니라, 콘 하드웨어 자체에 고정된 2개 GPIO
 *          출력(Relay 1/Relay 2 — 콘 기기를 바꾸기 전까진 항상 이 개수). 각 릴레이는
 *          센서 값(그룹 통계 또는 개별 장치 실측값)을 기준으로 히스테리시스+최소유지시간+
 *          추세 판단으로 자동 On/Off됨.
 *
 *          SSR-DK25DA(제어입력 2선, 피드백 없음) 전제 — "실제로 붙어있는지"는 소프트웨어로
 *          확인 불가(사용자 확인, 2026-09-16). 이 모듈은 "명령한 상태"만 관리/표시함.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_now_link.h"  /* sensor_channel_type_t */

#ifdef __cplusplus
extern "C" {
#endif

#define POWER_RELAY_COUNT           2
#define POWER_RELAY_ALIAS_MAX_LEN   32

/* 기준값 소스 종류 — 어제/오늘 대화로 확정: 그룹 계열 통계, 또는 특정 개별 장치 실측값 */
typedef enum {
    POWER_SRC_GROUP_STAT    = 0,
    POWER_SRC_SINGLE_DEVICE = 1,
} power_source_kind_t;

/* stats_view_group_t(ui_main.c 내부)의 공개용 미러 — power_relay.h는 ui_main.c의 private
 * 분류체계를 몰라야 하므로 별도 enum을 두고, ui_main_query_power_source_value()가 내부에서
 * 변환함 */
typedef enum {
    POWER_GROUP_AIR  = 0,
    POWER_GROUP_AGAR = 1,
    POWER_GROUP_GAS  = 2,
} power_group_t;

typedef enum {
    POWER_STAT_AVG = 0,
    POWER_STAT_MAX = 1,
    POWER_STAT_MIN = 2,
} power_source_stat_t;

/* 방향 — 릴레이마다 독립(같은 채널을 보는 히터용/에어컨용 릴레이가 반대 방향일 수 있음,
 * 2026-09-16 사용자 설계) */
typedef enum {
    POWER_DIR_ON_ABOVE = 0,  /* 값이 on_threshold보다 높아지면 On, off_threshold보다 낮아지면 Off */
    POWER_DIR_ON_BELOW = 1,  /* 값이 on_threshold보다 낮아지면 On, off_threshold보다 높아지면 Off */
} power_direction_t;

typedef struct {
    char    alias[POWER_RELAY_ALIAS_MAX_LEN];
    bool    configured;  /* 사용자가 아직 한 번도 설정 안 했으면 false(팝업에서 기본값 유도) */

    /* 2026-09-16(순수 대화로 재설계 — "공학 관점에선 좋은데 사용자 관점에선 쓰기 힘들다") —
     * AI On이면 팝업이 Based on/통계/추세/최소유지 항목을 안 보여주고 여기 저장된 값을
     * 스마트 기본값으로 채움(정밀 우선, 추세 On, 평균). AI Off면 전부 사용자가 직접 지정 —
     * 이 필드는 그 마지막 선택을 기억해서 팝업을 다시 열 때 같은 모드로 보여주기 위함일 뿐,
     * 판정 로직 자체는 아래 필드들만 봄(AI가 미리 계산해서 채워놓은 값이든 사용자가 직접
     * 채운 값이든 구분 안 함) */
    bool    ai_mode;

    power_source_kind_t source_kind;
    /* POWER_SRC_GROUP_STAT일 때만 유효 */
    power_group_t       group;
    bool                precise;  /* Fine(true)/Basic(false) — AIR 그룹에서만 의미 있음 */
    power_source_stat_t stat;
    /* POWER_SRC_SINGLE_DEVICE일 때만 유효 */
    uint8_t             device_mac[6];
    sensor_channel_type_t chan_type;  /* 두 소스 종류 공통 — 어떤 물리량(온도/CO2 등)인지 */

    power_direction_t direction;
    float    on_threshold;
    float    off_threshold;
    uint32_t min_hold_sec;   /* 채터링 방지 — 상태 전환 후 이 시간 안에는 반대로 안 바뀜 */

    bool     trend_enable;
    /* 2026-09-16(사용자 정리 — "판단기간" 하나로 통일) — 추세 기울기를 계산할 때 볼 과거
     * 창 길이와, 그 기울기로 미래를 내다볼 예측 폭을 같은 값으로 통일(과거를 본 만큼만
     * 앞을 내다본다는 단순한 원칙) — evaluate_relay()가 이 하나의 값을 두 용도로 다 씀 */
    uint32_t trend_window_sec;
} power_relay_config_t;

/* app_main()에서 fs_init() 이후 한 번 호출 — 저장된 설정 복원(없으면 미설정 기본값) */
void power_relay_load(void);

/* app_main() 끝부분에서 한 번 호출 — GPIO 초기화 + 주기적 판정 태스크 시작 */
void power_relay_start(void);

const power_relay_config_t *power_relay_get_config(int idx);  /* idx: 0..POWER_RELAY_COUNT-1 */
void power_relay_set_config(int idx, const power_relay_config_t *cfg);

/* 소프트웨어가 판단한 현재 명령 상태(물리 확인 불가 — SSR-DK25DA는 피드백 없음, 2026-09-16
 * 사용자 확인) */
bool power_relay_get_commanded_on(int idx);

#ifdef __cplusplus
}
#endif
