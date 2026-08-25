#pragma once

/**
 * 채널 추적(Channel-Sync) 레이어 — CNTL↔노드(CAM/Sens) 통신 신뢰성의 최하부 기반(2026-08-04).
 *
 * 배경: hkhome 같은 실제 공유기는 도심 환경에서 CSA(Channel Switch Announcement)로 스스로
 * 채널을 바꿀 수 있다(실기에서 30분 벤치마크 도중 CH6->CH11 전환을 직접 관측함). Cntl은
 * WiFi STA 드라이버가 이 전환을 자동으로 따라가지만, 노드(CAM/Sens)는 Cntl에 실제로 접속한
 * 게 아니라 ESP-NOW로만 옆에 붙어있어서 이 전환을 알 방법이 전혀 없다 — 스스로 "지금도
 * 닿는지"를 계속 확인하는 수밖에 없다.
 *
 * 기존 keepalive(PAIR_ACK 재사용 + send_cb 물리계층 ACK 기반 실패 카운트)의 구조적 문제:
 *  1. "살아있음"과 "페어링됨"의 의미가 한 메시지(PAIR_ACK)에 섞여 있었음.
 *  2. 생존 판정 기준이 물리계층 ACK — 오늘 세션 초반에 이미 "이건 진짜 도달 확인이 아니다"라고
 *     확정했던 문제를 keepalive 쪽엔 반영 안 했었음.
 *  3. 상위 트래픽(사진 전송 중 플래그)이 하부 생존감지를 통째로 억제할 수 있었음(계층 위반) —
 *     30분 벤치마크 내내 채널 전환 감지가 잠들어 있었던 실제 원인.
 *
 * 이 컴포넌트는 그 세 가지를 원리적으로 다시 설계한다:
 *  - 생존 확인은 전용 메시지(CHANNEL_PING/PONG)만 근거로 삼는다 — 페어링/전송 상태와 완전히
 *    무관하게, 언제나 독립적으로 돈다.
 *  - 애플리케이션 레벨 왕복(PING 보내고 PONG 옴, 또는 esp_now_channelsync_notify_alive()로
 *    들어오는 다른 진짜 왕복)만 "진짜 도달"로 인정한다.
 *  - 이 루프는 어떤 상위 플래그로도 멈추거나 건너뛸 수 없다 — 그래서 API에 "일시정지" 같은
 *    건 아예 없음. (2026-08-05 — PING이 사진 청크 버스트와 같은 ESP-NOW 송신큐를 공유해서
 *    큐잉 지연으로 타임아웃되는 문제를 실기에서 발견: 사진 전송량이 많을수록 PING이 뒤로
 *    밀려 오탐이 남. 이 루프 자체를 멈추는 대신, 청크 전송이 만들어내는 DONE_ACK 같은
 *    "다른 진짜 왕복"도 생존 증거로 인정하도록 확장 — PING만이 유일한 증거였던 걸
 *    완화하되, "물리 ACK나 로컬 상태만으로 판단하지 않는다"는 원칙은 그대로 유지함 —
 *    notify_alive()로 들어오는 것도 반드시 실제 esp_now_reliable_request() 성공, 즉
 *    진짜 애플리케이션 레벨 왕복이어야 함)
 *
 * 상태: UNSYNCED(광고 브로드캐스트 + 채널 스캔 중) <-> SYNCED(채널 고정 + PING/PONG으로 생존
 * 확인 중). 페어링(PAIR_REQUEST/PAIR_ACK, 사용자 승인)은 이 레이어 위에서 별도로 이뤄지며,
 * on_lost_sync() 콜백을 받으면 호출부가 자신의 페어링 상태를 정리해야 한다(채널이 끊긴 채
 * "페어링됨"으로 남아있는 desync를 구조적으로 방지).
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_now.h"

#ifdef __cplusplus
extern "C" {
#endif

/* channel: 새로 고정된(또는 재동기화된) 채널. hub_mac: 이후 esp_now_send()의 목적지로 바로
 * 쓸 수 있음(피어 등록은 이 컴포넌트가 알아서 함) — 재동기화 때마다(채널이 바뀌었을 수 있음)
 * 매번 호출됨 */
