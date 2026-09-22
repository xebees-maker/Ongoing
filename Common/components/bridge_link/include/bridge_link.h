#pragma once

/**
 * 2026-09-21(설계 — CNTL<->브릿지(XIAO Seeed, ESP32-C6) I2C 프로토콜, 사용자와의 대화 기반 설계)
 *
 * CNTL은 더 이상 ESP-NOW 라디오를 직접 쓰지 않음 — 별도 브릿지 보드가 ESP-NOW 드라이버 전체
 * (초기화/피어관리/채널/송수신/reliable 재시도 루프)를 대신 갖고, CNTL과는 콘 보드의 외부 I2C
 * 커넥터(GPIO8/9 공유버스를 레벨시프터로 뽑아낸 것)로 연결됨. CNTL이 I2C 마스터, 브릿지가 슬레이브.
 *
 * 브릿지는 Sens/CAM 쪽 esp_now_link.h 프로토콜을 전혀 해석하지 않는 투명 중계임 — payload는 그
 * 프로토콜의 원본 바이트를 그대로 담아 나른다.
 *
 * 2026-09-21 밤 재설계(사용자 지시, v1의 고정 1493바이트 통짜 구조체가 실기에서 I2C 타임아웃을
 * 유발한 걸 발견한 뒤) — "패킷" 개념 자체가 이 링크엔 원래 안 맞을 수 있다는 대화 끝에(멀티플렉싱/
 * 라우팅도, 회선 불안정 대비도 이 10cm 1:1 동기링크엔 불필요) 최소한으로 남김:
 *   - 가변 크기: 헤더(고정, 작음) + payload(실제 길이만큼만) 2단계 트랜잭션.
 *   - SEQ_NUMBER 없음 — dest_mac이 이미 상관키 역할(한 MAC당 동시 요청 1개), 순서뒤바뀜/중복 같은
 *     것 자체가 이 동기식 P2P 링크에서 구조적으로 안 생김(사용자 지시).
 *   - CRC는 최소한으로 유지 — 실패 감지 후 "재시도"가 가장 간단한 복구 전략이라는 전제(사용자
 *     지시: "값이 이상하면 트랜잭션 자체를 다시 해도 되잖아") — 정교한 오류정정이 아니라 딱
 *     재시도 트리거용.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ESP-NOW v2 한도(1470B) 기준 — 헤더/버퍼 크기의 상한일 뿐, 매 전송마다 이만큼 나가는 게
 * 아니라 실제 payload_len만큼만 두 번째 단계에서 전송됨(v1과의 핵심 차이) */
#define BRIDGE_LINK_MAX_PAYLOAD 1470
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

/* 1단계 트랜잭션 — 항상 고정 크기(sizeof(bridge_link_header_t)), payload_len으로 2단계가
 * 필요한지(그리고 몇 바이트인지) 알려줌. header_crc가 틀리면 2단계 없이 바로 재시도 */
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;      /* bridge_msg_type_t */
    uint8_t  mac[6];        /* RELIABLE_SEND/RESULT/FIRE_AND_FORGET: 상대 MAC. INCOMING: 발신 MAC */
    uint8_t  ok;            /* RELIABLE_RESULT 전용: 1=성공 0=타임아웃/실패 */
    int8_t   rssi;          /* INCOMING 전용 */
    uint32_t timeout_ms;    /* RELIABLE_SEND 전용 — 브릿지 내부 esp_now_reliable_request()에 그대로 전달 */
    uint8_t  max_attempts;  /* RELIABLE_SEND 전용 — 위와 동일 */
    uint8_t  accept_reply_types[BRIDGE_LINK_MAX_ACCEPT_TYPES]; /* RELIABLE_SEND 전용 */
    uint8_t  accept_reply_types_count;                          /* RELIABLE_SEND 전용 */
    uint16_t payload_len;   /* 0이면 2단계 트랜잭션 자체가 없음(PING/PONG/SET_CHANNEL 등 대부분) */
    uint16_t header_crc;    /* 아래 필드들(header_crc 자신 제외) CRC16 */
} bridge_link_header_t;

