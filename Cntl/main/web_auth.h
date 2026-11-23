/**
 * @file    web_auth.h
 * @brief   웹 로그인 — PIN(해시) + 오래 가는 로그인 토큰(쿠키). 콘 화면 잠금도 같은 PIN
 *
 * 2026-10-03(할 일 AD — SPA 설계, 사용자 결정) — 바깥에 열리는 웹이라 로그인을 넣음. HTTP 그대로(HTTPS는 3단계).
 * - 비밀번호는 원문을 저장하지 않음: SHA-256(salt || 비밀번호)만 LittleFS web_auth.bin에(설정은 파일로).
 *   원문은 git에도 안 넣음 — 그래서 펌웨어에 기본값이 없음. 2026-11-23(사용자 결정) — 비밀번호 대신 PIN(숫자 4자리),
 *   파일이 없으면(처음) 콘 화면이 PIN 등록을 요구하고 그때까지 웹은 닫혀 있음(같은 공유기 안이든 원격이든 구분 없이 로그인).
 * - 로그인에 성공하면 무작위 토큰(16바이트)을 발급해 쿠키(1년)로 줌 — 다음 접속부터 비밀번호 생략(사용자 결정).
 *   토큰은 최대 8개(기기별), 넘치면 가장 오래된 것부터 밀어냄. 모든 기기 로그아웃 = 토큰 전부 지움.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* 부팅 때 한 번(LittleFS 마운트 뒤) — web_auth.bin을 읽어 둠 */
void web_auth_init(void);

/* PIN이 정해져 있는지. 2026-11-23 — 없으면 웹은 모두 401(로그인은 403 nopin) */
bool web_auth_enabled(void);

/* 요청의 쿠키 토큰이 유효한지. PIN이 아직 없으면 항상 false */
bool web_auth_check(httpd_req_t *req);

/* 401 + {"error":"auth"} 응답(호출부는 이 반환값을 그대로 돌려주면 됨) */
esp_err_t web_auth_reject(httpd_req_t *req);

/* 2026-10-22(사용자 설계 — 복원 중엔 웹에 "복원 중" 화면) — 빈 SD에 측정 기록을 다시 옮기는 중(stats_store_is_restoring)이면
 * 503 {"error":"restoring"}을 보내고 false. 그동안 측정 기록을 읽는 요청은 옮기기 잠금을 기다리며 웹서버(태스크 하나)를 멈춤 */
bool web_restore_gate(httpd_req_t *req);

/* 핸들러 맨 앞에 — 인증 안 됐으면 401로 끝냄, 복원 중이면 503 */
#define WEB_AUTH_REQUIRE(req) do { \
        if (!web_auth_check(req)) return web_auth_reject(req); \
        if (!web_restore_gate(req)) return ESP_OK; \
    } while (0)

/* 2026-11-23(사용자 결정) — 개발용 키(X-Dev-Key, git 밖 dev_key.h)가 맞으면 로그인 없이 통과. 덤프 도구가 로그인하면 세션이 하나라
 * 사용자 기기가 로그아웃되기 때문. 임시 API(devlog_dump·screen·hittest)와 알림 목록 읽기에만. 출시 전 지움 */
bool web_auth_dev_key_ok(httpd_req_t *req);
#define WEB_AUTH_REQUIRE_OR_DEV(req) do { \
        if (!web_auth_dev_key_ok(req)) WEB_AUTH_REQUIRE(req); \
    } while (0)

/* 2026-11-23(사용자 결정 — PIN) — 콘 화면 잠금과 웹 로그인이 같은 PIN(숫자 4자리)을 씀. 콘 화면(LVGL 태스크)에서 부름.
 * PIN은 콘을 처음 켤 때 콘 화면에서만 정함(정하기 전엔 웹이 열리지 않음). 그 뒤엔 콘(지금 PIN 확인 뒤)이나 웹에서 바꿈.
 * 없애는 길은 없음. set: 숫자 4자리가 아니거나 저장 실패면 false. 바꾸면 웹 로그인 토큰을 모두 지움(모든 기기 다시 로그인) */
bool web_auth_set_pin_local(const char *pin);
bool web_auth_verify_pin_local(const char *pin);

/* 2026-11-23 — 로그인 세대(새 토큰을 낼 때마다 오름, 세션 하나). ws는 연결 맺을 때 값과 다르면 닫음 */
uint32_t web_auth_login_gen(void);

/* 2026-11-23(사용자 결정) — PIN 복구 키(숫자 8자리, 공급 때 콘마다 다름). 콘 화면(LVGL 태스크)에서 부름 */
#define WEB_AUTH_RECOVERY_LEN 8
typedef enum {
    WEB_AUTH_RECOVERY_OK,      /* 맞음 — PIN·웹 로그인을 지웠음(PIN 등록으로) */
    WEB_AUTH_RECOVERY_WRONG,
    WEB_AUTH_RECOVERY_WAIT,    /* 너무 많이 틀려 기다리는 중(web_auth_recovery_wait_s) */
    WEB_AUTH_RECOVERY_NONE,    /* 이 콘엔 복구 키가 없음 */
} web_auth_recovery_result_t;
bool web_auth_recovery_present(void);
int  web_auth_recovery_wait_s(void);   /* 0이면 바로 넣을 수 있음 */
web_auth_recovery_result_t web_auth_recovery_try(const char *key);

/* 핸들러 등록 — POST /api/login, POST /api/password, GET /api/restore_state(2026-10-22). logout_all은 11-23에 없앰 */
void web_auth_register_handlers(httpd_handle_t server);
