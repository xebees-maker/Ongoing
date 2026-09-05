#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_now_link.h"

/**
 * ESP-NOW 노드 신원 + 허브 페어링.
 *
 * 2026-09-05 — SCD41 헤드리스 빌드는 CAM의 CASK(WAKE_HELLO 웨이크 사이클) 구조로 전환됨
 * (esp_now_node_cask.c, project_cntl_cam_wake_hello_no_mem_desync/
 * project_cntl_web_full_ui_injection_design_2026_09_04 세션에서 이어지는 작업). 그 외
 * 빌드(레거시 C6 콤보, DHT22/SHT45/SHT40 헤드리스)는 이 파일(esp_now_node.c)의 예전
 * ADVERTISE/PAIR_REQUEST/PAIR_ACK/SENSOR_DATA 설계 그대로 유지 — 사용자가 이번 작업
 * 범위를 SCD41 헤드리스 빌드로만 명시적으로 한정함.
 */

const char *esp_now_node_get_name(void);
bool esp_now_node_is_paired(void);

/**
 * @brief Green 상태 LED GPIO 등록 (esp_now_node_init() 이전에 호출).
 *        생략하면(호출 안 하면) LED 갱신 없이 동작.
 */
void esp_now_node_set_status_led(gpio_num_t pin);

#if CONFIG_SENS_SENSOR_SCD41

/**
 * @brief 2026-09-05(사용자 지시로 정정) — sensor_kind/chan_count/chan_type은 페어링 완료
 *        (PAIR_ACK) 순간에 1회만 콘에 실어보냄(esp_now_pair_ack_t 참고) — 그래서 이 함수가
 *        그 값들을 받아서 기억해뒀다가 PAIR_ACK 전송 시 채움. 값 자체는 부팅 때부터 이미
 *        정해져 있음(빌드 시 하드코딩, 자동판별 안 함 — 사용자 지시). chan_count는
 *        ESP_NOW_MAX_CHANNELS를 넘으면 안 됨.
 */
void esp_now_node_init(sensor_kind_t sensor_kind, uint8_t chan_count, const uint8_t *chan_type);

/**
 * @brief 이번 부팅의 딥슬립 웨이크 원인 + 실제로 잤던 시간을 판정/기록(2026-09-05, CAM의
 *        cam_node.c capture_wake_reason()과 동일 이식). app_main 최상단, WiFi/센서 초기화
 *        "전"에 반드시 먼저 호출 — esp_now_node_report_reading()이 이 값을 그대로 실어보냄.
 */
void esp_now_node_capture_wake_info(void);

/**
 * @brief 센서 값 하나를 보고하고 허브와 CASK(WAKE_HELLO_SENS -> CONFIG -> SLEEP_NOW)
 *        왕복을 1회 수행(esp_now_node_cask.c). CAM의 esp_now_cam_reconnect()와 동일한
 *        역할을 겸함(패스트패스 시도 또는 폴백스캔 시작) — 그래서 esp_now_node_init()
 *        직후에도, 이후 CASK 루프의 매 재확인/Live 재전송에도 이 함수 하나만 반복 호출하면
 *        됨(2026-09-05, "캠과 동일한 구조로" 재수정). 아직 페어링 전이고 허브도 전혀
 *        모르면 그냥 false를 반환하고 아무것도 안 보냄(채널스캔은 독립적으로 계속 진행 중).
 *        2026-09-05(사용자 지시로 정정) — sensor_kind/chan_type은 여기서 안 받음(페어링 때
 *        1회만 esp_now_node_init()으로 전달, 매 캐스크마다 다시 보낼 필요 없음). chan_count는
 *        여전히 필요(chan_ok/chan_val 배열 길이). measurement_id는 sens_deep_sleep_node.c가
 *        RTC 메모리에 들고 있는 값 — 새로 측정 성공할 때마다 1 증가, 실패/미도달이면 직전
 *        값 그대로(콘이 "안 바뀐 값"을 구분하는 용도). Live 모드(직전 SLEEP_NOW.sleep_sec==0)
 *        로 재호출할 땐 새로 측정하지 말고 직전과 같은 chan_val/measurement_id를 그대로
 *        다시 넘기면 됨. chan_count는 ESP_NOW_MAX_CHANNELS를 넘으면 안 됨.
 */
bool esp_now_node_report_reading(uint8_t chan_count, const uint8_t *chan_ok,
                                  const float *chan_val, uint32_t measurement_id,
                                  uint16_t battery_adc_raw, uint16_t battery_mv);

