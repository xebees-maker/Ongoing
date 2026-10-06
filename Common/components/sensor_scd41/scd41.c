/**
 * @file    scd41.c
 * @brief   SCD41 CO2/온습도 센서 — I2C 드라이버 (Sensirion 프로토콜)
 */

#include "scd41.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"

static const char *TAG = "SENS";

#define CMD_WAKE_UP                     0x36F6
#define CMD_STOP_PERIODIC_MEASUREMENT   0x3F86
#define CMD_START_PERIODIC_MEASUREMENT  0x21B1
#define CMD_MEASURE_SINGLE_SHOT         0x219D  /* Sensirion SCD4x single-shot — 데이터시트 대조 권장 */
#define CMD_GET_DATA_READY_STATUS       0xE4B8
#define CMD_READ_MEASUREMENT            0xEC05
#define CMD_REINIT                      0x3646

#define I2C_TIMEOUT_MS         1000
#define REINIT_FAIL_THRESHOLD  3       /* 연속 통신 실패 횟수 — 도달 시 측정 재시작 시퀀스 재실행 */
#define STALE_TIMEOUT_MS       16000   /* 마지막 성공 측정 이후 이만큼 지나면 무에러 idle 고착으로 간주 */

static i2c_master_dev_handle_t  s_dev = NULL;
static i2c_master_bus_handle_t  s_bus = NULL;
static int                      s_fail_count = 0;
static bool                     s_reinit_in_progress = false;
static TickType_t               s_last_success_tick = 0;
/* 2026-09-06(사용자 지적 — "이런 실수가 너무 많다") — scd41_init_single_shot()만 고치고
 * force_reinit()이 여전히 start_measurement_sequence()(WAKE_UP+STOP+**START_PERIODIC**)를
 * 부르는 걸 놓쳤던 실수를 고침. force_reinit()은 note_failure()/check_stale()을 통해
 * data_ready()/read_measurement_frame() 안에서 자동으로 걸리므로, 싱글샷 전용 호출부
 * (scd41_init_single_shot)에서도 그대로 타서 싱글샷 도중에 컨티뉴어스가 다시 켜지는
 * 사고가 났음 — 이 플래그로 분기 */
static bool                     s_single_shot_mode = false;

/* single-shot 듀티사이클 상태 — continuous 모드(위 워치독들)와는 별개 경로. 2026-09-06 —
 * 예전엔 트리거 시각을 RTC/틱으로 기억해뒀다가 "데이터시트 최대값(5000ms) 지났나"만 보고
 * 판단했는데, data_ready()로 실제 준비 여부를 직접 물어보게 바꾸면서 더 이상 필요 없어짐 */
static bool s_single_shot_pending = false;

/* 2026-09-29 — 마지막 실패 원인(sensor_fault_t 값과 같음, 0=없음) */
static int s_last_fault = 0;

int scd41_last_fault(void) { return s_last_fault; }

/* 2026-10-05(진단, 임시 — SCD41 단발 측정이 계속 NOT_READY) — 이번 트리거 뒤 data_ready 폴 횟수·마지막 상태 원값 */
static int      s_diag_polls = 0;
static uint16_t s_diag_status = 0xFFFF;  /* 0xFFFF = 상태를 한 번도 못 받음 */
void scd41_diag_get(int *polls, uint16_t *last_status) { *polls = s_diag_polls; *last_status = s_diag_status; }
void scd41_clear_fault(void) { s_last_fault = 0; }

static bool start_measurement_sequence(void);

static void recover_bus(void)
{
    if (s_bus) i2c_master_bus_reset(s_bus);
}

/* 버스 리셋만으로는 복구되지 않는 상황(브라운아웃 등으로 센서가 주기 측정
 * 상태를 잃어버린 경우)에 대비해 wake_up/stop/start 시퀀스를 다시 실행한다.
 * start_measurement_sequence() 자체도 send_cmd()를 호출하므로, 재초기화 중
 * 발생하는 실패가 다시 재초기화를 트리거하지 않도록 재진입을 막는다. */
