/**
 * @file    sens_kind_store.h
 * @brief   2026-09-19(통계 분류 영구저장) — Sens mac별 sensor_kind_t를 SD에 영구 저장.
 *          device_config.c의 device_config_file_t와는 완전히 분리된 별도 파일 —
 *          device_config의 구조체/버전을 바꾸면 기존 저장값이 전부 기본값으로 리셋되는
 *          위험이 있어(버전 불일치 시 device_config_load()는 파일은 안 건드리고 메모리만
 *          기본값으로 시작 — 그 상태에서 뭔가 저장되면 기존 파일이 기본값으로 덮어써짐),
 *          그 위험을 피하려고 처음부터 독립된 작은 파일로 설계.
 *
 *          node_hub_node_t.sensor_kind는 순수 RAM이라 재부팅하면 사라짐 — Sens가 다시
 *          깨어나 페어링할 때까지 stats_collect_group_macs()가 그 mac을 분류할 방법이 없어서
 *          "재부팅 후 한동안 통계가 안 보이는" 버그가 있었음(2026-09-19 실기 확인). 이 모듈이
 *          그 mac->kind 매핑을 영구 보관해서, 재부팅 직후에도 과거 기록을 분류할 수 있게 함.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* app_main()에서 sd_storage_init() 이후 한 번 호출 */
void sens_kind_store_load(void);

/* 없으면 0(SENSOR_KIND_UNKNOWN) 반환 */
uint8_t sens_kind_store_get(const uint8_t mac[6]);

/* 값이 실제로 바뀔 때만 파일에 다시 씀(node_hub.c가 PAIR_ACK/keepalive마다 불러도
 * 무해하도록 호출부에서도 한 번 더 비교하지만, 여기서도 방어적으로 비교) */
void sens_kind_store_set(const uint8_t mac[6], uint8_t kind);

/* 알려진 mac+kind 전체 나열 — out_cap까지, 실제로 채운 개수 반환 */
int sens_kind_store_get_all(uint8_t out_macs[][6], uint8_t out_kinds[], int out_cap);

#ifdef __cplusplus
}
#endif
