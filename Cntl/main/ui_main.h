#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);

/* 2026-08-29 — WIFI_EVENT_SCAN_DONE 핸들러 등록. 기본 이벤트루프가 생긴 뒤(esp_now_hub_init()
 * 호출 이후)에 app_main()에서 불러야 함 — ui_init()보다 먼저는 절대 안 됨 */
void ui_main_register_wifi_events(void);

#ifdef __cplusplus
}
#endif
