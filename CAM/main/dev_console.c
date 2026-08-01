#include "dev_console.h"
#include "cam_node.h"
#include "cam_storage.h"
#include "esp_now_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "argtable3/argtable3.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "dev_console";

/* shot/ls/get 중 하나라도 쓰이면 이 시각까지 자동 촬영 타이머를 건너뛴다 — ls로 본 파일이
 * 자동 촬영의 순환삭제로 get 하기 전에 사라지는 걸 막기 위함(cam_node.c의
 * capture_timer_cb가 dev_console_auto_capture_paused()로 확인) */
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

#define GET_READ_CHUNK_LEN   4096

/* shot/ls 응답에도 get의 BEGIN_FILE/END_FILE처럼 마커를 둔다 — 10초 주기 자동 촬영
 * 타이머가 백그라운드에서 항상 돌고 있어서, 응답을 기다리는 사이에 그 타이머의 로그
 * (캡처 저장/FB-OVF 등)가 같은 콘솔 출력에 끼어드는 걸 호스트 스크립트가 마커 기준으로
 * 걸러낼 수 있게 하기 위함(실기에서 실제로 섞이는 걸 확인, 2026-07-20) */
static int cmd_shot(int argc, char **argv)
{
    note_console_activity();
    const char *size_name = (argc >= 2) ? argv[1] : NULL;
    printf(cam_node_capture_now_sized(size_name) ? "SHOT_OK\n" : "SHOT_FAIL\n");
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    (void)argc; (void)argv;
    note_console_activity();

    uint32_t ids[CAM_STORAGE_MAX_FILES];
    int count = cam_storage_list(PHOTO_REQUEST_MODE_ALL, 0, ids, CAM_STORAGE_MAX_FILES);
    if (count < 0) {
        printf("BEGIN_LS\nFAIL (SD 목록 조회 실패)\nEND_LS\n");
        return 1;
    }

    printf("BEGIN_LS\n");
    for (int i = 0; i < count; i++) {
        uint32_t size = 0, capture_time = 0;
        char kind = '?';
        if (cam_storage_stat(ids[i], &size, &kind, &capture_time) == ESP_OK) {
            time_t t = (time_t)capture_time;
            struct tm tm_buf;
            char time_buf[20];
            gmtime_r(&t, &tm_buf);
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
            printf("%c %u  %s  %u bytes\n", kind, (unsigned)ids[i], time_buf, (unsigned)size);
        }
    }
    printf("총 %d개\n", count);
    printf("END_LS\n");
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    note_console_activity();
    int deleted = cam_storage_delete_all();
    if (deleted < 0) {
        printf("FAIL (삭제 중 오류)\n");
        return 1;
    }
    printf("CLEARED %d\n", deleted);
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

static struct {
    struct arg_int *id;
    struct arg_end *end;
} s_get_args;

/* base64 왕복 없이 원본 바이트를 그대로 콘솔 출력 스트림에 싣는다.
 *
 * 2026-07-23 근본 원인 확정 (버그 #1, 작은 파일에서 항상 재현): esp_console_new_repl_usb_serial_jtag()가
 * 내부에서 usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF)를 호출해둬서, stdout으로
 * 나가는 모든 바이트 중 0x0A(LF)가 전부 0x0D 0x0A로 치환되고 있었다 — JPEG 마커는 짧고 구조적이라
 * 컨테이너는 멀쩡해 보이고 엔트로피 코딩된 픽셀 데이터만 밴딩/시어링으로 깨져 보였음.
 *
 * 2026-07-23 근본 원인 확정 (버그 #2, 5MP처럼 큰 파일에서 비결정적으로 재현): 심지어 버그 #1을
 * 고친 뒤에도 수백KB급 전송에서 크기는 그대로인데 내용만 다르게(재전송마다 다르게!) 깨지는
 * 사례가 나옴. esp_rom_crc32_le로 촬영 직후/플래시 재읽기 직후/PC 수신본 이렇게 세 지점을
 * 비교해서 촬영·플래시 저장·플래시 재읽기는 항상 일치하고 오직 "PC로 전송" 구간에서만 깨지는
 * 걸 확인. 원인은 usb_serial_jtag_vfs.c의 usbjtag_tx_char_via_driver() — 논블로킹 시도가
 * 실패하고 그 다음 블로킹 시도(타임아웃 TX_FLUSH_TIMEOUT_US=50ms)까지 한 번 실패하면, 이후로는
 * 링버퍼가 찰 때마다 그 바이트를 그냥 조용히 버린다(콘솔 로그용으로 설계된 동작 — 아무도 안 보고
 * 있으면 시스템이 멈추지 않게 하려는 의도). 그런데 그 위의 usb_serial_jtag_write()는 tx_func의
 * 반환값을 아예 안 보고 fwrite에 항상 "size 전부 성공"으로 보고한다 — 그래서 fwrite 루프는
 * 아무 이상을 못 느끼고 오프셋을 계속 전진시킨다. 파일이 클수록/호스트가 못 따라올수록 이 조건에
 * 더 자주 걸리니 크기에 비례해 잘 깨지고, 호스트 쪽 타이밍에 따라 달라지니 비결정적이었던 것.
 * 해결: stdio/fwrite를 완전히 우회하고 usb_serial_jtag_write_bytes()를 직접 호출한다 — 이 함수는
 * 실제로 쓴 바이트 수를 정직하게 반환하므로 usj_write_raw()에서 부족분을 재시도할 수 있다(아래).
 * 이 경로는 LF/CRLF 치환도 아예 안 타므로 버그 #1의 우회(LF 모드 전환)도 더 이상 필요 없다.
 * project_cam_dvp_corruption_investigation 메모리 참고.
 *
 * 호스트는 "BEGIN_BIN ... size=N" 헤더를 본 뒤 그 다음부터 정확히 N바이트를 raw로 읽으면
 * 됨(tools/cam_get.py). */
/* usb_serial_jtag_write_bytes()는 내부적으로 xRingbufferSend()를 한 번에 호출하는데, 이건
 * "요청한 만큼이 한꺼번에 들어갈 자리가 날 때까지" 기다리는 것이지 부분 성공을 허용하지 않는다
 * (esp_driver_usb_serial_jtag/src/usb_serial_jtag.c 참고). 그런데 콘솔의 TX 링버퍼는
 * USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() 기준 겨우 256바이트라서, 수백KB를 한 번에
 * 요청하면 ticks_to_wait를 아무리 늘려도 절대 성공할 수 없다(버퍼 자체보다 큰 요청은 영원히
 * 자리가 안 남) — 그래서 첫 시도에서 256바이트보다 훨씬 작은 청크로 나눠 보내야 한다. */
#define USJ_WRITE_CHUNK_LEN  200

static bool usj_write_raw(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > USJ_WRITE_CHUNK_LEN) {
            chunk = USJ_WRITE_CHUNK_LEN;
        }
        int wrote = usb_serial_jtag_write_bytes(buf + off, chunk, pdMS_TO_TICKS(2000));
        if (wrote <= 0) {
            return false;
        }
        off += (size_t)wrote;
    }
    return true;
}

