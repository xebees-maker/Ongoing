#pragma once

#include <stdbool.h>
#include <stdint.h>

/** @brief 캡처 타이머와 무관하게 즉시 1장 촬영 — 개발 콘솔(dev_console.c)의 shot 명령용 */
bool cam_node_capture_now(void);

/**
 * @brief size_name이 있으면 그 해상도로 센서를 바꾼 뒤(다음 촬영부터 계속 적용됨) 촬영,
 *        NULL/빈 문자열이면 현재 해상도 그대로 촬영. 지원: "5m"/"qvga"/"vga"(대소문자 무관)
 */
bool cam_node_capture_now_sized(const char *size_name);

/** @brief 10초 주기 자동 촬영 on/off — 개발 콘솔의 auto 명령용(기본 off) */
void cam_node_set_auto_capture(bool enable);
bool cam_node_get_auto_capture(void);

/** @brief JPEG 화질(0~63, 낮을수록 고화질) 런타임 변경 — 개발 콘솔의 q 명령용 */
bool cam_node_set_jpeg_quality(int quality);

/** @brief XCLK(MHz) 런타임 변경 후 PLL 재계산까지 수행 — 개발 콘솔의 xclk 명령용 */
bool cam_node_set_xclk(int mhz);

/**
 * @brief 자동촬영 주기(초 단위) 런타임 변경 — 0이면 자동촬영 끔. esp_now_cam.c의
 *        CAM_CONFIG_SET 핸들러가 부름. 부팅 시 저장된 값으로 자동 복원됨(cam_node_settings_load).
 */
void cam_node_set_capture_interval_sec(uint32_t sec);
uint32_t cam_node_get_capture_interval_sec(void);

/** @brief 응답성 설정(초) — esp_now_channelsync PING 주기 + Deep Sleep 사이클 길이(=RWDT
 *         예산 재무장 기준)에 그대로 씀. 2026-08-10: Deep Sleep 전환으로 값 구간이
 *         1/3/10/30/1800초(즉시/빠름/균형/절전/최대절전)로 재정의됨 */
void cam_node_set_response_interval_sec(uint32_t sec);
uint32_t cam_node_get_response_interval_sec(void);

/** @brief AGC(자동게인)/AEC(자동노출) On/Off — 2026-08-21, 세로줄 노이즈 진단용. 매 촬영
 *         직전에 sensor_t로 반영됨(cam_node.c의 apply_agc_aec_settings() 참고). 끔(0) 전환
 *         시엔 그 순간의 자동값에 고정될 뿐, 별도 수동 게인/노출값을 지정하지 않음 */
void cam_node_set_agc_enable(bool enable);
void cam_node_set_aec_enable(bool enable);

/** @brief XCLK(MHz) 원격 설정(1~40) — 2026-08-21, 화질/노이즈 진단용. 카메라가 이미
 *         초기화돼 있으면 즉시 반영(PLL 재계산), 아니면 다음 필요시 초기화 때 적용됨.
 *         콘솔 xclk 명령(cam_node_set_xclk)과 별개 진입점 — 그쪽은 항상 즉시 적용 시도 */
void cam_node_set_xclk_target_mhz(uint8_t mhz);

/** @brief 이번 부팅의 Deep Sleep 웨이크 원인(2026-08-10) — app_main 최상단에서 1회
 *         판정(cam_wake_reason_t, esp_now_link.h) */
uint8_t cam_node_get_wake_reason(void);

/** @brief 직전에 실제로 잔 시간(초, 2026-08-10) — RTC 메모리로 딥슬립 경계 너머 전달됨.
 *         wake_reason이 TIMER가 아니면(RWDT/POWERON) 0 — 실제로 안 잤으므로 */
uint32_t cam_node_get_last_actual_sleep_sec(void);

/** @brief Cntl의 ESP_NOW_MSG_SLEEP_NOW 수신 시 호출 — CASK 종료 신호. sleep_sec은 그
 *         메시지가 실어온 값 그대로(0=안 자고 곧장 재연결 루프, 그 외=실제 딥슬립 시간) */
void cam_node_note_sleep_now_requested(uint32_t sleep_sec);

/** @brief 2026-08-26 — WAKE_HELLO/PAIR_ACK를 "보내기 직전"에 호출해서 SLEEP_NOW 대기 상태를
 *         미리 깨끗하게 함(esp_now_cam.c 참고) — 순서 버그 수정: 이걸 전송 "후"에 하면 그
 *         사이 도착한 진짜 SLEEP_NOW가 덮어써져 사라짐 */
void cam_node_reset_sleep_now_state(void);

/** @brief 2026-08-23 — 이벤트드리븐 재확인 신호(CAML에서 검증 후 이식). 페어링 상태 변화
 *         (on_channel_synced), busy->idle 전환(mark_transfer_idle), SLEEP_NOW 수신 등
 *         app_main의 웨이크 루프 판정에 영향을 주는 이벤트가 생길 때마다 호출 — 대기 루프가
 *         세마포어로 이걸 기다리다가 즉시 깨서 재판정함(50ms 고정 폴링 대신) */
void cam_node_signal_recheck(void);

/** @brief 2026-08-25(CASK 재설계) — 폴백 채널스캔이 (재)시작될 때 esp_now_cam.c의
 *         esp_now_cam_reconnect()가 호출 — 이전 스윕 완료 기록을 무효화. 예전엔
 *         on_channel_lost_sync()가 불렀는데, 그 콜백 자체가 핑퐁과 함께 없어져서 호출부만
 *         옮겨짐(역할은 동일) */
void cam_node_note_scan_restarted(void);

/* 2026-08-23(폐기됨) — cam_node_note_channel_synced()는 "동기화=이번 웨이크 완료"라는 잘못된
 * 전제로 만들어졌다가, "ACK 받아도 스캔/광고는 페어링될 때까지 계속"으로 설계가 바뀌면서
 * 제거됨(사용자 지시). s_sweep_completed는 다시 원래대로 "랩어라운드 완료"만 뜻함 */

/** @brief 카메라가 이번 웨이크 사이클에 이미 초기화됐는지(2026-08-21, 지금촬영 핸드셰이크
 *         재설계용) — esp_now_cam.c가 CAPTURE_STATUS(INIT_NEEDED)를 보낼지 판단하는 데 씀 */
bool cam_node_is_camera_ready(void);

/** @brief 카메라 초기화만 수행(아직 안 됐으면) — ensure_camera_ready(true)의 공개 진입점.
 *         cam_node_capture_now()도 내부에서 똑같이 부르므로 이미 초기화됐으면 그냥 통과되는
 *         멱등 호출 — esp_now_cam.c가 INIT_NEEDED/INIT_DONE 사이에 실제 초기화를 수행하는 데 씀 */
bool cam_node_ensure_camera_ready(void);

/** @brief 배터리 전압 읽기(2026-08-22) — CH32V003 IO익스팬더 ADC(0x06) 원시값을 읽어
 *         스키매틱 분배비(R39=200K/R42=100K, ×3)와 CH32V003 ADC 추정 기준(10bit/3.3V)으로
 *         mV 환산. 실패 시 false(out_raw/out_mv 미정의) — mV->%% 변환은 Cntl 쪽 공통함수가
 *         담당(esp_now_hub.c), CAM은 mV까지만 계산해서 DEEP_SLEEP_STATS에 실어 보냄.
 * @param out_raw CH32V003 ADC 원시값(진단/실측대조용, 그대로 같이 보고됨)
 * @param out_mv  환산된 배터리 전압(mV)
 */
bool cam_node_read_battery_mv(uint16_t *out_raw, uint16_t *out_mv);
