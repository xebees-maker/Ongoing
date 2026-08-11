/**
 * @file    cam_node.c
 * @brief   CAM 헤드리스 노드 — 주기적으로 정지사진을 찍어 SD에 순환 저장하고,
 *          Cntl의 PHOTO_REQUEST에 맞춰 ESP-NOW로 청크 전송한다(esp_now_cam.c).
 *
 * 2026-07-22 돌파구: 웨이브셰어 공식 "06_usb_host_uvc" 예제(esp32-camera + USB UVC
 * 장치 모드)를 그대로 재현해서 PC에 웹캠으로 연결해보니 두 보드(OV5640/OV3660) 다
 * 영상이 깨끗하게 나왔다 — 카메라/센서/DVP 배선/드라이버 전부 정상이었다는 뜻. 그
 * 예제와 우리가 그동안 쓰던 설정의 핵심 차이가 XCLK(20MHz, 우리는 24MHz로 강제했었음)와
 * fb_count(2, 우리는 1) — 이 두 값을 그대로 가져와 콘솔 기반 단발 촬영 파이프라인에
 * 복원한다. project_cam_dvp_corruption_investigation 메모리 참고.
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_camera.h"

#include "bsp_esp32s3_cam.h"
#include "cam_storage.h"
#include "esp_now_cam.h"
#include "esp_now_channelsync.h"
#include "status_led.h"
#include "cam_node.h"
#include "dev_console.h"
#include "rwdt_guard.h"

static const char *TAG = "cam_node";

/* 2026-07-28: XCLK/fb_count는 센서마다 실기에서 확인된 값이 달라서(project_cam_esp_now_production
 * 메모리) main/Kconfig.projbuild의 CAM_SENSOR_VARIANT 선택("카메라 센서")으로 갈라진다 —
 * 소스는 하나, 센서 바꿀 때 이 Kconfig 값만 바꾸고 재빌드/재플래시하면 됨. OV5640은 XCLK
 * 24MHz에서 5MP까지 안정 확인, OV3660은 20MHz+fb_count=2로 QXGA까지 안정 확인(20MHz에서
 * fb_count=1로는 부팅 자체가 무응답이었던 사례가 있었으나 물리 USB 재연결 후 재현 안 됨 —
 * 완전히 갈라서 확인은 못 했음, 그래서 검증된 조합을 그대로 유지). */
#if CONFIG_CAM_SENSOR_OV3660
    #define CAM_VIDEO_XCLK_FREQ_HZ  20000000
    #define CAM_VIDEO_FB_COUNT      2
#else /* CAM_SENSOR_OV5640 */
    #define CAM_VIDEO_XCLK_FREQ_HZ  24000000
    #define CAM_VIDEO_FB_COUNT      1
#endif
#define CAM_JPEG_QUALITY        12
/* 부팅 시 노출/게인(AEC/AGC) 수렴용 워밍업 프레임 수(2026-08-10) — 예전엔 esp_camera_init()
 * 직후 무조건 vTaskDelay(2000ms)로 "어림잡아 2초 기다림"이었는데, 근거 주석이 전혀 없었고
 * (Espressif FAQ: "esp_camera_init() 안의 지연은 제거를 시도해볼 수 있다") 실제로 필요한 건
 * "시간"이 아니라 "센서가 몇 프레임을 실제로 캡처해서 AEC/AGC를 수렴시키는 것" — 매 촬영 전
 * 스테일 프레임 비우기(camera_capture_one() 참고)와는 목적이 다름(그건 fb_count개만 비워서
 * DMA 큐 신선도만 맞추는 것, 이건 노출 수렴). esp_camera_fb_get()이 진짜 새 프레임이 찍힐
 * 때까지 블로킹하므로, 정해진 시간을 기다리는 대신 실제 프레임 N장을 흘려보내는 쪽이 더
 * 정확하고(센서의 실제 프레임 주기에 맞춰짐) 보통 더 빠름. 값은 사용자 실무 경험 기준
 * (2026-08-10, "버리는 프레임은 최대 2개, 보통 1개") — 5장은 과했음이 실측으로 확인됨(가짜
 * "속도 개선 안 됨" 결과의 원인) */
#define CAM_WARMUP_FRAME_COUNT  2

#if CONFIG_CAM_JPEG_VGA
    #define CAM_FRAME_SIZE  FRAMESIZE_VGA
#elif CONFIG_CAM_JPEG_SVGA
    #define CAM_FRAME_SIZE  FRAMESIZE_SVGA
#elif CONFIG_CAM_JPEG_XGA
    #define CAM_FRAME_SIZE  FRAMESIZE_XGA
#elif CONFIG_CAM_JPEG_UXGA
    #define CAM_FRAME_SIZE  FRAMESIZE_UXGA
#elif CONFIG_CAM_JPEG_QXGA
    #define CAM_FRAME_SIZE  FRAMESIZE_QXGA
#elif CONFIG_CAM_JPEG_QSXGA
    #define CAM_FRAME_SIZE  FRAMESIZE_QSXGA
#else /* CAM_JPEG_5MP */
    #define CAM_FRAME_SIZE  FRAMESIZE_5MP