static int cmd_get(int argc, char **argv)
{
    note_console_activity();
    int nerrors = arg_parse(argc, argv, (void **)&s_get_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_get_args.end, argv[0]);
        return 1;
    }
    uint32_t file_id = (uint32_t)s_get_args.id->ival[0];

    FILE *fp = NULL;
    uint32_t size = 0;
    if (cam_storage_open_read(file_id, &fp, &size) != ESP_OK) {
        printf("FAIL (파일 없음: %u)\n", (unsigned)file_id);
        return 1;
    }

    uint8_t *buf = malloc(GET_READ_CHUNK_LEN);
    if (!buf) {
        printf("FAIL (메모리 부족)\n");
        fclose(fp);
        return 1;
    }

    printf("BEGIN_BIN id=%u size=%u\n", (unsigned)file_id, (unsigned)size);
    fflush(stdout);

    bool ok = true;
    size_t n;
    while (ok && (n = fread(buf, 1, GET_READ_CHUNK_LEN, fp)) > 0) {
        ok = usj_write_raw(buf, n);
    }
    if (!ok) {
        ESP_LOGE(TAG, "get: 전송 중단(호스트 응답 없음)");
    }

    printf("\nEND_BIN\n");

    free(buf);
    fclose(fp);
    return ok ? 0 : 1;
}

void dev_console_start(void)
{
    /* UART0 REPL이 아니라 네이티브 USB(USB-Serial/JTAG)로 붙인다 — 이 보드는 UART0 기본
     * TX/RX 핀(GPIO43/44)이 SD카드 CMD/D0로 이미 쓰이고 있어서(bsp_esp32s3_cam.h 참고)
     * esp_console_new_repl_uart()를 쓰면 REPL이 SD 핀과 충돌해 입력을 못 받는다. 플래시/
     * 로그와 같은 물리 USB 포트를 그대로 재사용(sdkconfig도 ESP_CONSOLE_USB_SERIAL_JTAG로
     * 맞춰둠) — 별도 배선 불필요. */
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "cam> ";
    repl_config.max_cmdline_length = 256;
    /* 기본 4096바이트로는 부족 — cmd_ls()의 uint32_t ids[CAM_STORAGE_MAX_FILES](2000B) +
     * cam_storage_list() 내부의 동일 크기 버퍼(2000B)가 겹쳐 StoreProhibited로 크래시하는 걸
     * 실기에서 확인(2026-07-20) */
    repl_config.task_stack_size = 8192;

    esp_console_dev_usb_serial_jtag_config_t usb_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb_config, &repl_config, &repl));

    ESP_ERROR_CHECK(esp_console_register_help_command());

    const esp_console_cmd_t shot_cmd = {
        .command = "shot",
        .help    = "즉시 1장 촬영해서 SD에 저장 (예: shot vga / shot qvga / shot 5m — 해상도 변경 후 촬영, 이후 계속 적용됨)",
        .hint    = "[5m|qvga|vga]",
        .func    = cmd_shot,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&shot_cmd));

    const esp_console_cmd_t ls_cmd = {
        .command = "ls",
        .help    = "SD에 저장된 파일 목록 (id=타임스탬프, 크기)",
        .func    = cmd_ls,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ls_cmd));

    s_get_args.id  = arg_int1(NULL, NULL, "<id>", "ls로 확인한 파일 id");
    s_get_args.end = arg_end(1);
    const esp_console_cmd_t get_cmd = {
        .command  = "get",
        .help     = "파일을 raw 바이너리로 콘솔에 덤프 (tools/cam_get.py로 로컬 저장)",
        .hint     = NULL,
        .func     = cmd_get,
        .argtable = &s_get_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&get_cmd));

    const esp_console_cmd_t clear_cmd = {
        .command = "clear",
        .help    = "SD에 저장된 사진 전부 삭제",
        .func    = cmd_clear,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&clear_cmd));

    const esp_console_cmd_t auto_cmd = {
        .command = "auto",
        .help    = "10초 주기 자동 촬영 on/off (예: auto off)",
        .hint    = "<on|off>",
        .func    = cmd_auto,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&auto_cmd));

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
    ESP_LOGI(TAG, "개발 콘솔 시작 — shot / ls / get <id> / clear / auto <on|off> / q <0-63> / xclk <1-40> / help");
}
