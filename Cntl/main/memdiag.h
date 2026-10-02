#pragma once
/* 내부 RAM 변화 추적(진단용) — 감싼 구간에서 내부 RAM이 MEMDIAG_MIN_DELTA 넘게 변했을 때만 D 로그.
 * 다른 코어가 같은 시간에 할당하면 그 몫도 섞이므로, 큰 변화가 어느 구간에서 반복되는지를 보는 용도 */
#include <stdbool.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "dev_log.h"

#define MEMDIAG_MIN_DELTA 512

/* 2026-10-02(사용자 지시 — 할 일 K) — Dev Log 저장 수준이 D일 때만 측정함. 예전엔 로그만 걸러지고 측정(RAM 읽기·시간 재기)은
 * 늘 돌았음. 화면에서 D로 바꾸는 순간부터 다시 잼. 직접 재는 곳도 이 함수로 감쌈 */
static inline bool memdiag_enabled(void)
{
    return dev_log_get_save_level() >= DEV_LOG_LVL_D;
}

/* 직접 재는 곳용 — D일 때만 내부 RAM 여유를 읽고, 아니면 0(그 값은 D 로그에만 쓰이고, D가 아니면 그 로그도 안 찍힘) */
#define MEMDIAG_HEAP() (memdiag_enabled() ? heap_caps_get_free_size(MALLOC_CAP_INTERNAL) : 0)

#define MEMDIAG_BEGIN() \
    bool _memdiag_on = memdiag_enabled(); \
    size_t _memdiag_before = _memdiag_on ? heap_caps_get_free_size(MALLOC_CAP_INTERNAL) : 0

#define MEMDIAG_END(tag, fmt, ...) do { \
    if (_memdiag_on) { \
        size_t _memdiag_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); \
        int _memdiag_used = (int)_memdiag_before - (int)_memdiag_after; \
        if (_memdiag_used > MEMDIAG_MIN_DELTA || _memdiag_used < -MEMDIAG_MIN_DELTA) { \
            ESP_LOGD(tag, "MEMDIAG " fmt ": internal %u -> %u (used %d)", ##__VA_ARGS__, \
                     (unsigned)_memdiag_before, (unsigned)_memdiag_after, _memdiag_used); \
        } \
    } \
} while (0)
