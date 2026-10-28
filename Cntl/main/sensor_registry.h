#pragma once
/**
 * @file    sensor_registry.h
 * @brief   센서 중심 구조(2026-10-23, 사용자 설계 — Docs/설계_센서중심_2026-10-23.md)
 *
 * 계층: 보드(통신만 — 어느 포트에 어떤 센서, 어떻게 읽는지) · 센서(관리 단위, 콘이 ID를 붙여 센스에 알려 줌) · 센싱(센서가 내는 값 하나 —
 * 무엇을·어디를·정밀도). 사람이 보는 단위는 센싱 값. 기록·집계·그래프·이름은 센싱 ID로 다룸.
 *
 * 2026-10-28(사용자 설계 — 고정 자리, Docs/설계_센서중심_2026-10-23.md 8절):
 * - 센싱 = 종류(무엇·어디·정밀도)별 고정 자리. 자리 수 = 상한(배지 온도 3, 캠 2, 나머지 1). 번호(ordinal)는 1..상한, 늘어나지 않음.
 * - 기록은 자리에 속함 — 센서가 바뀌어도 이어짐(교체 시점 표시 없음).
 * - 대치 = 새 센서가 옛 센서의 칸을 그대로 덮어씀(내부 번호만 새로). 자리가 1개인 종류는 옛 센서의 보드가 연결 없음이면 묻지 않고
 *   대치, 연결돼 있으면 사용자가 대기 목록에서 "대치"를 골라야 함. 자리가 여럿인 종류(배지 온도·캠)는 같은 종류 자리가 이미 있으면
 *   새 보드가 붙을 때 "대치(대상 고름) / 신규(다음 빈 자리)"를 물음.
 * - 끊기 = 자리·기록 남음. 완전히 지우기 = 자리·센서·기록 모두 지움(sreg_forget_*).
 * - "연결 없음"은 콘의 연결 판단(node_hub)을 따름 — 예전의 600초 기준은 없앰(근거 없이 넣었던 값).
 * 캠도 같은 표에 둠(보드 하나 = 센서 하나, 콘 안에서만 쓰는 SREG_KIND_CAM/SREG_WHAT_CAMERA). 캠 자리는 사진·설정의 저장 키(key_mac)를 가짐.
 * 용량(기술 제약): 센서 12, 센싱 16. 목록은 LittleFS sensors.bin.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_now_link.h"

#define SREG_MAX_SENSORS   12
#define SREG_MAX_SENSINGS  16
#define SREG_MAX_SLOTS     3     /* 한 종류의 자리 수 최대(배지 온도) */

/* 콘 안에서만 쓰는 값(통신 규약 아님) — 캠 보드 하나 = 센서 하나, 자리 하나 */
#define SREG_KIND_CAM      0xC0
#define SREG_WHAT_CAMERA   0xC0

typedef enum {
    SENSING_WHERE_AIR = 0,        /* 공기(온습도·가스) */
    SENSING_WHERE_SUBSTRATE = 1,  /* 배지 */
} sensing_where_t;

/* 센싱 하나(사람이 보는 값) = 자리 하나 */
typedef struct {
    uint16_t id;            /* 센싱 ID(1부터, 0 = 빈 칸) — 기록이 이 번호로 저장됨 */
    uint8_t  what;          /* sensor_channel_type_t(캠은 SREG_WHAT_CAMERA) — 무엇을 */
    uint8_t  where;         /* sensing_where_t — 어디를 */
    uint8_t  precise;       /* 1 = 정밀, 0 = 간이 */
    uint8_t  ordinal;       /* 자리 번호(1..그 종류의 상한) — 자동 이름 "배지 온도 2" */
    uint16_t sensor_id;     /* 지금 이 자리의 센서(0 = 없음) */
    uint8_t  sensor_chan;   /* 그 센서 채널 안의 순번 */
    uint8_t  reserved;
    uint8_t  key_mac[6];    /* 캠 자리: 사진·설정 저장 키(처음 이 자리에 붙은 캠의 주소). 센서 자리는 0 */
} sreg_sensing_t;

/* 등록 판정(자리 정하기) — 보드의 센서마다 */
typedef enum {
    SREG_ITEM_BOUND = 0,   /* 이미 자리에 묶여 있음 */
    SREG_ITEM_NEW,         /* 새 자리(같은 종류가 없거나, 사용자가 신규를 고름) */
    SREG_ITEM_REPLACE,     /* target 센서의 자리를 대치(자동 또는 사용자가 고름) */
    SREG_ITEM_ASK,         /* 자리가 여럿인 종류 — 대치/신규를 사용자가 골라야 함 */
    SREG_ITEM_BLOCKED,     /* 자리가 1개인 종류인데 지금 자리의 센서(target)가 연결돼 있음 — 사용자가 대치를 골라야 함 */
} sreg_item_state_t;

typedef struct {
    uint16_t sensor;        /* 그 자리의 센서(0 = 없음) */
    uint16_t sensing;       /* 대표 센싱 ID(이름) */
    uint8_t  ordinal;
    uint8_t  board[6];      /* 센서가 붙어 있던 보드 */
    bool     live;          /* 그 보드가 지금 연결돼 있음 */
} sreg_cand_t;

typedef struct {
    uint8_t     port;
    uint8_t     kind;       /* sensor_kind_t 또는 SREG_KIND_CAM */
    uint8_t     state;      /* sreg_item_state_t */
    bool        can_new;    /* 그 종류에 빈 자리가 있음 */
    uint16_t    target;     /* REPLACE/BLOCKED: 대상 센서 */
    uint8_t     ncand;      /* ASK/BLOCKED: 대치 후보(같은 종류 자리) */
    sreg_cand_t cand[SREG_MAX_SLOTS];
} sreg_plan_item_t;