#endif

#if CONFIG_CAM_CAPTURE_10S
    #define CAM_CAPTURE_INTERVAL_MS  (10 * 1000)
#elif CONFIG_CAM_CAPTURE_30M
    #define CAM_CAPTURE_INTERVAL_MS  (30U * 60 * 1000)
#elif CONFIG_CAM_CAPTURE_1H
    #define CAM_CAPTURE_INTERVAL_MS  (60U * 60 * 1000)
#elif CONFIG_CAM_CAPTURE_3H
    #define CAM_CAPTURE_INTERVAL_MS  (3U * 60 * 60 * 1000)
#else /* CAM_CAPTURE_10H */
    #define CAM_CAPTURE_INTERVAL_MS  (10U * 60 * 60 * 1000)
#endif

/* 절전/응답성 설정(2026-08-08, 2차 설계) — CAM/SENS는 설정을 로컬에 저장하지 않음(사용자
 * 지시: "앞으로는 CNTL이 지능의 주체가 돼야 하니까, CNTL에 접속했을 때 CNTL에게 받은 값을
 * 기반으로 돌면 된다" — 로컬 저장 자체가 없다는 뜻). CAM은 그냥 부팅 시 Kconfig 기본값으로
 * 시작했다가, 페어링될 때마다 Cntl이 CAM_CONFIG_SET으로 보내주는 값으로 갱신만 함 — 재부팅
 * 되면 다시 Kconfig 기본값에서 시작하고 다음 페어링 때 Cntl이 다시 채워줌. 값을 실제로
 * 기억하는 주체는 Cntl(/assets/settings.bin) — esp_now_hub.c 참고.
 * (1차 설계였던 SD카드 저장은 두 가지 문제로 폐기: 1) 이 원칙과 안 맞음 2) SD 접근 자체가
 * ESP-NOW 동시활동과 겹치면 힙이 깨지는 걸 실기에서 발견함 — 아래 clamp는 그 안전장치로
 * 계속 남겨둠, 값의 출처가 뭐든 항상 유효함)
 * 2026-08-10 Deep Sleep 전환 — 이 값이 곧 딥슬립 사이클 길이(esp_now_hub.c 페어링 전까지는
 * 이 기본값으로 한두 사이클 돔).
 * 2026-08-11, 사용자 지시로 0("즉시"/Live, 딥슬립 자체를 안 함)으로 변경 — Cntl한테서
 * 아직 실제 설정을 못 받은 최초 부팅/페어링 대기 구간엔 어떤 고정 주기로 자다깨다
 * 하기보다 그냥 계속 깨서 기다리는 게 더 안전한 기본값(cam_node_set_response_interval_sec의
 * RWDT_LIVE_MODE_BUDGET_SEC 분기가 이 기본값에도 그대로 적용됨) */
#define CAM_RESPONSE_INTERVAL_SEC_DEFAULT 0

/* 2026-08-11 — 응답성 0("즉시"/Live, 딥슬립 자체를 안 함) 전용 RWDT 예산. 일반 공식
 * (response_interval + AWAKE_MARGIN_SEC)을 그대로 쓰면 0일 때 너무 짧아서(약 90초) 정상
 * 동작(계속 깨있음)인데도 주기적으로 강제리셋됨 — cam_node_set_response_interval_sec 참고.
 * rwdt_guard_arm()의 uint32_t 틱 오버플로우 한계(슬로우클럭 136kHz 기준 약 8.7시간)보다
 * 충분히 짧은 1시간으로 설정(사용자 확인) */
#define RWDT_LIVE_MODE_BUDGET_SEC (60 * 60)

static uint32_t s_capture_interval_sec  = 0;  /* app_main에서 Kconfig 기본값으로 초기화 */
static uint32_t s_response_interval_sec = CAM_RESPONSE_INTERVAL_SEC_DEFAULT;
static esp_timer_handle_t s_capture_timer = NULL;

/* 2026-08-08 실기에서 확인된 크래시 안전장치 — capture_interval_sec=10(드롭다운의 "10초")로
 * 설정하고 CAM이 마침 ESP-NOW 채널동기/페어링 활동 중일 때 자동촬영 타이머가 겹쳐 발동하면
 * SD 카드 read(enforce_capacity_and_get_next_seq -> scan_all_files)의 DMA 버퍼 할당 도중
 * 힙 자체가 깨지는 LoadProhibited 크래시를 재현/확인함(camera_capture_one/capture_timer_cb
 * 백트레이스로 확정). 게다가 이 값이 SD에 저장되므로 한 번 걸리면 재부팅마다 똑같이
 * 크래시하는 무한 부트루프가 됨 — 재현/원인규명은 했지만 SD와 ESP-NOW의 동시접근 자체를
 * 안전하게 만드는 근본수정은 아직 안 함(오늘 스코프 밖). 그때까지는 실기로 오래 검증된
 * 프로덕션 주기(30분+)만 허용 — 0(끔)은 항상 안전(타이머 자체가 안 돎). 30분보다 짧은
 * 값이 오면 30분으로 올림(거부 대신 클램프 — Cntl이 옛 버전이거나 설정파일이 이 안전장치
 * 이전 값을 들고 있어도 항상 안전측으로 수렴하게). */
