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
#include <time.h>

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
#include "cam_speaker.h"
#include "io_expander_ch32v003.h"
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
/* 2026-08-26 수정(사용자 실기 관찰: "I는 초기 10초인데, 10초 잠들면서는 0초로 보고하네") —
 * 예전엔 일반 static이라 매 재부팅마다 CAM_RESPONSE_INTERVAL_SEC_DEFAULT(0)로 리셋됐음.
 * WAKE_HELLO는 이번 사이클의 CONFIG를 받기 "전"에 보내지므로(esp_now_cam.c 참고), 재부팅
 * 직후 첫 WAKE_HELLO는 늘 이 초기값을 그대로 보고함 — 순서버그가 고쳐져서 캠이 진짜로 매
 * 사이클 재부팅하기 시작하자 "설정은 10인데 보고는 0"이 매번 반복되는 게 실기에서 드러남.
 * RTC 메모리로 옮겨서 딥슬립 재부팅 경계 넘어 유지 — 처음 페어링 전(진짜 최초 부팅, RTC도
 * 아직 0)엔 여전히 0(Live, 안전한 기본값)이고, 한 번이라도 CONFIG를 받은 뒤로는 마지막
 * 적용값을 그대로 들고 다음 부팅까지 이어감 */
static RTC_DATA_ATTR uint32_t s_response_interval_sec = CAM_RESPONSE_INTERVAL_SEC_DEFAULT;
static esp_timer_handle_t s_capture_timer = NULL;

/* 2026-08-21 — 세로줄(컬럼 고정패턴노이즈) 진단용. 센서 전원인가 기본값이 곧 "켬"(자동)이라
 * 그대로 초기값도 true — 소프트웨어가 명시적으로 끈 적 없는 지금 상태와 일치시킴 */
static bool s_agc_enable = true;
static bool s_aec_enable = true;

/* 2026-08-21 — XCLK도 화질/노이즈 진단용 프리셋으로 원격 조정. 기본값은 기존 컴파일타임
 * 상수(CAM_VIDEO_XCLK_FREQ_HZ, 센서별로 다름)와 일치시켜서 아무 설정도 안 왔을 때 지금까지
 * 동작과 같게 함. camera_init() 아래에서 이 값을 씀 — cam_node_set_xclk_target_mhz() 참고 */
static uint8_t s_xclk_target_mhz = (uint8_t)(CAM_VIDEO_XCLK_FREQ_HZ / 1000000);

void cam_node_set_agc_enable(bool enable) { s_agc_enable = enable; }
void cam_node_set_aec_enable(bool enable) { s_aec_enable = enable; }

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
     * 응답성=0("즉시") 분기가 절대 안 걸렸음 — RWDT 워치독에 강제 리셋되는 걸로 실기에서
     * 발견됨. 부팅 시
     * 최초 호출(app_main)은 항상 이미 초기화된 CAM_RESPONSE_INTERVAL_SEC_DEFAULT를 넘기므로
     * 이 코드에 0이 들어오는 건 CNTL이 "즉시" 설정을 보낸 경우뿐 — 그대로 저장 */
    s_response_interval_sec = sec;
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
    ESP_LOGI(TAG, "응답성 설정 변경: %us (RWDT 재무장 %us)",
             (unsigned)s_response_interval_sec, (unsigned)rwdt_budget_sec);
}

uint32_t cam_node_get_response_interval_sec(void) { return s_response_interval_sec; }

/* 딥슬립 웨이크 원인(2026-08-10) — app_main 최상단에서 capture_wake_reason()이 1회 판정.
 * cam_wake_reason_t는 esp_now_link.h(공유 프로토콜 헤더, esp_now_cam.h를 통해 포함됨) 정의를
 * 그대로 씀 — Cntl에 보고할 때도 같은 값을 그대로 실어보내므로(esp_now_cam.c의
 * send_deep_sleep_stats) 별도 로컬 enum을 안 둠 */
static cam_wake_reason_t s_wake_reason = CAM_WAKE_REASON_OTHER;

