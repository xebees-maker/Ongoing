#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);

/* 2026-08-29 — WIFI_EVENT_SCAN_DONE 핸들러 등록. 기본 이벤트루프가 생긴 뒤(esp_now_hub_init()
 * 호출 이후)에 app_main()에서 불러야 함 — ui_init()보다 먼저는 절대 안 됨 */
void ui_main_register_wifi_events(void);

/* 2026-08-30(사용자 설계: "웹에 입력이 있으면, CNTL의 탭과 같은 입력 처리 과정을 거쳐야") —
 * 웹의 사진 가져오기도 온디바이스 탭과 같은 모델(s_selected_file_id)을 거치게 해서,
 * consume_ready_photo_if_current()의 "지금 기다리는 것과 다름" 오판(3008,
 * UI_ERR_PHOTO_SELECTION_STALE)을 막음 — main.c의 웹 API 핸들러가 esp_now_photo_fetch_by_id()를
 * 부르기 직전에 이것부터 호출 */
/* 2026-09-04(사용자 설계: "PC 원격제어처럼") — 웹 입력을 실제 탭/팝업/확인 시퀀스로 합성.
 * 전부 LVGL 태스크에서 동기적으로 실행되고(httpd 태스크는 완료까지 블로킹), 대상 위젯을
 * 못 찾으면(지금 화면/목록에 없음) false — main.c가 이걸로 "합성 자체의 실패"를 즉시 판정 */
bool ui_main_inject_connect(const uint8_t *mac);
bool ui_main_inject_disconnect(const uint8_t *mac);
/* 성공 시 out_ok=true + 새로 생긴 세대번호 반환(main.c가 esp_now_photo_list_wait_result()에
 * 그대로 넘기면 됨) */
uint32_t ui_main_inject_list_refresh(bool *out_ok);
bool ui_main_inject_photo_select(uint32_t file_id);

#ifdef __cplusplus
}
#endif