#define CAM_CAPTURE_INTERVAL_MIN_SAFE_SEC 1800

static uint32_t clamp_capture_interval_sec(uint32_t sec)
{
    if (sec == 0) return 0;
    if (sec < CAM_CAPTURE_INTERVAL_MIN_SAFE_SEC) {
        ESP_LOGW(TAG, "촬영주기 %us는 실기에서 크래시 확인된 범위 — %us로 올림",
                 (unsigned)sec, (unsigned)CAM_CAPTURE_INTERVAL_MIN_SAFE_SEC);
        return CAM_CAPTURE_INTERVAL_MIN_SAFE_SEC;
    }
    return sec;
}

void cam_node_set_capture_interval_sec(uint32_t sec)
{
    sec = clamp_capture_interval_sec(sec);
    s_capture_interval_sec = sec;

    if (!s_capture_timer) return;  /* app_main이 아직 타이머를 안 만든 시점(설정 로드 단계) */
    esp_timer_stop(s_capture_timer);
    if (sec == 0) {
        cam_node_set_auto_capture(false);
        ESP_LOGI(TAG, "자동촬영 끔(주기=0)");
        return;
    }
    cam_node_set_auto_capture(true);
    esp_timer_start_periodic(s_capture_timer, (uint64_t)sec * 1000000ULL);
    ESP_LOGI(TAG, "자동촬영 주기 변경: %us", (unsigned)sec);
}

uint32_t cam_node_get_capture_interval_sec(void) { return s_capture_interval_sec; }

void cam_node_set_response_interval_sec(uint32_t sec)
{
    /* 2026-08-11 버그수정 — 예전엔 sec==0을 "설정 안 됨"으로 보고 조용히 기본값으로
     * 되돌렸는데(원래 0이 무의미한 값이던 시절 로직), 이제 0은 "즉시/Live"(딥슬립 자체를
     * 안 함)라는 진짜 의미가 있는 값이라 이 되돌림 때문에 CAM이 항상 기본값으로 동작하고
     * cam_node_wake_window_done()의 0 분기가 절대 안 걸렸음 — SLEEP_NOW도 안 오는데
     * 재요청 루프만 돌다가 RWDT 워치독에 강제 리셋되는 걸로 실기에서 발견됨. 부팅 시
     * 최초 호출(app_main)은 항상 이미 초기화된 CAM_RESPONSE_INTERVAL_SEC_DEFAULT를 넘기므로
     * 이 코드에 0이 들어오는 건 CNTL이 "즉시" 설정을 보낸 경우뿐 — 그대로 저장 */
    s_response_interval_sec = sec;
    /* 채널동기 PING 주기를 그대로 연동 — "응답성"이라는 하나의 설정값이 두 군데(CAM 자신의
     * PING 주기 + Cntl의 무응답 타임아웃 산정)에 그대로 씀(2026-08-08 설계 대화 참고) */
    esp_now_channelsync_set_ping_interval_ms(s_response_interval_sec * 1000);
    /* RWDT 재무장(2026-08-10) — 이 값이 이번 사이클의 실제 딥슬립 주기가 되므로, 워치독
     * 예산도 그 값 기준으로 다시 잡아야 함(부팅 직후엔 확정 전 추정치로 무장돼 있었음,
     * app_main 참고). 깨어있는 시간(페어링/설정수신/명령처리) 여유분으로 마진을 더함.
     * 2026-08-11 — 즉시/Live(0)는 "이번 사이클엔 원래 안 잔다"는 뜻이라 저 공식(0+90=90초)을
     * 그대로 쓰면 정상 동작(계속 깨있음)인데도 90초마다 RWDT가 계속 강제리셋시킴. 대신
     * 별도의 긴 예산(RWDT_LIVE_MODE_BUDGET_SEC)을 씀 — 진짜 무한대는 안 됨: rwdt_guard_arm()
     * 내부에서 초를 슬로우클럭(약 136kHz) 틱으로 변환해 uint32_t에 담는데, 약 8.7시간
     * (2^32/136000초) 넘으면 정수 오버플로우로 오히려 훨씬 짧고 예측 불가능한 값이 됨
     * (사용자 확인 후 결정 — 오버플로우 한계보다 충분히 짧은 1시간으로) */
    uint32_t rwdt_budget_sec = s_response_interval_sec == 0
        ? RWDT_LIVE_MODE_BUDGET_SEC
        : s_response_interval_sec + CONFIG_CAM_DEEPSLEEP_AWAKE_MARGIN_SEC;
    rwdt_guard_arm(rwdt_budget_sec);
    ESP_LOGI(TAG, "응답성 설정 변경: %us (PING 주기 연동, RWDT 재무장 %us)",
             (unsigned)s_response_interval_sec, (unsigned)rwdt_budget_sec);
}

uint32_t cam_node_get_response_interval_sec(void) { return s_response_interval_sec; }

