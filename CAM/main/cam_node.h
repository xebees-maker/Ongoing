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
