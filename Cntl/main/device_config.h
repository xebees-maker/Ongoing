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

#ifdef __cplusplus
}
#endif
