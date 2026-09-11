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

/* 2026-09-11(재설계 — SD 신뢰성 항목4/5, [[project_cntl_sd_reliability_redesign_2026_09_10]]) —
 * 예전엔 채널 하나당 fopen/fwrite/fclose를 따로 했음(WAKE_HELLO_SENS 1건에 최대 5채널이면
 * 5번 여닫음). "쓸 것들이 여러 개면 모아서 한 번에"(사용자 지시)에 따라 한 번의
 * open+연속쓰기+close로 통합 — 크래시 시 파일이 깨질 수 있는 위험구간(열려있는 시간)도
 * 5배 줄어듦. fopen 실패(SD 자체 문제로 추정) 시 false 반환, 값 전체 유실(로그만 남김,
 * 치명적 아님 — sd_storage_init() 실패해도 앱 전체가 안 멈추는 기존 정책과 동일) */
bool stats_store_append_batch(const stats_record_t *records, uint32_t count);

/* 2026-09-11(SD 신뢰성 항목4 — 쓰기경로도 사용자에게 알려야 함) — stats_store_append_batch()가
 * WAKE_HELLO_SENS 처리 중(esp_now_hub.c, ESP-NOW recv_cb 컨텍스트, LVGL 태스크 아님)에
 * fopen 실패를 만나면 여기 true를 세팅. LVGL 태스크 쪽(ui_main.c의 1초 주기 refresh_dashboard)이
 * 매 틱 이 값을 확인+리셋(test-and-clear)해서 주화면 SD 상태를 갱신 — LVGL API를 다른
 * 태스크에서 직접 호출하면 안 되므로(스레드 안전성), 값 전달만 이 플래그로 하고 실제
 * lv_label_set_text() 등은 항상 LVGL 태스크 쪽에서 실행됨 */
bool stats_store_take_write_io_error(void);

/* 전체 레코드 수(파일 없으면 0) */
uint32_t stats_store_get_count(void);

/* page_index=0이 가장 최근 페이지. out에 최대 out_cap개, 파일에 쓰인 순서(=오래된 것부터)
 * 그대로 채우고 실제 채운 개수 반환 — 화면에 최신순으로 보여주려면 호출부가 역순으로 순회 */
uint32_t stats_store_read_page(uint32_t page_index, uint32_t page_size,
                                stats_record_t *out, uint32_t out_cap);

/* chan_type 전체 이력 중 최대/최소값 — 데이터 없으면 false */
bool stats_store_get_min_max(uint8_t chan_type, float *out_min, float *out_max);

/* 2026-09-07(통계탭 개괄 판넬 Scale 연동, 사용자 설계: "스케일마다 계산해야되") —
 * cutoff_unix_time 이후(선택된 기간)만의 최대/최소/평균. 파일 끝에서부터 훑다가 cutoff보다
 * 오래된 레코드를 만나면 중단(전체 스캔 아님, stats_store_read_since()와 동일 원칙).
 * 데이터 없으면(그 기간에 이 chan_type 레코드가 하나도 없으면) false */
bool stats_store_get_min_max_avg_since(uint32_t cutoff_unix_time, uint8_t chan_type,
                                        float *out_min, float *out_max, float *out_avg);

/* 2026-09-07(통계탭 "저장값 지우기" 기능, 사용자 지시) — 파일 전체 삭제. SD 미마운트 등으로
 * 파일이 아예 없어도 에러 아님(그 상태가 곧 "이미 비어있음") */
void stats_store_delete_all(void);

/* cutoff_unix_time 이후(그래프 시간범위) 이 chan_type의 레코드만 골라 out에 채움(파일
 * 끝에서부터 거꾸로 훑다가 cutoff보다 오래된 레코드를 만나면 즉시 중단 — 전체 스캔 아님).
 * out은 파일에 쓰인 순서(오래된 것부터)로 채워짐. 실제 채운 개수 반환(out_cap 초과분은 버림 —
 * 그래프는 어차피 다운샘플하므로 앞부분 유실은 허용) */
uint32_t stats_store_read_since(uint32_t cutoff_unix_time, uint8_t chan_type,
                                 stats_record_t *out, uint32_t out_cap);

/* 2026-09-10(사용자 설계 — "Storage[%(Remain MB)]... Measure zz(kk)") — 현재 파일 크기
 * (바이트). stats_store_get_count() * sizeof(stats_record_t)와 동일하지만 호출부가
 * 매번 곱하지 않아도 되게 별도 제공. 파일 없으면 0 */
uint64_t stats_store_get_used_bytes(void);

