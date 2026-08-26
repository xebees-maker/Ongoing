#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 2026-08-23 — SD 로그 대신 스피커 소리로 캠 상태를 듣기 위한 모듈. 배터리 단독
 *        구동 조사용으로 이벤트를 구분한다(2026-08-23 사용자 지시로 6->7 재설계 —
 *        "채널 스캔"과 "광고 전송"을 별개 소리로 분리; 2026-08-25 CASK 재설계로 7->10 ->
 *        8로 — 페어링 핸드셰이크 단계별 소리를 추가했다가, 핑퐁 자체가 프로토콜에서
 *        빠지면서 PING/PONG 이벤트도 같이 제거됨; 2026-08-26 — CASK의 실제 메시지들
 *        (WAKE_HELLO~SLEEP_NOW_ACK)에도 소리를 붙여 8->13). 각 이벤트는 cam_speaker_notify()로
 *        큐에 넣고, 내부 전용 태스크가 순서대로 재생한다(호출측을 절대 블로킹하지 않음 —
 *        특히 esp_now recv 콜백/타이머 콜백에서 안전하게 부를 수 있어야 해서).
 *
 *        번호(콘솔 soundlog solo N과 동일) / 이벤트 / 소리:
 *        1 POWERON_BATTERY  도 1초      2 POWERON_USB     솔 1초
 *        3 SCAN_CHANNEL     미 0.1초    4 SWEEP_DONE      미 0.5초
 *        5 ADVERTISE_SENT   솔 0.1초    6 ADVERTISE_ACK   시 0.2초
 *        7 PAIR_REQUESTED   도 0.1초    8 PAIR_ACK        시 0.3초
 *        9 WAKE_HELLO       미 0.1초    10 WAKE_HELLO_ACK 솔 0.2초
 *        11 CAM_CONFIG      레 0.1초    12 SLEEP_NOW      파 0.1초
 *        13 SLEEP_NOW_ACK   시 0.2초
 */
typedef enum {
    SPK_EVT_POWERON_BATTERY = 0,  /* 1. Power on (battery) — 도 1s */
    SPK_EVT_POWERON_USB,          /* 2. Power on (USB) — 솔 1s */
    SPK_EVT_SCAN_CHANNEL,         /* 3. 채널 스캔(채널 이동) — 미 0.1s */
    SPK_EVT_SWEEP_DONE,           /* 4. 스윕 완료 — 미 0.5s */
    SPK_EVT_ADVERTISE_SENT,       /* 5. 광고 전송 — 솔 0.1s */
    SPK_EVT_ADVERTISE_ACK,        /* 6. 광고 응답 수신 — 시 0.2s */
    SPK_EVT_PAIR_REQUESTED,       /* 7. PAIR_REQUEST 수신 — 도 0.1s */
    SPK_EVT_PAIR_ACK,             /* 8. PAIR_ACK 전송 — 시 0.3s */
    SPK_EVT_WAKE_HELLO,           /* 9. WAKE_HELLO 전송 — 미 0.1s (2026-08-26) */
    SPK_EVT_WAKE_HELLO_ACK,       /* 10. WAKE_HELLO_ACK 수신 — 솔 0.2s (2026-08-26) */
    SPK_EVT_CAM_CONFIG,           /* 11. CAM_CONFIG_SET 수신 — 레 0.1s (2026-08-26) */
    SPK_EVT_SLEEP_NOW,            /* 12. SLEEP_NOW 수신 — 파 0.1s (2026-08-26) */
    SPK_EVT_SLEEP_NOW_ACK,        /* 13. SLEEP_NOW_ACK 전송 — 시 0.2s (2026-08-26) */
    SPK_EVT_COUNT,
} cam_speaker_event_t;

/**
 * @brief 온보드 스피커(ES8311 코덱 + NS4150B 앰프) + 이벤트 재생 태스크 초기화.
 *        bsp_esp32s3_cam_init() 이후에 호출할 것(공유 I2C 버스, IO 익스팬더 PA_CTRL=EXIO4 사용).
 */
esp_err_t cam_speaker_init(void);

/**
 * @brief 이벤트 발생 알림 — 논블로킹(큐에 넣기만 함). 실제 재생은 내부 태스크가 함.
 *        큐가 가득 차면(재생이 밀리는 중) 조용히 버림(진단용이라 순서 보장보다 호출측
 *        안전이 우선). cam_speaker_set_enabled(false)면 아무 것도 안 함(조용히 무시).
 */
void cam_speaker_notify(cam_speaker_event_t event);

/**
 * @brief 소리 로그 켜기/끄기 — 기본값 꺼짐(cam_speaker_init() 직후). 하드웨어(I2S/코덱)
 *        초기화는 그대로 유지, 이벤트 재생만 켜고 끔.
 */
void cam_speaker_set_enabled(bool enabled);

/**
 * @brief 2026-08-23 — 재생할 이벤트를 비트마스크로 지정(여러 개 동시 선택 가능 — 사용자 지시:
 *        "소리를 여러개 지정할 수 있게 해"). 비트 N = cam_speaker_event_t 값 N(0-based).
 *        mask==0이면 필터 없음(켜진 이벤트 전부 재생, 기본값/원래 동작). 콘솔의
 *        "soundlog solo 3,4,5" 같은 콤마 목록이 이걸로 변환돼서 들어옴.
 */
void cam_speaker_set_solo_mask(uint32_t mask);

/**
 * @brief 2026-08-23 — 큐에 쌓인 소리 + 현재 재생 중인 소리가 다 끝날 때까지(최대 timeout_ms)
 *        대기. 딥슬립 진입 직전에 불러야 함 — 안 그러면 notify()가 큐에 넣기만 하고 실제
 *        재생(speaker_task)이 돌기 전에 esp_deep_sleep_start()로 하드웨어가 꺼져 소리가
 *        아예 안 남(스윕완료 즉시 잠드는 새 설계에서 실기로 발견). 꺼져있으면(cam_speaker_set_enabled
 *        false) 즉시 리턴 — 평소 전력 동작에는 영향 없음.
 */
void cam_speaker_wait_idle(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
