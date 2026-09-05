#pragma once

/**
 * @file    device_config.h
 * @brief   Cntl이 소유하는 CAM/SENS 원격 설정값의 단일 저장소(2026-08-08 설계).
 *          CAM/SENS는 이 값을 로컬에 저장하지 않음 — 페어링될 때마다 Cntl이 여기서 읽어서
 *          CAM_CONFIG_SET으로 밀어줌. 사용자가 설정탭에서 값을 바꾸면(Apply) 여기 저장되고,
 *          현재 페어링된 CAM에도 즉시 전송됨(esp_now_hub_apply_cam_capture_interval_sec/
 *          esp_now_hub_apply_response_interval_sec 참고).
 *
 *          촬영주기는 카메라별 설정(배터리/SD 트레이드오프), 응답성은 시스템 전체 공통
 *          설정(연결성/절전 트레이드오프) — 지금은 CAM이 보통 1대라 촬영주기도 전역 값
 *          하나로 둠(esp_now_hub_bench_start()가 이미 "첫 페어링된 CAM" 대상으로 동작하는
 *          것과 같은 단순화). CAM이 여러 대로 늘면 MAC별 저장으로 확장 필요.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* app_main()에서 fs_init() 이후 한 번 호출 — 저장된 값 있으면 복원, 없으면 기본값 유지 */
void device_config_load(void);

/* CAM 촬영주기(초, 0=자동촬영 끔) — 기본값 1800(30분, CAM Kconfig 기본과 동일) */
uint32_t device_config_get_cam_capture_interval_sec(void);
void     device_config_set_cam_capture_interval_sec(uint32_t sec);

/* 시스템 공통 응답성(초) — 기본값 2 */
uint32_t device_config_get_response_interval_sec(void);
void     device_config_set_response_interval_sec(uint32_t sec);

/* 적응형 반응시간(초, 2026-08-10) — 마지막 사용자 조작(지금촬영/목록갱신/삭제/전체삭제/
 * 사진선택) 이후 이만큼 조용하면 esp_now_hub가 현재 페어링된 CAM에 SLEEP_NOW를 보내
 * 유휴여유를 기다리지 않고 즉시 재움. 기본값 10초. CAM에는 전송 안 함(Cntl 내부 판단
 * 기준일 뿐) */
uint32_t device_config_get_adaptive_response_sec(void);
void     device_config_set_adaptive_response_sec(uint32_t sec);

/* AGC(자동게인)/AEC(자동노출) On/Off(2026-08-21, 세로줄 노이즈 진단용) — 카메라별 설정
 * (촬영주기와 같은 그룹), 기본값 true(센서 전원인가 기본값과 일치) */
bool device_config_get_agc_enable(void);
void device_config_set_agc_enable(bool enable);
bool device_config_get_aec_enable(void);
void device_config_set_aec_enable(bool enable);

/* XCLK(MHz) 프리셋(2026-08-21, 화질/노이즈 진단용) — 카메라별 설정, 기본값 24(OV5640 기존
 * 컴파일타임 상수와 일치) */
uint8_t device_config_get_xclk_mhz(void);
void    device_config_set_xclk_mhz(uint8_t mhz);

/* WiFi 모드(2026-08-29) — false=종속(STA, 기존 WIFI_SSID/PASSWORD 또는 아래 sta_ssid로 접속),
 * true=독립(AP, esp_now_hub.c의 CNTL_AP_SSID/PASSWORD/CHANNEL로 자체 AP). 기본값 false(STA) —
 * 기존 동작과 동일하게 유지. esp_now_hub.c가 부팅 시 이 값을 읽어 런타임에 분기(예전
 * CNTL_WIFI_STANDALONE_AP_TEST 컴파일타임 스위치를 대체) */
bool device_config_get_wifi_ap_mode(void);
void device_config_set_wifi_ap_mode(bool ap_mode);

/* STA 자격증명(2026-08-29, 여러 개 저장 가능하도록 재설계 — 사용자 지시) — 내부적으로
 * PSRAM에 할당된 슬롯 배열(device_config.c의 STA_CREDENTIAL_SLOTS)로 저장, set할 때마다
 * 그 SSID를 맨 앞(=활성)으로 옮김. get_sta_ssid/get_sta_password는 항상 "가장 최근에
 * set된(=활성)" 슬롯을 가리켜서 기존 호출부(esp_now_hub.c 등)는 그대로 씀. 빈 문자열이면
 * 미설정 상태 — 이땐 esp_now_hub.c가 기존 하드코딩 WIFI_SSID/WIFI_PASSWORD로 폴백 */
const char *device_config_get_sta_ssid(void);
const char *device_config_get_sta_password(void);
void        device_config_set_sta_credentials(const char *ssid, const char *password);

/* 저장된 슬롯 중 ssid가 일치하는 게 있으면 그 비밀번호를, 없으면 NULL을 반환 — "찾기"
 * 팝업에서 예전에 접속했던 네트워크를 다시 선택하면 비번을 미리 채우는 용도 */
const char *device_config_find_sta_password(const char *ssid);

/* NACK/DONE_ACK 최대 재전송 라운드 수(2026-08-21) — CAM/CNTL 둘 다 "몇 라운드째인가"를
 * 각자 판단 기준으로 쓰므로 반드시 같은 숫자여야 하는 값(둘 다 하드코딩했다가 off-by-one으로
 * 어긋난 적 있음, project_cntl_cam_photo_fetch_nack_round_bug 메모리 참고) — CNTL이 유일한
 * 소유자로서 이 값을 CAM_CONFIG_SET에 실어 보냄. 사용자가 조정할 이유가 없는 순수 프로토콜
 * 신뢰성 값이라 설정 UI/영구저장 없음 — 상수 하나를 여기 한 곳에서만 관리 */
uint8_t device_config_get_nack_max_rounds(void);

/* Sens 노드별 샘플링 주기(초, 2026-09-05) — 붙은 센서 종류가 노드마다 달라서(온도/습도/
 * CO2/암모니아 등) 캠의 촬영주기처럼 전역 하나로 두지 않고 STA 자격증명과 같은 mac 키
 * 슬롯 배열로 저장(사용자 지시: "센스마다 만들 필요도 있겠는데"). 없는 mac을 조회하면
 * 0을 반환(미설정) — 호출부(esp_now_hub.c push_sens_config_to)가 기본값으로 폴백 */
uint32_t device_config_get_sens_sample_interval_sec(const uint8_t *mac);
void     device_config_set_sens_sample_interval_sec(const uint8_t *mac, uint32_t sec);

#ifdef __cplusplus
}
#endif
