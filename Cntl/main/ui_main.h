#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "power_relay.h"
#include "photo_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);

/* 2026-09-16 — SR(Power Control) 판정 루프(power_relay.c)가 매 주기 호출. cfg가 가리키는
 * 소스(그룹 통계 또는 개별 장치)의 현재 값을 ui_main.c의 기존 분류/집계 로직(stats_classify,
 * stats_collect_group_macs, stats_blend_macs)으로 구해 돌려줌 — 그 로직 자체는 static이라
 * power_relay.c가 직접 못 쓰므로 이 함수 하나로만 연결 */
bool ui_main_query_power_source_value(const power_relay_config_t *cfg, float *out_value);

/* 2026-08-29 — WIFI_EVENT_SCAN_DONE 핸들러 등록. 기본 이벤트루프가 생긴 뒤(node_hub_init()
 * 호출 이후)에 app_main()에서 불러야 함 — ui_init()보다 먼저는 절대 안 됨 */
void ui_main_register_wifi_events(void);

/* 2026-08-30(사용자 설계: "웹에 입력이 있으면, CNTL의 탭과 같은 입력 처리 과정을 거쳐야") —
 * 웹의 사진 가져오기도 온디바이스 탭과 같은 모델(s_selected_file_id)을 거치게 해서,
 * consume_ready_photo_if_current()의 "지금 기다리는 것과 다름" 오판(3008,
 * UI_ERR_PHOTO_SELECTION_STALE)을 막음 — main.c의 웹 API 핸들러가 photo_rx_fetch_by_id()를
 * 부르기 직전에 이것부터 호출 */
/* 2026-09-04(사용자 설계: "PC 원격제어처럼") — 웹 입력을 실제 탭/팝업/확인 시퀀스로 합성.
 * 전부 LVGL 태스크에서 동기적으로 실행되고(httpd 태스크는 완료까지 블로킹), 대상 위젯을
 * 못 찾으면(지금 화면/목록에 없음) false — main.c가 이걸로 "합성 자체의 실패"를 즉시 판정 */
bool ui_main_inject_connect(const uint8_t *mac);
bool ui_main_inject_disconnect(const uint8_t *mac);
/* 2026-09-06(야간 자동 테스트용) — 센스 행은 카메라 행과 별도 리스트라 연결 합성도 별도 */
bool ui_main_inject_connect_sensor(const uint8_t *mac);
/* 응답성(response_interval) 드롭다운+Apply 합성 — sec는 0/3/10/30/60만 유효 */
bool ui_main_inject_set_response_interval(uint32_t sec);
/* 통계 전체 삭제 버튼+확인팝업 합성(2026-09-07, 어젯밤 쌓인 이산화탄소 0 레코드 정리용) */
bool ui_main_inject_delete_stats(void);
/* 2026-09-19(SD 제거 재설계 — 사진목록 UI 로컬화) — 목록갱신/사진선택 모두 이제 콘 SD를
 * 읽는 동기 로컬 동작이라(더 이상 CAM 응답을 기다리는 ESP-NOW 왕복이 아님) 예전의 세대번호/
 * 비파괴적 완료-확인 채널이 필요 없어짐. 반환 시점에 이미 결과가 반영돼 있음 */
bool ui_main_inject_list_refresh(void);
/* 새로고침된 목록을 그대로 복사(웹이 독자적으로 photo_storage를 다시 읽지 않고, 콘 화면이
 * 지금 보여주는 바로 그 배열을 읽어감 — "웹기생" 원칙) */
int ui_main_get_photo_list(photo_storage_item_t *out, int out_cap);
bool ui_main_inject_photo_select(uint8_t kind, uint32_t seq);
/* 선택된 사진의 원본(압축 해제 전) JPEG 바이트 — 웹의 "원본 그대로 보기"가 씀 */
bool ui_main_get_selected_photo_raw(const uint8_t **out_data, size_t *out_len);
/* 지금 선택된 사진의 (kind,seq) — 웹이 자신의 요청과 "지금 화면이 보여주는 것"이 같은지
 * 확인하는 용도(/photo 핸들러 참고) */
bool ui_main_get_selected_photo_id(uint8_t *out_kind, uint32_t *out_seq);

#ifdef __cplusplus
}
#endif
