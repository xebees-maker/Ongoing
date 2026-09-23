#pragma once

#include <stdint.h>
#include "esp_err.h"

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

/* 2026-09-23(사용자 지시 — 로그가 길어서 워드랩됨, 짧게+mnemonic+MAC은 끝 6자리만) —
 * 두 창 공통 로그 표기 규칙을 여기 유틸로 통일. out은 최소 7바이트(6글자+NUL) */
void ui_screen_mac6(const uint8_t mac[6], char out[7]);
/* esp_err_to_name() 결과에서 "ESP_ERR_" 접두어를 "ERR-"로 축약(예: ESP_ERR_TIMEOUT ->
 * ERR-TIMEOUT). ESP_OK/ESP_FAIL 등 접두어가 안 맞는 것도 처리. 반환값은 static 버퍼 —
 * 호출 직후 바로 쓸 것(다음 호출 전까지만 유효) */
const char *ui_screen_err_short(esp_err_t err);

/* 2026-09-23(사용자 지시 — "RX(MAC,RSSI,LEN) ADVERTISE 식으로") — esp_now_link.h의 msg_type
 * 값을 짧은 이름으로. 지금 페어링 흐름에서 보이는 것부터(ADVERTISE 등), 모르는 값은 숫자로
 * 폴백. 순수 표시용 — 브가 이 값으로 뭘 판단하지는 않음(투명 릴레이 원칙 그대로) */
const char *ui_screen_msg_type_name(uint8_t msg_type);

/* 2026-09-23(사용자 지시 — RL 로그용 RESULT_CODE) — esp_err_t를 2글자 코드로. 표에 없는
 * 값은 "E%d"로 폴백(원본 문자열 대신 숫자만 — 짧게 유지). 반환값은 static 버퍼 — 호출 직후
 * 바로 쓸 것(다음 호출 전까지만 유효) */
const char *ui_screen_result_code(esp_err_t err);
