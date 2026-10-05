/**
 * @file    notify.h
 * @brief   휴대폰 푸시 알림(ntfy) — 콘이 이벤트를 휴대폰으로 보냄
 *
 * 2026-10-03(할 일 AD 1단계) — 웹앱이 닫히고 휴대폰이 잠겨 있어도 알림을 받으려고 무료 서비스 ntfy(iOS·Android 앱)를 씀.
 * 콘은 http://ntfy.sh/<주제>로 POST 한 번(암호화 없는 HTTP — TLS 메모리를 안 씀). 알림을 누르면 Click 주소(우리 웹앱)가 열림.
 * 설정은 LittleFS 파일 notify.cfg(설정은 파일로 — NVS 안 씀). 줄 단위 key=value:
 *   topic=<주제 이름>        (없으면 알림 안 보냄)
 *   click=<웹앱 주소>        (예: http://<이름>.iptime.org:<포트>/app, 없으면 Click 헤더 생략)
 *   types=<알림 종류 비트>   (2026-10-05 — 웹 설정의 종류별 끔/켬, 없으면 전부 켬)
 * /admin/upload?file=notify.cfg로 바꾸면 다음 알림부터 바로 반영(보낼 때마다 읽음).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 2026-10-05(사용자 결정 — 모두 알림, 종류별로 끔/켬. 꺼도 콘 알림 기록에는 남음) — 비트 번호 */
typedef enum {
    NOTIFY_TYPE_ERROR = 0,
    NOTIFY_TYPE_WARN,
    NOTIFY_TYPE_RELAY,
    NOTIFY_TYPE_DISCONNECT,
    NOTIFY_TYPE_SD,
    NOTIFY_TYPE_BATTERY,
    NOTIFY_TYPE_CAN,
    NOTIFY_TYPE_COUNT
} notify_type_t;
#define NOTIFY_TYPES_ALL ((1u << NOTIFY_TYPE_COUNT) - 1u)

/* notify.cfg 내용(웹 설정 팝업이 읽고 씀) */
typedef struct {
    char     topic[64];
    char     click[160];
    uint32_t types;
} notify_settings_t;

/* 보내기 태스크 시작 — 부팅 때 한 번(LittleFS 마운트 뒤) */
void notify_init(void);

/* 알림 하나를 큐에 넣고 바로 반환(네트워크 대기 없음). title·msg는 ASCII(화면 영문 원칙과 같음).
 * 큐가 가득 차거나 init 전이면 false */
bool notify_send(const char *title, const char *msg);

/* 2026-10-03(사용자 지시) — 웹앱 바깥 주소(notify.cfg의 click=, 알림 클릭 주소와 같은 값)를 out에 복사. 콘 화면 Summary의 Web
 * 주소·웹 접속 QR이 씀(하드코딩 안 함). 설정 안 됐으면 false(out은 빈 문자열). 어느 태스크에서든 불러도 됨 */
bool notify_copy_public_url(char *out, size_t cap);

/* notify.cfg를 다시 읽음 — /admin/upload로 notify.cfg를 바꾼 직후 */
void notify_reload_cfg(void);

/* 2026-10-05(웹 설정) — 지금 설정 복사 / 검사 후 notify.cfg에 저장. topic은 영문·숫자·_·-만(빈 값 = 알림 안 보냄),
 * click은 공백·제어문자 없는 ASCII(HTTP 헤더에 그대로 들어감). 형식이 틀리면 false(저장 안 함) */
void notify_get_settings(notify_settings_t *out);
bool notify_set_settings(const notify_settings_t *in);