/* 2026-09-10(사용자 설계 — "할당된 용량의 90%가 될 때 10%만큼 오래된 걸 지운다") —
 * 파일을 앞부분(오래된 레코드)부터 지워서 최종 크기가 target_bytes 이하가 되게 함.
 * 이미 target_bytes 이하면 아무 것도 안 하고 0 반환. 실제로 지운 레코드 수를 반환.
 * 임시파일에 남길 부분만 다시 써서 교체하는 방식(레코드가 고정크기라 오프셋 계산이
 * 정확함) — SD 미마운트/파일 없음 등으로 실패해도 0 반환(치명적 아님) */
uint32_t stats_store_trim_to(uint64_t target_bytes);

/* 2026-09-10(재설계 — SD fail 회로차단기, 사용자 지시: "SD 조회 fail이면, 다른 값도 믿을
 * 수 없어. 즉시 중단이지") — 위 읽기 함수들 중 하나가 방금 "데이터 없음"이 아니라 진짜
 * fopen() 등 I/O 자체가 실패해서 실패값(0/false)을 반환했는지 구분. 각 읽기 함수는 자기
 * 시작 시점에 이 값을 false로 리셋하고, 실패 시에만 true로 세팅 — 호출부는 함수가 리턴한
 * 직후 이 값을 확인해서, true면 그 틱의 나머지 SD 조회까지 전부 건너뜀(회로차단기) */
bool stats_store_had_io_error(void);

/* ════════════════════════════════════════════════════════════
 * 그래프용 스케일별 사전집계 저장 — [[project_cntl_stats_graph_redesign_2026_09_10]]
 * 매 stats_store_append_batch() 호출마다(원본 기록과 같은 지점) 내부적으로 같이 갱신됨
 * (이 헤더에 노출 안 함, esp_now_hub.c는 여전히 stats_store_append_batch()만 부르면 됨).
 * 읽기 때 원본 로그를 매번 다시 스캔하던 것(1주 창 기준 추정 26초+)을 피하기 위해, 쓸 때
 * 미리 평균을 계산해 별도 저장 — 스케일 5단계(1시간/12시간/1일/3일/1주) 각각 60포인트로
 * 나뉘는 버킷 폭으로 미리 평균냄(그래프가 항상 60포인트 고정이므로).
 * ════════════════════════════════════════════════════════════ */

/* ui_main.c의 그래프 스케일 드롭다운(1H/12H/1D/3D/1W) 순서와 반드시 일치 —
 * 이 헤더가 정본(authoritative)이라 ui_main.c는 STATS_SCALE_SECONDS를 직접 참조함 */
#define STATS_SCALE_COUNT 5
extern const uint32_t STATS_SCALE_SECONDS[STATS_SCALE_COUNT];  /* 3600/43200/86400/259200/604800 */

/* 2026-09-11(사용자 결정 — "매 그래프는 60개 포인트로") — 스케일마다 이 개수만큼의 버킷으로
 * 나뉨(버킷폭 = STATS_SCALE_SECONDS[i] / 이 값) */
#define STATS_AGG_POINTS_PER_SCALE 60

typedef struct __attribute__((packed)) {
    uint32_t bucket_start_unix;  /* 벽시계 정렬(unix_time/버킷폭*버킷폭) — 그 버킷의 시작 */
    uint8_t  chan_type;
    uint8_t  sample_count;       /* 이 평균에 들어간 원본 샘플 개수(최소 2, 그 미만이면 버킷
                                     자체가 안 만들어짐 — "절대 단일 샘플 아님") */
    float    avg_value;
} stats_bucket_t;  /* 10바이트 고정 */

/* scale_idx(0..4)의 사전집계 저장에서, [window_start_unix, window_end_unix) 범위 안의
 * chan_type 버킷들을 out에 채움(파일에 쓰인 순서=시간순 그대로). 실제 채운 개수 반환.
 * 각 버킷이 스케일 안의 몇 번째 슬롯인지는 호출부가
 * (bucket_start_unix - window_start_unix) / (STATS_SCALE_SECONDS[scale_idx] /
 * STATS_AGG_POINTS_PER_SCALE)로 직접 계산 — 없는 슬롯(원본 기록이 아예 없던 구간)은 이
 * 함수가 채워주지 않으므로 호출부가 "데이터 없음"으로 처리 */
uint32_t stats_agg_read_window(uint8_t scale_idx, uint8_t chan_type,
                                uint32_t window_start_unix, uint32_t window_end_unix,
                                stats_bucket_t *out, uint32_t out_cap);

#ifdef __cplusplus
}
#endif
