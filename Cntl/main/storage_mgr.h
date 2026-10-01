/**
 * @file    storage_mgr.h
 * @brief   2026-09-26(사용자 설계 — 콘 UI 멈춤 원인 제거 + SD 정리 재설계) — SD 사용량 계산/예산
 *          판단/정리를 LVGL 태스크 밖의 파일처리 태스크(우선순위 10, project_cntl_task_priority_scheme)
 *          한 곳에서 담당.
 *
 *          예전엔 LVGL 태스크(refresh_dashboard 5초마다)가 사진 폴더 전체 스캔(실측 약 765ms)과
 *          정리(실측 약 2055ms, 깨진 항목 때문에 매번 실행)를 직접 돌려서 UI가 약 3초씩 멈췄음.
 *          이제:
 *            - 사용량은 photo_storage/stats_store가 저장/삭제할 때 RAM 합계로 직접 더하고 뺌
 *              (폴더 스캔은 마운트/포맷/재연결 직후 "재스캔" 때만)
 *            - 예산(사진 9 : 측정값 1) 초과 판단과 정리는 "저장/삭제로 사용량이 바뀐 직후"
 *              이벤트로만 돌고(주기 폴링 없음), 90% 이상일 때만 80%까지 정리
 *            - UI는 storage_mgr_get_snapshot()으로 RAM 값만 읽음(SD I/O 없음)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 2026-10-01(사용자 설계 — 할 일 T) — 관리 영역(폴더). 영역별 예산 비율·사용량·정리·재스캔은 storage_mgr.c의 표 한 곳에
 * 있음. 영역을 더하려면 여기 하나 + 그 표에 한 줄(예산 비율 합 100) + ui_main.c의 영역 이름 표에 한 줄 */
typedef enum {
    STORAGE_AREA_PICTURE = 0,  /* /sdcard/photos — 예산 90% */
    STORAGE_AREA_MEASURE,      /* /sdcard/stats(원시+집계) — 예산 10% */
    STORAGE_AREA_COUNT
} storage_area_t;

typedef struct {
    uint64_t used;           /* 그 폴더 파일 크기 합(RAM 합계) */
    uint64_t budget;         /* SD 전체 × 예산 비율 */
    uint64_t remain;         /* 예산 − 사용(0 미만은 0), 실제 SD 빈 공간을 넘지 않게 자름 */
} storage_area_usage_t;

typedef struct {
    bool     valid;          /* 재스캔이 끝나 아래 값들을 믿을 수 있는지 */
    uint64_t sd_total;       /* 바이트 */
    uint64_t sd_free;        /* 바이트(FAT 빈 공간, esp_vfs_fat_info — FSINFO 캐시라 빠름) */
    storage_area_usage_t area[STORAGE_AREA_COUNT];
    storage_area_usage_t total;  /* 영역들의 합(SD 전체 기준 아님 — 영역 밖 파일은 안 셈). remain도 SD 빈 공간을 넘지 않음 */
    uint32_t bad_entries;    /* 마지막 재스캔에서 합계/정리 대상에서 뺀 손상 의심 항목 수 */
} storage_mgr_snapshot_t;

/* sd_storage_init() 이후 1회 — 파일처리 태스크 생성, 시작하자마자 재스캔 1회 */
void storage_mgr_start(void);

/* 마운트/재연결/포맷 직후 — 폴더를 다시 훑어 합계를 새로 만듦(태스크가 아직 없으면 무시,
 * 태스크 시작 시 어차피 재스캔함) */
void storage_mgr_request_rescan(void);

/* 저장/삭제로 사용량이 바뀐 직후(어느 태스크에서든 호출 가능, 즉시 반환) — 예산 판단/정리 */
void storage_mgr_notify_changed(void);

/* UI용 — RAM 값 복사만(SD I/O 없음) */
void storage_mgr_get_snapshot(storage_mgr_snapshot_t *out);

/* UI용 test-and-clear — 정리가 실제로 지운 게 있으면 true(개수 채움, 이후 0으로 리셋).
 * 정리는 파일처리 태스크에서 일어나지만 안내 팝업은 LVGL 태스크에서만 띄워야 해서 이렇게 넘김 */
bool storage_mgr_take_cleanup(uint32_t out_deleted[STORAGE_AREA_COUNT]);

#ifdef __cplusplus
}
#endif
