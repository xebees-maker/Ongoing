/**
 * @file    scd41.h
 * @brief   SCD41 CO2/온습도 센서 — I2C 드라이버 (Sensirion 프로토콜)
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

#define SCD41_I2C_ADDR   0x62

/**
 * @brief SCD41 초기화: 전용 I2C 버스 생성 + 디바이스 등록 + 주기 측정 시작
 * @param i2c_port 전용으로 쓸 I2C 포트 번호 (예: 1 — 다른 버스와 분리 권장)
 * @param sda_gpio SDA 핀
 * @param scl_gpio SCL 핀
 * @return 성공 시 true
 */
bool scd41_init(int i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio);

/**
 * @brief SCD41 초기화(single-shot 전용) — I2C 버스 생성 + 디바이스 등록만 하고 끝.
 *        2026-09-06(사용자 지시) — single-shot만 쓰는 호출부(딥슬립 헤드리스 Sens)는
 *        WAKE_UP/STOP_PERIODIC/START_PERIODIC이 전혀 필요 없음(Sensirion 공식 문서:
 *        single-shot은 periodic measurement의 "대안"이지 같이 쓰는 게 아님, wake_up도
 *        power_down을 실제로 건 적이 있을 때만 필요한데 이 경로는 power_down을 안 씀).
 *        scd41_init()처럼 방어적으로 periodic을 멈추는 절차 자체가 필요 없어서, 그로 인한
 *        1000ms 안정화 지연도 같이 사라짐 — 매 딥슬립 사이클마다 부팅 후 측정 시작까지의
 *        시간이 크게 단축됨.
 * @param i2c_port 전용으로 쓸 I2C 포트 번호
 * @param sda_gpio SDA 핀
 * @param scl_gpio SCL 핀
 * @return 성공 시 true
 */
bool scd41_init_single_shot(int i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio);

/**
 * @brief 새 측정값이 준비됐으면 읽기 (5초 주기로 갱신됨, 그 전엔 false)
 * @param co2_ppm     CO2 농도 ppm 출력 (NULL 가능)
 * @param temperature 섭씨 온도 출력 (NULL 가능)
 * @param humidity    상대습도(%) 출력 (NULL 가능)
 * @return 새 데이터를 읽었으면 true, 준비 안 됐거나 오류면 false
 */
bool scd41_read(int *co2_ppm, float *temperature, float *humidity);

/**
 * @brief continuous periodic measurement 중지 — single-shot 듀티사이클 모드로 전환하기 전에
 *        한 번 호출해서 센서가 5초마다 계속 자체 측정하는 걸 멈춘다 (scd41_init()은 항상
 *        continuous 모드로 시작하므로, single-shot을 쓰려면 이 함수로 먼저 꺼야 함).
 * @return 성공 시 true (NACK이면 false지만 대부분 무시 가능 — 이미 정지 상태였다는 뜻)
 */
bool scd41_stop_periodic_measurement(void);

/**
 * @brief single-shot 측정 1회 트리거 (논블로킹 — 명령만 보내고 바로 리턴).
 *        완료까지 약 5초 걸리며, 그동안 센서는 유휴 상태(연속모드 대비 전력 절약).
 * @return 명령 전송 성공 시 true
 */
bool scd41_trigger_single_shot(void);

/**
 * @brief scd41_trigger_single_shot() 이후 주기적으로 호출 — 아직 측정 중이면 즉시 false
 *        (I2C 트래픽 없음, *out_ok는 안 건드림). 5초가 지났으면 결과를 읽어서 true를
 *        반환하고 *out_ok에 읽기 성공 여부를 채운다 — 리턴값과 *out_ok를 분리한 이유는
 *        "아직 측정 중"과 "측정은 끝났는데 읽기 실패"를 호출자가 구분해야 다음 트리거
 *        타이밍을 정할 수 있기 때문(둘 다 그냥 false로 뭉치면 실패 시 사이클이 영원히
 *        안 끝난 것처럼 보여 재트리거가 안 걸림). 리턴값이 true면(성공이든 실패든) 이번
 *        사이클은 끝난 것이므로, 다음 측정은 다시 scd41_trigger_single_shot()을 호출해야
 *        시작된다.
 * @param co2_ppm     CO2 농도 ppm 출력 (NULL 가능, *out_ok가 true일 때만 유효)
 * @param temperature 섭씨 온도 출력 (NULL 가능, *out_ok가 true일 때만 유효)
 * @param humidity    상대습도(%) 출력 (NULL 가능, *out_ok가 true일 때만 유효)
 * @param out_ok      이번 사이클이 끝났을 때(리턴 true) 읽기 성공 여부 (NULL 불가)
 * @return 이번 호출로 사이클이 종료됐으면(성공/실패 무관) true, 아직 측정 중이거나
 *         트리거된 적이 없으면 false
 */
bool scd41_poll_single_shot(int *co2_ppm, float *temperature, float *humidity, bool *out_ok);

/* 2026-09-29(측정 실패 진단) — 마지막 실패 원인. 값은 esp_now_link.h의 sensor_fault_t와 같음
 * (0=없음, 1=명령 NACK, 3=CRC, 4=수신 실패). "타임아웃까지 준비 안 됨"(2)은 드라이버가 아니라 대기하는 호출부가 판단 */
int scd41_last_fault(void);
/* 2026-10-05(진단, 임시) */
void scd41_diag_get(int *polls, uint16_t *last_status);
void scd41_log_identity(void);
/* 2026-10-05 — ASC가 켜져 있으면 끄고 persist(센서당 한 번). 유휴 상태에서 부를 것 */
void scd41_ensure_asc_off(void);
/* 2026-10-06(시험) — wake_up(결과 무시, 30ms) */
void scd41_wake_up(void);
void scd41_clear_fault(void);

/* 2026-09-29(Sensirion 데이터시트 3.9.5 — 사용자 지시) — 센서 재초기화: stop_periodic_measurement(500ms 대기) 후
 * reinit(20ms 대기). 데이터시트: 이것으로 안 되면 전원을 껐다 켜야 함(이 보드는 SCD41 전원을 GPIO로 못 끊음) */
bool scd41_reinit(void);
