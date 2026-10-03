/**
 * @file    notify.h
 * @brief   휴대폰 푸시 알림(ntfy) — 콘이 이벤트를 휴대폰으로 보냄
 *
 * 2026-10-03(할 일 AD 1단계) — 웹앱이 닫히고 휴대폰이 잠겨 있어도 알림을 받으려고 무료 서비스 ntfy(iOS·Android 앱)를 씀.
 * 콘은 http://ntfy.sh/<주제>로 POST 한 번(암호화 없는 HTTP — TLS 메모리를 안 씀). 알림을 누르면 Click 주소(우리 웹앱)가 열림.
 * 설정은 LittleFS 파일 notify.cfg(설정은 파일로 — NVS 안 씀). 줄 단위 key=value:
 *   topic=<주제 이름>        (없으면 알림 안 보냄)
 *   click=<웹앱 주소>        (예: http://<이름>.iptime.org:<포트>/app, 없으면 Click 헤더 생략)
 * /admin/upload?file=notify.cfg로 바꾸면 다음 알림부터 바로 반영(보낼 때마다 읽음).
 */
#pragma once

#include <stdbool.h>

/* 보내기 태스크 시작 — 부팅 때 한 번(LittleFS 마운트 뒤) */
void notify_init(void);

/* 알림 하나를 큐에 넣고 바로 반환(네트워크 대기 없음). title·msg는 ASCII(화면 영문 원칙과 같음).
 * 큐가 가득 차거나 init 전이면 false */
bool notify_send(const char *title, const char *msg);
