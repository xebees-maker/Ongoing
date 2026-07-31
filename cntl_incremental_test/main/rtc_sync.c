#include "rtc_sync.h"
#include "waveshare_rgb_lcd_port.h"
#include "pcf85063a.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"

static const char *TAG = "rtc_sync";

/* PCF85063A는 전원이 나가면(배터리 없음) 연도 레지스터가 1970으로 리셋됨 — 이보다 한참
 * 뒤인 값이면 "그럴듯한 시각이 이미 들어있다"로 보고 안 건드림 */
#define RTC_SANE_YEAR_MIN 2020

static pcf85063a_dev_t s_rtc_dev;
static bool            s_rtc_ready = false;

/* __DATE__ "Mmm dd yyyy" / __TIME__ "hh:mm:ss" — 컴파일 시각을 파싱해서 RTC 초기 seed로 씀 */
static bool parse_build_datetime(struct tm *out)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon_str[4] = { 0 };
    int day = 0, year = 0, hour = 0, min = 0, sec = 0;

    if (sscanf(__DATE__, "%3s %d %d", mon_str, &day, &year) != 3) return false;
    if (sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec) != 3) return false;

    const char *pos = strstr(months, mon_str);
    if (!pos) return false;
    int month = (int)((pos - months) / 3);  /* 0-based */

    memset(out, 0, sizeof(*out));
    out->tm_year = year - 1900;
    out->tm_mon  = month;
    out->tm_mday = day;
    out->tm_hour = hour;
    out->tm_min  = min;
    out->tm_sec  = sec;
    return true;
}

esp_err_t rtc_sync_init(void)
{
    i2c_master_bus_handle_t bus = waveshare_rgb_lcd_get_i2c_bus();
    if (!bus) {
        ESP_LOGW(TAG, "I2C 버스가 아직 없음 — waveshare_esp32_s3_rgb_lcd_init() 먼저 호출했는지 확인");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = pcf85063a_init(&s_rtc_dev, bus, PCF85063A_ADDRESS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063A 초기화 실패: %s (RTC 미실장/배선 문제?)", esp_err_to_name(err));
        return err;
    }
    s_rtc_ready = true;

    pcf85063a_datetime_t rtc_time = { 0 };
    esp_err_t read_err = pcf85063a_get_time_date(&s_rtc_dev, &rtc_time);
    bool need_seed = (read_err != ESP_OK) || (rtc_time.year < RTC_SANE_YEAR_MIN);

    if (need_seed) {
        struct tm build_tm;
        if (parse_build_datetime(&build_tm)) {
            pcf85063a_datetime_t seed = {
                .year  = (uint16_t)(build_tm.tm_year + 1900),
                .month = (uint8_t)(build_tm.tm_mon + 1),
                .day   = (uint8_t)build_tm.tm_mday,
                .dotw  = 0,
                .hour  = (uint8_t)build_tm.tm_hour,
                .min   = (uint8_t)build_tm.tm_min,
                .sec   = (uint8_t)build_tm.tm_sec,
            };
            esp_err_t set_err = pcf85063a_set_time_date(&s_rtc_dev, seed);
            ESP_LOGW(TAG, "RTC 값이 유효하지 않음(배터리 없음/최초 부팅) — 빌드 시각으로 초기화: %s",
                     esp_err_to_name(set_err));
            rtc_time = seed;
        } else {
            ESP_LOGW(TAG, "빌드 시각 파싱 실패 — RTC를 그대로 둠");
        }
    }

    struct tm tm_val = {
        .tm_year = rtc_time.year - 1900,
        .tm_mon  = rtc_time.month - 1,
        .tm_mday = rtc_time.day,
        .tm_hour = rtc_time.hour,
        .tm_min  = rtc_time.min,
        .tm_sec  = rtc_time.sec,
    };
    time_t t = mktime(&tm_val);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    ESP_LOGI(TAG, "RTC 시각: %04u-%02u-%02u %02u:%02u:%02u",
             rtc_time.year, rtc_time.month, rtc_time.day, rtc_time.hour, rtc_time.min, rtc_time.sec);
    return ESP_OK;
}

uint32_t rtc_sync_get_unix_time(void)
{
    return (uint32_t)time(NULL);
}