/* 딥슬립 웨이크 원인(2026-08-10) — app_main 최상단에서 capture_wake_reason()이 1회 판정.
 * cam_wake_reason_t는 esp_now_link.h(공유 프로토콜 헤더, esp_now_cam.h를 통해 포함됨) 정의를
 * 그대로 씀 — Cntl에 보고할 때도 같은 값을 그대로 실어보내므로(esp_now_cam.c의
 * send_deep_sleep_stats) 별도 로컬 enum을 안 둠 */
static cam_wake_reason_t s_wake_reason = CAM_WAKE_REASON_OTHER;

/* 2026-08-10 — "직전에 실제로 잔 시간"을 딥슬립 경계 너머로 전달하는 용도. esp_deep_sleep_start()
 * 직전에 이번에 쓸 응답성 값을 여기 적어두면, 다음 부팅(=이번 사이클이 끝난 뒤) 때 그대로
 * 남아있음(RTC 슬로우메모리는 딥슬립 중에도 전원 유지). CAM은 완전 재부팅이라 "얼마나 잤는지"를
 * 직접 잴 방법이 없는데, 타이머 기반 딥슬립은 설정한 시간만큼 정확히 자므로(RWDT 개입만
 * 없었다면) 이 값을 그대로 "실제 잔 시간"으로 신뢰할 수 있음 — wake_reason이 TIMER일 때만
 * (아래 send_deep_sleep_stats에서 판단) */
static RTC_DATA_ATTR uint32_t s_last_actual_sleep_sec = 0;

uint32_t cam_node_get_last_actual_sleep_sec(void)
{
    return (s_wake_reason == CAM_WAKE_REASON_TIMER) ? s_last_actual_sleep_sec : 0;
}

static void capture_wake_reason(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_DEEPSLEEP) {
        uint32_t causes = esp_sleep_get_wakeup_causes();  /* v6 비-deprecated 복수형 */
        s_wake_reason = (causes & (1U << ESP_SLEEP_WAKEUP_TIMER))
                         ? CAM_WAKE_REASON_TIMER : CAM_WAKE_REASON_OTHER;
    } else if (rr == ESP_RST_WDT) {
        s_wake_reason = CAM_WAKE_REASON_RWDT;
    } else if (rr == ESP_RST_POWERON) {
        s_wake_reason = CAM_WAKE_REASON_POWERON;
    } else {
        s_wake_reason = CAM_WAKE_REASON_OTHER;
    }
    ESP_LOGI(TAG, "웨이크 원인 판정: reset_reason=%d -> wake_reason=%d", rr, s_wake_reason);
}

uint8_t cam_node_get_wake_reason(void) { return (uint8_t)s_wake_reason; }

/* "지금 자도 되는가" 판정에 쓰는 유휴 타임스탬프(2026-08-10) — recv_cb가 의미있는 메시지를
 * 처리할 때마다 cam_node_note_activity()로 갱신됨(esp_now_cam.c 참고) */
static uint32_t s_last_activity_ms = 0;

