/**
 * @file    alarm.h
 * @brief   알림 기록 + 전달(웹앱 / 휴대폰 ntfy)
 *
 * 2026-10-05(할 일 AD 5단계 — 사용자 결정):
 * - 사건(에러·경고·릴레이·연결 끊김·SD·배터리·CAN)은 모두 알림 기록에 남김(SD alarm.bin, 최근 ALARM_HISTORY_MAX개).
 *   설정에서 끈 종류는 휴대폰으로 보내지 않고 기록에만 남김.
 * - 보낼 곳: 웹 세션이 살아 있으면 웹앱(주화면 조회 응답에 실어 보냄 — 웹앱이 화면에 띄움), 없으면 ntfy.
 *   웹앱이 받아 가기 전에 세션이 끝나면 그 알림은 ntfy로 보냄.
 * - 제목·내용은 ASCII(콘 화면 영문 원칙과 같음 — ntfy 제목 헤더에도 그대로 들어감).
 * alarm_post는 어느 태스크에서든(뮤텍스를 잡은 채로도) 불러도 됨 — 큐에 넣기만 함(SD·네트워크는 알림 태스크가).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "notify.h"

#define ALARM_HISTORY_MAX 500

#define ALARM_F_PUSH 0x01   /* 그때 켜져 있던 종류 — 휴대폰(웹앱 또는 ntfy)으로 보낼 대상 */
#define ALARM_F_SEEN 0x02   /* 웹앱이 받아 감 */
#define ALARM_F_SENT 0x04   /* ntfy로 보냄 */

typedef struct __attribute__((packed)) {
    uint32_t id;          /* 1부터 하나씩 — 0 = 빈 칸 */
    uint32_t t;           /* 콘 시각(현지값을 초로) */
    uint8_t  type;        /* notify_type_t */
    uint8_t  flags;
    char     title[32];
    char     msg[86];
} alarm_rec_t;            /* 128바이트 고정 — alarm.bin에서 (id % ALARM_HISTORY_MAX)번째 칸 */

/* notify_init 바로 뒤 — 큐·버퍼·태스크만(사건은 여기서부터 쌓임). 처리는 alarm_start 뒤부터 */
void alarm_init(void);
/* SD 마운트·RTC 읽기 뒤 — 기록 파일을 읽고 쌓인 사건 처리 시작 */
void alarm_start(void);

void alarm_post(notify_type_t type, const char *title, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* 마지막 알림 번호(웹앱의 안 본 개수 계산용) */
uint32_t alarm_last_id(void);
/* 알림 기록 한 페이지 — 0 = 최신, 최신이 앞. 반환: 채운 개수 */
int alarm_read_page(uint32_t page, uint32_t page_size, alarm_rec_t *out);
/* 웹앱에 아직 안 간 보낼 대상(ntfy로도 안 보낸 것) — 주화면 내용(HTTP 조회·WebSocket)에 실음. 표시하지 않음 */
int alarm_peek_web_new(alarm_rec_t *out, int cap);
/* 웹앱이 띄웠다고 알려 온 번호까지 "받아 감"으로(2026-10-05 — 보냈다고 받은 것은 아님: 휴대폰이 잠기는 순간이면 못 띄움) */
void alarm_ack_web(uint32_t upto_id);
