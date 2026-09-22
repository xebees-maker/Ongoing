#pragma once

/**
 * 2026-09-22 — 브(브릿지) 전용 화면: 콘의 UI/대시보드를 전혀 가져오지 않고 처음부터 새로
 * 설계(사용자 지시: "브 기반으로만 작업"). 맨 위에 메모리 표시(콘에 있던 것과 같은 개념,
 * 항상 보임), 그 아래 로그창 2개(왼쪽=무선/ESP-NOW, 오른쪽=CAN).
 */

void ui_screen_init(void);

/* 무선(ESP-NOW) 쪽 로그 한 줄 추가 — 왼쪽 창 */
void ui_screen_log_wireless(const char *fmt, ...);
/* CAN 쪽 로그 한 줄 추가 — 오른쪽 창 */
void ui_screen_log_can(const char *fmt, ...);
