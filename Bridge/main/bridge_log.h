#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"

/**
 * 2026-10-02 — 실물 브 보드(Waveshare ESP32-S3-RS485-CAN, 화면 없음)로 옮기며 ui_screen(LVGL 화면: 메모리 표시 +
 * 무선/CAN 로그창)을 없애고 그 로그만 시리얼로 남김. 예전엔 같은 줄을 화면 창과 시리얼(D)에 함께 찍었음.
 * 태그 "wireless"(무선/ESP-NOW)와 "can_ui"(CAN)는 main.c에서 D로 켬 — 상용화 때 시리얼 출력과 함께 정리(조건부 C9).
 */
#define bridge_log_wireless(fmt, ...) ESP_LOGD("wireless", fmt, ##__VA_ARGS__)
#define bridge_log_can(fmt, ...)      ESP_LOGD("can_ui", fmt, ##__VA_ARGS__)

/* 2026-09-23(사용자 지시 — 로그가 길어서 워드랩됨, 짧게+mnemonic+MAC은 끝 6자리만) —
 * 로그 표기 규칙을 여기 유틸로 통일. out은 최소 7바이트(6글자+NUL) */
void bridge_log_mac6(const uint8_t mac[6], char out[7]);
/* esp_err_to_name() 결과에서 "ESP_ERR_" 접두어를 "ERR-"로 축약(예: ESP_ERR_TIMEOUT ->
 * ERR-TIMEOUT). ESP_OK/ESP_FAIL 등 접두어가 안 맞는 것도 처리. 반환값은 static 버퍼 —
 * 호출 직후 바로 쓸 것(다음 호출 전까지만 유효) */
const char *bridge_log_err_short(esp_err_t err);

/* 2026-09-23(사용자 지시 — "RX(MAC,RSSI,LEN) ADVERTISE 식으로") — esp_now_link.h의 msg_type
 * 값을 짧은 이름으로. 지금 페어링 흐름에서 보이는 것부터(ADVERTISE 등), 모르는 값은 숫자로
 * 폴백. 순수 표시용 — 브가 이 값으로 뭘 판단하지는 않음(투명 릴레이 원칙 그대로) */
const char *bridge_log_msg_type_name(uint8_t msg_type);

/* 2026-09-23(사용자 지시 — RL 로그용 RESULT_CODE) — esp_err_t를 2글자 코드로. 표에 없는
 * 값은 "E%d"로 폴백(원본 문자열 대신 숫자만 — 짧게 유지). 반환값은 static 버퍼 — 호출 직후
 * 바로 쓸 것(다음 호출 전까지만 유효) */
const char *bridge_log_result_code(esp_err_t err);
