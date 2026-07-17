/**
 * @file    history_log.h
 * @brief   측정값 1개월치 시간 단위 히스토리 로그 — NVS 영구 저장
 *
 * 6개 지표(metric)를 "틱" 단위 링버퍼로 저장한다. 1틱 = HISTORY_TICK_SEC초.
 * 운영 시 1틱 = 1시간(3600초)이 되지만, 테스트 중에는 짧은 값으로 바꿔서
 * 빠르게 검증할 수 있다 — 윈도우(8h/일/주/월)는 모두 "틱 개수"로 정의되어
 * 있어서 HISTORY_TICK_SEC 하나만 바꾸면 모든 윈도우의 실제 길이가 같이 변한다.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HISTORY_TICK_SEC           10      /* 테스트값. 운영 전환 시 3600(1시간)으로 변경 */

#define HISTORY_WINDOW_8H_TICKS    8
#define HISTORY_WINDOW_DAY_TICKS   24
#define HISTORY_WINDOW_WEEK_TICKS  168
#define HISTORY_WINDOW_MONTH_TICKS 720

#define HISTORY_TICK_CAPACITY      HISTORY_WINDOW_MONTH_TICKS  /* 720 — 링버퍼 크기 */

/* CH0~CH4는 슬롯일 뿐 의미가 고정되어 있지 않음 — 어떤 센서를 붙였는지는 컴파일 타임에
 * 정해지므로, 그 의미(라벨/단위/저장 배율)는 호출하는 쪽이 정한다.
 * 예: 헤드리스 SCD41 노드는 CH0=CO2/CH1=TEMP/CH2=HUMI만 쓰고, 레거시 LCD 콤보 앱(DHT22+SCD41
 * 동시 장착)은 CH0=DHT_TEMP/CH1=DHT_HUMI/CH2=SCD_CO2/CH3=SCD_TEMP/CH4=SCD_HUMI로 5개 다 씀. */
typedef enum {
    HISTORY_METRIC_CH0 = 0,
    HISTORY_METRIC_CH1,
    HISTORY_METRIC_CH2,
    HISTORY_METRIC_CH3,
    HISTORY_METRIC_CH4,
    HISTORY_METRIC_BATT_PCT,
    HISTORY_METRIC_COUNT,
} history_metric_t;

typedef enum {
    HISTORY_WINDOW_8H = 0,
    HISTORY_WINDOW_DAY,
    HISTORY_WINDOW_WEEK,
    HISTORY_WINDOW_MONTH,
    HISTORY_WINDOW_COUNT,
} history_window_t;

typedef struct {
    bool     valid;          /**< false면 해당 윈도우에 샘플이 하나도 없음(콜드부트) */
    float    min_val;
    time_t   min_time;
    float    max_val;
    time_t   max_time;
    float    avg_val;
    uint32_t sample_count;   /**< 윈도우 내 실제로 집계된 틱 수 */
} history_stats_t;

/**
 * @brief NVS에서 로드(없으면 새로 초기화) — nvs_flash_init() 이후 1회 호출
 * @return 성공 시 true
 */
bool history_log_init(void);

/**
 * @brief 지표의 NVS 저장 배율을 재설정 (기본값 10.0, CH0~CH2는 10.0/BATT_PCT는 1.0).
 *        CO2처럼 값 범위가 커서 *10이 int16 범위(-32768~32767)를 넘는 지표는 반드시
 *        history_log_record()를 처음 호출하기 전에 1.0으로 낮춰서 호출해야 한다.
 */
void history_log_set_scale(history_metric_t metric, float scale);

/** @brief 벽시계 시각을 강제 설정(주입) — settimeofday() 반영 + 다음 커밋부터 영속화 */
void history_log_set_time(time_t now);

/** @brief 현재 벽시계 시각 (time(NULL)과 동일) */
time_t history_log_now(void);

/**
 * @brief 이번 틱에 기록할 값을 스테이징한다. 같은 틱 안에서 6개 지표 모두
 *        호출한 뒤 history_log_tick_commit()을 1회 호출해야 한다.
 */
void history_log_record(history_metric_t metric, float value);

/** @brief 스테이징된 값들을 head/epoch 갱신과 함께 NVS에 1회 커밋 */
void history_log_tick_commit(void);

/**
 * @brief 지표의 윈도우별 min/max(+시각)/avg 조회
 * @return 성공 시 true (out->valid는 샘플 존재 여부)
 */
bool history_log_get_stats(history_metric_t metric, history_window_t window, history_stats_t *out);

#ifdef __cplusplus
}
#endif
