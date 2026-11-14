#pragma once
/**
 * @file    sensing_state.h
 * @brief   센싱 하나의 상태(2026-11-12, 사용자와 대화로 확정 — Docs/한일_2026-09-28.txt 11-12절 6번 표 1)
 *
 * 상태: 미등록 / 확인 중 / 사용 가능 / 끊김 / 고장.
 *  - 미등록: 센싱에 붙은 센서가 없거나, 그 보드를 이번 부팅에 사용자가 끊음(해제)·자리를 잃음.
 *  - 확인 중: 등록됐지만 아직 값이 없음(콘 재시작 뒤 그 보드의 끊김 판정 시간 안, 또는 막 붙어 첫 값 전).
 *  - 사용 가능: 보드가 연결돼 있고 정상 값이 있음.
 *  - 끊김: 보드 응답이 끊김 판정 시간을 넘김(node_hub의 연결 판단 그대로).
 *  - 고장: 보드는 응답하는데 그 센서가 연속 5회 실패(센스가 보내는 센서별 연속 실패 수), 또는 마지막 값이 범위 밖.
 * 상태는 저장하지 않고 물을 때마다 계산함(값·범위 밖 여부만 기억). 값은 매 응답(같은 측정의 재전송 포함)마다 node_hub가 넘김.
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SENSING_ST_UNREG = 0,
    SENSING_ST_CHECKING,
    SENSING_ST_USABLE,
    SENSING_ST_LOST,
    SENSING_ST_FAULT,
} sensing_st_t;

#define SENSING_FAULT_STREAK 5   /* 사용자 결정 11-12 — 연속 실패 5회면 고장 */

/* node_hub(통신 태스크) — 응답마다 채널 하나씩. ok = 센스가 그 채널 측정에 성공함 */
void sensing_state_note(uint16_t sensing, uint8_t what, bool ok, float value);

/* 지금 상태. 사용 가능이면 out_value에 마지막 값(NULL 가능) */
sensing_st_t sensing_state_get(uint16_t sensing, float *out_value);

/* 로그용 영어 이름 */
const char *sensing_state_name(sensing_st_t st);