void cam_node_note_activity(void)
{
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/* 2026-08-11 재설계(사용자 지시: "이 모든 경우에 캠은 알아서 잘 수 없다고") — 예전엔
 * 페어링됐는데 SLEEP_NOW를 70초(CAM_DEEPSLEEP_IDLE_GRACE_MS, 이제 삭제됨) 못 받으면
 * 그냥 자율적으로 자버렸는데, 이러면 "SLEEP_NOW를 못 받는 상태"(Cntl 죽음/재부팅/설정에서
 * 연결 끊음 등)와 "SLEEP_NOW를 받고 정상적으로 자는 것"을 구분할 수 없게 됨. 대신 일정
 * 간격마다 SLEEP_NOW_REQUEST(재요청)를 Cntl에 계속 보내면서 깨있음 — 오직 진짜 SLEEP_NOW를
 * 받았을 때만(s_sleep_now_requested) 잠. Cntl이 정말 영영 안 돌아오면 RWDT 워치독
 * (rwdt_guard.c, response_interval_sec+AWAKE_MARGIN_SEC)이 최종 안전망 */
#define CAM_DEEPSLEEP_NUDGE_INTERVAL_MS   5000
/* 2026-08-11 재설계(사용자 지시) — 예전엔 15초 동안 계속 깨서 시도하다 못 찾으면
 * 포기하고 응답성 간격(예: 10초)만큼 통째로 자버렸는데, 이러면 타이밍이 안 좋을 때
 * 영원히 못 붙을 위험이 있고 그 15초 내내 전력도 씀. 대신 짧게(채널스캔+광고 한 번
 * 왕복할 정도) 시도하고, 못 찾으면 잠깐(3초, 2026-08-11 — 1초에서 상향. 사용자 지시:
 * USB-Serial-JTAG가 재부팅마다 재열거되는데 1초 간격이면 너무 자주 재부팅돼서 플래시
 * 도구가 안정적인 포트 접속 순간을 못 잡는 문제가 실사용 중 발견됨)만 자고 다시 시도 —
 * 재부팅마다 RTC_DATA_ATTR로 마지막 성공 채널을 기억해서 재시도가 빠르므로
 * (esp_now_channelsync) 이 주기로도 충분히 빠르게 재시도됨. app_main()이 이 타임아웃으로
 * 깰 때 페어링 여부를 직접 확인해서, 안 됐으면 CAM_DEEPSLEEP_RETRY_SLEEP_SEC만 잠 */
#define CAM_DEEPSLEEP_PAIR_WAIT_TIMEOUT_MS 3000
#define CAM_DEEPSLEEP_RETRY_SLEEP_SEC      3

/* Cntl이 ESP_NOW_MSG_SLEEP_NOW를 보내면 세팅(2026-08-10) — 남은 유휴여유를 기다리지 않고
 * cam_node_wake_window_done()이 즉시 true를 반환하게 함(단, 바쁜 동안은 여전히 안 재움) */
static volatile bool s_sleep_now_requested = false;

void cam_node_note_sleep_now_requested(void)
{
    s_sleep_now_requested = true;
}

/* 페어링 상태가 막 true가 된 순간을 감지해서 재요청 타이머를 그때부터 세기 시작함(2026-08-11)
 * — 안 그러면 s_last_nudge_ms가 0으로 시작해서 페어링되자마자 첫 재요청이 즉시 나가버림
 * (Cntl이 정상 처리할 시간도 안 준 채) */
static bool s_was_paired_this_wake = false;
static uint32_t s_last_nudge_ms = 0;

bool cam_node_wake_window_done(uint32_t awake_start_ms)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (!esp_now_cam_is_paired()) {
        s_was_paired_this_wake = false;
        return (now_ms - awake_start_ms) > CAM_DEEPSLEEP_PAIR_WAIT_TIMEOUT_MS;
    }
    if (!s_was_paired_this_wake) {
        s_was_paired_this_wake = true;
        s_last_nudge_ms = now_ms;
    }
    if (esp_now_cam_is_busy()) return false;
    if (s_sleep_now_requested) return true;
    /* 2026-08-11, 사용자 지시 — 응답성 0("즉시"/Live)이면 재우지도, 재촉(재요청)하지도
     * 않음. "즉시"는 짧은 주기로 반복 취침한다는 뜻이 아니라 딥슬립 자체를 안 하겠다는
     * 뜻(사용자: "1초=0초=즉시인거라 캠을 안재우겠다는 건데... 슬립요청도 안가고,
     * 슬립재요청도 안오는 상태여야하거든") */
    if (s_response_interval_sec == 0) return false;
    if (now_ms - s_last_nudge_ms >= CAM_DEEPSLEEP_NUDGE_INTERVAL_MS) {
        s_last_nudge_ms = now_ms;
        esp_now_cam_send_sleep_now_request();
    }
    return false;  /* SLEEP_NOW 없이는 절대 자율적으로 자지 않음(사용자 지시) */
}

static bool s_camera_ready = false;

static esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn     = BSP_CAM_SENSOR_PWDN_PIN,
        .pin_reset    = BSP_CAM_SENSOR_RESET_PIN,
        .pin_xclk     = BSP_CAM_DVP_XCLK,
        .pin_sccb_sda = -1,   /* 2026-07-28: SD 카드 인에이블(IO 익스팬더)이 카메라 SCCB와
                                  같은 물리 I2C 버스(GPIO7/8)를 공유해야 해서, 각자 새 마스터를
                                  만들면 충돌한다 — bsp_esp32s3_cam_init()이 만든 공유 버스를
                                  sccb_i2c_port로 재사용(take_picture.c 스타일 전용 GPIO 지정은
                                  콘솔 corruption 조사 때 SD를 꺼둔 채로만 쓰던 임시 설정이었음,
                                  실제 원인과는 무관했음 — project_cam_dvp_corruption_investigation
                                  메모리 참고). */
        .pin_sccb_scl = -1,
        .sccb_i2c_port = BSP_CAM_I2C_PORT,
        .pin_d0       = BSP_CAM_DVP_D0,
        .pin_d1       = BSP_CAM_DVP_D1,
        .pin_d2       = BSP_CAM_DVP_D2,
        .pin_d3       = BSP_CAM_DVP_D3,
        .pin_d4       = BSP_CAM_DVP_D4,
        .pin_d5       = BSP_CAM_DVP_D5,
        .pin_d6       = BSP_CAM_DVP_D6,
        .pin_d7       = BSP_CAM_DVP_D7,
        .pin_vsync    = BSP_CAM_DVP_VSYNC,
        .pin_href     = BSP_CAM_DVP_DE,   /* BSP 주석대로 DE==HREF, 같은 물리 핀(GPIO18) */
        .pin_pclk     = BSP_CAM_DVP_PCLK,
        .xclk_freq_hz = CAM_VIDEO_XCLK_FREQ_HZ,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = CAM_FRAME_SIZE,
        .jpeg_quality = CAM_JPEG_QUALITY,
        .fb_count     = CAM_VIDEO_FB_COUNT,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
    };

    ESP_RETURN_ON_ERROR(esp_camera_init(&config), TAG, "esp_camera_init 실패");

    sensor_t *s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, -2);
    }

    /* 2026-08-10 — SD에 저장해서 육안 확인하는 진단판은 main 태스크 스택오버플로우로
     * 되돌림(cam_storage_save_capture()가 이 지점의 기본 스택엔 너무 무거웠음) — 버리기만
     * 하는 원래 방식 유지, 화질 확인은 실제 촬영 경로(camera_capture_one())로 이미 충분히 됨 */
    int64_t warmup_start_us = esp_timer_get_time();
    for (int i = 0; i < CAM_WARMUP_FRAME_COUNT; i++) {
        camera_fb_t *warmup_fb = esp_camera_fb_get();
        if (warmup_fb) esp_camera_fb_return(warmup_fb);
    }
    ESP_LOGI(TAG, "노출 워밍업 %d프레임 소요: %lldms", CAM_WARMUP_FRAME_COUNT,
             (esp_timer_get_time() - warmup_start_us) / 1000);

    s_camera_ready = true;
    ESP_LOGI(TAG, "카메라 초기화 완료 (XCLK=%dMHz, fb_count=%d, 해상도 %d, JPEG q=%d)",
             CAM_VIDEO_XCLK_FREQ_HZ / 1000000, CAM_VIDEO_FB_COUNT, CAM_FRAME_SIZE, CAM_JPEG_QUALITY);
    return ESP_OK;
}

