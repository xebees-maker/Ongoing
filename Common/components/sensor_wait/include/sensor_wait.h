/**
 * @file    sensor_wait.h
 * @brief   센서가 "최소 이만큼" 기다려야 하는 대기(명령 실행 시간, 변환 시간, 전원 안정화) — 2026-10-23.
 *
 * vTaskDelay(pdMS_TO_TICKS(ms))는 "틱 경계를 n번 지날 때까지"라 실제 대기는 (n-1)틱 ~ n틱이다. 센스 틱은 10ms(100Hz)라
 *   1~9ms  → pdMS_TO_TICKS = 0틱 → 전혀 기다리지 않음
 *   10ms   → 1틱 → 실제 0~10ms(호출 시점이 틱 경계에 가까우면 거의 0)
 * 2026-10-23(실기) — SHT45 측정 대기 10ms가 측정 시간(최대 8.3ms)보다 짧게 끝나 측정이 거의 다 NACK로 실패했다. 예전엔 측정
 * 직전 LED가 vTaskDelay로 100ms를 막아 측정이 늘 틱 경계 직후에 시작해 우연히 넘겼고, LED를 타이머 방식으로 바꾸자 드러났다.
 * SCD41의 1~2ms 대기도 0틱이었다.
 *
 * 규칙(사용자 지시 — "같은 문제 반복하지 않게"): 센서 타이밍 대기는 반드시 이 함수로. 앞에서 무엇을 했는지(LED·로그·통신)에
 * 기대지 않고 데이터시트 시간을 항상 보장함 — 1틱보다 짧으면 바쁜 대기(정확), 아니면 틱으로 올림 + 1틱.
 */
#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

static inline void sensor_wait_ms(uint32_t ms)
{
    if (ms == 0) return;
    if (ms < portTICK_PERIOD_MS) {
        esp_rom_delay_us(ms * 1000);
        return;
    }
    vTaskDelay((TickType_t)((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS + 1));
}