/* CRC-16/CCITT(다항식 0x1021, 초기값 0xFFFF) — 테이블 없이 비트루프, 트랜잭션당 1~2회뿐이라
 * 양쪽 다 성능 문제 없음. 정교한 오류정정 목적이 아니라 "깨졌으면 재시도" 트리거용(사용자 설계) */
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

static inline void bridge_link_header_seal(bridge_link_header_t *hdr)
{
    hdr->header_crc = bridge_link_crc16((const uint8_t *)hdr, offsetof(bridge_link_header_t, header_crc));
}

static inline int bridge_link_header_verify(const bridge_link_header_t *hdr)
{
    if (hdr->payload_len > BRIDGE_LINK_MAX_PAYLOAD) return 0;
    return bridge_link_crc16((const uint8_t *)hdr, offsetof(bridge_link_header_t, header_crc)) == hdr->header_crc;
}

/* 2단계 트랜잭션(header->payload_len > 0일 때만) — payload_len바이트 + 뒤에 CRC16(2바이트).
 * 호출부는 payload_len+2바이트짜리 버퍼를 주고받은 뒤 이 두 함수로 검증/실링 */
static inline void bridge_link_payload_seal(uint8_t *payload_buf_with_crc, uint16_t payload_len)
{
    uint16_t crc = bridge_link_crc16(payload_buf_with_crc, payload_len);
    payload_buf_with_crc[payload_len]     = (uint8_t)(crc & 0xFF);
    payload_buf_with_crc[payload_len + 1] = (uint8_t)(crc >> 8);
}

static inline int bridge_link_payload_verify(const uint8_t *payload_buf_with_crc, uint16_t payload_len)
{
    uint16_t crc = bridge_link_crc16(payload_buf_with_crc, payload_len);
    uint16_t got = (uint16_t)payload_buf_with_crc[payload_len] | ((uint16_t)payload_buf_with_crc[payload_len + 1] << 8);
    return crc == got;
}

/* 코드에서 다루기 편하게 헤더+페이로드를 묶은 메모리상의 논리 메시지 — 와이어 포맷 그 자체는
 * 아님(실제 전송은 항상 헤더 먼저, payload_len>0일 때만 그 뒤에 payload_len+2바이트가 별도
 * I2C 트랜잭션으로 이어짐 — Cntl/main/i2c_bridge.c, Bridge/main/i2c_slave_link.c 참고).
 * packed로 선언해 header 뒤에 payload가 패딩 없이 바로 붙게 해서, 실제 전송 시
 * "&msg 시작부터 sizeof(header)+payload_len+2바이트"를 그대로 한 번에 슬라이스해 쓸 수 있게 함.
 * payload_len==0이면 payload[] 내용/CRC는 의미 없음(전송도 안 됨) */
typedef struct __attribute__((packed)) {
    bridge_link_header_t header;
    uint8_t payload[BRIDGE_LINK_MAX_PAYLOAD + 2];  /* +2: payload_len>0일 때 끝에 붙는 CRC16 */
} bridge_link_msg_t;

static inline void bridge_link_msg_seal(bridge_link_msg_t *msg)
{
    bridge_link_header_seal(&msg->header);
    if (msg->header.payload_len > 0) {
        bridge_link_payload_seal(msg->payload, msg->header.payload_len);
    }
}

/* 이 메시지를 실제로 전송할 때 필요한 총 바이트수(헤더 단계 + payload_len>0이면 페이로드
 * 단계) — i2c_master_transmit()/i2c_slave_write() 호출 시 그대로 씀 */
static inline size_t bridge_link_msg_wire_len(const bridge_link_msg_t *msg)
{
    return sizeof(msg->header) + (msg->header.payload_len > 0 ? (size_t)msg->header.payload_len + 2 : 0);
}
