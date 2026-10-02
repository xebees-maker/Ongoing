/**
 * @file    ch422g.c
 * @brief   CH422G IO 익스팬더 드라이버 구현
 */
#include "ch422g.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
/* 2026-09-29 — 화면 끄기(LVGL 태스크의 백라이트 IO2)와 SD 재연결(IO4 SD_CS)이 실행 중에 서로 다른 태스크에서 같은
 * 출력 그림자를 읽고-고치고-쓰므로 잠금으로 직렬화(예전엔 백라이트를 부팅 때 한 번만 써서 필요 없었음) */
static SemaphoreHandle_t s_lock = NULL;
static StaticSemaphore_t s_lock_buf;
static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }
/* 2026-10-02(SSR 연결) — OC0~3 출력 그림자. 회로상 OC가 Low일 때 절연 출력(DOUT)이 켜지므로(포토커플러 LED가 3V3→510R→OC로
 * 흐름) 0x0F(전부 High = 꺼짐)로 시작 — 예전 초기값 0이면 DO0을 처음 쓸 때 DO1에도 Low가 같이 써져 SSR2가 켜졌음 */
#define CH422G_OD_ALL_OFF 0x0F
static uint8_t s_od_out_shadow = CH422G_OD_ALL_OFF;
/* 2026-10-02 — 모드 레지스터는 IO_OE만 씀(set_io/set_do/read_di 복원 모두 같게). 예전 set_do는 0x04(CH422G_MODE_OD_EN)만 써서
 * IO_OE를 지웠고(백라이트·SD_CS·LCD 리셋이 입력으로 바뀜), 0x04를 IO_OE와 함께 쓰자 GT911 터치 I2C 에러가 계속 났음(실측 25초 2,820회).
 * OC는 기본(푸시풀)으로도 충분함: High=3.3V면 포토커플러 LED(3V3→510R→OC)가 꺼지고 Low면 켜짐 */
static uint8_t mode_bits(bool io_out) { return io_out ? CH422G_MODE_IO_OE : 0; }

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
     * GT911 리셋(waveshare_esp32_s3_touch_reset)이 CH422G에 대한 첫 번째 실제
     * 트랜잭션이어야 함(Waveshare 공식 코드 순서 그대로 재현, 실기에서 이 순서를
     * 지켜야 터치가 응답한다는 게 확인됨 — 미리 다른 쓰기를 해두면 실패). */
    s_io_out_shadow = 0;
    s_od_out_shadow = CH422G_OD_ALL_OFF;
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);

    ESP_LOGI(TAG, "init OK (devices registered, no writes yet)");
    return ESP_OK;
}

esp_err_t ch422g_set_io_raw(uint8_t mode_value, uint8_t io_value)
{
    lock();
    esp_err_t err = write_byte(s_dev_mode, mode_value);
    if (err != ESP_OK) {
        unlock();
        ESP_LOGE(TAG, "raw mode write failed: %s", esp_err_to_name(err));
        return err;
    }
    s_io_out_shadow = io_value;
    err = write_byte(s_dev_io_out, io_value);
    unlock();
    return err;
}

esp_err_t ch422g_set_io(uint8_t bits, bool level)
{
    lock();
    if (level) {
        s_io_out_shadow |= bits;
    } else {
        s_io_out_shadow &= (uint8_t)~bits;
    }
    /* Waveshare 공식 CH422G_io_output()은 IO_OUT을 쓰기 직전마다 매번 Mode를
     * IO_OE 단독값(0x01)으로 다시 씀 — OD_EN을 같이 켜두면(0x05) IO뱅크 쪽
     * 출력이 제대로 안 나가는 걸로 실기에서 확인됨(데이터시트 주석엔 OD_EN이
     * OC0~3에만 영향 준다지만 실제로는 IO뱅크 드라이브에도 영향 있는 듯).
     * 2026-10-02 — ch422g_set_do()도 OD_EN 없이 IO_OE만 씀(위 mode_bits 주석). */
    esp_err_t err = write_byte(s_dev_mode, mode_bits(true));
    if (err == ESP_OK) err = write_byte(s_dev_io_out, s_io_out_shadow);
    unlock();
    return err;
}

esp_err_t ch422g_read_di(bool *out_di0, bool *out_di1)
{
    esp_err_t err;

    /* 뱅크 전체를 잠깐 입력모드로 — 이 사이 백라이트/LCD리셋/SD_CS도 하이임피던스가
     * 되므로 최대한 짧게 유지하고 바로 복원한다 */
    lock();
    err = write_byte(s_dev_mode, mode_bits(false));  /* IO_OE=0(입력) */
    if (err != ESP_OK) {
        unlock();
        return err;
    }

    uint8_t value = 0;
    err = i2c_master_receive(s_dev_io_in, &value, 1, CH422G_I2C_TIMEOUT_MS);

    /* 출력모드로 즉시 복원 후 이전 섀도우 값 재적용 */
    esp_err_t restore_err = write_byte(s_dev_mode, mode_bits(true));
    esp_err_t rewrite_err = write_byte(s_dev_io_out, s_io_out_shadow);
    unlock();
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
    lock();
    if (level) {
        s_od_out_shadow |= bits;
    } else {
        s_od_out_shadow &= (uint8_t)~bits;
    }
    esp_err_t err = write_byte(s_dev_mode, mode_bits(true));
    if (err == ESP_OK) err = write_byte(s_dev_od_out, s_od_out_shadow);
    unlock();
    return err;
}
