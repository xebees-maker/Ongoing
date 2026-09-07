/**
 * @file    stats_store.h
 * @brief   Sens 채널값 시계열을 SD카드(/sdcard/stats/values.bin)에 고정크기 이진 레코드로
 *          영구 저장(2026-09-06, 사용자 설계 — "1년 지나서라도" 조회 가능해야 함).
 *
 *          모든 노드/채널을 하나의 파일에 도착순(=시간순, Cntl 자기 벽시계 기준)으로 이어
 *          씀 — WAKE_HELLO_SENS 처리는 s_nodes_mutex 아래 한 곳에서만 일어나므로 별도
 *          동기화 없이도 파일 내 기록 순서가 항상 시간순으로 보장됨. 레코드가 고정크기라
 *          "레코드 번호 -> 파일 오프셋"이 곱셈 한 번으로 나와서, 페이지네이션(20줄/페이지)도
 *          그래프의 "최근 N시간"도 파일 전체를 순차로 읽을 필요가 없음 — 예전에 "파일에서
 *          필요한 만큼만 읽어서 디스플레이"하다가 실패했다던 것(기록엔 없음, 2026-09-06
 *          사용자 언급)을 가변길이/순차스캔이 원인이었을 가능성으로 보고 구조적으로 피함.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) {
    uint32_t unix_time;
    uint8_t  mac[6];
    uint8_t  chan_type;   /* sensor_channel_type_t */
    uint8_t  chan_index;  /* 그 노드 chan_type[]/chan_val[] 배열에서의 인덱스(0..4) —
                            * 레거시 콤보처럼 같은 chan_type이 한 노드에 2개 이상일 때 구분용 */
    float    value;
} stats_record_t;  /* 16바이트 고정 — 페이지번호*20*16 = 파일 오프셋 */

#define STATS_STORE_PAGE_SIZE 20

/* SD 미마운트 등으로 폴더가 없으면 파일 열기만 실패하고 로그만 남김(치명적 아님) —
 * sd_storage_init() 실패해도 앱 전체가 안 멈추는 기존 정책과 동일 */
void stats_store_append(const uint8_t mac[6], uint8_t chan_type, uint8_t chan_index,
                         uint32_t unix_time, float value);

/* 전체 레코드 수(파일 없으면 0) */
uint32_t stats_store_get_count(void);

/* page_index=0이 가장 최근 페이지. out에 최대 out_cap개, 파일에 쓰인 순서(=오래된 것부터)
 * 그대로 채우고 실제 채운 개수 반환 — 화면에 최신순으로 보여주려면 호출부가 역순으로 순회 */
uint32_t stats_store_read_page(uint32_t page_index, uint32_t page_size,
                                stats_record_t *out, uint32_t out_cap);

/* chan_type 전체 이력 중 최대/최소값 — 데이터 없으면 false */
bool stats_store_get_min_max(uint8_t chan_type, float *out_min, float *out_max);

/* cutoff_unix_time 이후(그래프 시간범위) 이 chan_type의 레코드만 골라 out에 채움(파일
 * 끝에서부터 거꾸로 훑다가 cutoff보다 오래된 레코드를 만나면 즉시 중단 — 전체 스캔 아님).
 * out은 파일에 쓰인 순서(오래된 것부터)로 채워짐. 실제 채운 개수 반환(out_cap 초과분은 버림 —
 * 그래프는 어차피 다운샘플하므로 앞부분 유실은 허용) */
uint32_t stats_store_read_since(uint32_t cutoff_unix_time, uint8_t chan_type,
                                 stats_record_t *out, uint32_t out_cap);

#ifdef __cplusplus
}
#endif
