#pragma once

/**
 * ESP-NOW 허브 — CAM/SENS 노드의 advertise 브로드캐스트 수신, 발견된 노드 테이블 유지,
 * 페어링(PAIR_REQUEST/ACK) 처리. 구 Cntl의 esp_now_hub.c에서 페어링/노드테이블 부분만
 * 이식 — 웹 대시보드/사진 전송은 아직 포함 안 함(다음 단계).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_now_link.h"

#define ESP_NOW_HUB_MAX_NODES 8
/* 2026-08-04: CAM keepalive를 1s->10s로 늘려서(esp_now_cam.c, 채널 혼잡 튜닝) 5회 누락
 * 기준을 유지하려면 50s가 맞음. Sens는 아직 1s 주기 그대로라 목록에서 실제로 사라지는
 * 반응이 그만큼 느려지는 부작용 있음(끊긴 뒤 최대 50s간 목록엔 남아있음) — CAM 튜닝
 * 결과 보고 필요하면 노드 종류별로 분리 예정 */
#define ESP_NOW_HUB_NODE_TIMEOUT_MS 50000U

/* CAM/SENS는 ADVERTISE 메시지에 장치 종류를 안 실어보내서(name/mac만 있음), 이름 접두사로
 * 구분함 — CAM은 기본 이름이 "Cam-XXXX"(esp_now_cam.c), SENS는 "Sens-XXXX"(esp_now_node.c).
 * 사용자가 CONFIG_*_NODE_NAME으로 이름을 커스텀하면 이 구분이 깨질 수 있음(알려진 한계). */
typedef enum {
    HUB_NODE_KIND_UNKNOWN = 0,
    HUB_NODE_KIND_CAM,
    HUB_NODE_KIND_SENS,
} hub_node_kind_t;

typedef struct {
    char            name[ESP_NOW_LINK_NAME_LEN];
    uint8_t         mac[6];
    hub_node_kind_t kind;
    bool            paired;
    /* CAM/SENS는 살아있음을 알리려고 PAIR_ACK를 주기적으로 재전송함(keepalive) — 사용자가
     * 명시적으로 연결 해제한 뒤에도 이 keepalive가 도착하면 다시 paired=true가 될 수 있어서,
     * 이 플래그로 막음(esp_now_hub_pair()가 다시 호출되기 전까진 PAIR_ACK를 무시) */
    bool            user_unpaired;
    /* CAM Deep Sleep 자동 재페어링용(2026-08-10) — CAM은 매 웨이크마다 완전 재부팅되어
     * ADVERTISE부터 다시 보내므로, "방금 막 페어링이 풀린 순간"(paired: true->false 전환)
     * 에만 재연결을 시도하면 그 시도 자체가 실패했을 때(무선 유실 등, 실기에서 실제로 발생
     * 확인됨) 이후 어떤 ADVERTISE가 와도 다시 시도할 계기가 없어 영구히 "연결 대기 중"에
     * 멈춰버림. ever_paired는 이번 Cntl 부팅 세션에서 이 노드가 단 한 번이라도 페어링에
     * 성공했으면 계속 true로 남아, 그 뒤로 오는 모든 ADVERTISE마다(성공했든 방금 실패했든
     * 상관없이) 매번 재연결을 다시 시도하게 함 — 최초 승인은 여전히 사용자가 한 번 해야
     * 하지만, 그 뒤로는 실패해도 다음 사이클(~20초 뒤)에 자동으로 다시 시도됨 */
    bool            ever_paired;
    /* ever_paired가 true로 세팅될 때마다(=페어링 성공할 때마다) 같이 갱신(2026-08-10) —
     * esp_now_hub_is_reconnect_stuck()이 "마지막으로 페어링 성공한 지 얼마나 됐는가"를
     * 판단하는 기준 */
    uint32_t        last_paired_ms;
    /* 이번 페어링에서 이미 SLEEP_NOW를 보냈는가(2026-08-10) — PAIR_ACK 수신 시 false로
     * 리셋, try_send_sleep_now()가 보낸 뒤 true로 세팅. 없으면 CAM이 실제로는 첫 신호
     * 받고 바로 잠들었어도(확인 응답이 없는 fire-and-forget이라 Cntl은 모름) 다음
     * ADVERTISE 올 때까지 계속 허공에 재전송하게 됨(실사용 중 발견, 무해하지만 낭비) */
    bool            sleep_now_sent;
    /* 2026-08-11 — 이번 페어링 사이클에서 CAM_CONFIG_ACK를 받았는가. PAIR_ACK 수신 시 false로
     * 리셋, CAM_CONFIG_ACK 수신 시 true. SLEEP_NOW 전송 조건 중 하나(적응형 반응시간 경과 +
     * 이 값이 true) — 예전엔 "페어링 후 1초 지났으면 설정 핸드셰이크가 끝났겠지"라고
     * 추측(ADAPTIVE_SLEEP_PAIR_SETTLE_MS 고정값)했는데, CAM_CONFIG_SET이 이미 reliable
     * stack(ACK+재시도)이라 실제 완료 이벤트를 그대로 쓸 수 있어서 추측을 없앰 */
    bool            config_acked_this_cycle;
    /* 2026-08-11 — CAM이 SLEEP_NOW_REQUEST(재요청)를 보낼 때마다 증가, PAIR_ACK 수신 시
     * 0으로 리셋. 임계치(SLEEP_NOW_REQUEST_ERROR_THRESHOLD) 넘으면 워닝 대신 에러코드로
     * 격상(recv_cb의 SLEEP_NOW_REQUEST 핸들러 참고) */
    uint32_t        sleep_now_request_count;
    uint32_t        last_seen_ms;
    /* Deep Sleep 사이클 통계(2026-08-10, ESP_NOW_MSG_DEEP_SLEEP_STATS) — 보낼 수 있는
     * 노드(CAM)만 채워짐, has_deepsleep_stats=false면 아직 한 번도 못 받음(구버전 노드이거나
     * 막 페어링됨). ds_cycle_count/ds_rwdt_catch_count는 Cntl이 리포트를 받을 때마다 직접
     * 누적(CAM은 딥슬립마다 완전 재부팅이라 자기 사이클 수를 기억 못 함) */
    bool            has_deepsleep_stats;
    uint32_t        ds_cycle_count;
    /* 직전에 실제로 잔 시간(초) — 누적 아님, 매 리포트마다 그대로 덮어씀(2026-08-10, 사용자
     * 지시). CAM이 RTC_DATA_ATTR로 딥슬립 경계 너머 전달한 값(cam_node.c 참고) — 예전엔
     * "앞으로 잘 예정 시간"을 누적해서 아직 안 잔 걸 잔 걸로 잘못 보여주는 버그가 있었음 */
    uint32_t        ds_last_actual_sleep_sec;
    uint32_t        ds_rwdt_catch_count;
    uint8_t         ds_last_wake_reason;
    uint32_t        ds_last_awake_uptime_ms;
    uint32_t        ds_last_sleep_interval_sec;
    /* 2026-08-10 — try_send_sleep_now()가 SLEEP_NOW를 보낼 때 판단 근거를 여기 남겨둠 —
     * 통계탭 판넬에 자체 줄로(사용자 지시, -mm:ss와 함께) 표시하려는 것. sleep_now_send_count는
     * ds_cycle_count와 같은 방식의 단조증가 세대번호 — refresh_power_panel()이 이게 바뀔 때만
     * 새 줄을 추가(안 그러면 안 바뀐 값을 매 틱마다 다시 찍게 됨) */
    uint32_t        last_sleep_now_elapsed_ms;
    uint32_t        last_sleep_now_threshold_ms;
    uint32_t        sleep_now_send_count;
} esp_now_hub_node_t;

