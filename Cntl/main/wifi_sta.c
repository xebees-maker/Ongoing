#include "wifi_sta.h"
#include "device_config.h"
#include "main.h"  /* web_dashboard_start */

#include <string.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"

/* 2026-09-26 — node_hub.c(구 esp_now_hub.c)에서 분리(사용자 지시: ESP-NOW/노드 관리와 무관한
 * Wi-Fi 네트워크 기능이라 별도 파일·이름으로). 코드는 옮기기만 하고 동작은 그대로 —
 * STA 연결/재연결, "찾기" 팝업의 실시간 접속 시도, SoftAP 모드, 웹 대시보드 URL용 IP */
static const char *TAG = "wifi_sta";

/* Cntl 실제 sdkconfig 기준 STA 모드+SSID/PW — 이 PC(원래 PC)의 네트워크로 복원
 * (다른 PC 세션에서 그쪽 네트워크 hkhome으로 바뀌어 커밋됨 — 2026-08-08 원복) */
#define WIFI_SSID     "iptime2.4"
#define WIFI_PASSWORD "sk1234!@#"

/* 2026-08-04 — 채널 격리: Cntl을 hkhome STA에서 떼어내 순수 SoftAP 전용으로 돌려서,
 * ESP-NOW(CAM/Sens) 트래픽이 더 이상 실제 WiFi 트래픽과 같은 채널을 나눠 쓰지 않게 함.
 * CAM 쪽 코드는 전혀 안 건드림 — CAM은 원래도 실제 WiFi join 없이 ESP-NOW 자체 광고/
 * 채널스캔으로 Cntl을 찾으므로, Cntl이 AP든 STA든 상관없이 그대로 찾아서 페어링됨.
 * 2026-08-29 — 컴파일타임 스위치였던 걸 device_config_get_wifi_ap_mode()로 런타임 분기하도록
 * 승격(설정 UI의 "네트워크" 행에서 토글, 재시작 시 적용). */
#define CNTL_AP_SSID       "Cntl-Test-AP"
#define CNTL_AP_PASSWORD   "cntltest123"
#define CNTL_AP_CHANNEL    1
#define CNTL_AP_MAX_CONN   4
/* AP 모드일 땐 STA 인터페이스가 아예 안 떠있어서, ESP-NOW 피어 등록/자기 MAC 조회를
 * WIFI_IF_STA로 하면 ESP_ERR_ESPNOW_IF로 전부 실패함(실기에서 확인 — HUB_RESET 브로드캐스트
 * 부터 막힘). 아래 런타임 변수로 통일해서 모드 전환 시 자동으로 맞게 함 — wifi_bringup()에서
 * 딱 한 번 설정, 그 이후로는 읽기 전용 */
static wifi_interface_t s_wifi_if = WIFI_IF_STA;

/* 2026-08-21 — 상황판 요약 맨 윗줄에 웹 대시보드 URL 표시용(사용자 지시). IP 못 받은
 * 상태(빈 문자열)면 ui_main.c가 URL 자체를 숨김 */
static char s_own_ip_str[16] = "";

const char *wifi_sta_get_own_ip_str(void)
{
    return s_own_ip_str;
}

/* 2026-08-29(사용자 지시: "저장소에 있다가... 필요할 때 올리면 되잖아") — 예전엔
 * s_active_sta_ssid/password라는 별도 static 사본을 계속 들고 있었는데, 그 내용이
 * device_config에 이미 있는 값(활성 슬롯[0])과 거의 항상 같아서 중복이었음. 저장된 값이
 * 있으면 그걸, 없으면(첫 부팅, 아직 "찾기"로 아무 것도 저장 안 한 상태) 하드코딩
 * WIFI_SSID/PASSWORD 폴백을 그때그때 반환 — 별도 저장 공간 자체가 필요 없어짐 */
static void get_active_or_fallback_sta(const char **out_ssid, const char **out_password)
{
    const char *ssid = device_config_get_sta_ssid();
    if (ssid[0] != '\0') {
        *out_ssid     = ssid;
        *out_password = device_config_get_sta_password();
    } else {
        *out_ssid     = WIFI_SSID;
        *out_password = WIFI_PASSWORD;
    }
}

const char *wifi_sta_get_active_ssid(void)
{
    const char *ssid, *password;
    get_active_or_fallback_sta(&ssid, &password);
    return ssid;
}

