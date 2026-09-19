#include "sens_kind_store.h"
#include "sd_storage.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "sens_kind_store";

#define SENS_KIND_FILE_PATH SD_STORAGE_MOUNT_POINT "/sens_kind.bin"
#define SENS_KIND_SLOTS 8  /* device_config.c의 ALIAS_SLOTS(알려진 장치 상한)와 동일 관례 */
#define SENS_KIND_FILE_VERSION 1

typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    uint8_t kind;
    uint8_t in_use;
} sens_kind_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t          version;
    sens_kind_entry_t entries[SENS_KIND_SLOTS];
} sens_kind_file_t;

static sens_kind_entry_t s_entries[SENS_KIND_SLOTS];

static void save(void)
{
    FILE *f = fopen(SENS_KIND_FILE_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "저장 실패(fopen): %s", SENS_KIND_FILE_PATH);
        return;
    }
    sens_kind_file_t s = { .version = SENS_KIND_FILE_VERSION };
    memcpy(s.entries, s_entries, sizeof(s.entries));
    fwrite(&s, sizeof(s), 1, f);
    fclose(f);
}

void sens_kind_store_load(void)
{
    memset(s_entries, 0, sizeof(s_entries));

    FILE *f = fopen(SENS_KIND_FILE_PATH, "rb");
    if (!f) return;  /* 미장착/최초부팅 등 — 빈 상태로 시작 */
    sens_kind_file_t s = { 0 };
    bool ok = (fread(&s, sizeof(s), 1, f) == 1) && s.version == SENS_KIND_FILE_VERSION;
    fclose(f);
    if (!ok) {
        ESP_LOGW(TAG, "형식 불일치 — 빈 상태로 시작: %s", SENS_KIND_FILE_PATH);
        return;
    }
    memcpy(s_entries, s.entries, sizeof(s_entries));
    ESP_LOGI(TAG, "복원 완료");
}

static sens_kind_entry_t *find_slot(const uint8_t mac[6])
{
    for (int i = 0; i < SENS_KIND_SLOTS; i++) {
        if (s_entries[i].in_use && memcmp(s_entries[i].mac, mac, 6) == 0) return &s_entries[i];
    }
    return NULL;
}

uint8_t sens_kind_store_get(const uint8_t mac[6])
{
    sens_kind_entry_t *e = find_slot(mac);
    return e ? e->kind : 0;  /* SENSOR_KIND_UNKNOWN */
}

void sens_kind_store_set(const uint8_t mac[6], uint8_t kind)
{
    sens_kind_entry_t *e = find_slot(mac);
    if (e && e->kind == kind) return;  /* 이미 같은 값 — 불필요한 SD 쓰기 생략 */
    if (!e) {
        for (int i = 0; i < SENS_KIND_SLOTS; i++) {
            if (!s_entries[i].in_use) { e = &s_entries[i]; break; }
        }
        if (!e) {
            ESP_LOGW(TAG, "슬롯 꽉 참(%d개) — 저장 못 함", SENS_KIND_SLOTS);
            return;
        }
        memcpy(e->mac, mac, 6);
        e->in_use = 1;
    }
    e->kind = kind;
    save();
}

int sens_kind_store_get_all(uint8_t out_macs[][6], uint8_t out_kinds[], int out_cap)
{
    int n = 0;
    for (int i = 0; i < SENS_KIND_SLOTS && n < out_cap; i++) {
        if (!s_entries[i].in_use) continue;
        memcpy(out_macs[n], s_entries[i].mac, 6);
        out_kinds[n] = s_entries[i].kind;
        n++;
    }
    return n;
}