static void force_reinit(const char *reason)
{
    if (s_reinit_in_progress) return;

    ESP_LOGW(TAG, "%s - sensor reinit attempt", reason);
    s_fail_count = 0;
    s_reinit_in_progress = true;
    recover_bus();

    /* 2026-09-06(위 s_single_shot_mode 주석 참고) — 싱글샷 전용 경로는 여기서도 컨티뉴어스를
     * 절대 시작하면 안 됨. I2C 버스 리셋까지만 하고 끝 — 다음 싱글샷 트리거는 호출부
     * (scd41_trigger_single_shot)가 알아서 다시 함 */
    if (s_single_shot_mode) {
        s_reinit_in_progress = false;
        s_last_success_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "Single-shot mode - I2C bus reset only (no continuous restart)");
        return;
    }

    bool ok = start_measurement_sequence();
    s_reinit_in_progress = false;
    s_last_success_tick = xTaskGetTickCount();  /* 재시도 폭주 방지 — 다음 측정까지는 정상으로 간주 */

    if (ok) {
        ESP_LOGI(TAG, "Sensor reinit OK - periodic measurement restarted");
    } else {
        ESP_LOGW(TAG, "Sensor reinit failed - retry on next failure/stall");
    }
}

static void note_failure(void)
{
    /* 2026-10-05(실기 — 재초기화가 몰아침) — 단발 모드는 재초기화를 호출부(센스 측정 코드) 한 곳에서만 판단. 드라이버가 따로
     * 실패를 세면 두 재초기화가 서로의 실행 시간을 모른 채 겹침. 실패 때 버스 리셋(recover_bus)은 그대로 */
    if (s_single_shot_mode) return;
    if (s_reinit_in_progress) return;
    if (++s_fail_count < REINIT_FAIL_THRESHOLD) return;
    force_reinit("repeated I2C failures");
}

static void note_success(void)
{
    s_fail_count = 0;
    s_last_success_tick = xTaskGetTickCount();
}

/* I2C 자체는 정상 응답(ACK)하지만 센서가 주기 측정을 하지 않는 idle 상태로
 * 고착된 경우 — 에러가 안 나서 note_failure()가 트리거되지 않으므로,
 * 마지막 성공 측정 이후 경과 시간으로 별도 감지한다. */
static void check_stale(void)
{
    if (s_single_shot_mode) return;  /* note_failure와 같은 이유 */
    if (s_reinit_in_progress) return;
    if (s_last_success_tick == 0) return;  /* 아직 기준 시각 없음 (초기화 직후) */
    if ((xTaskGetTickCount() - s_last_success_tick) < pdMS_TO_TICKS(STALE_TIMEOUT_MS)) return;
    force_reinit("measurement stalled (no data ready for 16s+)");
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
    ESP_LOGW(TAG, "send_cmd(0x%04X) failed: %s", cmd, esp_err_to_name(err));
    s_last_fault = 1;  /* SENSOR_FAULT_CMD_NACK */
    recover_bus();
    note_failure();
    return false;
}