/* 2026-09-08(사용자 재설계 — 상단바 네트워크 컨트롤에 "AP라면 SSID도" 표기) */
const char *wifi_sta_get_ap_ssid(void)
{
    return CNTL_AP_SSID;
}

/* 2026-08-29 — "찾기" 팝업의 실시간 접속 시도(wifi_sta_test_connect) 상태.
 * s_sta_test_active일 때만 아래 wifi_event_handler가 정상 재연결 루프 대신 이 분기를 탐 */
static bool                     s_sta_reconnect_paused = false;

void wifi_sta_set_reconnect_paused(bool paused)
{
    s_sta_reconnect_paused = paused;
}

/* 2026-08-30(사용자 지시: 부팅 후 아직 한 번도 연결된 적 없는 상태에서만 — "찾기"로 이미
 * 붙어있다가 일시적으로 끊긴 경우는 대상 아님, 명시적으로 범위 확인함) — 부팅 후 25초(찾기의
 * 접속시도 타임아웃과 동일 근거값, 사용자 지시: "같은 값으로 하자") 동안 한 번도 못 붙으면
 * 자동 재연결을 멈추고 화면에 "AP 없음" 표시. "찾기"로 수동 연결하면(진짜 GOT_IP) 정상
 * 상태로 복귀 — "찾기" 자체는 이 플래그와 무관하게 항상 그대로 동작(자체 disconnect/connect를
 * 직접 관리하므로 아래 일반 재연결 분기를 안 탐, s_sta_test_active 가드 참고) */
static bool               s_sta_ever_connected     = false;
static bool               s_sta_boot_giveup         = false;
static esp_timer_handle_t s_sta_boot_giveup_timer   = NULL;

static void sta_boot_giveup_timer_cb(void *arg)
{
    (void)arg;
    if (!s_sta_ever_connected) {
        ESP_LOGW(TAG, "부팅 후 25초간 저장된 AP를 못 찾음 — 자동 재연결 중단, AP 없음 표시");
        s_sta_boot_giveup = true;
    }
}

bool wifi_sta_boot_giveup(void)
{
    return s_sta_boot_giveup;
}

/* 2026-08-29 버그수정(사용자 리포트: "비번 입력하느라 시간이 걸리면 연결이 반복 실패") —
 * esp_wifi_sta_get_ap_info()는 "완전히 연결됨" 여부만 알려줘서, "지금 한창 인증/연결
 * 시도 중(아직 연결은 안 됐지만 idle도 아님)"인 상태를 "연결 안 됨"으로 오판함. 이 상태에서
 * wifi_sta_test_connect()가 disconnect() 없이 곧장 새 설정으로 connect()를 걸면
 * 진행 중이던 인증 핸드셰이크 한복판에 끼어들어서 계속 AUTH_EXPIRE로 깨짐(실측 로그로 확인).
 * esp_wifi_connect()를 걸 때마다 true, 진짜 STA_DISCONNECTED 이벤트를 받을 때만 false로
 * 스스로 추적 — "연결됐거나 연결 시도 중"을 타이머/추측 없이 이벤트로 정확히 앎 */
static bool s_sta_conn_in_flight = false;

static void sta_do_connect(void)
{
    s_sta_conn_in_flight = true;
    esp_wifi_connect();
}

static bool                     s_sta_test_active = false;
/* 2026-08-29 버그수정(사용자 리포트: "틀린 비번→재시도(옳은 비번)"가 계속 실패/지연) —
 * 이전 시도의 "실패 후 원래 네트워크로 되돌리기" 재연결(sta_test_finish의 esp_wifi_connect())이
 * 아직 안 끝난 채로 새 시도를 또 걸면, AP가 "Association refused, retry later"로 거절해서
 * 지연되거나 상태가 꼬임(실측 로그로 확인: "wifi:sta is connected, disconnect before
 * connecting to new ap"). DISCONNECTING 단계를 둬서, 새 시도는 항상 먼저 disconnect()로
 * 이전 상태를 확실히 정리하고 그 확인(STA_DISCONNECTED 이벤트)을 받은 뒤에야 진짜 접속을
 * 시작하도록 함 */
typedef enum { STA_TEST_PHASE_DISCONNECTING, STA_TEST_PHASE_CONNECTING } sta_test_phase_t;
static sta_test_phase_t          s_sta_test_phase;
static wifi_sta_test_cb_t s_sta_test_cb = NULL;
static wifi_sta_test_stage_cb_t s_sta_test_stage_cb = NULL;
static void                     *s_sta_test_ctx = NULL;
static char                     s_sta_test_ssid[33] = "";
static char                     s_sta_test_password[65] = "";
static esp_timer_handle_t       s_sta_test_timeout_timer = NULL;