void esp_now_hub_init(void);

/* 지금 Cntl이 실제로 붙어있는 WiFi 채널 — ESP-NOW도 이 채널을 그대로 씀(같은 라디오).
 * 공유기 자동채널선택으로 세션 중간에 바뀔 수 있어서(2026-08-02 실기에서 확인) 화면에
 * 상시 표시하는 용도로 추가 */
uint8_t esp_now_hub_get_wifi_channel(void);

/* kind로 필터링해서 살아있는(timeout 이내) 노드만 out에 채워서 개수 반환 —
 * kind=HUB_NODE_KIND_UNKNOWN이면 전체(필터 없음) */
int esp_now_hub_get_nodes(hub_node_kind_t kind, esp_now_hub_node_t *out, int max);

/* 리스트 아이템 탭 시 사용 — mac은 esp_now_hub_get_nodes()로 얻은 노드의 mac[6] */
void esp_now_hub_pair(const uint8_t *mac);
void esp_now_hub_unpair(const uint8_t *mac);

/* 2026-08-10, CAM Deep Sleep 전환 — 딥슬립 사이클마다 순간적으로 언페어 상태를 스치는 건
 * 정상 동작(자동 재연결이 다음 사이클 안에 알아서 다시 붙임)이지 에러가 아님. 이 함수는
 * "정말로 문제가 있는 상태"(자동 재연결이 기대 시간 안에 회복을 못 하고 있음)만 true를
 * 반환 — esp_now_hub_get_conn_state()의 WAITING 판정에 씀. 기준 시간은
 * esp_now_hub_node_timeout_ms()와 동일 — 이미 "이 정도 무응답이면 정상 범위를 넘었다"는
 * 판단 기준으로 쓰이고 있는 값 재사용 */
bool esp_now_hub_is_reconnect_stuck(const uint8_t *mac);

/* ESP-NOW는 connectionless라 "연결/연결끊김"이라는 개념 자체가 없음(2026-08-10, 사용자
 * 정정) — 실제로는 세 상태뿐:
 *  WAITING: 이번 세션에 한 번도 페어링된 적 없거나, 자동 재연결이 정상 범위를 넘겨 실패 중
 *  PAIRED:  한 번이라도 페어링됨(ever_paired) — 앱 레벨로는 계속 "아는 기기". CAM이 딥슬립
 *           사이의 무선 무응답 구간에 있어도 이 상태는 안 바뀜(정상 동작이라서)
 *  ACTIVE:  PAIRED 중에서도 "지금 이 순간" 라디오 레벨로 막 페어 확인이 된 상태(paired==true)
 * UI는 WAITING/PAIRED/ACTIVE 중 PAIRED와 ACTIVE를 대부분 동일하게 취급(목록/판넬 유지),
 * 요약판넬의 상태 문구에서만 PAIRED vs ACTIVE를 구분해서 보여줌(ui_main.c 참고) */