/**
 * @brief 가장 최근 CASK 왕복에서 받은 SLEEP_NOW.sleep_sec — 0이면 계속 깨어있기(Live),
 *        그 외엔 그 값 그대로 실제 esp_sleep_enable_timer_wakeup()에 씀(캠과 동일 구조,
 *        노드 쪽 특별취급 없음). 2026-09-05(사용자 지시로 정정) — "센스는 자기 측정주기마다
 *        깨야 한다"는 요구는 콘 쪽(esp_now_hub.c의 send_cask_sleep_now())이 그 센스에
 *        저장된 측정주기를 기준값으로 이 sleep_sec을 계산하는 것으로 충족됨 — 노드 쪽에서
 *        별도로 로컬 값을 다시 끼워 넣으면 "콘이 계산한 값과 실제로 자는 시간이 다를 수
 *        있음"이라는 어긋남 경로가 생기므로 만들지 않음.
 */
uint32_t esp_now_node_get_last_sleep_sec(void);

/**
 * @brief SENS_CONFIG_SET으로 받은(RTC 메모리에 유지되는) 센스 고유 측정주기(초). 실제
 *        딥슬립 시간(esp_now_node_get_last_sleep_sec())과는 별개 용도 — 전력소모가 큰
 *        센서가 있어서 "깰 때마다"가 아니라 "이 주기만큼 실제로 지났을 때만" 재측정해야
 *        하므로(사용자 지시), sens_deep_sleep_node.c가 이 값을 기준으로 그 판단을 함.
 */
uint32_t esp_now_node_get_sample_interval_sec(void);

/**
 * @brief esp_deep_sleep_start() 호출 "직전"에 sensor_node.c가 호출 — 다음 부팅에서
 *        esp_now_node_capture_wake_info()가 "실제로 얼마나 잤는지" 계산할 기준시각을 남김
 *        (CAM의 s_sleep_entry_unix_time과 동일 이유).
 */
void esp_now_node_note_sleep_entry(void);

/**
 * @brief 아래 4개는 CAM의 cam_node_signal_recheck()/cam_node_note_sleep_now_requested()/
 *        cam_node_reset_sleep_now_state()/cam_node_note_scan_restarted()와 동일 역할 —
 *        여기(esp_now_node.h)에 선언돼있지만 구현은 esp_now_node_cask.c가 아니라
 *        sens_deep_sleep_node.c(app_main 쪽, 이벤트기반 대기 루프의 세마포어/플래그를
 *        소유)에 있음. esp_now_node_cask.c의 recv_cb/report_reading이 상태 변화 지점마다
 *        이 함수들을 불러서 app_main의 대기를 깨우거나 상태를 갱신함(캠의 esp_now_cam.c가
 *        cam_node_*()를 부르는 것과 완전히 동일한 교차호출 패턴, 2026-09-05).
 */
void esp_now_node_signal_recheck(void);
void esp_now_node_note_sleep_now_requested(uint32_t sleep_sec);
void esp_now_node_reset_sleep_now_state(void);
void esp_now_node_note_scan_restarted(void);

#else /* !CONFIG_SENS_SENSOR_SCD41 — 예전 설계 그대로(DHT22/SHT45/SHT40 헤드리스, 레거시 C6 콤보) */

void esp_now_node_init(void);

/**
 * @brief 매 샘플 주기마다 앱(sensor-c6.c / sensor_node.c)이 호출 — 다음 SENSOR_DATA
 *        전송 타이머가 돌 때 이 값을 그대로 실어보낸다. chan_count는
 *        ESP_NOW_MAX_CHANNELS를 넘으면 안 됨.
 */
void esp_now_node_set_readings(sensor_kind_t sensor_kind, uint8_t chan_count,
                                const uint8_t *chan_type, const uint8_t *chan_ok,
                                const float *chan_val,
                                bool batt_ok, int batt_pct, bool powered);

/**
 * @brief 페어링 후 SENSOR_DATA 전송 주기 변경 (기본 1초). 센서 샘플링 인터벌을 늘린
 *        빌드(예: SCD41 절전모드, 10초)에서 라디오도 같이 그 주기로 늦춰서 절전 효과가
 *        나게 하려는 용도 — esp_now_node_init() 전후 아무 때나 호출 가능, 이미 타이머가
 *        돌고 있으면 즉시 새 주기로 재시작한다.
 */
void esp_now_node_set_data_period_ms(uint32_t ms);

#endif /* CONFIG_SENS_SENSOR_SCD41 */