static bool start_measurement_sequence(void)
{
    /* SCD41은 슬립 모드 지원 — wake_up은 NACK이 정상(깨우는 용도), 결과 무시 */
    send_cmd(CMD_WAKE_UP);
    vTaskDelay(pdMS_TO_TICKS(30));

    /* 이전 상태와 무관하게 정지 후 재시작 (정지 명령 실패는 무시) */
    bool stop_ok = send_cmd(CMD_STOP_PERIODIC_MEASUREMENT);
    ESP_LOGD(TAG, "stop_periodic_measurement: %s", stop_ok ? "ACK" : "NACK(ignored)");
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool start_ok = false;
    for (int retry = 0; retry < 5 && !start_ok; retry++) {
        start_ok = send_cmd(CMD_START_PERIODIC_MEASUREMENT);
        if (!start_ok) {
            ESP_LOGW(TAG, "start_periodic_measurement failed (try %d/5)", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    return start_ok;
}

/* scd41_init()/scd41_init_single_shot() 공용 — 전용 I2C 버스 생성 + 디바이스 등록만.
 * 2026-09-06 — 두 초기화 경로가 공유하도록 분리(측정 시퀀스 시작 여부만 다름) */
static bool init_i2c_bus_and_device(int i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio)
{
    /* LP_I2C_SCLK_DEFAULT는 LP_I2C 페리페럴이 있는 칩(esp32c6 등)의 port 1 전용 —
     * esp32c3는 LP_I2C 자체가 없어서(SOC_I2C_NUM=1) 이 매크로도 미정의. 칩 역량
     * 매크로로 분기해서 legacy C6 콤보 보드 동작은 그대로 유지한다. */
    i2c_clock_source_t clk_src = I2C_CLK_SRC_DEFAULT;
#if SOC_LP_I2C_SUPPORTED
    if (i2c_port == 1) {
        clk_src = (i2c_clock_source_t)LP_I2C_SCLK_DEFAULT;
    }
#endif
    i2c_master_bus_config_t bus_cfg = {
        .clk_source            = clk_src,
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
    ESP_LOGI(TAG, "Dedicated I2C bus OK  SDA=%d SCL=%d", sda_gpio, scl_gpio);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SCD41_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed");
        return false;
    }
    return true;
}

bool scd41_init(int i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio)
{
    s_single_shot_mode = false;
    if (!init_i2c_bus_and_device(i2c_port, sda_gpio, scl_gpio)) return false;

    if (!start_measurement_sequence()) {
        ESP_LOGW(TAG, "No sensor response - check connection");
        return false;
    }
    s_last_success_tick = xTaskGetTickCount();

    ESP_LOGI(TAG, "SCD41 periodic measurement start (5s interval)");
    return true;
}

bool scd41_init_single_shot(int i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio)
{
    s_single_shot_mode = true;
    return init_i2c_bus_and_device(i2c_port, sda_gpio, scl_gpio);
}

static bool data_ready(void)
{
    if (!send_cmd(CMD_GET_DATA_READY_STATUS)) {
        ESP_LOGW(TAG, "data_ready: command send failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t resp[3] = { 0 };
    if (i2c_master_receive(s_dev, resp, sizeof(resp), I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "data_ready: response receive failed");
        s_last_fault = 4;  /* SENSOR_FAULT_READ */
        recover_bus();
        note_failure();
        return false;
    }
    if (crc8(resp, 2) != resp[2]) {
        ESP_LOGW(TAG, "data_ready: CRC mismatch");
        s_last_fault = 3;  /* SENSOR_FAULT_CRC */
        note_failure();  /* 통신은 됐지만 데이터가 깨짐 — 연속 실패 카운트에 포함시켜야
                           * REINIT_FAIL_THRESHOLD로 재초기화가 걸림(안 그러면 이 경로는
                           * note_failure()도 check_stale()도 안 타서 영원히 복구 안 됨) */
        return false;
    }

    uint16_t status = ((uint16_t)resp[0] << 8) | resp[1];
    s_diag_polls++;
    s_diag_status = status;
    bool ready = (status & 0x07FF) != 0;
    ESP_LOGD(TAG, "data_ready: status=0x%04X ready=%d", status, ready);
    if (!ready) check_stale();
    return ready;
}

/* CMD_READ_MEASUREMENT 전송 + 9바이트 수신/CRC검증/변환 — continuous(scd41_read)와
 * single-shot(scd41_poll_single_shot) 양쪽이 공유하는 실제 읽기 본문. 호출 전에 데이터가
 * 준비됐다는 건 호출자가 이미 확인했다고 가정(continuous는 data_ready(), single-shot은
 * 트리거 후 경과시간). success/failure 카운트(note_success/note_failure)는 continuous
 * 모드의 워치독(force_reinit)에서만 의미가 있지만, 실패 로그 자체는 공통으로 남긴다. */
static bool read_measurement_frame(int *co2_ppm, float *temperature, float *humidity)
{
    if (!send_cmd(CMD_READ_MEASUREMENT)) return false;
    vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t resp[9] = { 0 };
    if (i2c_master_receive(s_dev, resp, sizeof(resp), I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "read_measurement: response receive failed");
        s_last_fault = 4;  /* SENSOR_FAULT_READ */
        recover_bus();
        note_failure();
        return false;
    }

    for (int w = 0; w < 3; w++) {
        if (crc8(&resp[w * 3], 2) != resp[w * 3 + 2]) {
            ESP_LOGW(TAG, "CRC mismatch (word %d)", w);
            s_last_fault = 3;  /* SENSOR_FAULT_CRC */
            note_failure();  /* data_ready()와 동일 이유 — CRC 실패도 실패로 집계해야 함 */
            return false;
        }
    }

    note_success();

    uint16_t co2_raw  = ((uint16_t)resp[0] << 8) | resp[1];
    uint16_t temp_raw = ((uint16_t)resp[3] << 8) | resp[4];
    uint16_t humi_raw = ((uint16_t)resp[6] << 8) | resp[7];

    if (co2_ppm)     *co2_ppm     = co2_raw;
    if (temperature) *temperature = -45.0f + 175.0f * ((float)temp_raw / 65536.0f);
    if (humidity)     *humidity   = 100.0f * ((float)humi_raw / 65536.0f);

    ESP_LOGD(TAG, "co2=%u ppm  temp_raw=%u  humi_raw=%u", co2_raw, temp_raw, humi_raw);
    return true;
}

bool scd41_read(int *co2_ppm, float *temperature, float *humidity)
{
    if (!s_dev) return false;
    if (!data_ready()) return false;
    return read_measurement_frame(co2_ppm, temperature, humidity);
}

bool scd41_stop_periodic_measurement(void)
{
    if (!s_dev) return false;
    return send_cmd(CMD_STOP_PERIODIC_MEASUREMENT);
}

bool scd41_reinit(void)
{
    if (!s_dev) return false;
    /* 데이터시트: reinit 전에 stop 필수, stop 뒤 500ms는 다른 명령을 받지 않음. 싱글샷 모드라 주기 측정은 원래
     * 꺼져 있지만 절차대로 보냄(센서가 어떤 상태로 갇혀 있는지 모르므로) */
    bool stop_ok = send_cmd(CMD_STOP_PERIODIC_MEASUREMENT);
    vTaskDelay(pdMS_TO_TICKS(500));
    bool reinit_ok = send_cmd(CMD_REINIT);
    vTaskDelay(pdMS_TO_TICKS(30));  /* 2026-10-05 — 데이터시트 reinit 실행 시간 30ms(예전 20ms) — 그동안 다른 명령 금지 */
    s_single_shot_pending = false;
    ESP_LOGW(TAG, "SCD41 reinit: stop=%s reinit=%s", stop_ok ? "ACK" : "NACK", reinit_ok ? "ACK" : "NACK");
    return reinit_ok;
}

/* 2026-10-05(진단, 임시) — 센서 일련번호(0x3682)·종류(get_sensor_variant 0x202F, 데이터시트: 상위 4비트 0=SCD40, 1=SCD41)를
 * 읽어 로그로 남김. 둘 다 유휴 상태에서만 받는 명령, 실행 시간 1ms */
static bool read_words(uint16_t cmd, uint16_t *w, int n)
{
    if (!send_cmd(cmd)) return false;
    vTaskDelay(pdMS_TO_TICKS(2));
    uint8_t resp[9] = { 0 };
    if (i2c_master_receive(s_dev, resp, (size_t)n * 3, I2C_TIMEOUT_MS) != ESP_OK) return false;
    for (int i = 0; i < n; i++) {
        if (crc8(&resp[i * 3], 2) != resp[i * 3 + 2]) return false;
        w[i] = ((uint16_t)resp[i * 3] << 8) | resp[i * 3 + 1];
    }
    return true;
}

void scd41_log_identity(void)
{
    if (!s_dev) return;
    uint16_t sn[3] = { 0 }, var = 0;
    bool sn_ok = read_words(0x3682, sn, 3);
    bool var_ok = read_words(0x202F, &var, 1);
    ESP_LOGW(TAG, "SCD4x identity: serial=%s%04X%04X%04X variant=%s0x%04X",
             sn_ok ? "" : "(fail)", sn[0], sn[1], sn[2], var_ok ? "" : "(fail)", var);

    /* 2026-10-05(사용자 지시 — 읽기만) — get_automatic_self_calibration_enabled(0x2313): 1 = ASC 켜짐, 0 = 꺼짐 */
    uint16_t asc = 0xFFFF;
    bool asc_ok = read_words(0x2313, &asc, 1);
    ESP_LOGW(TAG, "SCD4x ASC: %s%u", asc_ok ? "" : "(read fail) ", (unsigned)asc);

}

/* 2026-10-05(사용자 결정 — SCD41 개선 3단계) — ASC 끄기 + persist_settings, 센서당 한 번만.
 * 근거: Sensirion 저전력 앱노트 5.2 — ASC가 켜져 있으면 단발 48번마다 보정 이력을 EEPROM에 씀(15초 간격이면 하루 약 120번,
 * persist 수명 2000회 이상). EEPROM 쓰기는 "켜져 있을 때만" — 끈 뒤로는 읽기만 하므로 전원을 여러 번 켜도 센서당 한 번.
 * 저장 뒤 다시 읽어도 켜져 있으면 이번 전원 동안은 다시 쓰지 않음(RTC 메모리 — 딥슬립에도 유지, 전원 끄면 지워짐) */
static RTC_DATA_ATTR bool s_asc_write_tried = false;

void scd41_ensure_asc_off(void)
{
    if (!s_dev) return;
    uint16_t asc = 0xFFFF;
    if (!read_words(0x2313, &asc, 1)) { ESP_LOGW(TAG, "SCD4x ASC: read fail - not writing"); return; }
    if (asc == 0) { ESP_LOGI(TAG, "SCD4x ASC: off (no write)"); return; }
    if (s_asc_write_tried) { ESP_LOGE(TAG, "SCD4x ASC: still on after persist this power cycle - not writing again"); return; }
    s_asc_write_tried = true;
    uint8_t buf[5] = { 0x24, 0x16, 0x00, 0x00, 0 };  /* set_automatic_self_calibration_enabled(0) */
    buf[4] = crc8(&buf[2], 2);
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(2));
    bool persist_ok = (err == ESP_OK) && send_cmd(0x3615);  /* persist_settings — 실행 800ms */
    vTaskDelay(pdMS_TO_TICKS(800));
    uint16_t after = 0xFFFF;
    bool rb_ok = read_words(0x2313, &after, 1);
    ESP_LOGW(TAG, "SCD4x ASC: was on -> set=%s persist=%s readback=%s%u",
             err == ESP_OK ? "ACK" : "NACK", persist_ok ? "ACK" : "NACK", rb_ok ? "" : "(fail) ", (unsigned)after);
}

/* 2026-10-06(시험 — Sensirion 단발 예제 그대로) — 공식 wakeUp(): 0x36F6을 보내고 결과는 무시, 30ms 대기.
 * 유휴 상태에선 NACK이 정상이라 send_cmd(실패 시 버스 리셋·로그)를 쓰지 않음 */
void scd41_wake_up(void)
{
    if (!s_dev) return;
    uint8_t buf[2] = { (uint8_t)(CMD_WAKE_UP >> 8), (uint8_t)(CMD_WAKE_UP & 0xFF) };
    (void)i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(30));
}

bool scd41_trigger_single_shot(void)
{
    if (!s_dev) return false;
    s_diag_polls = 0;
    s_diag_status = 0xFFFF;
    if (!send_cmd(CMD_MEASURE_SINGLE_SHOT)) return false;
    s_single_shot_pending = true;
    return true;
}

/* 2026-09-06(사용자 지적) — 예전엔 "데이터시트 최대값(5000ms)만큼 소프트웨어 타이머로
 * 기다렸다가 딱 한 번만 읽고, 그게 실패하면(센서가 그 언저리에서 아주 조금만 더 걸려도)
 * 재시도 없이 포기"하는 구조였음. 실제로 센서에 "값 준비됐냐"를 물어본 적이 한 번도 없이
 * 그냥 시간만 재고 있었던 것 — continuous 모드(scd41_read)가 이미 쓰고 있는
 * CMD_GET_DATA_READY_STATUS(data_ready())로 실제 준비 여부를 물어보도록 통일함. 통상
 * 1초 정도면 준비될 수 있고(사용자 언급, 확인 필요) 늦어도 데이터시트 최대값 안에서 준비되면
 * 그 즉시 잡아냄 — 고정 타이머 추측이 아니라 센서가 직접 답하는 값 기준 */
bool scd41_poll_single_shot(int *co2_ppm, float *temperature, float *humidity, bool *out_ok)
{
    if (!s_dev || !s_single_shot_pending) return false;
    if (!data_ready()) return false;  /* 아직 준비 안 됨 — 호출자가 다음 폴에서 다시 확인 */
    *out_ok = read_measurement_frame(co2_ppm, temperature, humidity);
    s_single_shot_pending = false;  /* 성공/실패 무관 이번 사이클 종료 — 다음 트리거는 호출자 책임 */
    return true;
}