/* 필요시 초기화(2026-08-10) — 촬영이 실제로 필요해진 시점(수동/자동 둘 다)에만 호출.
 * 이미 이번 사이클에 한 번 초기화됐으면(s_camera_ready) 그대로 통과, 재초기화 안 함 */
static esp_err_t ensure_camera_ready(void)
{
    if (s_camera_ready) return ESP_OK;
    esp_err_t err = camera_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "카메라 초기화 실패(필요시 초기화)");
    return err;
}

/* 자동(타이머)/수동(shot) 캡처가 절대 동시에 안 돌게 직렬화 — esp_camera_fb_get()이 최대
 * ~4초 블로킹될 수 있어서, 자동촬영이 이미 호출을 시작한 직후에 수동 shot이 들어오면
 * "콘솔 사용 중엔 자동촬영 건너뛰기" 플래그로도 못 막고 로그가 섞이는 걸 실기에서 확인
 * (2026-07-21) — 이미 진행 중인 캡처가 있으면 새 요청은 그게 끝날 때까지 뮤텍스로 대기 */
static SemaphoreHandle_t s_capture_mutex = NULL;

static bool camera_capture_one(cam_capture_kind_t kind)
{
    if (ensure_camera_ready() != ESP_OK) return false;

    xSemaphoreTake(s_capture_mutex, portMAX_DELAY);

    /* DMA 프레임 버퍼 슬롯(fb_count개)은 esp_camera_fb_get()+fb_return()으로 소비해야만
     * ISR이 새로 채운다 — 촬영 사이 유휴 시간엔 아무도 안 비우므로 부팅 시점(또는 그
     * 이전 촬영 처리 중)에 찍힌 오래된 프레임이 큐에 그대로 멈춰있다가 나옴. fb_count=2면
     * 정확히 "요청 2번 전" 프레임이 나오는 게 실기로 확인됨(2026-08-05, 1/2/3/4 숫자
     * 화면으로 재현) — 진짜로 저장할 프레임을 받기 전에 큐를 fb_count개만큼 미리
     * 비워서 신선하게 만듦 */
    for (int i = 0; i < CAM_VIDEO_FB_COUNT; i++) {
        camera_fb_t *stale = esp_camera_fb_get();
        if (stale) esp_camera_fb_return(stale);
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGW(TAG, "esp_camera_fb_get 실패");
        xSemaphoreGive(s_capture_mutex);
        return false;
    }

    uint32_t file_id = 0;
    esp_err_t err = cam_storage_save_capture(fb->buf, fb->len, kind, &file_id);
    bool ok = (err == ESP_OK);
    if (ok) {
        ESP_LOGI(TAG, "SD에 캡처 저장 완료: id=%u, %u bytes", (unsigned)file_id, (unsigned)fb->len);
    } else {
        ESP_LOGW(TAG, "SD 저장 실패: %s", esp_err_to_name(err));
    }
    esp_camera_fb_return(fb);

    xSemaphoreGive(s_capture_mutex);
    return ok;
}

static bool s_auto_capture_enabled = false;  /* 기본 OFF — 콘솔에서 auto on으로 명시적으로 켜야 함 */

void cam_node_set_auto_capture(bool enable) { s_auto_capture_enabled = enable; }
bool cam_node_get_auto_capture(void) { return s_auto_capture_enabled; }

bool cam_node_set_jpeg_quality(int quality)
{
    if (quality < 0 || quality > 63) {
        ESP_LOGW(TAG, "JPEG 화질 범위 밖(0~63): %d", quality);
        return false;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || sensor->set_quality(sensor, quality) != 0) {
        ESP_LOGW(TAG, "JPEG 화질 변경 실패");
        return false;
    }
    ESP_LOGI(TAG, "JPEG 화질 변경: %d (다음 촬영부터 계속 적용됨)", quality);
    return true;
}

