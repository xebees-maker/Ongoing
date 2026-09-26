#include "dev_console.h"
#include "cam_node.h"
#include "esp_now_link.h"
#include "cam_speaker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "dev_console";

/* shot을 쓰면 이 시각까지 자동 촬영 타이머를 건너뛴다(cam_node.c의 capture_timer_cb가
 * dev_console_auto_capture_paused()로 확인). 2026-09-26 — SD 제거로 ls/get/clear 명령은
 * 삭제됨(예전엔 ls로 본 파일이 get 전에 순환삭제로 사라지는 걸 막는 게 주 목적이었음) */
#define AUTO_CAPTURE_PAUSE_US (60LL * 1000 * 1000)
static int64_t s_pause_until_us = 0;

static void note_console_activity(void)
{
    s_pause_until_us = esp_timer_get_time() + AUTO_CAPTURE_PAUSE_US;
}

bool dev_console_auto_capture_paused(void)
{
    return esp_timer_get_time() < s_pause_until_us;
}

/* shot 응답은 SHOT_OK/SHOT_FAIL 마커로 끝남 — 백그라운드 로그가 같은 콘솔 출력에 끼어들어도
 * 호스트 스크립트가 마커 기준으로 걸러낼 수 있게 하기 위함(실기에서 실제로 섞이는 걸 확인,
 * 2026-07-20) */
static int cmd_shot(int argc, char **argv)
{
    note_console_activity();
    const char *size_name = (argc >= 2) ? argv[1] : NULL;
    printf(cam_node_capture_now_sized(size_name) ? "SHOT_OK\n" : "SHOT_FAIL\n");
    return 0;
}

static int cmd_soundlog(int argc, char **argv)
{
    if (argc < 2) {
        printf("사용법: soundlog <on|off|solo <1-%d[,1-%d...]>|solo off>\n", SPK_EVT_COUNT, SPK_EVT_COUNT);
        return 1;
    }
    if (strcmp(argv[1], "off") == 0) {
        cam_speaker_set_enabled(false);
        printf("SOUNDLOG_OFF\n");
    } else if (strcmp(argv[1], "on") == 0) {
        cam_speaker_set_enabled(true);
        printf("SOUNDLOG_ON\n");
    } else if (strcmp(argv[1], "solo") == 0) {
        /* 이벤트 번호 1..SPK_EVT_COUNT — 정확한 목록은 cam_speaker.h 참고(2026-08-26 기준 13개) */
        if (argc < 3) {
            printf("사용법: soundlog solo <1-%d[,1-%d...]|off>\n", SPK_EVT_COUNT, SPK_EVT_COUNT);
            return 1;
        }
        if (strcmp(argv[2], "off") == 0) {
            cam_speaker_set_solo_mask(0);
            printf("SOUNDLOG_SOLO_OFF\n");
        } else {
            /* 콤마로 여러 개(예: "3,4,5") — 콘솔이 공백 기준으로 토큰을 나누므로 argv[2]는
             * 콤마 포함 통짜 문자열 하나로 들어옴 */
            char buf[64];
            strncpy(buf, argv[2], sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            uint32_t mask = 0;
            bool bad = false;
            char *save = NULL;
            for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                int n = atoi(tok);
                /* 2026-08-26 — 이벤트가 7->8->13으로 늘어나는 동안 여기 상한이 그대로 7로
                 * 박혀있어서 8번(PAIR_ACK)부터는 아예 지정 불가능했던 버그. SPK_EVT_COUNT를
                 * 직접 참조해 이후 이벤트가 더 늘어도 다시 어긋나지 않게 함 */
                if (n < 1 || n > (int)SPK_EVT_COUNT) {
                    bad = true;
                    break;
                }
                mask |= (1u << (uint32_t)(n - 1));
            }
            if (bad || mask == 0) {
                printf("사용법: soundlog solo <1-%d[,1-%d...]|off>\n", SPK_EVT_COUNT, SPK_EVT_COUNT);
                return 1;
            }
            cam_speaker_set_solo_mask(mask);
            printf("SOUNDLOG_SOLO_%s\n", argv[2]);
        }
    } else {
        printf("사용법: soundlog <on|off|solo <1-%d[,1-%d...]>|solo off>\n", SPK_EVT_COUNT, SPK_EVT_COUNT);
        return 1;
    }
    return 0;
}

