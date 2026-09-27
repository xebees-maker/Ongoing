#pragma once
/* 내부 RAM 변화 추적(진단용) — 감싼 구간에서 내부 RAM이 MEMDIAG_MIN_DELTA 넘게 변했을 때만 D 로그.
 * 다른 코어가 같은 시간에 할당하면 그 몫도 섞이므로, 큰 변화가 어느 구간에서 반복되는지를 보는 용도 */
#include "esp_heap_caps.h"
#include "esp_log.h"

#define MEMDIAG_MIN_DELTA 512

#define MEMDIAG_BEGIN() size_t _memdiag_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL)

#define MEMDIAG_END(tag, fmt, ...) do { \
    size_t _memdiag_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); \
    int _memdiag_used = (int)_memdiag_before - (int)_memdiag_after; \
    if (_memdiag_used > MEMDIAG_MIN_DELTA || _memdiag_used < -MEMDIAG_MIN_DELTA) { \
        ESP_LOGD(tag, "MEMDIAG " fmt ": internal %u -> %u (used %d)", ##__VA_ARGS__, \
                 (unsigned)_memdiag_before, (unsigned)_memdiag_after, _memdiag_used); \
    } \
} while (0)