typedef void (*esp_now_channelsync_on_synced_cb_t)(uint8_t channel, const uint8_t *hub_mac);

/* 연속 PING 무응답으로 동기화가 끊겼다고 판단한 순간 호출 — 호출부는 여기서 자신의 paired 등
 * 상위 상태를 정리해야 함(이 컴포넌트는 페어링 개념을 모름, 채널 동기화만 앎) */
typedef void (*esp_now_channelsync_on_lost_sync_cb_t)(void);

/* node_name/node_mac: ADVERTISE 브로드캐스트에 실어보낼 이 노드의 식별 정보(esp_now_link.h의
 * esp_now_advertise_t와 동일한 용도) */
void esp_now_channelsync_init(const char *node_name, const uint8_t *node_mac,
                               esp_now_channelsync_on_synced_cb_t on_synced,
                               esp_now_channelsync_on_lost_sync_cb_t on_lost_sync);

/* recv_cb(ESP-NOW 드라이버 태스크)에서 매 수신마다 호출 — ADVERTISE_ACK/CHANNEL_PONG만
 * 소비하고 그 외 타입은 조용히 무시하고 리턴하므로, 기존 dispatch 로직 맨 앞(또는 아무 데나)에
 * 한 줄만 추가하면 됨 */
void esp_now_channelsync_on_recv(const esp_now_recv_info_t *info, uint8_t msg_type,
                                  const uint8_t *data, int len);

bool esp_now_channelsync_is_synced(void);

/* PING 주기를 기본 500ms에서 바꿈(2026-08-08, 배터리 노드 절전 설정 연동용) — interval_ms=0이면
 * 기본값(500ms)으로 리셋. SYNCED 상태면 즉시 새 주기로 재시작, UNSYNCED면 다음 동기화 시
 * 이 값으로 시작함. PING_FAIL_THRESHOLD(3회)는 그대로라 "끊김 판정까지 걸리는 시간"도 이
 * 값에 비례해서 늘어남(의도된 동작 — 노드가 그만큼 뜸하게 확인하기로 한 것이므로) */
void esp_now_channelsync_set_ping_interval_ms(uint32_t interval_ms);

/* CHANNEL_PING/PONG이 아닌 다른 경로로 방금 hub와의 실제 애플리케이션 레벨 왕복에 성공했음을
 * 알림(2026-08-05) — 예: esp_now_reliable_request()가 DONE_ACK/LIST_DONE_ACK 등을 실제로
 * 받은 직후. PING과 청크 버스트가 같은 ESP-NOW 송신큐를 공유해서(ESP-IDF에 우선순위 분리
 * API 없음, 확인함) 전송량이 많을 때 PING이 큐에 밀려 오탐(허위 "동기화 끊김")이 나는 문제를
 * 완화함 — "PING만이 유일한 증거"였던 걸 "진짜 왕복이면 뭐든 증거"로 확장. 물리 ACK나
 * "바쁘니까 봐준다"류 로컬 상태는 여전히 증거로 안 씀(호출부가 진짜 성공한 왕복에 대해서만
 * 불러야 함) — SYNCED 상태가 아니면 조용히 무시 */
void esp_now_channelsync_notify_alive(void);

/* 2026-08-23(사용자 지시) — 실제 페어링(PAIR_REQUEST/PAIR_ACK)이 완료된 순간 호출. 그 전까지는
 * 채널 동기화(ADVERTISE_ACK)만으로는 PING/PONG을 시작하지 않음 — PING은 "이미 페어링된 연결의
 * 생존 확인" 용도인데, 페어링 전에 시작하면 아직 계속 돌고 있는 채널 스캔(scan_timer)이 라디오를
 * 다른 채널로 계속 옮겨버려서 PING이 구조적으로 매번 실패 -> 동기화 끊김 판정 -> 재스캔 ->
 * 재동기화 -> 다시 PING 실패의 무한루프에 빠짐(실기로 발견). 이 함수가 스캔을 멈추고(그제서야
 * 채널에 눌러앉음) PING을 시작함 */
