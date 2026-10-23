#pragma once
/**
 * @file    sensor_registry.h
 * @brief   센서 중심 구조(2026-10-23, 사용자 설계 — Docs/설계_센서중심_2026-10-23.md)
 *
 * 계층: 보드(통신만 — 어느 포트에 어떤 센서, 어떻게 읽는지) · 센서(관리 단위, 콘이 ID를 붙여 센스에 알려 줌) · 센싱(센서가 내는 값 하나 —
 * 무엇을·어디를·정밀도). 사람이 보는 단위는 센싱 값. 기록·집계·그래프·이름은 센싱 ID로 다룸.
 * 센서 교체: 같은 센싱(무엇·어디·정밀도)이 시스템에 하나고 그 센서가 떨어져 있으면 새 센서가 자동으로 이어받음, 여럿이면
 * 새 센싱(사용자에게 묻는 화면은 4단계).
 * 용량(기술 제약): 센서 8, 센싱 12. 목록은 LittleFS sensors.bin.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_now_link.h"

#define SREG_MAX_SENSORS   8
#define SREG_MAX_SENSINGS  12

typedef enum {
    SENSING_WHERE_AIR = 0,        /* 공기(온습도·가스) */
    SENSING_WHERE_SUBSTRATE = 1,  /* 배지 */
} sensing_where_t;

/* 센싱 하나(사람이 보는 값) */
typedef struct {
    uint16_t id;            /* 센싱 ID(1부터, 0 = 빈 칸) */
    uint8_t  what;          /* sensor_channel_type_t — 무엇을 */
    uint8_t  where;         /* sensing_where_t — 어디를 */
    uint8_t  precise;       /* 1 = 정밀, 0 = 간이 */
    uint8_t  ordinal;       /* 같은 (무엇·어디·정밀도) 안의 번호(1부터) — 자동 이름 "배지 온도 2" */
    uint16_t sensor_id;     /* 지금 이 값을 내는 센서 */
    uint8_t  sensor_chan;   /* 그 센서 채널 안의 순번 */
    uint8_t  reserved;
} sreg_sensing_t;

/* 페어링 응답으로 센서 목록을 반영. 새로 ID를 알려 줘야 할 센서가 있으면 out_assign을 채우고 true(보낼 것이 있음).
 * 옛 형식 보드(센서 목록 없음)는 센서 하나로 보고 ID는 콘만 가짐(알려 주지 않음). 통신 태스크에서 불림 — 파일 저장은 기록 태스크로 */
bool sreg_on_pair(const uint8_t mac[6], uint8_t node_kind, uint8_t chan_count, const uint8_t *chan_type,
                  uint8_t sensor_count, const esp_now_sensor_desc_t *sensors, esp_now_sensor_id_assign_t *out_assign);

/* 보드 채널 → 센싱. 없으면 false */
bool sreg_sensing_for(const uint8_t mac[6], uint8_t chan_index, sreg_sensing_t *out);

/* 센싱 ID → 센싱. 없으면 false */
bool sreg_get_sensing(uint16_t sensing_id, sreg_sensing_t *out);

/* 지금 있는 센싱 목록(최대 SREG_MAX_SENSINGS) — 반환 = 개수 */
int sreg_list_sensings(sreg_sensing_t *out, int max);

/* 센싱의 보드가 지금 연결돼 있는지(그래프 "기기 없는 계열은 그리지 않음" 등) */
bool sreg_sensing_board(uint16_t sensing_id, uint8_t out_mac[6]);

/* 센싱 자동 이름(영문/한글 문자열 표는 ui_strings — 여기는 구분값만) */

/* 2026-10-23 — 센서별 측정 상태(센스가 상태 메시지마다 보냄, RAM만). 로그로 남길 때(상태가 바뀜, 또는 실패가 이어지는 동안
 * 40번마다)면 true와 센서 ID. 통신 태스크에서 불림 */
bool sreg_note_status(const uint8_t mac[6], uint8_t port, uint8_t fault, uint16_t streak, uint16_t *out_sensor_id);

/* 부팅 때 한 번(LittleFS 뒤) */
void sreg_init(void);

/* /api/sensors — 센서·센싱 목록 JSON, 길이 반환 */
int sreg_json(char *buf, size_t cap);
