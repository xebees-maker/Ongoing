#include "sc05.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sc05";

static uart_port_t s_port      = UART_NUM_1;
static bool         s_installed = false;

bool sc05_init(uart_port_t uart_port, gpio_num_t rx_gpio, gpio_num_t tx_gpio, int baud)
{
    uart_config_t cfg = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(uart_port, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "uart_driver_install 실패");
        return false;
    }
    if (uart_param_config(uart_port, &cfg) != ESP_OK ||
        uart_set_pin(uart_port, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGW(TAG, "uart_param_config/set_pin 실패");
        uart_driver_delete(uart_port);
        return false;
    }
    s_port = uart_port;
    s_installed = true;
    return true;
}

/* 데이터시트 Table4/7/8 체크섬: Byte1~Byte7 합의 2의보수(예시값 FF 17 04 00 00 25 13 88
 * 25에서 직접 검산: sum(17,04,00,00,25,13,88)=0xDB, ~0xDB+1=0x25 — 일치) */
static uint8_t frame_checksum(const uint8_t *frame)
{
    uint8_t sum = 0;
    for (int i = 1; i <= 7; i++) sum += frame[i];
    return (uint8_t)(~sum + 1);
}

bool sc05_read(float *ppm, uint32_t timeout_ms)
{
    if (!s_installed) return false;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        TickType_t remain = deadline - xTaskGetTickCount();
        uint8_t sync = 0;
        int n = uart_read_bytes(s_port, &sync, 1, remain);
        if (n <= 0) continue;          /* 이번 대기창엔 아무것도 안 옴 — 데드라인 재확인 */
        if (sync != 0xFF) continue;    /* 시작바이트 아님 — 계속 스캔 */

        uint8_t frame[9] = { 0xFF };
        int got = uart_read_bytes(s_port, &frame[1], 8, pdMS_TO_TICKS(200));
        if (got != 8) {
            ESP_LOGW(TAG, "프레임 미완성(시작바이트 이후 %dB만 수신)", got < 0 ? 0 : got);
            continue;                  /* 프레임 깨짐 — 처음부터 다시 동기화 */
        }
        if (frame_checksum(frame) != frame[8]) {
            ESP_LOGW(TAG, "체크섬 불일치 — 프레임 버림");
            continue;
        }

        uint16_t conc_raw = ((uint16_t)frame[4] << 8) | frame[5];
        if (ppm) *ppm = (float)conc_raw / 100.0f;
        return true;
    }
    return false;
}
