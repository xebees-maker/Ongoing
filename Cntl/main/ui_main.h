#pragma once

#include <stdint.h>

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
void ui_main_set_selected_photo(uint32_t file_id);

#ifdef __cplusplus
}
#endif