static void sta_test_finish(bool success)
{
    if (!s_sta_test_active) return;  /* 이미 끝난 시도에 대해 타임아웃/이벤트 중복 도착 방어 */
    s_sta_test_active = false;
    if (s_sta_test_timeout_timer) esp_timer_stop(s_sta_test_timeout_timer);

    wifi_sta_test_cb_t cb = s_sta_test_cb;
    void *ctx = s_sta_test_ctx;
    s_sta_test_cb = NULL;
    s_sta_test_ctx = NULL;

    if (!success) {
        /* 실패 — 원래 접속해있던 네트워크 설정으로 복귀시켜서, 다음 STA_DISCONNECTED
         * 재시도 루프가 엉뚱한(방금 실패한) 자격증명이 아니라 원래 걸로 다시 붙게 함.
         * 성공 시에는 아무 것도 안 함 — 호출부(ui_main.c)가 성공 콜백 안에서
         * device_config_set_sta_credentials()를 불러서 device_config 활성 슬롯이 이미
         * 새 값으로 바뀌므로, get_active_or_fallback_sta()가 다음부터 그걸 그대로 반환함 */
        const char *ssid, *password;
        get_active_or_fallback_sta(&ssid, &password);
        wifi_config_t wifi_cfg = { 0 };
        strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        sta_do_connect();
    }

    if (cb) cb(success, ctx);
}

static void sta_test_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "STA 접속 시도 타임아웃");
    sta_test_finish(false);
}

/* 2026-08-29 버그수정(사용자 리포트: "첫 시도만 25초 대기 후 실패, 재시도부터는 시도도
 * 안 해보고 즉시 실패") — DISCONNECTING 단계 정리가 끝났을 때(또는 애초에 끊을 대상이 없어서
 * 건너뛸 때) 여기서 진짜 테스트 대상으로 접속 시작 */
