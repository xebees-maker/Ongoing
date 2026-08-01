#pragma once

/**
 * 보드 실장 PCF85063A RTC(터치/CH422G와 같은 I2C 버스, GPIO8/9) — Cntl 자체 시각의
 * 근거. 지금은 배터리(CR927)가 안 꽂혀있어서 전원이 꺼지면 시각이 유지되지 않는데, 그런
 * 경우(RTC가 말이 안 되는 옛날 값을 갖고 있을 때) 빌드 시각으로 초기 seed를 해둔다.
 * 추가로, 플래시 스크립트가 매번 써주는 /assets/time_sync.txt(플래시 시점 PC 시각)가
 * RTC 현재값보다 미래면 그 값으로 전진시킨다(둘 다 [[project-cntl-cam-photo-list-rtc]]
 * 참고) — 나중에 배터리를 꽂거나 설정 화면에서 시각을 맞추면 이 로직들은 RTC가 이미
 * 더 최신이라 자연히 안 타게 됨.
 */

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* waveshare_esp32_s3_rgb_lcd_init() 호출 후에만 사용할 것(I2C 버스가 그때 만들어짐) */
esp_err_t rtc_sync_init(void);

/* 현재 시각(RTC로 세팅된 시스템 클록 기준) — 페어링 시 노드에 알려줄 때 씀 */
uint32_t rtc_sync_get_unix_time(void);

#ifdef __cplusplus
}
#endif