bool cam_node_set_xclk(int mhz)
{
    if (mhz < 1 || mhz > 40) {
        ESP_LOGW(TAG, "XCLK 범위 밖(1~40MHz): %d", mhz);
        return false;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || !sensor->set_xclk) {
        ESP_LOGW(TAG, "이 센서는 set_xclk 미지원");
        return false;
    }
    if (sensor->set_xclk(sensor, LEDC_TIMER_0, mhz) != 0) {
        ESP_LOGW(TAG, "XCLK 변경 실패");
        return false;
    }
    if (sensor->set_framesize(sensor, sensor->status.framesize) != 0) {
        ESP_LOGW(TAG, "XCLK 변경 후 PLL 재계산 실패");
        return false;
    }
    ESP_LOGI(TAG, "XCLK 변경: %dMHz (PLL 재계산 완료)", mhz);
    return true;
}

static void capture_timer_cb(void *arg)
{
    (void)arg;
    if (!s_auto_capture_enabled) {
        return;  /* 콘솔의 auto off 명령으로 꺼둔 상태 */
    }
    if (dev_console_auto_capture_paused()) {
        return;  /* 콘솔 사용 중 — ls로 본 파일이 순환삭제로 사라지지 않게 이번 주기 건너뜀 */
    }
    if (!camera_capture_one(CAM_CAPTURE_KIND_AUTO)) {
        ESP_LOGW(TAG, "이번 캡처 실패 — 다음 주기에 재시도");
    }
}

bool cam_node_capture_now(void)
{
    return cam_node_capture_now_sized(NULL);
}

bool cam_node_capture_now_sized(const char *size_name)
{
    /* 2026-08-10 — 필요시 초기화: 해상도 오버라이드(size_name)는 esp_camera_sensor_get()으로
     * 센서 핸들이 필요해서 camera_capture_one() 진입 전에 여기서 먼저 준비돼 있어야 함 */
    if (ensure_camera_ready() != ESP_OK) {
        ESP_LOGW(TAG, "수동 촬영 요청 — 카메라 초기화 실패");
        return false;
    }

    if (size_name && size_name[0] != '\0') {
        framesize_t fs;
        if (strcasecmp(size_name, "5m") == 0) {
            fs = FRAMESIZE_5MP;
        } else if (strcasecmp(size_name, "qvga") == 0) {
            fs = FRAMESIZE_QVGA;
        } else if (strcasecmp(size_name, "vga") == 0) {
            fs = FRAMESIZE_VGA;
        } else {
            ESP_LOGW(TAG, "알 수 없는 해상도 이름: %s (5m/qvga/vga만 지원)", size_name);
            return false;
        }

        sensor_t *sensor = esp_camera_sensor_get();
        if (!sensor || sensor->set_framesize(sensor, fs) != 0) {
            ESP_LOGW(TAG, "해상도 변경 실패");
            return false;
        }
        ESP_LOGI(TAG, "해상도 변경: %s (다음 촬영부터 계속 적용됨)", size_name);
    }

    return camera_capture_one(CAM_CAPTURE_KIND_MANUAL);
}