static void sta_test_begin_connecting(void)
{
    s_sta_test_phase = STA_TEST_PHASE_CONNECTING;
    if (s_sta_test_stage_cb) s_sta_test_stage_cb(STA_TEST_STAGE_CONNECTING, s_sta_test_ctx);
    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, s_sta_test_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, s_sta_test_password, sizeof(wifi_cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    sta_do_connect();
}

void wifi_sta_test_connect(const char *ssid, const char *password,
                                   wifi_sta_test_cb_t on_result,
                                   wifi_sta_test_stage_cb_t on_stage, void *ctx)
{
    if (s_sta_test_active) return;  /* 동시에 하나만 — UI가 모달이라 실질적으로 재진입 없음 */
    s_sta_reconnect_paused = false;  /* 스캔용으로 걸려있었을 수 있는 일시정지 해제 — 이제부터는
                                         이 함수/sta_test_finish()가 connect()를 직접 관리함 */
    s_sta_test_active = true;
    s_sta_test_cb  = on_result;
    s_sta_test_stage_cb = on_stage;
    s_sta_test_ctx = ctx;
    strncpy(s_sta_test_ssid, ssid, sizeof(s_sta_test_ssid) - 1);
    s_sta_test_ssid[sizeof(s_sta_test_ssid) - 1] = '\0';
    strncpy(s_sta_test_password, password, sizeof(s_sta_test_password) - 1);
    s_sta_test_password[sizeof(s_sta_test_password) - 1] = '\0';

    if (!s_sta_test_timeout_timer) {
        const esp_timer_create_args_t args = { .callback = sta_test_timeout_cb, .name = "sta_test_to" };
        esp_timer_create(&args, &s_sta_test_timeout_timer);
    }
    /* 2026-08-29 — 25초(AP가 "Association refused, retry later"로 한두 번만 튕겨도 넘길 수
     * 있는 값, 실측 확인). 접속 시도 전체(정리 disconnect 포함)의 유일한 실패 판정 기준 —
     * 이 아래 CONNECTING 단계에서는 더 이상 개별 disconnect를 실패로 판정하지 않음 */
    esp_timer_start_once(s_sta_test_timeout_timer, 25000000);

    /* 2026-08-29 버그수정(사용자 리포트: "첫 시도만 25초 대기 후 실패" 다음 "비번 입력하느라
     * 시간 걸리면 반복 인증실패") — STA가 진짜 idle(연결도 안 돼 있고 연결/인증 시도 중도
     * 아님)일 때만 disconnect() 없이 곧장 CONNECTING으로 가야 함. 처음엔 이걸
     * esp_wifi_sta_get_ap_info()(완전 연결 여부만 앎)로 판단했는데, "연결 시도 중이지만
     * 아직 연결은 안 됨" 상태를 idle로 오판해서 진행 중인 인증 핸드셰이크에 새 connect()가
     * 끼어드는 문제가 있었음 — s_sta_conn_in_flight(이벤트로 스스로 추적)로 교체 */
    if (s_sta_conn_in_flight) {
        s_sta_test_phase = STA_TEST_PHASE_DISCONNECTING;
        if (s_sta_test_stage_cb) s_sta_test_stage_cb(STA_TEST_STAGE_DISCONNECTING, s_sta_test_ctx);
        esp_wifi_disconnect();
    } else {
        sta_test_begin_connecting();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (s_wifi_if == WIFI_IF_STA) {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            if (!s_sta_ever_connected && !s_sta_boot_giveup_timer) {
                const esp_timer_create_args_t args = { .callback = sta_boot_giveup_timer_cb,
                                                          .name = "sta_boot_giveup" };
                esp_timer_create(&args, &s_sta_boot_giveup_timer);
                esp_timer_start_once(s_sta_boot_giveup_timer, 25000000);
            }
            if (!s_sta_reconnect_paused) sta_do_connect();
        } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "WiFi 연결 끊김(reason=%d, rssi=%d) — 재시도",
                     disc ? disc->reason : -1, disc ? disc->rssi : 0);
            s_own_ip_str[0] = '\0';  /* IP 무효화 — 재연결해서 새 IP 받을 때까지 URL 숨김 */
            s_sta_conn_in_flight = false;  /* 진짜 끊김 확인 — "연결/시도 중" 추적 해제 */
            if (s_sta_test_active) {
                if (s_sta_test_phase == STA_TEST_PHASE_DISCONNECTING) {
                    /* 정리용 disconnect 완료 확인됨 — 이제부터 진짜 테스트 대상으로 접속 시도 */
                    sta_test_begin_connecting();
                } else {
                    /* 2026-08-29 버그수정(사용자 리포트: "재시도 때는 시도도 안 해보고 즉시
                     * 실패") — CONNECTING 단계의 첫 disconnect를 곧장 실패로 판정했었는데,
                     * 실제 WiFi 접속은 흔히 한 번에 안 붙고 auth/assoc 재시도를 몇 번 거쳐야
                     * 성공함(정상 동작 — 예전 성공 로그에도 "Association refused
                     * temporarily... comeback time" 이후 재시도해서 붙는 과정이 있었음).
                     * 여기서 실패로 결론짓지 않고 그냥 다시 connect() — 진짜 실패(비번 틀림
                     * 등) 판정은 wifi_sta_test_connect()가 걸어둔 25초 타임아웃
                     * 하나로만 함 */
                    sta_do_connect();
                }
                return;
            }
            /* 2026-08-29 버그수정(사용자 리포트: "찾기 팝업... 검색된 네트워크 없습니다") —
             * esp_wifi_scan_start()는 STA가 연결 시도 중이면 실패한다. 이 재연결 루프가
             * 끊길 때마다 바로 다시 connect()를 걸어서 스캔이 낄 조용한 틈이 없었음 —
             * 스캔 팝업이 열려있는 동안은(wifi_sta_set_reconnect_paused) 재연결을
             * 안 걸고 그냥 끊긴 채로 둠. s_sta_boot_giveup — 부팅 후 25초간 한 번도 못
             * 붙었으면(2026-08-30) 더 이상 자동 재시도 안 함(화면엔 "AP 없음") — "찾기"로
             * 수동 연결하면 GOT_IP 핸들러에서 이 플래그가 풀림 */
            if (!s_sta_reconnect_paused && !s_sta_boot_giveup) sta_do_connect();
        } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "IP 받음: " IPSTR, IP2STR(&evt->ip_info.ip));
            /* 2026-08-21 — 상황판 요약에 웹 대시보드 접속 URL을 보여주기 위해 저장(사용자 지시) */
            snprintf(s_own_ip_str, sizeof(s_own_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
            s_sta_ever_connected = true;
            s_sta_boot_giveup = false;
            if (s_sta_boot_giveup_timer) esp_timer_stop(s_sta_boot_giveup_timer);
            if (s_sta_test_active) sta_test_finish(true);
        }
    } else {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "AP: 클라이언트 접속 " MACSTR, MAC2STR(evt->mac));
        } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "AP: 클라이언트 접속해제 " MACSTR, MAC2STR(evt->mac));
        }
    }
}

