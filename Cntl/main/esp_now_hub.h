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
    uint32_t        last_seen_ms;
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

/* 통신 시도 전 항상 이걸로 실제 페어링 여부를 확인(2026-08-04, 사용자 지시) — 화면의
 * "연결됨" 표시나 로컬 상태만 믿지 말고 매번 이 함수로 재확인. esp_now_photo.c의 모든
 * 요청 함수가 이 가드를 거쳐가게 되어 있음(esp_now_photo_capture_now 등 참고). */
bool esp_now_hub_is_paired(const uint8_t *mac);

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
void esp_now_hub_apply_response_interval_sec(uint32_t sec);

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
