#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "driver/gpio.h"
#include "esp_now_link.h"

/* 수동(M)/자동(T) 촬영 구분 — META의 kind로 CNTL에 전달돼 CNTL이 파일명 접두사를 정함.
 * 2026-09-26 — SD 제거로 삭제된 cam_storage.h에서 옮겨옴(값은 그대로) */
typedef enum {
    CAM_CAPTURE_KIND_MANUAL = 'M',
    CAM_CAPTURE_KIND_AUTO   = 'T',
} cam_capture_kind_t;

/**
 * CAM의 ESP-NOW 리프 노드 로직 — Sens/main/esp_now_node.c와 광고/채널스캔/페어링 부분은
 * 거의 동일(같은 세션에서 막 바꾼 로직이라 실기 검증 전이라 공유 컴포넌트로 뽑지 않고
 * 각자 사본으로 둠 — 나중에 둘 다 검증되면 Common/components/esp_now_peer로 통합 고려).
 * 페어링 후에는 Sens의 SENSOR_DATA 주기 전송 대신, Cntl의 PHOTO_REQUEST에 응답해서
 * SD에 저장된 사진을 청크로 잘라 보낸다.
 */

void esp_now_cam_init(void);
const char *esp_now_cam_get_name(void);
bool esp_now_cam_is_paired(void);

/** @brief 사진전송/목록조회/삭제 큐가 대기 중이거나 처리 중인가 — Deep Sleep 진입 전 대기
 *         윈도우 판정(cam_node.c)에 씀 */
bool esp_now_cam_is_busy(void);

/** @brief 2026-08-25(CASK 재설계) — 알려진 CNTL에 광고 없이 곧장 유니캐스트로 재연결 시도.
 *         성공하면 true(PAIRED 상태까지 전환됨), 실패하면 채널스캔 폴백을 알아서 시작(또는
 *         이미 돌고 있으면 재개)하고 false 리턴 — cam_node.c의 웨이크 루프가 침묵
 *         타임아웃으로 재시도할 때도 이 함수 하나만 다시 부르면 됨(esp_now_cam_init()도
 *         내부적으로 이 함수를 씀) */
bool esp_now_cam_reconnect(void);

/** @brief 상태 LED 키 등록(esp_now_cam_init() 이전에 호출). 키는 status_led로 이미 초기화돼
 *  있어야 함 — CAM은 BSP_CAM_PWR_LED_STATUS_ID(IO 익스팬더 EXIO6, status_led_init_custom) */
void esp_now_cam_set_status_led(gpio_num_t pin);

/**
 * @brief 2026-09-18(SD 제거 재설계) — 방금 촬영한 프레임을 CNTL로 즉시 푸시(SD 저장을
 *        대체). cam_node.c의 camera_capture_one()이 fb->buf/fb->len을 그대로 넘겨 호출 —
 *        반드시 실제 스택이 있는 태스크에서만 호출할 것(전송 완료까지 블로킹될 수 있음,
 *        esp_timer 콜백 금지). 페어링 안 돼있으면 즉시 false. file_id는 내부에서 세션 로컬
 *        카운터로 자동 부여(재부팅마다 리셋 — CNTL이 실제 영구 파일명/순번을 소유하므로
 *        무관함).
 * @param buf  JPEG 바이트(카메라 드라이버의 PSRAM 프레임버퍼를 그대로 가리킴)
 * @param len  JPEG 바이트 수
 * @param kind CAM_CAPTURE_KIND_MANUAL/AUTO — CNTL이 파일명(M/T 접두사)을 정하는 데 씀
 * @return 전송(META~DONE_ACK) 성공 여부
 */
bool esp_now_cam_push_captured_photo(const uint8_t *buf, size_t len, cam_capture_kind_t kind);

/**
 * @brief 2026-09-18(SD 제거 재설계) — 주기촬영 타이머 콜백(cam_node.c capture_timer_cb,
 *        작은 스택의 esp_timer 태스크 컨텍스트)에서 호출. 무거운 작업(촬영+전송)은 직접 하지
 *        않고 photo_transfer_task(24KB 스택의 전용 태스크)로 큐잉만 함 — 큐가 가득 차 있으면
 *        (직전 촬영 처리 중) 조용히 버림, 다음 주기가 재시도함 */
/* 2026-09-19(주기촬영 재설계) — 큐잉이 실제로 됐는지(false면 큐가 꽉 차서 이번엔 건너뜀)
 * 반환. 성공하면 이 시점에 이미 s_transfer_busy=true로 표시돼 있어서(photo_transfer_task가
 * 큐에서 실제로 꺼내기 전이라도), esp_now_cam_is_transfer_busy()를 곧바로 폴링해도 "아직 안
 * 바쁨"으로 오판해 잠들어버리는 레이스가 없음 */
bool esp_now_cam_enqueue_auto_capture(void);
/* 2026-09-19 — cam_node.c의 CASK 루프가 "방금 큐잉한 촬영이 다 끝났는지" 기다릴 때 씀.
 * mark_transfer_idle()이 매번 cam_node_signal_recheck()도 같이 불러주므로, 호출부는
 * s_wake_recheck_sem을 기다리다 깨면 이 값을 다시 확인하는 폴링 루프로 쓰면 됨 */
bool esp_now_cam_is_transfer_busy(void);
