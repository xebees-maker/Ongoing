#pragma once
/* 2026-10-14(시험, 임시 — 할 일 AF에서 제거) SD·내부 플래시 쓰기 속도 측정. fs_bench.c 참고 */
#include <stdbool.h>

bool fs_bench_start(void);
const char *fs_bench_result(void);
