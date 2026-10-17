#pragma once
/**
 * @file    net_diag.h
 * @brief   2026-10-17(진단, 임시 — 할 일 AF에서 제거) — 내부 RAM이 갑자기 37K 빠졌다가 약 25분 뒤 한 번에 돌아온 일(10-17 11:58~12:24)의
 *          원인 확인용. 추측: 받다가 사라진 웹 연결(인터넷 스캐너가 로그인 없이 받는 /app 48KB 등)이 lwIP 송신 버퍼(최대 45KB, 내부 RAM)를
 *          재전송 포기까지 붙잡음. TCP 연결 목록(상태·포트·상대 IP·안 보낸/확인 못 받은 바이트·재전송 횟수)과 내부 힙 정보를 글로 만듦.
 *          GET /api/devlog_dump?lvl=net 으로 받고, 내부 RAM이 10초 사이 15K 넘게 줄면 ui_main이 net_diag_log()로 Dev Log에 남김.
 */
#include <stddef.h>

/* 지금 상태를 buf에 글로(줄바꿈 구분). lwIP 스레드에서 읽고 돌아올 때까지 기다림(최대 1초). 쓴 길이 반환 */
size_t net_diag_dump(char *buf, size_t cap);

/* 지금 상태를 경고 로그로 남김(Dev Log·시리얼). 기다리지 않음(기록 태스크가 함) — why는 문자열 상수만 */
void net_diag_log(const char *why);