void esp_now_channelsync_notify_paired(void);

/* 2026-08-23(사용자 지시) — "상태머신이면 상태에 따라 출력이 달라져야지": 지금까지는 광고
 * 전송이 scan_timer가 도는지에만 의존했음(간접적 — notify_paired가 타이머를 꺼주는 걸
 * 믿을 뿐, 광고 전송 코드 자체는 페어링 상태를 전혀 안 봄). 이 콜백을 등록하면
 * send_advertise_on_current_channel()이 실제로 보내기 직전에 물어봐서, false면 안 보냄 —
 * 타이머가 무슨 이유로든 다시 돌더라도 광고 자체가 상태에 따라 확실히 막힘(방어적 이중
 * 게이트). NULL이면(등록 안 하면) 항상 보냄 — 기존 동작 그대로(CNTL/Sens는 이 개념 자체가
 * 없으니 등록 안 함) */
typedef bool (*esp_now_channelsync_should_advertise_cb_t)(void);
void esp_now_channelsync_set_should_advertise_cb(esp_now_channelsync_should_advertise_cb_t cb);

/* 2026-08-23 — 부가 기능(스피커 소리 알림 등)용 선택적 이벤트 훅. 이 컴포넌트는 Common/
 * 공유라 CNTL/Sens처럼 카메라/스피커가 없는 프로젝트도 스캔하므로, cam_speaker 같은
 * 하드웨어 전용 컴포넌트를 직접 include/REQUIRES 하면 안 됨(실기로 CNTL 빌드가 깨지는 걸
 * 확인) — 대신 인자 없는 함수 포인터로 느슨하게 연결. 전부 NULL 허용(등록 안 하면 그냥
 * 아무 일도 안 함) */
typedef void (*esp_now_channelsync_event_cb_t)(void);

/* 2026-08-23 — on_channel_scanned: 채널을 옮길 때마다(광고 전송 직전) 호출 — on_advertise_sent와
 * 거의 같은 시점이지만 개념상 별개 이벤트로 분리(사용자 지시: "채널 스캔"과 "광고 전송"을
 * 소리로 구분해서 듣고 싶음).
 * 2026-08-25 — on_advertise_ack_received 추가: ADVERTISE_ACK를 실제로 받아 채널 동기화가
 * 확정된 순간(esp_now_channelsync_on_recv 내부, s_on_synced 직후) 호출 */
void esp_now_channelsync_set_event_hooks(esp_now_channelsync_event_cb_t on_channel_scanned,
                                          esp_now_channelsync_event_cb_t on_advertise_sent,
                                          esp_now_channelsync_event_cb_t on_advertise_ack_received,
                                          esp_now_channelsync_event_cb_t on_scan_sweep_done,
                                          esp_now_channelsync_event_cb_t on_ping_sent,
                                          esp_now_channelsync_event_cb_t on_pong_received);

/* 2026-08-24(사용자 지시: "실제 송출될 때만 소리가 나도록") — esp_now_send()의 동기 리턴값은
 * "로컬 송신큐에 접수됐다"는 뜻일 뿐, 무선으로 진짜 나갔다는 확인이 아니다(그건
 * esp_now_send_cb_t로 나중에 비동기로 옴). 이 컴포넌트는 전역 send_cb를 직접 등록하지
 * 않으므로(호출부가 이미 자신의 send_cb를 등록해 씀), 호출부가 자기 send_cb 안에서 목적지가
 * 브로드캐스트(FF:FF:FF:FF:FF:FF)이고 status==성공이면 이 함수를 불러줘야 함 — 그래야
 * "ADVERTISE 전송됨" 로그/소리가 진짜 송출 완료 시점에만 나감(광고는 이 컴포넌트에서 유일하게
 * 브로드캐스트로 나가는 메시지라 목적지만으로 구분 가능) */
void esp_now_channelsync_notify_advertise_send_done(void);

#ifdef __cplusplus
}
#endif
