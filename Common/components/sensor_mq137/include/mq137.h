/**
 * @file    mq137.h
 * @brief   MQ137 암모니아(NH3) 가스 센서 — 아날로그(AO) + 디지털(DO) 드라이버
 *
 * MQ137은 반도체식(히터 필요) 센서라 배터리 노드엔 부적합할 수 있음 —
 * 2026-09-12 실험 단계, SC05-NH3(전기화학식)와 실측 비교 후 하나를 고를 예정.
 *
 * 2026-09-12(사용자 지시 — "변환해") — ppm 변환 구현. Winsen 공식 MQ137 데이터시트(정식
 * 매뉴얼, ver1.6)에 나온 저항 계산식만 확실하고("Rs=(Vc/VRL-1)×RL", Vc=5.0V), Rs/Ro를
 * ppm으로 바꾸는 식 자체는 데이터시트에 없음(그래프만 있음) — 아래는 전부 사용자가 제공한
 * 잠정치이고, "나중에 실제로 보정해야 함"이 사용자 지시로 명시됨(TODO 참고):
 *   - RL=10kΩ: 보드 트리머(Vr)를 안 돌렸을 때의 통상 기본값(사용자 확인)
 *   - Ro: 고정값 — 2026-09-12에 "지금이 맑은 공기"라는 사용자 확인 상태에서 측정된 AO=
 *     1861.5mV 1회 측정치로부터 역산(RL=10kΩ, Vc=5.0V 가정). 매 부팅 자동 재측정 안 함
 *     (사용자 지시: "보정값을 만들 때까지는 고정값으로").
 *   - a=102.6, b=-2.48: 데이터시트 로그-로그 그래프를 외부에서 피팅한 근사 계수(사용자 제공,
 *     Winsen이 직접 공표한 공식 아님).
 * TODO(할일, 사용자 지시로 명시) — a, b, Ro 전부 나중에 실제 보정 필요(다수 농도점 실측
 * 후 재피팅). 지금 값은 전부 잠정치.
 *
 * 2026-09-12(사용자 지시 — "71ppm 나오는데 0에 수렴하도록 보정", "웹에서 아티클/Vr 기본값
 * 찾아보고") 추가 조사 및 수정:
 *   - Winsen 공식 매뉴얼(ver1.4) 원문 재확인 결과 Fig5(VRL-농도 그래프)는 RL=4.7KΩ 기준이라고
 *     명시되어 있음 — "RL=10kΩ은 트리머 기본값"이라는 기존 가정은 근거를 찾지 못함. 이 보드류
 *     (LM393 비교기 탑재 브레이크아웃) 트리머는 보통 DO 임계값만 조정하고 AO 경로엔 영향이
 *     없는 사례가 일반적. 다만 RL 절대값 오차는 Rs/Ro 비율 계산에서 Ro·Rs 양쪽에 동일 배율로
 *     걸려 상쇄되므로 ppm 비율 자체엔 영향 없음(Rs 절대 Ω 표시에만 영향, 아직 안 함).
 *   - a,b 계수 자체도 의심됨: Ro가 "맑은 공기 저항"으로 정의된다면 ratio(Rs/Ro)=1일 때 ppm은
 *     0에 가까워야 하는데, 순수 거듭제곱식 a×ratio^b는 ratio=1일 때 항상 a를 반환하고 유한한
 *     ratio에서는 절대 0이 될 수 없는 구조 — a=102.6, b=-2.48이 실제 Winsen Fig3 그래프를
 *     정확히 피팅한 값인지 근거가 약함.
 *   - 그래서 mq137_read()의 출력을 "raw = a×ratio^b" 그대로 쓰지 않고 "Ro 기준점 대비 초과분"
 *     (raw - a, 0 미만은 0으로 클램프)으로 재정의함 — 기준점과 같거나 더 깨끗하면 0, 기준보다
 *     탁하면(Rs가 Ro보다 낮아지면) 그 초과분만큼 양수. 여전히 단일 지점 보정의 잠정치.
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

/**
 * @brief MQ137 초기화
 * @param adc_handle 이미 만들어진 공유 ADC 유닛 핸들(battery_get_adc_handle() 등) —
 *                   esp32c3는 ADC 유닛이 사실상 1개뿐이라 여기서 새로 만들지 않고 공유
 * @param ao_channel AO가 물린 ADC 채널
 * @param atten      감쇠 설정
 * @param do_gpio    DO(디지털 임계값 출력)가 물린 GPIO
 * @return 성공 시 true
 */
bool mq137_init(adc_oneshot_unit_handle_t adc_handle, adc_channel_t ao_channel,
                 adc_atten_t atten, gpio_num_t do_gpio);

/**
 * @brief 측정 1회 수행 — ppm은 위 파일 설명의 잠정 계수(RL/Ro/a/b) 기반 근사치
 * @param ppm       암모니아 농도(ppm) 추정치 출력, NULL 가능
 * @param do_alarm  DO 핀 레벨(보통 임계값 초과 시 LOW 또는 HIGH, 모듈마다 다름) 출력, NULL 가능
 * @return 성공 시 true
 */
bool mq137_read(float *ppm, bool *do_alarm);