static void wifi_bringup(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    bool ap_mode = device_config_get_wifi_ap_mode();
    s_wifi_if = ap_mode ? WIFI_IF_AP : WIFI_IF_STA;

    if (!ap_mode) {
        esp_netif_create_default_wifi_sta();

        /* 2026-08-29 — "네트워크" 설정에서 스캔+선택한 SSID/PW가 저장돼 있으면 그걸 쓰고,
         * 없으면(미설정) 기존 하드코딩 WIFI_SSID/PASSWORD로 폴백(기존 동작 보존) —
         * get_active_or_fallback_sta()가 이 판단을 공용으로 함(위 정의 참고) */
        const char *sta_ssid, *sta_password;
        get_active_or_fallback_sta(&sta_ssid, &sta_password);

        wifi_config_t wifi_cfg = { 0 };
        strncpy((char *)wifi_cfg.sta.ssid, sta_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, sta_password, sizeof(wifi_cfg.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "STA 시작: SSID=%s", sta_ssid);
    } else {
        /* 채널 격리(위 주석 참고) — 실제 WiFi STA 연결 없이 Cntl 혼자만의 SoftAP로.
         * CAM/Sens용 ESP-NOW 채널을 Cntl이 완전히 독점하게 됨 */
        esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

        wifi_config_t wifi_cfg = { 0 };
        strncpy((char *)wifi_cfg.ap.ssid, CNTL_AP_SSID, sizeof(wifi_cfg.ap.ssid) - 1);
        wifi_cfg.ap.ssid_len = strlen(CNTL_AP_SSID);
        strncpy((char *)wifi_cfg.ap.password, CNTL_AP_PASSWORD, sizeof(wifi_cfg.ap.password) - 1);
        wifi_cfg.ap.channel = CNTL_AP_CHANNEL;
        wifi_cfg.ap.max_connection = CNTL_AP_MAX_CONN;
        wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "SoftAP 시작: SSID=%s CH=%d (독립 AP 모드)", CNTL_AP_SSID, CNTL_AP_CHANNEL);

        /* AP 자신의 IP는 STA처럼 IP_EVENT로 오는 게 아니라 시작 즉시 고정값이라, 대시보드
         * 웹 URL(s_own_ip_str, wifi_sta_get_own_ip_str())에 바로 채워넣음 */
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            snprintf(s_own_ip_str, sizeof(s_own_ip_str), IPSTR, IP2STR(&ip_info.ip));
        }

        /* 2026-08-30 버그수정(사용자 리포트: AP모드에서 웹페이지 접속 안 됨) — 웹서버 시작이
         * IP_EVENT_STA_GOT_IP 하나에만 걸려있었는데, AP 모드는 그 이벤트가 아예 안 옴(위
         * 주석처럼 IP를 이벤트가 아니라 시작 즉시 동기적으로 얻음) — 여기서 직접 호출 */
        web_dashboard_start();
    }

    /* WiFi 절전(모뎀 슬립) 끔(2026-08-02) — 부팅 로그에 "wifi:pm start, type: 1"이 찍혀서
     * 기본값(절전 켜짐)으로 동작 중이었음을 확인. 절전 중엔 라디오가 공유기 비콘 주기에
     * 맞춰서만 깨어나는데, CAM이 보내는 ESP-NOW 청크는 그 주기와 무관하게 아무 때나
     * 도착해서 라디오가 자는 타이밍에 온 청크를 놓침 — 사진 가져오기 청크 누락(3002)의
     * 실제 원인으로 추정(실기에서 확인된 증상: 느리고 ETA가 들쭉날쭉하다 결국 실패).
     * Cntl은 상시전원(배터리 아님)이라 절전을 꺼도 손해가 없음 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void wifi_sta_init(void)
{
    wifi_bringup();
}

uint8_t wifi_sta_get_channel(void)
{
    uint8_t channel = 0;
    wifi_second_chan_t second_chan;
    esp_wifi_get_channel(&channel, &second_chan);
    return channel;
}
