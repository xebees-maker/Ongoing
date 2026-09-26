#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 2026-09-26 — Cntl의 Wi-Fi 네트워크(STA 연결/재연결, "찾기" 실시간 접속 시도, SoftAP 모드,
 * 웹 대시보드 URL용 IP). node_hub(구 esp_now_hub)에서 분리 — 노드 관리/ESP-NOW와 무관 */

/* esp_netif_init()+esp_event_loop_create_default()+Wi-Fi 시작까지. node_hub_init()보다 먼저,
 * 그리고 Wi-Fi/IP 이벤트 핸들러 등록(main.c)보다 먼저 불러야 함 */
void wifi_sta_init(void);

/* 지금 Cntl이 실제로 붙어있는 WiFi 채널 — ESP-NOW도 이 채널을 그대로 씀(같은 라디오).
 * 공유기 자동채널선택으로 세션 중간에 바뀔 수 있어서(2026-08-02 실기에서 확인) 화면에
 * 상시 표시하는 용도로 추가 */
uint8_t wifi_sta_get_channel(void);

/* 2026-08-30 — 부팅 후(STA 모드) 25초간 저장된 AP를 한 번도 못 찾았을 때 true. 로고부제
 * 자리(refresh_clock)가 이걸 보고 시계 대신 "AP 없음" 표시. "찾기"로 수동 연결 성공하면
 * 자동으로 풀림(wifi_sta.c의 GOT_IP 핸들러 참고) */
bool wifi_sta_boot_giveup(void);

/* 2026-08-21 — CNTL 자신의 STA IP 문자열("192.168.0.17" 형식) — 상황판 요약의 웹 대시보드
 * URL 표시용. 아직 IP를 못 받았으면 빈 문자열("") 반환 */
const char *wifi_sta_get_own_ip_str(void);

/* 2026-08-29 — STA 모드에서 실제 접속에 쓰인 SSID(저장된 값 있으면 그것, 없으면 폴백 기본값).
 * AP 모드일 땐 빈 문자열. "네트워크" 설정 행의 우측 표시용 */
const char *wifi_sta_get_active_ssid(void);
const char *wifi_sta_get_ap_ssid(void);

/* 2026-08-29(사용자 지시: "AP 찾고 선택하고 접속하는 과정은 재시작 안 함") — 재시작 없이
 * 실시간으로 STA 자격증명을 시도해보고 성공/실패를 비동기 콜백으로 통보. 실패하면 원래
 * 접속해있던 네트워크로 자동 복귀(다음 재시도 루프가 이어감). 시도 도중 채널이 잠깐
 * 흔들려도 ESP-NOW 피어(CAM/Sens)는 esp_now_channelsync로 스스로 다시 찾아오므로 CNTL
 * 쪽에서 별도 조치 불필요(project_rs485... 아님, Common/components/esp_now_channelsync
 * 참고 — 실제 공유기 CSA 채널전환 때도 이미 검증된 동작) */
typedef void (*wifi_sta_test_cb_t)(bool success, void *ctx);

/* 2026-08-29(사용자 지시: "토스트로 뭐하는지 단계마다 나오게") — 접속 시도 중간 단계
 * 변경 알림. UI(ui_main.c)가 lv_async_call()로 감싸서 토스트로 보여줌 */
typedef enum {
    STA_TEST_STAGE_DISCONNECTING,  /* 기존 연결/시도 정리 중 */
    STA_TEST_STAGE_CONNECTING,     /* 실제 대상으로 인증/접속 시도 중 */
} wifi_sta_test_stage_t;
typedef void (*wifi_sta_test_stage_cb_t)(wifi_sta_test_stage_t stage, void *ctx);

void wifi_sta_test_connect(const char *ssid, const char *password,
                                   wifi_sta_test_cb_t on_result,
                                   wifi_sta_test_stage_cb_t on_stage, void *ctx);

/* 2026-08-29 버그수정(사용자 리포트: "찾기 팝업에서 아무 것도 안하고 검색된 네트워크
 * 없습니다") — esp_wifi_scan_start()는 STA가 연결 시도 중이면 ESP_ERR_WIFI_STATE로
 * 실패한다. wifi_event_handler의 STA_DISCONNECTED 재연결 루프가 끊김마다 바로
 * esp_wifi_connect()를 다시 걸어서 "조용한 틈"이 거의 없었음 — 스캔 팝업이 열려있는
 * 동안 이 재연결 루프를 잠깐 멈춰서 스캔이 되게 함 */
void wifi_sta_set_reconnect_paused(bool paused);