typedef struct {
    uint8_t          n;
    sreg_plan_item_t it[ESP_NOW_MAX_SENSORS];
} sreg_plan_t;

/* 사용자가 고른 것(대기 목록 팝업) — 포트별. target 0 = 신규 */
typedef struct {
    uint8_t n;
    struct { uint8_t port; uint16_t target; } c[ESP_NOW_MAX_SENSORS];
} sreg_decision_t;

/* 지금 연결된 보드 목록(node_hub가 만들어 넘김 — 등록부는 node_hub 잠금을 잡지 않음) */
typedef struct {
    const uint8_t (*mac)[6];
    int n;
} sreg_live_t;

/* 보드의 센서 목록. sensor_count = 0이면 옛 형식(보드 하나 = 센서 하나, node_kind·채널 전부) */
typedef struct {
    uint8_t node_kind;
    uint8_t chan_count;
    const uint8_t *chan_type;
    uint8_t sensor_count;
    const esp_now_sensor_desc_t *sensors;
} sreg_board_t;

/* 판정만(목록을 바꾸지 않음). 모든 센서가 자리를 가질 수 있으면 true(ASK·BLOCKED가 하나라도 있으면 false) */
bool sreg_plan(const uint8_t mac[6], const sreg_board_t *b, const sreg_decision_t *dec, const sreg_live_t *live, sreg_plan_t *out);

/* 페어링 응답으로 센서 목록을 반영(sreg_plan과 같은 판정 — ASK·BLOCKED인 센서는 건너뜀). 새로 ID를 알려 줘야 할 센서가 있으면
 * out_assign을 채우고 true. 대치가 일어나 옛 보드 주소가 다르면 out_replaced_board에(설정 옮기기용, 없으면 0으로 채움).
 * 통신 태스크에서 불림 — 파일 저장은 기록 태스크로 */
bool sreg_on_pair(const uint8_t mac[6], const sreg_board_t *b, const sreg_decision_t *dec, const sreg_live_t *live,
                  esp_now_sensor_id_assign_t *out_assign, uint8_t out_replaced_board[6]);

/* 보드 채널 → 센싱. 없으면 false */
bool sreg_sensing_for(const uint8_t mac[6], uint8_t chan_index, sreg_sensing_t *out);

/* 센싱 ID → 센싱. 없으면 false */
bool sreg_get_sensing(uint16_t sensing_id, sreg_sensing_t *out);

/* 지금 있는 센싱 목록(최대 SREG_MAX_SENSINGS) — 반환 = 개수 */
int sreg_list_sensings(sreg_sensing_t *out, int max);

/* 센싱의 센서가 지금 어느 보드에 붙어 있는지(그래프 "기기 없는 계열은 그리지 않음" 등) */
bool sreg_sensing_board(uint16_t sensing_id, uint8_t out_mac[6]);

/* 캠 보드 주소 → 그 캠 자리의 저장 키(사진 폴더·설정). 자리가 없으면 mac 그대로 */
void sreg_cam_storage_mac(const uint8_t mac[6], uint8_t out[6]);

/* 자리 목록(설정 화면) — 센서 하나당 한 줄(센서가 없는 옛 자리는 자리마다 한 줄, sensor = 0) */
typedef struct {
    uint16_t sensor;
    uint8_t  kind;
    uint8_t  board[6];
    bool     attached;
    uint8_t  nsensing;
    uint16_t sensing[ESP_NOW_MAX_CHANNELS];   /* 자리 번호순, 첫 번째가 대표(이름) */
} sreg_slot_row_t;
int sreg_list_slots(sreg_slot_row_t *out, int max);

/* 완전히 지우기 — 그 줄의 센서와 자리를 지움. 지운 센싱을 out에(기록 지우기·캠 사진 지우기용), 반환 = 개수 */
int sreg_forget_row(const sreg_slot_row_t *row, sreg_sensing_t *out, int max);
/* 보드에 붙은 센서 전부 */
int sreg_forget_board(const uint8_t mac[6], sreg_sensing_t *out, int max);
/* 이 보드에 등록된 센서가 남아 있는지 */
bool sreg_board_has_sensors(const uint8_t mac[6]);
/* 2026-10-28 — 자리를 가진 보드들(센싱이 가리키는 센서의 보드, 겹치면 하나). is_cam = 캠 자리. 반환: 개수 */
int sreg_list_owner_boards(uint8_t (*out)[6], bool *is_cam, int max);

/* 2026-10-28(임시 — 10-27에 갈라진 이산화탄소 자리 합치기, /api/sensing/merge) — from 자리의 센서를 to 자리로 옮기고 from을 지움 */
bool sreg_merge_sensing(uint16_t from, uint16_t to);

/* 2026-10-23 — 센서별 측정 상태(센스가 상태 메시지마다 보냄, RAM만). 로그로 남길 때(상태가 바뀜, 또는 실패가 이어지는 동안
 * 40번마다)면 true와 센서 ID. 통신 태스크에서 불림 */
bool sreg_note_status(const uint8_t mac[6], uint8_t port, uint8_t fault, uint16_t streak, uint16_t *out_sensor_id);

/* 부팅 때 한 번(LittleFS 뒤) */
void sreg_init(void);

/* /api/sensors — 센서·센싱 목록 JSON, 길이 반환 */
int sreg_json(char *buf, size_t cap);
