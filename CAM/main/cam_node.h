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

/** @brief 이번 부팅의 Deep Sleep 웨이크 원인(2026-08-10) — app_main 최상단에서 1회
 *         판정(cam_wake_reason_t, esp_now_link.h) */
uint8_t cam_node_get_wake_reason(void);

/** @brief 지금 자도 되는가 판정(2026-08-10, Deep Sleep 제어흐름) — 페어링 완료 + 유휴
 *         + 최소 유예시간 경과, 또는 Cntl 못 찾은 채 상한시간 초과 */
bool cam_node_wake_window_done(uint32_t awake_start_ms);

/** @brief recv_cb가 의미있는 메시지(사용자 명령)를 처리할 때마다 호출 — 유휴 판정용
 *         타임스탬프 갱신(2026-08-10, cam_node_wake_window_done()이 참고) */
void cam_node_note_activity(void);

/** @brief Cntl의 ESP_NOW_MSG_SLEEP_NOW 수신 시 호출(2026-08-10, 적응형 반응시간) — 이번
 *         사이클 남은 유휴여유를 기다리지 않고 cam_node_wake_window_done()이 즉시 true를
 *         반환하게 함 */
void cam_node_note_sleep_now_requested(void);
