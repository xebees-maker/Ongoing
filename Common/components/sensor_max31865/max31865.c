/**
 * @file    max31865.c
 * @brief   max31865.h 구현 — MAX31865(SPI 모드 1, 최대 5MHz) + PT100(R0=100Ω), 기준저항 430Ω
 *
 * 레지스터(읽기 주소 0x0n, 쓰기는 0x8n): 00h 설정, 01h/02h RTD MSB/LSB(LSB bit0 = 고장 플래그), 07h 고장 상태.
 * 설정 레지스터 비트: D7 바이어스, D6 자동 변환, D5 1-shot, D4 3선, D1 고장 상태 지우기, D0 50Hz(0=60Hz).
 */
#include "max31865.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "rom/ets_sys.h"

static const char *TAG = "SENS";

#define MAX_REG_CONFIG       0x00
#define MAX_REG_RTD_MSB      0x01
#define MAX_REG_HIGH_THRESH  0x03  /* 03h/04h MSB/LSB, 05h/06h 하한 — RTD 코드와 같은 형식(15비트 << 1) */
#define MAX_REG_LOW_THRESH   0x05
#define MAX_REG_FAULT_STATUS 0x07
#define MAX_REG_WRITE        0x80

#define CFG_BIAS         0x80
#define CFG_1SHOT        0x20
#define CFG_3WIRE        0x10
#define CFG_FAULT_CLEAR  0x02
/* D0=0 → 60Hz 필터(국내 전원). 1-shot 변환 52ms(50Hz면 62.5ms) */

#define RREF_OHM         430.0f  /* 모듈 기준저항(사용자 확인 — "430") */
#define R0_OHM           100.0f  /* PT100 */
#define BIAS_SETTLE_MS   10      /* 데이터시트: 10.5 시정수 + 1ms — 모듈 필터(수백 Ω × 100nF)면 2ms 미만, 넉넉히 */
#define CONVERSION_MS    65      /* 60Hz 1-shot 52ms + 여유 */
#define SPI_CLOCK_HZ     1000000
/* 정상 범위 밖이면 칩이 고장 비트를 세움 — 기본 임계값(0x0000~0xFFFF)으로는 선이 끊겨 코드가 최대치(0x7FFF)가
 * 돼도 고장으로 안 잡힘. PT100 −50°C ≈ 80.31Ω, 150°C ≈ 157.33Ω → 코드 = R / 430 × 32768 */
#define LOW_THRESH_CODE  6120
#define HIGH_THRESH_CODE 11990

static spi_device_handle_t s_dev = NULL;
static gpio_num_t s_cs = GPIO_NUM_NC;
/* CS는 SPI 드라이버에 맡기지 않고 직접 내리고, 이만큼 기다린 뒤 클럭을 보냄(데이터시트 tCC 최소 400ns).
 * 2026-09-30 실측(COM25): 드라이버가 CS를 제어하면 쉬고 난 뒤 첫 전송이 쓰기일 때 칩에 안 들어감(바이어스 비트가
 * 안 켜짐, 앞에 읽기를 한 번 하면 들어감) — 전이중 전송엔 CS 선행 시간 설정(cs_ena_pretrans)이 적용되지 않고,
 * 이 모듈은 CS가 레벨시프터를 거침. 직접 제어로 바꾼 뒤 1MHz, SDO 풀업 없이 정상 */
#define CS_SETUP_US      10
static uint8_t s_base_cfg = 0;   /* 바이어스·1-shot 없는 기본 설정(3선 비트, 60Hz) */

static bool write_reg(uint8_t reg, uint8_t val)
{
    spi_transaction_t t = {
        .flags  = SPI_TRANS_USE_TXDATA,
        .length = 16,
        .tx_data = { (uint8_t)(reg | MAX_REG_WRITE), val },
    };
    gpio_set_level(s_cs, 0);
    ets_delay_us(CS_SETUP_US);
    bool ok = spi_device_polling_transmit(s_dev, &t) == ESP_OK;
    ets_delay_us(CS_SETUP_US);
    gpio_set_level(s_cs, 1);
    return ok;
}

/* n은 1~3 — 주소 1바이트 뒤에 오는 응답 바이트를 out에 채움 */
static bool read_regs(uint8_t reg, uint8_t *out, int n)
{
    spi_transaction_t t = {
        .flags  = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = (size_t)(1 + n) * 8,
        .tx_data = { reg, 0, 0, 0 },
    };
    gpio_set_level(s_cs, 0);
    ets_delay_us(CS_SETUP_US);
    bool ok = spi_device_polling_transmit(s_dev, &t) == ESP_OK;
    ets_delay_us(CS_SETUP_US);
    gpio_set_level(s_cs, 1);
    if (!ok) return false;
    memcpy(out, &t.rx_data[1], (size_t)n);
    return true;
}

/* Callendar-Van Dusen — 0°C 이상은 이차식의 역, 0°C 미만은 근사 다항식(Adafruit 라이브러리와 같은 계수) */
static float rtd_to_celsius(float r)
{
    const float A = 3.9083e-3f, B = -5.775e-7f;
    float t = (sqrtf(A * A - 4.0f * B + 4.0f * B / R0_OHM * r) - A) / (2.0f * B);
    if (t >= 0.0f) return t;

    float rn = r / R0_OHM * 100.0f;  /* PT100 기준으로 정규화 */
    float p = rn;
    t = -242.02f + 2.2228f * p;
    p *= rn; t += 2.5859e-3f * p;
    p *= rn; t -= 4.8260e-6f * p;
    p *= rn; t -= 2.8183e-8f * p;
    p *= rn; t += 1.5243e-10f * p;
    return t;
}

