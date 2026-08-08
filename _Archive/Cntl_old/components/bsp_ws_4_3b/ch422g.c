/**
 * @file    ch422g.c
 * @brief   CH422G IO 익스팬더 드라이버 구현
 */
#include "ch422g.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ch422g";

/* 이 칩은 레지스터 오프셋이 없고 "기능=I2C 슬레이브 주소" 구조 —
 * Waveshare 공식 데모/IO_Test 예제(CH422G.h)에서 그대로 가져온 값 */
#define CH422G_ADDR_MODE     0x24
#define CH422G_ADDR_OD_OUT   0x23
#define CH422G_ADDR_IO_OUT   0x38
#define CH422G_ADDR_IO_IN    0x26

#define CH422G_I2C_TIMEOUT_MS  100

static i2c_master_dev_handle_t s_dev_mode   = NULL;
static i2c_master_dev_handle_t s_dev_od_out = NULL;
static i2c_master_dev_handle_t s_dev_io_out = NULL;
static i2c_master_dev_handle_t s_dev_io_in  = NULL;

/* 마지막으로 쓴 IO_OUT/OD_OUT 바이트를 기억해뒀다가 비트 단위로 수정 —
 * 이 칩엔 "현재 출력값을 그대로 읽는" 별도 레지스터가 없음(IO_IN은 항상 실제 핀
 * 전압을 읽으므로 출력모드일 땐 우리가 쓴 값과 같아야 하지만, 입력모드 전환 없이
 * 조용히 확인할 방법은 섀도우 상태뿐) */
static uint8_t s_io_out_shadow = 0;
static uint8_t s_od_out_shadow = 0;

static esp_err_t write_byte(i2c_master_dev_handle_t dev, uint8_t value)
{
    return i2c_master_transmit(dev, &value, 1, CH422G_I2C_TIMEOUT_MS);
}

static esp_err_t add_dev(i2c_master_bus_handle_t bus, uint16_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    return i2c_master_bus_add_device(bus, &cfg, out);
}

esp_err_t ch422g_init(i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_ERROR(add_dev(bus, CH422G_ADDR_MODE,   &s_dev_mode),   TAG, "add mode dev failed");
    ESP_RETURN_ON_ERROR(add_dev(bus, CH422G_ADDR_OD_OUT, &s_dev_od_out), TAG, "add od_out dev failed");
    ESP_RETURN_ON_ERROR(add_dev(bus, CH422G_ADDR_IO_OUT, &s_dev_io_out), TAG, "add io_out dev failed");
    ESP_RETURN_ON_ERROR(add_dev(bus, CH422G_ADDR_IO_IN,  &s_dev_io_in),  TAG, "add io_in dev failed");

    /* 실제 레지스터 쓰기는 여기서 하지 않음 — I2C 디바이스 핸들 등록만.
     * GT911 리셋(bsp_touch_reset_via_ch422g)이 CH422G에 대한 첫 번째 실제
     * 트랜잭션이어야 함(Waveshare 공식 코드 순서 그대로 재현, 실기에서 이 순서를
     * 지켜야 터치가 응답한다는 게 확인됨 — 미리 다른 쓰기를 해두면 실패). */
    s_io_out_shadow = 0;
    s_od_out_shadow = 0;

    ESP_LOGI(TAG, "init OK (devices registered, no writes yet)");
    return ESP_OK;
}

esp_err_t ch422g_set_io_raw(uint8_t mode_value, uint8_t io_value)
{
    ESP_RETURN_ON_ERROR(write_byte(s_dev_mode, mode_value), TAG, "raw mode write failed");
    s_io_out_shadow = io_value;
    return write_byte(s_dev_io_out, io_value);
}

esp_err_t ch422g_set_io(uint8_t bits, bool level)
{
    if (level) {
        s_io_out_shadow |= bits;
    } else {
        s_io_out_shadow &= (uint8_t)~bits;
    }
    /* Waveshare 공식 CH422G_io_output()은 IO_OUT을 쓰기 직전마다 매번 Mode를
     * IO_OE 단독값(0x01)으로 다시 씀 — OD_EN을 같이 켜두면(0x05) IO뱅크 쪽
     * 출력이 제대로 안 나가는 걸로 실기에서 확인됨(데이터시트 주석엔 OD_EN이
     * OC0~3에만 영향 준다지만 실제로는 IO뱅크 드라이브에도 영향 있는 듯).
     * OD_EN은 ch422g_set_do()에서 그때그때 따로 켠다. */
    esp_err_t err = write_byte(s_dev_mode, CH422G_MODE_IO_OE);
    if (err != ESP_OK) {
        return err;
    }
    return write_byte(s_dev_io_out, s_io_out_shadow);
}

esp_err_t ch422g_read_di(bool *out_di0, bool *out_di1)
{
    esp_err_t err;

    /* 뱅크 전체를 잠깐 입력모드로 — 이 사이 백라이트/LCD리셋/SD_CS도 하이임피던스가
     * 되므로 최대한 짧게 유지하고 바로 복원한다 */
    err = write_byte(s_dev_mode, 0);  /* IO_OE=0(입력) */
    if (err != ESP_OK) {
        return err;
    }

    uint8_t value = 0;
    err = i2c_master_receive(s_dev_io_in, &value, 1, CH422G_I2C_TIMEOUT_MS);

    /* 출력모드로 즉시 복원 후 이전 섀도우 값 재적용 */
    esp_err_t restore_err = write_byte(s_dev_mode, CH422G_MODE_IO_OE);
    esp_err_t rewrite_err = write_byte(s_dev_io_out, s_io_out_shadow);
    if (err == ESP_OK) {
        err = (restore_err != ESP_OK) ? restore_err : rewrite_err;
    }
    if (err != ESP_OK) {
        return err;
    }

    if (out_di0) {
        *out_di0 = (value & CH422G_IO_DI0) != 0;
    }
    if (out_di1) {
        *out_di1 = (value & CH422G_IO_DI1) != 0;
    }
    return ESP_OK;
}

esp_err_t ch422g_set_do(uint8_t bits, bool level)
{
    if (level) {
        s_od_out_shadow |= bits;
    } else {
        s_od_out_shadow &= (uint8_t)~bits;
    }
    esp_err_t err = write_byte(s_dev_mode, CH422G_MODE_OD_EN);
    if (err != ESP_OK) {
        return err;
    }
    return write_byte(s_dev_od_out, s_od_out_shadow);
}
