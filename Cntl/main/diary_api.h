/**
 * @file    diary_api.h
 * @brief   2026-11-26(사용자 설계 — 영농일지 1단계) — 영농일지 저장 API. 설계: Docs/설계_영농일지_2026-11-26.md
 *
 * 콘은 웹앱이 만든 JSON 문서를 이름별 파일로 SD(/sdcard/diary)에 두고 돌려주기만 함. 일정 계산·할 일 생성 같은 규칙은
 * 웹앱(app.html)에 있음 — 영농일지는 웹에서만 쓰기로 함(사용자 결정, 콘 화면 없음).
 *  - GET  /api/diary?f=<이름>  → 저장된 JSON 그대로, 없으면 null
 *  - POST /api/diary?f=<이름>  (본문 = JSON) → 통째로 바꿔 저장 {"ok":true}
 *  이름 = 영문 소문자·숫자·_·- 1~32자(경로 문자 없음). SD가 없으면 503 {"error":"sd"}.
 */
#pragma once

#include "esp_http_server.h"

void diary_api_register_handlers(httpd_handle_t server);