bool max31865_init(spi_host_device_t host, gpio_num_t sclk, gpio_num_t mosi, gpio_num_t miso,
                   gpio_num_t cs, bool three_wire)
{
    spi_bus_config_t bus = {
        .sclk_io_num   = sclk,
        .mosi_io_num   = mosi,
        .miso_io_num   = miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    if (spi_bus_initialize(host, &bus, SPI_DMA_DISABLED) != ESP_OK) {
        ESP_LOGE(TAG, "MAX31865 spi_bus_initialize failed (CLK=%d SDI=%d SDO=%d)", sclk, mosi, miso);
        return false;
    }
    spi_device_interface_config_t dev = {
        .mode           = 1,
        .clock_speed_hz = SPI_CLOCK_HZ,
        .spics_io_num   = -1,   /* CS는 직접 제어(CS_SETUP_US) */
        .queue_size     = 1,
    };
    s_cs = cs;
    gpio_reset_pin(cs);
    gpio_set_direction(cs, GPIO_MODE_OUTPUT);
    gpio_set_level(cs, 1);
    if (spi_bus_add_device(host, &dev, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "MAX31865 spi_bus_add_device failed (CS=%d)", cs);
        return false;
    }

    s_base_cfg = three_wire ? CFG_3WIRE : 0;
    uint8_t cfg = 0xFF;
    if (!write_reg(MAX_REG_CONFIG, s_base_cfg | CFG_FAULT_CLEAR) || !read_regs(MAX_REG_CONFIG, &cfg, 1)) {
        ESP_LOGE(TAG, "MAX31865 SPI transfer failed");
        return false;
    }
    /* 고장 지우기 비트(D1)는 스스로 0으로 돌아가서 되읽을 때 빠짐 */
    if (cfg != s_base_cfg) {
        ESP_LOGW(TAG, "MAX31865 not responding (config wrote 0x%02X, read 0x%02X)", s_base_cfg, cfg);
        return false;
    }
    const uint16_t hi = HIGH_THRESH_CODE << 1, lo = LOW_THRESH_CODE << 1;
    if (!write_reg(MAX_REG_HIGH_THRESH, hi >> 8) || !write_reg(MAX_REG_HIGH_THRESH + 1, hi & 0xFF) ||
        !write_reg(MAX_REG_LOW_THRESH, lo >> 8) || !write_reg(MAX_REG_LOW_THRESH + 1, lo & 0xFF)) {
        ESP_LOGE(TAG, "MAX31865 threshold write failed");
        return false;
    }
    ESP_LOGI(TAG, "MAX31865 SPI OK  CLK=%d SDI=%d SDO=%d CS=%d %s-wire", sclk, mosi, miso, cs,
             three_wire ? "3" : "2/4");
    return true;
}

max31865_result_t max31865_read(float *temp_c, uint8_t *fault_status)
{
    if (fault_status) *fault_status = 0;
    if (!s_dev) return MAX31865_ERR_NO_CHIP;

    /* 바이어스를 켜고 되읽어 칩 확인 — 부팅 뒤 선이 빠졌거나 칩이 응답하지 않으면 여기서 걸림.
     * 고장 지우기는 고장이 났을 때만 아래에서 따로 씀 */
    uint8_t cfg = 0xFF;
    if (!write_reg(MAX_REG_CONFIG, s_base_cfg | CFG_BIAS)) return MAX31865_ERR_SPI;
    if (!read_regs(MAX_REG_CONFIG, &cfg, 1)) return MAX31865_ERR_SPI;
    if (cfg != (s_base_cfg | CFG_BIAS)) {
        static int s_last_logged = -1;  /* 같은 값이면 매 측정마다 되풀이하지 않음 */
        if (cfg != s_last_logged) {
            ESP_LOGW(TAG, "MAX31865 not responding (config wrote 0x%02X, read 0x%02X)", s_base_cfg | CFG_BIAS, cfg);
            s_last_logged = cfg;
        }
        return MAX31865_ERR_NO_CHIP;
    }

    vTaskDelay(pdMS_TO_TICKS(BIAS_SETTLE_MS));
    if (!write_reg(MAX_REG_CONFIG, s_base_cfg | CFG_BIAS | CFG_1SHOT)) return MAX31865_ERR_SPI;
    vTaskDelay(pdMS_TO_TICKS(CONVERSION_MS));

    uint8_t rtd[2] = { 0 };
    bool ok = read_regs(MAX_REG_RTD_MSB, rtd, 2);
    write_reg(MAX_REG_CONFIG, s_base_cfg);  /* 바이어스 끔(RTD 자기발열·소비전류 줄임) */
    if (!ok) return MAX31865_ERR_SPI;

    if (rtd[1] & 0x01) {
        uint8_t fs = 0;
        read_regs(MAX_REG_FAULT_STATUS, &fs, 1);
        write_reg(MAX_REG_CONFIG, s_base_cfg | CFG_FAULT_CLEAR);
        if (fault_status) *fault_status = fs;
        return MAX31865_ERR_FAULT;
    }

    uint16_t code = (uint16_t)(((uint16_t)rtd[0] << 8 | rtd[1]) >> 1);  /* 15비트 ADC 코드 */
    float r = (float)code * RREF_OHM / 32768.0f;
    *temp_c = rtd_to_celsius(r);
    return MAX31865_OK;
}
