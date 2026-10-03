/**
 * @file    web_session.h
 * @brief   웹 접속 주인(웹/콘) — 웹 접속 중엔 콘 화면을 잠금
 *
 * 2026-10-03(할 일 AD — SPA 설계, 사용자 결정) — 웹과 콘 화면을 동시에 쓰지 않음.
 * - SPA가 화면 API(web_session_gate를 거치는 API)를 부르면 웹이 주인이 되고 콘 화면은 잠금 화면.
 *   SPA는 5초마다 조회하므로, WEB_SESSION_IDLE_MS 동안 요청이 없으면(브라우저 닫힘·휴대폰 잠김) 접속 끝 → 잠금 해제.
 * - 콘 잠금 화면의 "웹 연결 끊고 사용"(web_session_takeover)으로 콘이 넘겨받으면, 그 뒤 웹의 화면 API는 409
 *   {"error":"local"}. 웹이 시작한 진행 중 작업은 그대로 끝까지 감. 웹의 편집 중 값은 저장되지 않은 채 버려짐
 *   (설정은 Apply 한 번의 API로만 저장).
 * - 웹이 POST /api/session/claim("다시 연결")을 부르면 바로 다시 웹이 주인(사용자 결정 — 기다리지 않음).
 * - 웹 사용자는 한 명 전제(단일 접속) — 로그인 토큰이 달라도 같은 세션으로 봄.
 */
#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

/* 잠금 상태가 바뀔 때 부를 콜백(어느 태스크에서든 불림 — LVGL 작업은 받는 쪽이 넘겨서 할 것) */
typedef void (*web_session_lock_cb_t)(bool locked);

void web_session_init(web_session_lock_cb_t cb);

/* 화면 API 핸들러 맨 앞(인증 뒤)에서 — 콘이 넘겨받은 상태면 409를 보내고 false(핸들러는 ESP_OK로 끝냄) */
bool web_session_gate(httpd_req_t *req);

/* 콘 잠금 화면 버튼 — 웹 연결을 끊고 콘이 사용 */
void web_session_takeover(void);

/* 지금 웹이 주인(콘 잠금)인지 */
bool web_session_web_active(void);

/* POST /api/session/claim 등록 */
void web_session_register_handlers(httpd_handle_t server);

/* 화면 API 핸들러 맨 앞 — 인증 + 세션 */
#define WEB_SCREEN_API_BEGIN(req) do { \
        if (!web_auth_check(req)) return web_auth_reject(req); \
        if (!web_session_gate(req)) return ESP_OK; \
    } while (0)