/* 2026-08-10 도입 -> 2026-08-26 수정(사용자 지시: "SLEPT는 그저 받은 SLEEP 시간 값을 보내고
 * 있어, 실제로 잔 시간을 측정했어야 해") — 예전엔 esp_deep_sleep_start() 직전에 CNTL이
 * 명령한 sleep_sec을 그대로 여기 적어뒀다가 다음 부팅 때 "실제로 잔 시간"이라며 그대로
 * 보고했음. 이건 측정이 아니라 명령값 에코라, 혹시 딥슬립이 명령과 다르게 동작하는 버그가
 * 생겨도 로그가 절대 그걸 드러낼 수 없는 구조였음(자기 입력을 그대로 돌려주니 항상
 * "일치"로만 보임). 이제 진짜 벽시계 시간차를 잰다 — 잠들기 직전에 현재 unix time을
 * s_sleep_entry_unix_time에 남겨두고(RTC 슬로우메모리라 딥슬립 중에도 유지, RTC 클럭
 * 도메인 자체도 딥슬립 동안 계속 흐름), 다음 부팅 때 그 시각과 지금 시각의 차이를 계산 */
static RTC_DATA_ATTR uint32_t s_last_actual_sleep_sec  = 0;
static RTC_DATA_ATTR time_t   s_sleep_entry_unix_time  = 0;

/* 2026-08-26 수정(사용자 지적: "안 자고도 이 전력 로그를 보내게 되는 경우는?") — 이 값은
 * 부팅 시작 시점에 딱 한 번만 계산되는데(위 capture_wake_reason() 참고), 같은 부팅 안에서
 * WAKE_HELLO를 여러 번 보내면(재시도 루프 등) 실제로 안 잤는데도 "그 부팅 전 마지막으로
 * 잔 시간"을 매번 그대로 반복 보고하게 됨 — CNTL 전력 로그에 안 잔 사이클도 SL이 찍히는
 * 버그. AW(누적치 버그)와 같은 종류라, 같은 해법: 한 번 보고하면 소비(consume)하고 지움 —
 * 다음 WAKE_HELLO부턴 진짜 새로 잔 게 없으면 0(호출부가 딱 하나뿐이라 안전, esp_now_cam.c
 * 참고) */
uint32_t cam_node_get_last_actual_sleep_sec(void)
{
    if (s_wake_reason != CAM_WAKE_REASON_TIMER) return 0;
    uint32_t v = s_last_actual_sleep_sec;
    s_last_actual_sleep_sec = 0;
    return v;
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

    /* 실측 — 타이머 웨이크일 때만 의미 있음(POWERON/RWDT는 어차피 getter가 0 반환).
     * s_sleep_entry_unix_time==0이면 이번이 첫 사이클(재플래시 등으로 RTC가 막 초기화됨)
     * 이라 비교 기준이 없으므로 건너뜀 */
    if (s_wake_reason == CAM_WAKE_REASON_TIMER && s_sleep_entry_unix_time != 0) {
        time_t now = time(NULL);
        s_last_actual_sleep_sec = (now > s_sleep_entry_unix_time)
            ? (uint32_t)(now - s_sleep_entry_unix_time) : 0;
    }
}

uint8_t cam_node_get_wake_reason(void) { return (uint8_t)s_wake_reason; }

/* 2026-08-10 도입 -> 2026-08-26 삭제 — CASK_SILENCE_TIMEOUT_MS(위 참고)가 없어지면서
 * s_last_activity_ms를 읽던 유일한 곳도 같이 없어짐. recv_cb 쪽 호출부(esp_now_cam.c)도
 * 전부 같이 제거함 */

/* 2026-08-23(사용자 설계) — 미페어링 중 깨어있는 시간을 고정 예산(3초)으로 두지 않고,
 * "채널 스윕(1~13) 한 바퀴를 실제로 끝낼 때까지"로 바꿈 — 예전 3초 예산은 300ms×13=3.9초
 * 걸리는 스윕 하나도 못 끝내서, 매번 채널 1~10 근방만 반복하고 그 이후 채널(11~13)은
 * 구조적으로 영원히 스캔 대상이 안 되는 버그가 있었음(실사용 중 발견). 스윕 완료는
 * esp_now_channelsync의 on_scan_sweep_done 훅(cam_node.c의 spk_on_scan_sweep_done 참고)으로
 * 감지. 못 찾으면 짧게(3초)만 자고 다시 시도 — RTC_DATA_ATTR로 마지막 성공 채널을
 * 기억해서(esp_now_channelsync) 재시도가 빠름 */
