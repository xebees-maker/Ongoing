#pragma once

/**
 * @file    device_config.h
 * @brief   Cntl이 소유하는 CAM/SENS 원격 설정값의 단일 저장소(2026-08-08 설계).
 *          CAM/SENS는 이 값을 로컬에 저장하지 않음 — 페어링될 때마다 Cntl이 여기서 읽어서
 *          CAM_CONFIG_SET으로 밀어줌. 사용자가 설정탭에서 값을 바꾸면(Apply) 여기 저장되고,
 *          현재 페어링된 CAM에도 즉시 전송됨(node_hub_apply_cam_capture_interval_sec/
 *          node_hub_apply_response_interval_sec 참고).
 *
 *          촬영주기/AGC/AEC/XCLK는 카메라별(mac) 설정(배터리/SD/화질 트레이드오프가 카메라
 *          마다 다를 수 있음), 응답성/적응형 반응시간은 시스템 전체 공통 설정(연결성/절전
 *          트레이드오프, 캠·센스 공용) — 2026-09-18: 촬영주기/AGC/AEC/XCLK를 전역 값 하나
 *          공유 방식에서 mac 키 슬롯 방식(Sens 측정주기와 동일 패턴)으로 재설계.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* app_main()에서 fs_init() 이후 한 번 호출 — 저장된 값 있으면 복원, 없으면 기본값 유지 */
void device_config_load(void);

/* CAM 촬영주기(초, 0=자동촬영 끔) — 카메라별(mac) 설정, 기본값 1800(30분).
 * 2026-09-18(사용자 지시 — "개별 캠마다 설정") — 전역 값 하나에서 Sens 측정주기와 동일한
 * mac 키 슬롯 방식으로 재설계. getter는 항상 유효한 값을 반환(없는 mac이면 디폴트) */
uint32_t device_config_get_cam_capture_interval_sec(const uint8_t *mac);
void     device_config_set_cam_capture_interval_sec(const uint8_t *mac, uint32_t sec);

/* 시스템 공통 응답성(초) — 기본값 2 */
uint32_t device_config_get_response_interval_sec(void);
void     device_config_set_response_interval_sec(uint32_t sec);

/* 적응형 반응시간(초, 2026-08-10) — 마지막 사용자 조작(지금촬영/목록갱신/삭제/전체삭제/
 * 사진선택) 이후 이만큼 조용하면 node_hub가 현재 페어링된 CAM에 SLEEP_NOW를 보내
 * 유휴여유를 기다리지 않고 즉시 재움. 기본값 10초. CAM에는 전송 안 함(Cntl 내부 판단
 * 기준일 뿐) */
uint32_t device_config_get_adaptive_response_sec(void);
void     device_config_set_adaptive_response_sec(uint32_t sec);

/* AGC(자동게인)/AEC(자동노출) On/Off(2026-08-21, 세로줄 노이즈 진단용) — 카메라별(mac)
 * 설정, 기본값 true. 2026-09-18: mac 키 슬롯 방식(위 촬영주기와 동일 이유) */
bool device_config_get_agc_enable(const uint8_t *mac);
void device_config_set_agc_enable(const uint8_t *mac, bool enable);
bool device_config_get_aec_enable(const uint8_t *mac);
void device_config_set_aec_enable(const uint8_t *mac, bool enable);

/* XCLK(MHz) 프리셋(2026-08-21, 화질/노이즈 진단용) — 카메라별(mac) 설정, 기본값 10.
 * 2026-09-18: mac 키 슬롯 방식(위와 동일 이유) */
uint8_t device_config_get_xclk_mhz(const uint8_t *mac);
void    device_config_set_xclk_mhz(const uint8_t *mac, uint8_t mhz);

/* WiFi 모드(2026-08-29) — false=종속(STA, 기존 WIFI_SSID/PASSWORD 또는 아래 sta_ssid로 접속),
 * true=독립(AP, node_hub.c의 CNTL_AP_SSID/PASSWORD/CHANNEL로 자체 AP). 기본값 false(STA) —
 * 기존 동작과 동일하게 유지. node_hub.c가 부팅 시 이 값을 읽어 런타임에 분기(예전
 * CNTL_WIFI_STANDALONE_AP_TEST 컴파일타임 스위치를 대체) */
bool device_config_get_wifi_ap_mode(void);
void device_config_set_wifi_ap_mode(bool ap_mode);

/* STA 자격증명(2026-08-29, 여러 개 저장 가능하도록 재설계 — 사용자 지시) — 내부적으로
 * PSRAM에 할당된 슬롯 배열(device_config.c의 STA_CREDENTIAL_SLOTS)로 저장, set할 때마다
 * 그 SSID를 맨 앞(=활성)으로 옮김. get_sta_ssid/get_sta_password는 항상 "가장 최근에
 * set된(=활성)" 슬롯을 가리켜서 기존 호출부(node_hub.c 등)는 그대로 씀. 빈 문자열이면
 * 미설정 상태 — 이땐 node_hub.c가 기존 하드코딩 WIFI_SSID/WIFI_PASSWORD로 폴백 */
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
 * 슬롯 배열로 저장(사용자 지시: "센스마다 만들 필요도 있겠는데"). 2026-09-18: 없는 mac을
 * 조회해도 0(sentinel) 대신 항상 유효한 디폴트를 반환 — 호출부는 반환값을 그대로 신뢰할 것,
 * 0-체크로 각자 폴백하지 말 것(과거에 node_hub.c/ui_main.c가 서로 다른 폴백값을 써서
 * 불일치가 생겼던 문제). "명시적으로 설정한 적 있는가"는 아래 _is_set()으로 따로 확인 */
uint32_t device_config_get_sens_sample_interval_sec(const uint8_t *mac);
void     device_config_set_sens_sample_interval_sec(const uint8_t *mac, uint32_t sec);
bool     device_config_sens_sample_interval_is_set(const uint8_t *mac);

/* Alias(2026-09-08, 사용자 설계) — 장치별 사용자 지정 표시 이름("Sens xxxxxx" 대신 임의
 * 문자열). sens_interval과 같은 mac 키 슬롯 배열이지만 이 슬롯의 존재 자체가 "이번에 처음이
 * 아니라 예전에 한 번이라도 페어링에 성공한 적 있는 장치"라는 의미도 겸함(get_alias는
 * 빈 문자열이면 미설정 — 호출부가 기본 이름으로 폴백, is_known_device로 슬롯 유무만 별도
 * 확인 가능) */
#define DEVICE_CONFIG_ALIAS_MAX_LEN 32
const char *device_config_get_alias(const uint8_t *mac);
void        device_config_set_alias(const uint8_t *mac, const char *alias);
bool        device_config_is_known_device(const uint8_t *mac);
/* 페어링 성공 시 node_hub가 호출 — 슬롯이 없으면 alias 빈 문자열로 새로 만듦(이미 있으면
 * 손 안 댐, 기존 alias 보존) */
void        device_config_mark_known_device(const uint8_t *mac);

/* 자동연결(2026-09-08, 사용자 설계) — 대기중 장치를 수동 확인 없이 즉시 페어링.
 * auto_connect_new가 켜지면 처음 보는 장치까지 포함하므로 auto_connect_known을 사실상
 * 포함하는 관계 — UI에서 이 종속관계를 표현할 것. 둘 다 기본값 false(기존 수동 확인 동작과
 * 동일하게 유지) */
bool device_config_get_auto_connect_known(void);
void device_config_set_auto_connect_known(bool enable);
bool device_config_get_auto_connect_new(void);
void device_config_set_auto_connect_new(bool enable);

#ifdef __cplusplus
}
#endif
