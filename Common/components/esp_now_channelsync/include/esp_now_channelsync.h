#pragma once

/**
 * 채널 추적(Channel-Sync) 레이어 — CNTL↔노드(CAM/Sens) 채널 발견의 최하부 기반(2026-08-04,
 * 2026-08-25 CASK 재설계로 범위 축소).
 *
 * 배경: hkhome 같은 실제 공유기는 도심 환경에서 CSA(Channel Switch Announcement)로 스스로
 * 채널을 바꿀 수 있다(실기에서 30분 벤치마크 도중 CH6->CH11 전환을 직접 관측함). Cntl은
 * WiFi STA 드라이버가 이 전환을 자동으로 따라가지만, 노드(CAM/Sens)는 Cntl에 실제로 접속한
 * 게 아니라 ESP-NOW로만 옆에 붙어있어서 이 전환을 알 방법이 전혀 없다 — 그래서 모르는(또는
 * 잃어버린) CNTL을 찾아야 할 때 브로드캐스트 ADVERTISE로 채널을 스윕하며 다시 발견한다.
 *
 * 2026-08-25 — 이 컴포넌트는 원래 CHANNEL_PING/PONG 상시 하트비트로 "이미 페어링된 연결이
 * 계속 살아있는지"까지 감시했는데(2026-08-04/05 설계, CAM Deep Sleep 전환보다도 먼저 만들어짐),
 * 그 부분은 완전히 제거됐다. 이유: CAM은 딥슬립 사이클마다 완전 재부팅되는 구조라 "세션 도중
 * 계속 감시"할 필요가 애초에 옅고, 대신 매 웨이크의 WAKE_HELLO(esp_now_cam.c 참고)가 이미
 * reliable 요청/응답이라 그 자체로 생존 확인을 겸한다 — project_cam_cntl_ping_pong_redesign_proposal
 * 메모리 참고. 이제 이 컴포넌트는 순수하게 "모르는 CNTL을 채널 스윕으로 찾아서 채널에
 * 고정시키는 것"까지만 책임진다.
 *
 * 상태: UNSYNCED(광고 브로드캐스트 + 채널 스캔 중) <-> SYNCED(채널 고정됨). 페어링
 * (PAIR_REQUEST/PAIR_ACK, 사용자 승인)은 이 레이어 위에서 별도로 이뤄진다. 2026-08-25 —
 * 예전엔 여기서 "동기화가 끊겼다"를 판단해 on_lost_sync() 콜백으로 알렸는데(PING-fail/
 * boot_id 불일치 기준), 그 판단 기준 자체가 핑퐁과 함께 없어짐 — 지금은 애초에 "페어링된
 * 연결이 계속 살아있는가"를 이 레이어가 감시하지 않으므로 alert할 것도 없음. 페어링 이후의
 * 생존 확인은 전부 상위(esp_now_cam.c의 WAKE_HELLO/CASK 흐름)의 몫이다.
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

/* node_name/node_mac: ADVERTISE 브로드캐스트에 실어보낼 이 노드의 식별 정보(esp_now_link.h의
 * esp_now_advertise_t와 동일한 용도) */
void esp_now_channelsync_init(const char *node_name, const uint8_t *node_mac,
                               esp_now_channelsync_on_synced_cb_t on_synced);

/* recv_cb(ESP-NOW 드라이버 태스크)에서 매 수신마다 호출 — ADVERTISE_ACK만 소비하고 그 외
 * 타입은 조용히 무시하고 리턴하므로, 기존 dispatch 로직 맨 앞(또는 아무 데나)에 한 줄만
 * 추가하면 됨 */
void esp_now_channelsync_on_recv(const esp_now_recv_info_t *info, uint8_t msg_type,
                                  const uint8_t *data, int len);

bool esp_now_channelsync_is_synced(void);

/* 2026-08-25 — 이미 init()된 상태에서 스캔을 다시 시작(재시도 등). 노드 이름/mac/콜백은
 * 그대로 유지, 스캔만 재개(내부적으로 init()의 마지막 단계와 동일) */
void esp_now_channelsync_resume_scan(void);

/* 2026-08-23(사용자 지시) — 실제 페어링(PAIR_REQUEST/PAIR_ACK)이 완료된 순간 호출. 채널
 * 동기화(ADVERTISE_ACK)만으로는 스캔을 안 멈춤(다른 CNTL도 찾을 기회를 주려고) — 이 함수가
 * 그제서야 스캔을 멈추고 채널에 눌러앉힘. 2026-08-25 — PING 시작 역할은 CASK 재설계로
 * 없어짐(핑퐁 자체가 제거됨), 이제 순수하게 "스캔 정지"만 함 */
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
 * 확정된 순간(esp_now_channelsync_on_recv 내부, s_on_synced 직후) 호출. 같은 날 CASK
 * 재설계로 on_ping_sent/on_pong_received는 제거(핑퐁 자체가 없어짐) */
void esp_now_channelsync_set_event_hooks(esp_now_channelsync_event_cb_t on_channel_scanned,
                                          esp_now_channelsync_event_cb_t on_advertise_sent,
                                          esp_now_channelsync_event_cb_t on_advertise_ack_received,
                                          esp_now_channelsync_event_cb_t on_scan_sweep_done);

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
