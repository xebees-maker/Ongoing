#include "bridge_log.h"
#include "esp_now_link.h"
#include "esp_now.h"

#include <stdio.h>
#include <string.h>

void bridge_log_mac6(const uint8_t mac[6], char out[7])
{
    snprintf(out, 7, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

const char *bridge_log_err_short(esp_err_t err)
{
    static char buf[24];
    if (err == ESP_OK) return "OK";
    const char *name = esp_err_to_name(err);
    if (strncmp(name, "ESP_ERR_", 8) == 0) {
        snprintf(buf, sizeof(buf), "ERR-%s", name + 8);
        return buf;
    }
    if (strcmp(name, "ESP_FAIL") == 0) return "ERR-FAIL";
    return name;  /* 매칭 안 되는 드문 케이스는 원본 그대로(길어도 정보는 유지) */
}

const char *bridge_log_msg_type_name(uint8_t msg_type)
{
    /* esp_now_link.h의 메시지 종류 전체 — 로그 한 줄이 워드랩되지 않게 짧게 */
    switch (msg_type) {
        case ESP_NOW_MSG_ADVERTISE:        return "ADVERTISE";
        case ESP_NOW_MSG_PAIR_REQUEST:     return "PAIR_REQ";
        case ESP_NOW_MSG_PAIR_ACK:         return "PAIR_ACK";
        case ESP_NOW_MSG_SENSOR_DATA:      return "SENS_DATA";
        case ESP_NOW_MSG_ADVERTISE_ACK:    return "ADV_ACK";
        case ESP_NOW_MSG_PHOTO_REQUEST:    return "PH_REQ";
        case ESP_NOW_MSG_PHOTO_META:       return "PH_META";
        case ESP_NOW_MSG_PHOTO_CHUNK:      return "PH_CHUNK";
        case ESP_NOW_MSG_PHOTO_DONE:       return "PH_DONE";
        case ESP_NOW_MSG_CAM_CONFIG_SET:   return "CAM_CFG";
        case ESP_NOW_MSG_UNPAIR:           return "UNPAIR";
        case ESP_NOW_MSG_CAPTURE_STATUS:   return "CAP_STAT";
        case ESP_NOW_MSG_PHOTO_DONE_ACK:   return "PH_DONE_ACK";
        case ESP_NOW_MSG_CAPTURE_STATUS_ACK: return "CAP_STAT_ACK";
        case ESP_NOW_MSG_PHOTO_WINDOW_STATUS_REQUEST: return "WIN_REQ";
        case ESP_NOW_MSG_PHOTO_WINDOW_STATUS_ACK:     return "WIN_ACK";
        case ESP_NOW_MSG_CAM_CONFIG_ACK:   return "CAM_CFG_ACK";
        case ESP_NOW_MSG_SLEEP_NOW:        return "SLEEP";
        case ESP_NOW_MSG_SLEEP_NOW_ACK:    return "SLEEP_ACK";
        case ESP_NOW_MSG_PHOTO_META_ACK:   return "PH_META_ACK";
        case ESP_NOW_MSG_WAKE_HELLO:       return "WAKE_HELLO";
        case ESP_NOW_MSG_WAKE_HELLO_ACK:   return "WAKE_ACK";
        case ESP_NOW_MSG_UNPAIR_ACK:       return "UNPAIR_ACK";
        case ESP_NOW_MSG_CASK_WORK_NONE:   return "WORK_NONE";
        case ESP_NOW_MSG_CASK_WORK_NONE_ACK: return "WORK_NONE_ACK";
        case ESP_NOW_MSG_WAKE_HELLO_SENS:  return "WAKE_SENS";
        case ESP_NOW_MSG_WAKE_HELLO_SENS_ACK: return "WAKE_SENS_ACK";
        case ESP_NOW_MSG_SENS_CONFIG_SET:  return "SENS_CFG";
        case ESP_NOW_MSG_SENS_CONFIG_ACK:  return "SENS_CFG_ACK";
        default: {
            static char buf[12];
            snprintf(buf, sizeof(buf), "#%u", msg_type);
            return buf;
        }
    }
}

const char *bridge_log_result_code(esp_err_t err)
{
    static char buf[12];
    switch (err) {
        case ESP_OK:                    return "OK";
        case ESP_ERR_TIMEOUT:           return "TO";
        case ESP_ERR_ESPNOW_NOT_FOUND:  return "NP";
        case ESP_FAIL:                  return "FL";
        case ESP_ERR_NO_MEM:            return "NM";
        case ESP_ERR_INVALID_ARG:       return "BA";
        case ESP_ERR_INVALID_STATE:     return "BS";
        default:
            snprintf(buf, sizeof(buf), "E%d", (int)err);
            return buf;
    }
}