void app_main(void)
{
    /* Deep Sleep 주기적 웨이크로 전환(2026-08-10) — Light Sleep 3차 시도(2026-08-09)까지
     * 실측한 결과 light_sleep_count가 순수 전원 상태에서도 0에 머물러 실제로 전혀 진입하지
     * 않는 것으로 확인됐고, 근본 원인을 못 찾아 8/8에 이미 합의된 폴백으로 전환함
     * (project_cntl_rtc_and_unified_sleep_plan 메모리 참고). 이번 방식은 CPU 주파수/모뎀슬립
     * 튜닝이 아니라 "할 일 다 하면 명시적으로 esp_deep_sleep_start()를 부른다"는 완전히
     * 다른 제어흐름 — 매 사이클 전체가 재부팅이라 상태를 하나도 안 들고 감(응답성/촬영주기도
     * 로컬 저장 없이 매번 Kconfig 기본값에서 시작, 페어링되면 Cntl이 다시 채워줌).
     * RWDT(rwdt_guard.c)가 이번 사이클 전체를 지키는 안전망 — 소프트웨어가 무슨 이유로든
     * esp_deep_sleep_start()에 못 이르면 강제로 리셋시킴. */

    capture_wake_reason();
    rwdt_guard_arm(CAM_RESPONSE_INTERVAL_SEC_DEFAULT + CONFIG_CAM_DEEPSLEEP_AWAKE_MARGIN_SEC);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_capture_mutex = xSemaphoreCreateMutex();

    /* 2026-07-28: 격리 테스트 종료, 정상 경로 복원(project_cam_dvp_corruption_investigation
     * 메모리 참고 — corruption의 실제 원인은 콘솔 전송 레이어였고 SD/WiFi/공유 I2C 버스는
     * 전부 무관했음이 확인됨) */
    ESP_ERROR_CHECK(bsp_esp32s3_cam_init());

    if (cam_storage_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD 초기화 실패 — 계속 재시도하지 않고 그대로 진행");
    }

    /* Kconfig 기본값으로 시작 — 로컬 저장 없음(위 설정 관련 주석 참고), 페어링되면 Cntl이
     * CAM_CONFIG_SET으로 실제 값을 채워줌. clamp_capture_interval_sec는 개발용 Kconfig
     * 값(CAM_CAPTURE_10S)까지도 안전측으로 걸러줌 — 크래시 안전장치 주석 참고 */
    s_capture_interval_sec = clamp_capture_interval_sec(CAM_CAPTURE_INTERVAL_MS / 1000);

    /* 2026-08-10 — 카메라는 여기서 무조건 초기화하지 않음(필요시 초기화로 전환, 사용자 지시).
     * CAM이 깨는 이유는 대부분 "명령을 받기 위해서"이지 촬영이 아님 — 실제로 촬영이 필요한
     * 순간(수동 CAPTURE_NOW 또는 자동촬영 주기 도달)에만 ensure_camera_ready()가 호출됨
     * (camera_capture_one() 참고). 즉 이번 사이클에 촬영 요청이 하나도 없으면 카메라 하드웨어
     * 초기화 비용(워밍업 포함) 자체가 한 번도 발생하지 않고 그대로 다시 잠듦 */

    /* Cntl과는 ESP-NOW로만 붙음 — 로컬 HTTP 대시보드도, AP도 안 씀(Cntl도 STA,
     * esp_now_hub.c:171). AP 모드였을 때는 100ms마다 비콘을 계속 내보내야 했는데,
     * 지금촬영 중 esp_camera_fb_get()의 긴 블로킹과 겹치면서 WiFi 스택 내부
     * (ieee80211_hostap_send_beacon_process, AP 전용 코드)가 실기에서 크래시하는 걸
     * 확인함(2026-08-01, Guru Meditation LoadProhibited) — max_connection=0으로 닫아도
     * 재현되어 원인이 "외부 접속 시도"가 아니라 비콘 송신 코드 자체였음이 드러남.
     * STA는 비콘을 안 보내므로 이 크래시 코드 경로가 아예 없음 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* 절전 1단계 — 모뎀 슬립(2026-08-08). Light Sleep(위)과 별개로 WiFi 라디오 자체도
     * 필요할 때만 켜지게 함. CAM은 AP에 join 안 해서 DTIM 동기화 대상이 없지만, 드라이버가
     * ESP-NOW 수신에 맞춰 알아서 깨는지는 실기로 확인 필요(2026-08-08 설계 대화에서 짚은
     * 미검증 지점) — 문제가 보이면 이 줄만 빼면 원복됨 */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "WiFi 모뎀슬립 설정: %s", esp_err_to_name(ps_err));

    esp_now_cam_set_status_led(GPIO_NUM_NC);  /* TODO: 실기 LED GPIO 확정되면 채우기 */
    esp_now_cam_init();
    /* esp_now_cam_init() 안에서 esp_now_channelsync_init()이 불려야 피어/타이머가 만들어짐 —
     * 그 다음에 저장된(또는 기본) 응답성 설정을 반영(RWDT도 여기서 실제값으로 재무장됨) */
    cam_node_set_response_interval_sec(s_response_interval_sec);

    const esp_timer_create_args_t capture_args = { .callback = capture_timer_cb, .name = "cam_capture" };
    ESP_ERROR_CHECK(esp_timer_create(&capture_args, &s_capture_timer));
    if (s_capture_interval_sec > 0) {
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_capture_timer, (uint64_t)s_capture_interval_sec * 1000000ULL));
        cam_node_set_auto_capture(true);
    }

    ESP_LOGI(TAG, "CAM 노드 시작 (%s, 촬영주기=%us 응답성=%us, wake_reason=%d)", esp_now_cam_get_name(),
             (unsigned)s_capture_interval_sec, (unsigned)s_response_interval_sec, s_wake_reason);

#if CONFIG_CAM_DEEPSLEEP_ENABLE
    dev_console_start();  /* 유지 — 딥슬립 사이클마다 USB 재열거됨(개발 시 감안) */

    uint32_t awake_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    while (!cam_node_wake_window_done(awake_start_ms)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* 2026-08-11 재설계(사용자 지시) — 페어링 못 한 채로 깨는 거면(Cntl을 못 찾음) 응답성
     * 간격을 통째로 자지 않고 짧게(CAM_DEEPSLEEP_RETRY_SLEEP_SEC)만 자고 곧바로 다시 시도 —
     * CAM_DEEPSLEEP_PAIR_WAIT_TIMEOUT_MS 주석 참고. 페어링됐으면(정상 경로) 기존대로
     * 응답성 간격만큼 잠 */
    bool paired_now = esp_now_cam_is_paired();
    uint32_t sleep_sec = paired_now ? s_response_interval_sec : CAM_DEEPSLEEP_RETRY_SLEEP_SEC;
    ESP_LOGI(TAG, "딥슬립 진입: %us 후 웨이크 (paired=%d)", (unsigned)sleep_sec, (int)paired_now);
    s_last_actual_sleep_sec = sleep_sec;  /* 다음 부팅 때 "직전에 실제로 잔 시간"으로 보고 */
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_sec * 1000000ULL);
    esp_deep_sleep_start();  /* RWDT는 이미 무장돼있음, 안 건드림 */
#else
    dev_console_start();  /* 딥슬립 완전 비활성 — 상시 동작(벤치/콘솔 개발용, Kconfig 이스케이프 해치) */
#endif
}