static int cmd_auto(int argc, char **argv)
{
    if (argc < 2) {
        printf("사용법: auto <on|off> (현재: %s)\n", cam_node_get_auto_capture() ? "on" : "off");
        return 1;
    }
    if (strcmp(argv[1], "off") == 0) {
        cam_node_set_auto_capture(false);
        printf("AUTO_OFF\n");
    } else if (strcmp(argv[1], "on") == 0) {
        cam_node_set_auto_capture(true);
        printf("AUTO_ON\n");
    } else {
        printf("사용법: auto <on|off>\n");
        return 1;
    }
    return 0;
}

static int cmd_q(int argc, char **argv)
{
    if (argc < 2) {
        printf("사용법: q <0~63> (낮을수록 고화질)\n");
        return 1;
    }
    int quality = atoi(argv[1]);
    printf(cam_node_set_jpeg_quality(quality) ? "Q_OK\n" : "Q_FAIL\n");
    return 0;
}

static int cmd_xclk(int argc, char **argv)
{
    if (argc < 2) {
        printf("사용법: xclk <1~40> (MHz)\n");
        return 1;
    }
    int mhz = atoi(argv[1]);
    printf(cam_node_set_xclk(mhz) ? "XCLK_OK\n" : "XCLK_FAIL\n");
    return 0;
}

void dev_console_start(void)
{
    /* UART0 REPL이 아니라 네이티브 USB(USB-Serial/JTAG)로 붙인다 — 플래시/로그와 같은 물리
     * USB 포트를 그대로 재사용(sdkconfig도 ESP_CONSOLE_USB_SERIAL_JTAG로 맞춰둠), 별도 배선
     * 불필요. (원래 이유는 UART0 기본 핀 GPIO43/44가 SD카드 CMD/D0와 겹쳐서였음 — SD는
     * 2026-09-26 코드에서도 제거됨) */
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "cam> ";
    repl_config.max_cmdline_length = 256;
    /* 8192 — 원래는 ls 명령(SD 목록 버퍼 2개)이 기본 4096에서 크래시해서 늘린 값(2026-07-20).
     * ls는 SD와 함께 제거됐지만 shot이 이 태스크에서 촬영+푸시까지 하므로 그대로 둠 */
    repl_config.task_stack_size = 8192;

    esp_console_dev_usb_serial_jtag_config_t usb_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb_config, &repl_config, &repl));

    ESP_ERROR_CHECK(esp_console_register_help_command());

    const esp_console_cmd_t shot_cmd = {
        .command = "shot",
        .help    = "즉시 1장 촬영해서 콘으로 전송 (예: shot vga / shot qvga / shot 5m — 해상도 변경 후 촬영, 이후 계속 적용됨)",
        .hint    = "[5m|qvga|vga]",
        .func    = cmd_shot,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&shot_cmd));

    const esp_console_cmd_t auto_cmd = {
        .command = "auto",
        .help    = "10초 주기 자동 촬영 on/off (예: auto off)",
        .hint    = "<on|off>",
        .func    = cmd_auto,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&auto_cmd));

    const esp_console_cmd_t soundlog_cmd = {
        .command = "soundlog",
        .help    = "스피커 소리 로그 on/off, solo <1-13[,1-13...]>으로 이벤트 골라 재생(기본 꺼짐)",
        .hint    = "<on|off|solo N[,N...]|solo off>",
        .func    = cmd_soundlog,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&soundlog_cmd));

    const esp_console_cmd_t q_cmd = {
        .command = "q",
        .help    = "JPEG 화질 변경 (0~63, 낮을수록 고화질 — 예: q 15), 다음 촬영부터 계속 적용됨",
        .hint    = "<0-63>",
        .func    = cmd_q,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&q_cmd));

    const esp_console_cmd_t xclk_cmd = {
        .command = "xclk",
        .help    = "XCLK 변경(MHz, 예: xclk 20) — PLL 재계산까지 수행, 다음 촬영부터 계속 적용됨",
        .hint    = "<1-40>",
        .func    = cmd_xclk,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&xclk_cmd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "개발 콘솔 시작 — shot / auto <on|off> / q <0-63> / xclk <1-40> / help");
}