#define CAM_DEEPSLEEP_RETRY_SLEEP_SEC      ESP_NOW_NODE_UNPAIRED_RETRY_SEC

/* 2026-08-25 도입 -> 2026-08-26 삭제 -> 같은 날 재도입(사용자 지시로 실기 검증 후 원칙 정정) —
 * 한 번 없앴다가, 실기에서 정확히 이 자리의 부재로 캠이 영원히 멈추는 걸 확인함(SLEEP_NOW가
 * 한 번 유실되면 다음 WAKE_HELLO를 보낼 계기 자체가 없어서, "다음 WAKE_HELLO가 재시도+폴백을
 * 해줄 것"이라던 원래 삭제 근거가 성립을 못 함). 사용자가 정정한 원칙: WAKE_HELLO 성공 여부
 * "판정"은 WAKE_HELLO_ACK 하나만으로 끝나는 게 아니라, CASK 전체(CONFIG->할일->SLEEP_NOW)가
 * 다 와야 비로소 성립한다 — 즉 이건 새 타이머 개념이 아니라, WAKE_HELLO의 100ms×3 재시도
 * 판정 범위를 CASK 전체로 넓힌 것. 값은 CNTL이 한 사이클에 순차로 보낼 수 있는 전부의 합
 * (2026-08-26 esp_now_tx.c의 effective_max_attempts 시간기반 부풀리기도 같은 날 삭제되어
 * 이제 각 메시지가 호출부가 넘긴 고정 횟수만 씀) — CONFIG 800ms×3=2.4s + 할일 중 가장 큰
 * PHOTO_LIST_REQUEST 3000ms×3=9s + SLEEP_NOW 300ms×3=0.9s = 최소 12.3s, 여유를 더해 15초 */
#define CASK_TIMEOUT_MS                    15000

/* Live 모드(SLEEP_NOW.sleep_sec==0)에서 재체크인 페이싱 기준(사용자 지정 x=1초) */
#define CASK_LIVE_PACE_MS                  1000

/* 2026-08-23 — 미페어링 재시도 백오프(딥슬립 경계 넘어 유지, s_last_synced_channel과 같은
 * RTC_DATA_ATTR 패턴). CNTL을 못 찾는 상태가 길어질수록 재시도 간격을 늘려서 배터리 소모와
 * 끝없는 광고음을 줄임 — esp_now_channelsync.c의 라이트슬립용 스윕 백오프와 같은 문턱값(1분/
 * 11분, 10초/30초)을 재사용해 두 절전 정책이 일관되게 함. 페어링에 성공하면 0으로 리셋 —
 * 다음에 다시 못 찾게 되면 처음부터(3초)로 다시 시작 */
static RTC_DATA_ATTR uint32_t s_unpaired_backoff_elapsed_sec = 0;

#define UNPAIRED_BACKOFF_SHORT_UNTIL_SEC   60          /* 1분까지: 짧게(3초) */
#define UNPAIRED_BACKOFF_MID_SEC           10          /* 1~11분: 10초 */
#define UNPAIRED_BACKOFF_MID_UNTIL_SEC     (60 + 600)
#define UNPAIRED_BACKOFF_LONG_SEC          30          /* 11분 이후: 30초 */

static uint32_t next_unpaired_retry_sleep_sec(void)
{
    if (s_unpaired_backoff_elapsed_sec < UNPAIRED_BACKOFF_SHORT_UNTIL_SEC) return CAM_DEEPSLEEP_RETRY_SLEEP_SEC;
    if (s_unpaired_backoff_elapsed_sec < UNPAIRED_BACKOFF_MID_UNTIL_SEC) return UNPAIRED_BACKOFF_MID_SEC;
    return UNPAIRED_BACKOFF_LONG_SEC;
}

/* Cntl의 ESP_NOW_MSG_SLEEP_NOW 수신 시 세팅 — CASK의 마지막 단계(app_main의 웨이크 루프
 * 참고). sleep_sec은 메시지가 실어온 값 그대로 보관(0=안 자고 곧장 재연결) */