typedef enum {
    HUB_CONN_STATE_WAITING = 0,
    HUB_CONN_STATE_PAIRED,
    HUB_CONN_STATE_ACTIVE,
} hub_conn_state_t;

hub_conn_state_t esp_now_hub_get_conn_state(const uint8_t *mac);

/* 적응형 반응시간(2026-08-10) — esp_now_photo.c의 5개 사용자 액션 함수(지금촬영/목록갱신/
 * 삭제/전체삭제/사진선택-fetch)가 호출. 마지막 호출 시각으로부터
 * device_config_get_adaptive_response_sec() 이상 조용하면 esp_now_hub 내부 타이머가 현재
 * 페어링된 CAM에 SLEEP_NOW를 보냄 */
void esp_now_hub_note_user_action(void);

/* 2026-08-08 재설계 — CAM/SENS 원격 설정은 이제 device_config.h가 값의 주인(영구저장),
 * 여기 두 함수는 "값을 바꾸고 + 지금 페어링된 대상에게 즉시 반영"까지 한 번에 처리하는
 * Apply 동작 전용. 둘 다 esp_now_tx 큐잉 패턴이라 즉시 리턴(UI 안 얼어붙음) — 실제 결과는
 * CAM_CONFIG_ACK 수신으로 옴(esp_now_tx가 재시도까지 처리, 실패해도 로그/토스트만).
 *
 * 촬영주기: 카메라별 설정이지만 지금은 CAM이 보통 1대라 mac 하나만 대상으로 함(여러 대로
 * 늘면 이 함수 자체를 mac별 반복호출 구조로 바꾸면 됨, device_config는 이미 값 하나뿐이라
 * 그때 같이 확장 필요).
 * 응답성: 시스템 전체 공통 설정이라 현재 페어링된 모든 CAM에 한 번에 반영. */
void esp_now_hub_apply_cam_capture_interval_sec(const uint8_t *mac, uint32_t sec);
/* 반환값(2026-08-10) — 지금 라디오레벨로 살아있는(paired==true) CAM이 하나라도 있어서 실제로
 * push_cam_config_to()를 시도했으면 true. false면 대상이 하나도 없어 아무 것도 안 보냈다는
 * 뜻 — 호출부(ui_main.c)가 이 경우 응답 대기 팝업을 띄우지 않고 "저장만 됨" 안내로 대체함
 * (그래도 값 자체는 항상 device_config에 저장됨 — 다음 페어링 때 자동 재반영) */
bool esp_now_hub_apply_response_interval_sec(uint32_t sec);

/* 설정탭 Apply 버튼 진행팝업용 상태 폴링(2026-08-08) — esp_now_photo_get_capture_stage()와
 * 같은 패턴. IDLE=아직 아무 것도 안 보냄, SENT=push_cam_config_to() 호출됨(esp_now_tx가
 * 재시도 중), ACKED=CAM_CONFIG_ACK 수신 확인. clear()는 팝업 닫을 때 호출해서 다음 Apply를
 * 위해 IDLE로 되돌림. */
typedef enum {
    HUB_CONFIG_APPLY_IDLE = 0,
    HUB_CONFIG_APPLY_SENT,
    HUB_CONFIG_APPLY_ACKED,
} hub_config_apply_stage_t;

hub_config_apply_stage_t esp_now_hub_get_config_apply_stage(void);
void esp_now_hub_config_apply_stage_clear(void);

/* 노드별 실제 무응답 타임아웃(ms) — device_config의 시스템 응답성 설정값 배수(여유마진).
 * esp_now_hub_get_nodes()/is_paired()가 내부적으로 이걸 씀 — UI가 "몇 초 뒤에 끊김으로
 * 판단하는지" 표시하고 싶을 때도 그대로 재사용 가능하게 공개 */
uint32_t esp_now_hub_node_timeout_ms(void);

/* 처리량 벤치마크 트리거(2026-08-04, 임시 개발용) — 페어링된 CAM 중 첫 번째에게
 * duration_sec 동안 벤치마크를 시작하라고 요청. 결과는 양쪽 시리얼 로그로만
 * 확인(esp_now_cam.c/esp_now_hub.c의 BENCH 로그 참고), UI에는 안 보여줌.
 * mode(2026-08-05 추가, esp_now_bench_mode_t): 0=기존 순수 채널 처리량(BENCH_BLAST),
 * 1=현재 사진전송 방식 반복(실제 프로토콜 오버헤드 포함), 2=Selective Repeat 반복 —
 * esp_now_link.h의 esp_now_bench_mode_t 주석 참고. */
void esp_now_hub_bench_start(uint16_t duration_sec, uint8_t mode);
