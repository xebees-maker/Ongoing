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

/** @brief 응답성 설정(초, 1~10 권장) — esp_now_channelsync PING 주기 산정에 그대로 씀 */
void cam_node_set_response_interval_sec(uint32_t sec);
uint32_t cam_node_get_response_interval_sec(void);

/**
 * @brief 2026-08-08 기준 no-op — Light Sleep을 두 번(2026-07-28, 2026-08-08) 시도했으나 둘 다
 *        USB-Serial-JTAG 콘솔이 유휴 상태에서 응답 없어지는 문제로 되돌림(project_cam_esp_now_
 *        production 메모리 참고, 2차 시도 상세는 cam_node.c app_main() 주석). Light Sleep
 *        자체가 꺼져있어서(cam_node.c의 s_no_sleep_lock == NULL) 지금은 아무 동작도 안 함 —
 *        호출부(camera_capture_one/dev_console.c/esp_now_cam.c)는 나중에 다른 절전 설계(예:
 *        콘솔을 운영 빌드에서 완전히 빼고 나서 재시도)로 돌아올 때를 위해 그대로 남겨둠.
 */
void cam_node_sleep_lock_acquire(void);
void cam_node_sleep_lock_release(void);