static volatile bool     s_sleep_now_requested = false;
static volatile uint32_t s_sleep_sec_from_cntl = 0;

/* 2026-08-23 — 이벤트드리븐 재확인 신호(CAML에서 검증 후 이식, cam_node.h 참고). 바이너리
 * 세마포어: 이벤트 발생 지점(핸들러)들이 Give만 하고, app_main의 대기 루프는 Take로
 * 기다리다가 즉시 깨서 재판정 — 고정주기 폴링 대신 이벤트 기반 */
static SemaphoreHandle_t s_wake_recheck_sem = NULL;

void cam_node_signal_recheck(void)
{
    if (s_wake_recheck_sem) xSemaphoreGive(s_wake_recheck_sem);
}

void cam_node_note_sleep_now_requested(uint32_t sleep_sec)
{
    s_sleep_sec_from_cntl  = sleep_sec;
    s_sleep_now_requested  = true;
    cam_node_signal_recheck();
}

/* 2026-08-26(사용자 지시) — 순서 버그 수정: 예전엔 이 리셋이 WAKE_HELLO/PAIR_ACK를 "보낸
 * 후"(app_main 루프 맨 위)에 일어났음. CNTL은 ACK를 받자마자 곧바로 CONFIG+할일+SLEEP_NOW를
 * 연달아 보내는데, 그게 무선으로는 몇 ms 안에 CAM에 도착해서 recv_cb(별도 태스크)가 먼저
 * s_sleep_now_requested=true로 세워놔도, 메인 태스크가 뒤늦게 이 함수를 불러 그걸 다시
 * false로 덮어써버림 — 방금 도착한 진짜 SLEEP_NOW가 조용히 사라짐(실기로 확인, "캠이 안
 * 잔다" 버그의 원인). 이제 이 함수를 WAKE_HELLO/PAIR_ACK를 "보내기 직전"에 불러서, 응답이
 * 도착하기 전에 이미 깨끗한 상태를 보장함 — 늦게 도착한 응답이 지워질 시점 자체가 없어짐 */
void cam_node_reset_sleep_now_state(void)
{
    s_sleep_now_requested = false;
    s_sleep_sec_from_cntl = 0;
}

/* 2026-08-23 — 미페어링 중 깨어있는 시간을 "스윕 한 바퀴 완료"로 게이트하는 데 씀(app_main
 * 참고) — spk_on_scan_sweep_done()이 세팅. 2026-08-25 — 리셋 호출부가 (삭제된)
 * on_channel_lost_sync()에서 esp_now_cam_reconnect()의 폴백 분기로 옮겨짐(역할은 동일) */
static volatile bool s_sweep_completed = false;

void cam_node_note_scan_restarted(void)
{
    s_sweep_completed = false;
}

static bool s_camera_ready = false;

/* save_warmup_frames: 워밍업 프레임을 버리지 않고 kind 'A'+i로 SD에 저장(2026-08-21, 적정
 * CAM_WARMUP_FRAME_COUNT 값을 실기 화질로 판단하기 위한 임시 진단 — cam_storage.c의
 * s_valid_kinds 주석 참고). **수동 촬영 경로에서만 true로 넘김** — 2026-08-10에 같은 걸
 * 무조건 켜뒀다가 main 태스크 스택오버플로우로 되돌린 전례가 있어서(그땐 camera_init()이
 * app_main에서 무조건 호출되던 시절), 지금은 필요시 초기화라 그 경로 자체는 없어졌지만
 * 혹시 모를 작은 스택(esp_timer 태스크, 자동촬영 경로)에서까지 SD 쓰기를 태우지 않으려고
 * 여전히 수동(대용량 스택의 photo_tx 태스크/콘솔)에서만 켬 — camera_capture_one() 참고 */
