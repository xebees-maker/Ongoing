/**
 * @file    web_auth.h
 * @brief   웹 로그인 — 비밀번호(해시) + 오래 가는 로그인 토큰(쿠키)
 *
 * 2026-10-03(할 일 AD — SPA 설계, 사용자 결정) — 바깥에 열리는 웹이라 로그인을 넣음. HTTP 그대로(HTTPS는 3단계).
 * - 비밀번호는 원문을 저장하지 않음: SHA-256(salt || 비밀번호)만 LittleFS web_auth.bin에(설정은 파일로).
 *   원문은 git에도 안 넣음 — 그래서 펌웨어에 기본 비밀번호가 없고, 파일이 없으면(처음) 인증 없이 열려 있다가
 *   POST /api/password로 처음 정하는 순간부터 모든 API에 인증이 걸림.
 * - 로그인에 성공하면 무작위 토큰(16바이트)을 발급해 쿠키(1년)로 줌 — 다음 접속부터 비밀번호 생략(사용자 결정).
 *   토큰은 최대 8개(기기별), 넘치면 가장 오래된 것부터 밀어냄. 모든 기기 로그아웃 = 토큰 전부 지움.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* 부팅 때 한 번(LittleFS 마운트 뒤) — web_auth.bin을 읽어 둠 */
void web_auth_init(void);

/* 비밀번호가 정해져 있는지(=인증을 요구하는지) */
bool web_auth_enabled(void);

/* 요청의 쿠키 토큰이 유효한지. 비밀번호가 아직 없으면 항상 true */
bool web_auth_check(httpd_req_t *req);

/* 401 + {"error":"auth"} 응답(호출부는 이 반환값을 그대로 돌려주면 됨) */
esp_err_t web_auth_reject(httpd_req_t *req);

/* 핸들러 맨 앞에 — 인증 안 됐으면 401로 끝냄 */
#define WEB_AUTH_REQUIRE(req) do { if (!web_auth_check(req)) return web_auth_reject(req); } while (0)

/* 핸들러 등록 — POST /api/login, POST /api/password, POST /api/logout_all */
void web_auth_register_handlers(httpd_handle_t server);
