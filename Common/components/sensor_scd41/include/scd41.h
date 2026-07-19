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
