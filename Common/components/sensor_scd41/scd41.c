/**
 * @file    scd41.c
 * @brief   SCD41 CO2/온습도 센서 — I2C 드라이버 (Sensirion 프로토콜)
 */

#include "scd41.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "scd41";

#define CMD_WAKE_UP                     0x36F6
#define CMD_STOP_PERIODIC_MEASUREMENT   0x3F86
#define CMD_START_PERIODIC_MEASUREMENT  0x21B1
#define CMD_GET_DATA_READY_STATUS       0xE4B8
#define CMD_READ_MEASUREMENT            0xEC05

#define I2C_TIMEOUT_MS   1000

static i2c_master_dev_handle_t  s_dev = NULL;
static i2c_master_bus_handle_t  s_bus = NULL;

static void recover_bus(void)
{
    if (s_bus) i2c_master_bus_reset(s_bus);
}

/* Sensirion CRC8: poly 0x31, init 0xFF */
static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool send_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
    if (err == ESP_OK) return true;
    ESP_LOGW(TAG, "send_cmd(0x%04X) 실패: %s", cmd, esp_err_to_name(err));
    recover_bus();
    return false;
}

bool scd41_init(int i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source            = (i2c_clock_source_t)LP_I2C_SCLK_DEFAULT,  /* I2C port 1 = LP_I2C, 일반 클럭 소스 미지원 */
        .i2c_port              = i2c_port,
        .scl_io_num            = scl_gpio,
        .sda_io_num            = sda_gpio,
        .glitch_ignore_cnt     = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed (SDA=%d SCL=%d)", sda_gpio, scl_gpio);
        return false;
    }
    ESP_LOGI(TAG, "전용 I2C 버스 OK  SDA=%d SCL=%d", sda_gpio, scl_gpio);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SCD41_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed");
        return false;
    }

    /* SCD41은 슬립 모드 지원 — wake_up은 NACK이 정상(깨우는 용도), 결과 무시 */
    send_cmd(CMD_WAKE_UP);
    vTaskDelay(pdMS_TO_TICKS(30));

    /* 이전 상태와 무관하게 정지 후 재시작 (정지 명령 실패는 무시) */
    bool stop_ok = send_cmd(CMD_STOP_PERIODIC_MEASUREMENT);
    ESP_LOGI(TAG, "stop_periodic_measurement: %s", stop_ok ? "ACK" : "NACK(무시)");
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool start_ok = false;
    for (int retry = 0; retry < 5 && !start_ok; retry++) {
        start_ok = send_cmd(CMD_START_PERIODIC_MEASUREMENT);
        if (!start_ok) {
            ESP_LOGW(TAG, "start_periodic_measurement 실패 (시도 %d/5)", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (!start_ok) {
        ESP_LOGW(TAG, "센서 응답 없음 — 연결 확인 필요");
        return false;
    }

    ESP_LOGI(TAG, "SCD41 주기 측정 시작 (5초 간격)");
    return true;
}

static bool data_ready(void)
{
    if (!send_cmd(CMD_GET_DATA_READY_STATUS)) {
        ESP_LOGW(TAG, "data_ready: 명령 전송 실패");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t resp[3] = { 0 };
    if (i2c_master_receive(s_dev, resp, sizeof(resp), I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "data_ready: 응답 수신 실패");
        recover_bus();
        return false;
    }
    if (crc8(resp, 2) != resp[2]) {
        ESP_LOGW(TAG, "data_ready: CRC 불일치");
        return false;
    }

    uint16_t status = ((uint16_t)resp[0] << 8) | resp[1];
    bool ready = (status & 0x07FF) != 0;
    ESP_LOGI(TAG, "data_ready: status=0x%04X ready=%d", status, ready);
    return ready;
}

bool scd41_read(int *co2_ppm, float *temperature, float *humidity)
{
    if (!s_dev) return false;
    if (!data_ready()) return false;

    if (!send_cmd(CMD_READ_MEASUREMENT)) return false;
    vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t resp[9] = { 0 };
    if (i2c_master_receive(s_dev, resp, sizeof(resp), I2C_TIMEOUT_MS) != ESP_OK) {
        recover_bus();
        return false;
    }

    for (int w = 0; w < 3; w++) {
        if (crc8(&resp[w * 3], 2) != resp[w * 3 + 2]) {
            ESP_LOGW(TAG, "CRC 불일치 (word %d)", w);
            return false;
        }
    }

    uint16_t co2_raw  = ((uint16_t)resp[0] << 8) | resp[1];
    uint16_t temp_raw = ((uint16_t)resp[3] << 8) | resp[4];
    uint16_t humi_raw = ((uint16_t)resp[6] << 8) | resp[7];

    if (co2_ppm)     *co2_ppm     = co2_raw;
    if (temperature) *temperature = -45.0f + 175.0f * ((float)temp_raw / 65536.0f);
    if (humidity)     *humidity   = 100.0f * ((float)humi_raw / 65536.0f);

    ESP_LOGI(TAG, "co2=%u ppm  temp_raw=%u  humi_raw=%u", co2_raw, temp_raw, humi_raw);
    return true;
}
