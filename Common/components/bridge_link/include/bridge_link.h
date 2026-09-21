#pragma once

/**
 * 2026-09-21(설계 — CNTL<->브릿지(XIAO Seeed) I2C 프로토콜, 사용자와의 대화 기반 설계)
 *
 * CNTL은 더 이상 ESP-NOW 라디오를 직접 쓰지 않음 — 별도 브릿지 보드(XIAO Seeed, ESP32-C3)가
 * ESP-NOW 드라이버 전체(초기화/피어관리/채널/송수신/reliable 재시도 루프)를 대신 갖고,
 * CNTL과는 isolated I2C 4단자(VCC/GND/SDA/SCL, 여분 핀 없음)로만 연결됨. CNTL이 I2C 마스터
 * (지능을 가진 쪽이 버스도 주도 — 사용자 설계), 브릿지가 슬레이브.
 *
 * 브릿지는 Sens/CAM 쪽 esp_now_link.h 프로토콜을 전혀 해석하지 않는 투명 중계임 — 여기 정의된
 * bridge_link_packet_t의 payload는 그 프로토콜의 원본 바이트를 그대로 담아 나른다. CNTL의
 * CASK 상태머신(esp_now_hub.c)은 이 대화에서 전혀 안 바뀜 — 실제 esp_now_send()/reliable
 * 호출부만 이 패킷을 통해 브릿지로 위임하는 얇은 계층(Cntl/main/i2c_bridge.c)으로 바뀜.
 *
 * 고정 크기 구조체 하나로 설계(가변 길이 프레이밍 대신) — I2C 트랜잭션 구현을 단순하게 유지하기
 * 위한 1차 구현 선택. 사진 전송(PHOTO_CHUNK, 최대 ~1MB)도 이 경로를 그대로 타므로(브릿지는
 * 무조건 투명 중계 — 다른 물리 경로가 없음, 2026-09-21 사용자 확인) I2C 처리량이 문제가 되면
 * 그때 가변 길이/스트리밍 방식으로 재설계할 것 — 이번 1차 구현의 의도적 단순화.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ESP-NOW v2 한도(1470B) 기준 — Common/esp_now_reliable.c의 REPLY_BUF_CAP과 동일 전례를 따름
 * (어떤 응답 타입이 오든 이 안에 들어옴이 이미 그 컴포넌트에서 검증됨) */
#define BRIDGE_LINK_MAX_PAYLOAD 1470

/* accept_reply_types 배열 — 실사용 호출부는 전부 1개짜리라(esp_now_tx.c 등) 여유있게 4로 잡음 */
#define BRIDGE_LINK_MAX_ACCEPT_TYPES 4

typedef enum {
    BRIDGE_MSG_NONE = 0,        /* 빈 슬롯 표시(폴링 읽기에서 "대기 중인 게 없음") */

    /* DATA — Sens/CAM으로 가는/오는 실제 CASK 트래픽의 투명 중계 */
    BRIDGE_MSG_RELIABLE_SEND,   /* CNTL->브릿지: mac에 payload를 reliable로 보내고 결과 기다림 */
    BRIDGE_MSG_RELIABLE_RESULT, /* 브릿지->CNTL: 위 요청의 최종 결과(성공/실패+응답 payload) */
    BRIDGE_MSG_FIRE_AND_FORGET, /* CNTL->브릿지: mac에 payload를 그냥 1회 전송(ACK 등, 재시도 없음) */
    BRIDGE_MSG_INCOMING,        /* 브릿지->CNTL: mac에서 자발적으로 온 수신(rssi 포함) */

    /* CONTROL — 브릿지 자체 관리 */
    BRIDGE_MSG_SET_CHANNEL,     /* CNTL->브릿지: mac[0]에 채널 번호(1~13) */
    BRIDGE_MSG_PING,            /* CNTL->브릿지: 생존확인 */
    BRIDGE_MSG_PONG,            /* 브릿지->CNTL: PING 응답 */
    BRIDGE_MSG_RESET,           /* CNTL->브릿지: 브릿지 자체 esp_restart() 요청 */
} bridge_msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t  msg_type;      /* bridge_msg_type_t */
    uint8_t  mac[6];        /* RELIABLE_SEND/RESULT/FIRE_AND_FORGET: 상대 MAC. INCOMING: 발신 MAC */
    uint8_t  ok;            /* RELIABLE_RESULT 전용: 1=성공 0=타임아웃/실패 */
    int8_t   rssi;          /* INCOMING 전용 */
    uint32_t timeout_ms;    /* RELIABLE_SEND 전용 — 브릿지 내부 esp_now_reliable_request()에 그대로 전달 */
    uint8_t  max_attempts;  /* RELIABLE_SEND 전용 — 위와 동일 */
    uint8_t  accept_reply_types[BRIDGE_LINK_MAX_ACCEPT_TYPES]; /* RELIABLE_SEND 전용 */
    uint8_t  accept_reply_types_count;                          /* RELIABLE_SEND 전용 */
    uint16_t payload_len;
    uint8_t  payload[BRIDGE_LINK_MAX_PAYLOAD];
    uint16_t crc;           /* 아래 bridge_link_crc16()으로 msg_type..payload[payload_len-1]까지 계산 */
} bridge_link_packet_t;

/* CRC-16/CCITT(포함 다항식 0x1021, 초기값 0xFFFF) — 테이블 없이 비트루프로 계산, 매 트랜잭션마다
 * 한 번씩만 부르므로 양쪽 다 성능 문제 없음. len==0이면 msg_type+mac+ok+rssi+timeout_ms+
 * max_attempts+accept_reply_types+accept_reply_types_count+payload_len 고정 헤더만 계산됨 —
 * 호출부는 항상 offsetof(payload)+payload_len을 넘겨야 함(bridge_link_crc_len() 참고) */
static inline uint16_t bridge_link_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* CRC 계산 대상 길이 — 고정 헤더 전체(offsetof(payload)) + 실제 payload_len만큼만(패딩 바이트는
 * 안 봄, 매번 다른 쓰레기값이라 포함하면 CRC가 무의미해짐) */
static inline size_t bridge_link_crc_len(const bridge_link_packet_t *pkt)
{
    return offsetof(bridge_link_packet_t, payload) + pkt->payload_len;
}

static inline void bridge_link_seal(bridge_link_packet_t *pkt)
{
    pkt->crc = bridge_link_crc16((const uint8_t *)pkt, bridge_link_crc_len(pkt));
}

static inline int bridge_link_verify(const bridge_link_packet_t *pkt)
{
    if (pkt->payload_len > BRIDGE_LINK_MAX_PAYLOAD) return 0;
    return bridge_link_crc16((const uint8_t *)pkt, bridge_link_crc_len(pkt)) == pkt->crc;
}
