#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief RTC 워치독(RWDT) 안전장치(2026-08-10) — Deep Sleep 사이클마다 소프트웨어가
 *        (버그로) esp_deep_sleep_start()를 못 부르거나 어딘가에서 멈춰버려도, 이번 사이클
 *        전체(깨어있는 시간+의도한 슬립 시간)를 넘기면 RTC 워치독이 강제로 리셋시킨다.
 *        ESP32-S3는 편의 API(esp_hw_support/rtc_wdt.h)가 ESP32/S2 전용이라 저수준
 *        hal/wdt_hal.h로 직접 구현(rwdt_guard.c 참고).
 */

/** @brief 이번 사이클 전체를 커버하는 예산으로 RWDT를 (재)무장. app_main 최상단, 카메라/
 *         WiFi 초기화보다 먼저 1회 호출하고, 응답성 설정이 실제 값으로 확정되면
 *         (cam_node_set_response_interval_sec) 그 값 기준으로 다시 호출해 재무장한다.
 *         esp_deep_sleep_start() 직전까지는 다시 안 건드림 — RTC 슬로우클럭 기준이라
 *         딥슬립 중에도 계속 카운트되다가, 다음 부팅 때 이 함수가 다시 불리며 자연히
 *         재무장된다. */
void rwdt_guard_arm(uint32_t total_budget_sec);