static esp_err_t camera_init(bool save_warmup_frames)
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
        .xclk_freq_hz = (int)s_xclk_target_mhz * 1000000,  /* 2026-08-21 — 컴파일타임 상수 대신
                                                               원격 설정값(기본은 그 상수와 동일) */
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

    int64_t warmup_start_us = esp_timer_get_time();
    for (int i = 0; i < CAM_WARMUP_FRAME_COUNT; i++) {
        camera_fb_t *warmup_fb = esp_camera_fb_get();
        if (warmup_fb) {
            if (save_warmup_frames && i < 26) {
                uint32_t warmup_file_id = 0;
                esp_err_t werr = cam_storage_save_capture(warmup_fb->buf, warmup_fb->len,
                                                            (cam_capture_kind_t)('A' + i), &warmup_file_id);
                ESP_LOGI(TAG, "워밍업 프레임 %d(kind=%c) 저장: %s", i, (char)('A' + i), esp_err_to_name(werr));
            }
            esp_camera_fb_return(warmup_fb);
        }
    }
    ESP_LOGI(TAG, "노출 워밍업 %d프레임 소요: %lldms", CAM_WARMUP_FRAME_COUNT,
             (esp_timer_get_time() - warmup_start_us) / 1000);

    s_camera_ready = true;
    ESP_LOGI(TAG, "카메라 초기화 완료 (XCLK=%dMHz, fb_count=%d, 해상도 %d, JPEG q=%d)",
             (int)s_xclk_target_mhz, CAM_VIDEO_FB_COUNT, CAM_FRAME_SIZE, CAM_JPEG_QUALITY);
    return ESP_OK;
}

/* 필요시 초기화(2026-08-10) — 촬영이 실제로 필요해진 시점(수동/자동 둘 다)에만 호출.
 * 이미 이번 사이클에 한 번 초기화됐으면(s_camera_ready) 그대로 통과, 재초기화 안 함 */
static esp_err_t ensure_camera_ready(bool save_warmup_frames)
{
    if (s_camera_ready) return ESP_OK;
    esp_err_t err = camera_init(save_warmup_frames);
    if (err != ESP_OK) ESP_LOGE(TAG, "카메라 초기화 실패(필요시 초기화)");
    return err;
}

bool cam_node_is_camera_ready(void) { return s_camera_ready; }

bool cam_node_ensure_camera_ready(void)
{
    return ensure_camera_ready(true) == ESP_OK;
}

/* 2026-08-22 — 배터리 전압(mV) 환산 상수. 스키매틱(ESP32-S3-CAM-XXXX-schematic.pdf) 확인값:
 * VBAT --R39(200K)-- BAT_ADC노드 --R42(100K)-- GND, 분배비 = R42/(R39+R42) = 1/3 이므로
 * Vbat = Vadc * 3. CH32V003 자체 ADC 비트폭/기준전압은 Waveshare 공식 문서/예제 어디에도
 * 없어서(cam_node.h 주석 참고) 10bit/3.3V로 추정 — 실측 대조 전까지는 근사치임 */
#define CH32V003_ADC_MAX_COUNT   1023.0f  /* 10bit 추정 */
#define CH32V003_ADC_VREF_MV     3300.0f  /* 자체 VDD 추정 */
#define CAM_BAT_DIVIDER_RATIO    3.0f     /* (R39+R42)/R42 = 300K/100K */

bool cam_node_read_battery_mv(uint16_t *out_raw, uint16_t *out_mv)
{
    uint16_t raw = 0;
    if (ch32v003_get_adc(&raw) != ESP_OK) return false;
    float adc_mv = (raw / CH32V003_ADC_MAX_COUNT) * CH32V003_ADC_VREF_MV;
    *out_raw = raw;
    *out_mv  = (uint16_t)(adc_mv * CAM_BAT_DIVIDER_RATIO);
    return true;
}

/* 자동(타이머)/수동(shot) 캡처가 절대 동시에 안 돌게 직렬화 — esp_camera_fb_get()이 최대
 * ~4초 블로킹될 수 있어서, 자동촬영이 이미 호출을 시작한 직후에 수동 shot이 들어오면
 * "콘솔 사용 중엔 자동촬영 건너뛰기" 플래그로도 못 막고 로그가 섞이는 걸 실기에서 확인
 * (2026-07-21) — 이미 진행 중인 캡처가 있으면 새 요청은 그게 끝날 때까지 뮤텍스로 대기 */
static SemaphoreHandle_t s_capture_mutex = NULL;

