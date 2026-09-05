#include "rwdt_guard.h"

#include "hal/wdt_hal.h"
#include "esp_clk_tree.h"
#include "esp_log.h"

static const char *TAG = "rwdt_guard";

void rwdt_guard_arm(uint32_t total_budget_sec)
{
    wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_init(&rwdt, WDT_RWDT, 0, false);  /* 이전 무장 해제 + 재설정(=매 사이클 새 워치독) */

    uint32_t slow_clk_hz = 0;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_RTC_SLOW,
                                  ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX, &slow_clk_hz);
    if (slow_clk_hz == 0) slow_clk_hz = 136000;  /* 폴백(RC_SLOW 통상값) */
    uint64_t ticks = (uint64_t)total_budget_sec * slow_clk_hz;

    wdt_hal_write_protect_disable(&rwdt);
    /* wdt_hal_init()이 기본으로 켜두는 "Deep Sleep 중엔 일시정지"를 반드시 꺼야 함 — 안 그러면
     * 이 워치독이 지키려는 바로 그 상황(딥슬립 진입 실패)에서 자신도 같이 멈춰서 무용지물이
     * 됨(wdt_hal_iram.c의 wdt_hal_init 구현 확인, rwdt_ll_set_pause_in_sleep_en(...,true)가
     * 기본값). */
    rwdt_ll_set_pause_in_sleep_en(rwdt.rwdt_dev, false);
    wdt_hal_config_stage(&rwdt, WDT_STAGE0, (uint32_t)ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_enable(&rwdt);
    wdt_hal_write_protect_enable(&rwdt);

    ESP_LOGI(TAG, "RWDT 무장: %us 예산 (slow_clk=%uHz)", (unsigned)total_budget_sec, (unsigned)slow_clk_hz);
}
