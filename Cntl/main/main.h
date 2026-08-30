#pragma once

/* 2026-08-30 — AP 모드는 STA처럼 IP_EVENT_STA_GOT_IP로 웹서버 시작을 트리거할 수 없어서
 * (esp_now_hub.c가 AP 시작 직후 동기적으로 얻은 IP를 갖고 직접 호출) 노출 */
void web_dashboard_start(void);