/* 2026-08-21 — 매 촬영 직전에 현재 설정을 반영(카메라 초기화 시 1회가 아니라 촬영마다) —
 * Cntl에서 설정을 바꾼 게 이번 웨이크 사이클 중간(카메라 이미 초기화된 뒤)에 와도 다음
 * 촬영부터 바로 적용되게 함. sensor_t::set_gain_ctrl/set_exposure_ctrl은 켬(1)->끔(0)
 * 전환 시 그 순간의 자동값에 고정하는 것뿐이라 별도 수동값 지정은 안 함(단순 On/Off) */
static void apply_agc_aec_settings(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    if (s->set_gain_ctrl)     s->set_gain_ctrl(s, s_agc_enable ? 1 : 0);
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, s_aec_enable ? 1 : 0);
}

static bool camera_capture_one(cam_capture_kind_t kind)
{
    /* save_warmup_frames는 수동(MANUAL) 촬영일 때만 true — 자동촬영은 esp_timer 태스크의
     * 작은 스택에서 도는 데다 무인 반복이라, 진단용 SD 쓰기를 그 경로에 태우지 않음(위
     * camera_init() 주석 참고) */
    if (ensure_camera_ready(kind == CAM_CAPTURE_KIND_MANUAL) != ESP_OK) return false;
    apply_agc_aec_settings();

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

/* 2026-08-21 — CAM_CONFIG_SET으로 Cntl이 원격 지정. 카메라가 이미 이번 사이클에 초기화된
 * 상태(s_camera_ready)면 위 cam_node_set_xclk()로 바로 반영(PLL 재계산 포함) — 콘솔 xclk
 * 명령과 동일 경로. 아직 초기화 전이면 camera_init()이 다음에 이 값으로 esp_camera_init()을
 * 부르게 저장만 해둠(그 시점엔 sensor 핸들이 없어서 set_xclk 자체를 못 부름) */
void cam_node_set_xclk_target_mhz(uint8_t mhz)
{
    if (mhz < 1 || mhz > 40) {
        ESP_LOGW(TAG, "XCLK 목표값 범위 밖(1~40MHz), 무시: %u", (unsigned)mhz);
        return;
    }
    s_xclk_target_mhz = mhz;
    if (s_camera_ready) {
        cam_node_set_xclk((int)mhz);
    }
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
     * 센서 핸들이 필요해서 camera_capture_one() 진입 전에 여기서 먼저 준비돼 있어야 함.
     * 이 함수 자체가 수동(콘솔 shot/CAPTURE_NOW) 전용 경로라 save_warmup_frames=true 고정 */
    if (ensure_camera_ready(true) != ESP_OK) {
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

/* 2026-08-23 — esp_now_channelsync_set_event_hooks()에 넘길 무인자 래퍼(위 cam_speaker_init
 * 호출부 참고, esp_now_channelsync.h의 컴포넌트 결합 회피 주석 참고). 소리는 실제 동작을
 * 그대로 반영해야 함(사용자 지적: "소리를 소거하는 게 아니라 스캔이 안 돌게 하려는 거잖아") —
 * 그래서 여기서 조건부로 죽이지 않음. 페어링 후 스캔이 다시 안 도는 게 진짜 목표이고, 그게
 * 지켜지면 이 소리들도 자연히 다시 안 남(esp_now_channelsync.c의 s_scan_locked 참고) */
static void spk_on_channel_scanned(void)      { cam_speaker_notify(SPK_EVT_SCAN_CHANNEL); }
static void spk_on_advertise_sent(void)       { cam_speaker_notify(SPK_EVT_ADVERTISE_SENT); }
static void spk_on_advertise_ack_received(void) { cam_speaker_notify(SPK_EVT_ADVERTISE_ACK); }
/* 2026-08-25 — spk_on_ping_sent/spk_on_pong_received 제거(CASK 재설계로 핑퐁 자체가
 * 없어짐, cam_speaker.h의 SPK_EVT_COUNT 10->8 축소와 짝) */

/* 스윕 완료 훅 — 위 s_sweep_completed 참고 */
static void spk_on_scan_sweep_done(void)
{
    cam_speaker_notify(SPK_EVT_SWEEP_DONE);
    s_sweep_completed = true;
    cam_node_signal_recheck();
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

    /* 2026-08-23 — 스피커로 6가지 이벤트만 소리로 구분(CAML에서 검증, 기본 꺼짐 —
     * dev_console의 soundlog on/off로 켬). 실패해도 계속 진행 */
    if (cam_speaker_init() != ESP_OK) {
        ESP_LOGW(TAG, "스피커 초기화 실패 — 소리 알림 없이 계속 진행");
    } else {
        esp_reset_reason_t spk_rr = esp_reset_reason();
        if (spk_rr == ESP_RST_USB) {
            cam_speaker_notify(SPK_EVT_POWERON_USB);
        } else if (spk_rr == ESP_RST_POWERON) {
            cam_speaker_notify(SPK_EVT_POWERON_BATTERY);
        }
        /* esp_now_channelsync는 Common 공유 컴포넌트라 cam_speaker를 직접 모름(CNTL/Sens
         * 빌드가 깨지지 않도록) — 대신 이 느슨한 훅으로 연결 */
        esp_now_channelsync_set_event_hooks(spk_on_channel_scanned, spk_on_advertise_sent,
                                             spk_on_advertise_ack_received, spk_on_scan_sweep_done);
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
    esp_now_cam_init();  /* fast path 시도(성공하면 이미 PAIRED) 또는 폴백 스캔 시작 */
    /* esp_now_cam_init() 다음에 저장된(또는 기본) 응답성 설정을 반영(RWDT도 여기서
     * 실제값으로 재무장됨) */
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

    s_wake_recheck_sem = xSemaphoreCreateBinary();

    /* 2026-08-25(CASK 재설계) — "체크인(WAKE_HELLO) -> CASK 수행 -> SLEEP_NOW 대기 ->
     * (0이면 다시 체크인, 아니면 진짜 딥슬립)" 전체를 이 루프 하나가 담당. 예전의 고정
     * 유휴여유+재요청 넛지(CAM_DEEPSLEEP_NUDGE_INTERVAL_MS)는 통째로 없어짐 — SLEEP_NOW
     * 자체가 이미 reliable(ACK+재시도)이라 CNTL이 "깜빡 안 보낼" 방법이 구조적으로 없음.
     * esp_now_cam_init()이 이미 fast path를 한 번 시도했으므로(성공했으면 이미 PAIRED) 그
     * 결과부터 시작 */
    bool     paired_now = esp_now_cam_is_paired();
    uint32_t sleep_sec  = CAM_DEEPSLEEP_RETRY_SLEEP_SEC;

    for (;;) {
        if (!paired_now) {
            /* 알려진 CNTL이 없었거나 fast path가 실패해서 지금 폴백 스캔 중(백그라운드 —
             * esp_now_cam_reconnect() 참고) — PAIR_REQUEST가 비동기로 도착할 때까지
             * 이벤트 기반 대기, 스윕 한 바퀴 다 돌 때까지만(그 이상은 무의미 — 이 채널
             * 범위엔 없다는 뜻) */
            while (!esp_now_cam_is_paired() && !s_sweep_completed) {
                xSemaphoreTake(s_wake_recheck_sem, pdMS_TO_TICKS(1000));
            }
            if (!esp_now_cam_is_paired()) {
                /* 못 찾음 — 백오프 간격만큼 짧게 자고 처음부터(광고) 재시도 */
                sleep_sec = next_unpaired_retry_sleep_sec();
                s_unpaired_backoff_elapsed_sec += sleep_sec;
                ESP_LOGW(TAG, "폴백 스윕 완료 — CNTL 못 찾음, %us 후 재시도", (unsigned)sleep_sec);
                break;
            }
        }
        s_unpaired_backoff_elapsed_sec = 0;  /* 페어링 성공 — 백오프 리셋 */

        /* CASK 대기 — CONFIG부터 SLEEP_NOW까지, 이벤트 기반으로 기다리되 CASK_TIMEOUT_MS
         * 전체 상한을 둠(위 정의 참고 — "WAKE_HELLO 성공 판정을 CASK 전체로 넓힌 것", 새
         * 타이머 개념이 아님). SLEEP_NOW 수신 시 cam_node_note_sleep_now_requested()가
         * s_sleep_now_requested를 세우고 cam_node_signal_recheck()로 이 세마포어를 깨움.
         * 2026-08-26 — s_sleep_now_requested 리셋은 여기서 안 함(순서 버그 수정) —
         * esp_now_cam_reconnect()/PAIR_ACK 전송 "직전"에 이미 cam_node_reset_sleep_now_state()로
         * 끝냈음(esp_now_cam.c 참고) */
        uint32_t cask_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool cask_timed_out = false;
        while (!s_sleep_now_requested) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms - cask_start_ms >= CASK_TIMEOUT_MS) {
                ESP_LOGW(TAG, "CASK 미완주(%ums 경과, SLEEP_NOW 못 받음) — WAKE_HELLO부터 재시도",
                         (unsigned)CASK_TIMEOUT_MS);
                cask_timed_out = true;
                break;
            }
            xSemaphoreTake(s_wake_recheck_sem, pdMS_TO_TICKS(1000));
        }

        if (cask_timed_out) {
            paired_now = esp_now_cam_reconnect();
            continue;
        }

        if (s_sleep_sec_from_cntl != 0) {
            sleep_sec = s_sleep_sec_from_cntl;
            break;
        }

        /* Live 루프(SLEEP_NOW(0)) — 이번 CASK가 걸린 시간이 1초보다 짧으면 나머지를 채워
         * 대기, 길었으면 곧장 재체크인(사용자 설계: "x초보다 짧으면 x초에 통신 재개, x초보다
         * 길면 바로 나 왔어요") */
        uint32_t elapsed_ms = (uint32_t)(esp_timer_get_time() / 1000) - cask_start_ms;
        if (elapsed_ms < CASK_LIVE_PACE_MS) {
            vTaskDelay(pdMS_TO_TICKS(CASK_LIVE_PACE_MS - elapsed_ms));
        }
        paired_now = esp_now_cam_reconnect();
    }

    ESP_LOGI(TAG, "딥슬립 진입: %us 후 웨이크", (unsigned)sleep_sec);
    /* 2026-08-26 — 명령값(sleep_sec)이 아니라 실제 잠든 시각을 남김(위 capture_wake_reason()의
     * 실측 계산이 다음 부팅 때 이 값과 비교함) */
    time(&s_sleep_entry_unix_time);
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_sec * 1000000ULL);
    /* 2026-08-23 — soundlog 켜져있으면 큐에 예약된 소리가 실제로 재생될 시간을 줌(최대 2초).
     * 안 그러면 스윕완료 등 이벤트 즉시 잠들어서 notify()가 큐에 넣기만 한 채 하드웨어가
     * 꺼져 소리가 통째로 안 남(실기로 발견) — 꺼져있으면 즉시 리턴이라 평소엔 영향 없음 */
    cam_speaker_wait_idle(2000);
    /* 2026-08-26(사용자 지시) — "실제 esp_deep_sleep_start가 되는지, 이 함수 속에서 혹시
     * 그냥 리턴하는 건 아닌지도 의심스러워서" — esp_deep_sleep_start()는 noreturn이라 내부에
     * 로그를 넣을 순 없으니, 호출 바로 직전에 명확한 마커를 찍어 여기까지 실제로 도달하는지
     * (그리고 그 이후 시리얼이 뚝 끊기는지 = 진짜 딥슬립 진입했는지) 눈으로 바로 확인 가능하게 함 */
    ESP_LOGI(TAG, "esp_deep_sleep_start() 호출 직전 (sleep_sec=%u) — 이 줄 이후 시리얼 끊기면 정상 진입",
             (unsigned)sleep_sec);
    esp_deep_sleep_start();  /* RWDT는 이미 무장돼있음, 안 건드림 */
    ESP_LOGE(TAG, "esp_deep_sleep_start()가 리턴함(있어선 안 되는 상황) — sleep_sec=%u", (unsigned)sleep_sec);
#else
    dev_console_start();  /* 딥슬립 완전 비활성 — 상시 동작(벤치/콘솔 개발용, Kconfig 이스케이프 해치) */
#endif
}
