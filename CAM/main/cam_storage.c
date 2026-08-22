#include "cam_storage.h"
#include "bsp_esp32s3_cam.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "cam_storage";

static sdmmc_card_t *s_card = NULL;

/* file_id 재설계(2026-08-01, 사용자 지시) — 예전엔 유닉스 타임스탬프(10자리)를 그대로
 * file_id 겸 파일명으로 써서 파일명이 길었고, Cntl이 file_id를 촬영시각으로 역해석하는
 * 편법을 씀. 이제 file_id = M/T 공용(전역) 순번 하나뿐 — 4자리 base36(0-9,A-Z,
 * 36^4=1,679,616개, 상주 파일 상한 500개 대비 충분히 여유)으로 파일명에 그대로 씀.
 * 별도 카운터 파일 없이, SD를 훑어서 가장 최근(mtime) 파일의 seq+1부터 계속 이어감
 * (2026-08-01, 사용자 지시 — "마지막 날짜/시간의 파일명에서 계속 순환 증가시키면 됨").
 * M/T가 순번을 공유해서 같은 seq가 두 kind에 동시에 존재할 수 없으므로 file_id만으로
 * 유일하게 식별됨(kind는 resolve_path에서 둘 다 시도해서 찾음). 촬영시각은 file_id에서
 * 뽑는 대신 파일의 FAT mtime을 그대로 읽어서 별도로 전달(정렬/최근시간 필터/오래된순
 * 삭제도 전부 mtime 기준 — file_id 자체엔 이제 시간 순서 정보가 없음) */
#define CAM_STORAGE_SEQ_BASE   36u
#define CAM_STORAGE_SEQ_DIGITS 4
#define CAM_STORAGE_SEQ_MOD    1679616u  /* 36^4 */

static const char s_base36_digits[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static void encode_seq(uint32_t seq, char *out /* 5바이트: 4자리+NUL */)
{
    seq %= CAM_STORAGE_SEQ_MOD;
    for (int i = CAM_STORAGE_SEQ_DIGITS - 1; i >= 0; i--) {
        out[i] = s_base36_digits[seq % CAM_STORAGE_SEQ_BASE];
        seq /= CAM_STORAGE_SEQ_BASE;
    }
    out[CAM_STORAGE_SEQ_DIGITS] = '\0';
}

/* 0-9/A-Z 아니면 -1 */
static int decode_base36_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

static void file_path_kind(uint32_t file_id, char kind, char *out, size_t out_len)
{
    char seq_str[CAM_STORAGE_SEQ_DIGITS + 1];
    encode_seq(file_id, seq_str);
    snprintf(out, out_len, "%s/%c%s.jpg", BSP_CAM_SD_MOUNT_POINT, kind, seq_str);
}

/* 2026-08-21 — 워밍업 프레임 화질 확인용 임시 진단 kind('A'/'B', CAM_WARMUP_FRAME_COUNT=2
 * 프레임 기준 하나씩) 추가. M/T와 똑같이 목록/삭제/순번공유 대상에 포함시켜서 Cntl 목록에서
 * 실제 촬영본(M)과 나란히 보고 비교할 수 있게 함 — 적정 워밍업 프레임 수가 확정되면
 * (cam_node.c의 CAM_WARMUP_FRAME_COUNT 주석 참고) 이 kind들과 관련 코드는 지워도 됨,
 * 정식 기능 아님 */
#define CAM_STORAGE_KIND_COUNT 4
static const char s_valid_kinds[CAM_STORAGE_KIND_COUNT] = {
    CAM_CAPTURE_KIND_MANUAL, CAM_CAPTURE_KIND_AUTO, 'A', 'B'
};

static bool is_valid_kind(char kind)
{
    for (int i = 0; i < CAM_STORAGE_KIND_COUNT; i++) {
        if (s_valid_kinds[i] == kind) return true;
    }
    return false;
}

/* file_id(=순번)만으로는 어느 kind인지 몰라서 등록된 kind를 전부 시도 — 전부 순번을
 * 공유하는 한 같은 seq가 두 kind에 동시에 존재할 수 없어서 결과는 항상 유일함 */
static bool resolve_path(uint32_t file_id, char *out, size_t out_len, char *out_kind)
{
    for (int i = 0; i < CAM_STORAGE_KIND_COUNT; i++) {
        file_path_kind(file_id, s_valid_kinds[i], out, out_len);
        struct stat st;
        if (stat(out, &st) == 0) {
            if (out_kind) *out_kind = s_valid_kinds[i];
            return true;
        }
    }
    return false;
}

typedef struct {
    uint32_t file_id;
    char     kind;
    time_t   mtime;   /* FAT 수정시각 — 정렬/필터/순환삭제/다음 순번 결정 전부 이 기준 */
    uint32_t size;    /* 2026-08-11 추가 — scan_all_files()가 어차피 stat()을 이미 하므로
                        * st_size를 공짜로 같이 담아둠(목록 전송이 파일마다 또 stat() 안
                        * 해도 되게, cam_storage_list_full() 참고) */
} file_entry_t;

/* 파일명 길이 = kind(1) + 순번(CAM_STORAGE_SEQ_DIGITS) + ".jpg"(4) */
#define CAM_STORAGE_FNAME_LEN (1 + CAM_STORAGE_SEQ_DIGITS + 4)

/* /sdcard 안의 <M|T><4자리 base36>.jpg 패턴 파일들을 전부 훑어서 배열에 채움(정렬 안 된
 * 상태로) — cam_storage_list()/용량 관리/다음 순번 결정이 공유 */
static int scan_all_files(file_entry_t *entries, int max)
{
    DIR *dir = opendir(BSP_CAM_SD_MOUNT_POINT);
    if (!dir) return -1;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        size_t len = strlen(ent->d_name);
        if (len != CAM_STORAGE_FNAME_LEN /* "M002A.jpg" */) continue;
        char kind = ent->d_name[0];
        if (!is_valid_kind(kind)) continue;
        if (strcmp(ent->d_name + 1 + CAM_STORAGE_SEQ_DIGITS, ".jpg") != 0) continue;

        uint32_t seq = 0;
        bool ok = true;
        for (int i = 1; i <= CAM_STORAGE_SEQ_DIGITS; i++) {
            int d = decode_base36_digit(ent->d_name[i]);
            if (d < 0) { ok = false; break; }
            seq = seq * CAM_STORAGE_SEQ_BASE + (uint32_t)d;
        }
        if (!ok) continue;

        char path[64];
        snprintf(path, sizeof(path), "%s/%.*s", BSP_CAM_SD_MOUNT_POINT, CAM_STORAGE_FNAME_LEN, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        entries[count].file_id = seq;
        entries[count].kind    = kind;
        entries[count].mtime   = st.st_mtime;
        entries[count].size    = (uint32_t)st.st_size;
        count++;
    }
    closedir(dir);
    return count;
}

/* 정렬/최신판정/순환삭제는 전부 file_id 기준(2026-08-02, 사용자 지시: "mtime으로 개체를
 * 처리하는 건 아주 잘못된거야. 무조건 ID로 해야지") — file_id 자체가 이미 M/T 공용으로
 * 촬영마다 1씩 증가하는 순번이라 mtime(FAT는 보통 2초 단위라 연속촬영 시 자주 겹침)보다
 * 오히려 더 정확한 순서 기준임. mtime은 실제 시각이 필요한 곳(capture_time 표시,
 * RECENT_HOURS 필터의 now-N시간 컷오프 비교)에만 남겨둠 — file_id 자체엔 절대시각
 * 정보가 없어서 그 두 용도는 mtime이 아니면 할 수가 없음. */
static int cmp_by_file_id(const void *a, const void *b)
{
    const file_entry_t *ea = (const file_entry_t *)a, *eb = (const file_entry_t *)b;
    if (ea->file_id < eb->file_id) return -1;
    if (ea->file_id > eb->file_id) return 1;
    return 0;
}

/* 상한 넘으면 오래된 것부터 순환삭제하고, 다음에 쓸 순번(가장 최근 파일의 seq+1, 파일이
 * 하나도 없으면 0)을 반환 — 스캔을 한 번만 해서 용량관리와 순번결정을 같이 처리 */
static uint32_t enforce_capacity_and_get_next_seq(void)
{
    file_entry_t all[CAM_STORAGE_MAX_FILES + 1];
    int count = scan_all_files(all, CAM_STORAGE_MAX_FILES + 1);
    if (count <= 0) return 0;

    qsort(all, count, sizeof(all[0]), cmp_by_file_id);  /* 오래된 것부터 */

    if (count > CAM_STORAGE_MAX_FILES) {
        int to_delete = count - CAM_STORAGE_MAX_FILES;
        for (int i = 0; i < to_delete; i++) {
            char path[64];
            file_path_kind(all[i].file_id, all[i].kind, path, sizeof(path));
            if (remove(path) == 0) {
                ESP_LOGI(TAG, "순환 삭제: %s", path);
            } else {
                ESP_LOGW(TAG, "순환 삭제 실패: %s", path);
            }
        }
    }

    return (all[count - 1].file_id + 1) % CAM_STORAGE_SEQ_MOD;
}

esp_err_t cam_storage_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,  /* 마운트 실패해도 카드 내용을 함부로 안 지움 */
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;  /* 1-bit 모드 — Waveshare 예제(SD_MMC.begin(mode1bit=true))와 동일 */
    slot_cfg.clk   = BSP_CAM_SD_CLK;
    slot_cfg.cmd   = BSP_CAM_SD_CMD;
    slot_cfg.d0    = BSP_CAM_SD_D0;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_CAM_SD_MOUNT_POINT, &host, &slot_cfg,
                                             &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD 마운트 실패: %s (SD카드 있는지, IO 익스팬더 인에이블 핀 확인)",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SD 마운트 완료 (%s, %lluMB)", BSP_CAM_SD_MOUNT_POINT,
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

esp_err_t cam_storage_get_sd_usage(uint32_t *out_total_kb, uint32_t *out_used_kb)
{
    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(BSP_CAM_SD_MOUNT_POINT, &total_bytes, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD 용량 조회 실패: %s", esp_err_to_name(err));
        *out_total_kb = 0;
        *out_used_kb  = 0;
        return err;
    }
    *out_total_kb = (uint32_t)(total_bytes / 1024);
    *out_used_kb  = (uint32_t)((total_bytes - free_bytes) / 1024);
    return ESP_OK;
}

esp_err_t cam_storage_save_capture(const uint8_t *jpeg_data, size_t len, cam_capture_kind_t kind, uint32_t *out_file_id)
{
    uint32_t seq = enforce_capacity_and_get_next_seq();
    char path[64];
    file_path_kind(seq, (char)kind, path, sizeof(path));

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "캡처 저장 실패(열기): %s", path);
        return ESP_FAIL;
    }
    size_t written = fwrite(jpeg_data, 1, len, fp);
    fclose(fp);

    if (written != len) {
        ESP_LOGE(TAG, "캡처 저장 실패(쓰기 %u/%u): %s", (unsigned)written, (unsigned)len, path);
        remove(path);
        return ESP_FAIL;
    }

    if (out_file_id) *out_file_id = seq;
    ESP_LOGI(TAG, "캡처 저장: %s (%u bytes)", path, (unsigned)len);
    return ESP_OK;
}

int cam_storage_list(photo_request_mode_t mode, uint32_t param, uint32_t *out_file_ids, int max)
{
    file_entry_t all[CAM_STORAGE_MAX_FILES];
    int total = scan_all_files(all, CAM_STORAGE_MAX_FILES);
    if (total < 0) return -1;
    qsort(all, total, sizeof(all[0]), cmp_by_file_id);  /* 오래된 것부터 */

    if (mode == PHOTO_REQUEST_MODE_LATEST) {
        if (total == 0 || max < 1) return 0;
        out_file_ids[0] = all[total - 1].file_id;
        return 1;
    }

    if (mode == PHOTO_REQUEST_MODE_BY_ID) {
        if (max < 1) return 0;
        for (int i = 0; i < total; i++) {
            if (all[i].file_id == param) {
                out_file_ids[0] = param;
                return 1;
            }
        }
        return 0;  /* 목록 가져온 뒤 사용자가 고르기 전에 이미 지워진 경우 등 */
    }

    time_t cutoff = 0;
    if (mode == PHOTO_REQUEST_MODE_RECENT_HOURS) {
        time_t now = time(NULL);
        time_t window = (time_t)param * 3600;
        cutoff = (window < now) ? (now - window) : 0;
    }

    int count = 0;
    for (int i = 0; i < total && count < max; i++) {
        if (mode == PHOTO_REQUEST_MODE_RECENT_HOURS && all[i].mtime < cutoff) continue;
        out_file_ids[count++] = all[i].file_id;
    }
    return count;
}

int cam_storage_list_full(esp_now_photo_list_item_t *out_items, int max)
{
    file_entry_t all[CAM_STORAGE_MAX_FILES];
    int total = scan_all_files(all, CAM_STORAGE_MAX_FILES);
    if (total < 0) return -1;
    qsort(all, total, sizeof(all[0]), cmp_by_file_id);  /* 오래된 것부터 */

    int count = 0;
    for (int i = 0; i < total && count < max; i++) {
        out_items[count].index        = (uint16_t)count;
        out_items[count].file_id      = all[i].file_id;
        out_items[count].kind         = (uint8_t)all[i].kind;
        out_items[count].capture_time = (uint32_t)all[i].mtime;
        out_items[count].file_size    = all[i].size;
        count++;
    }
    return count;
}

esp_err_t cam_storage_stat(uint32_t file_id, uint32_t *out_size, char *out_kind, uint32_t *out_capture_time)
{
    char path[64];
    char kind;
    if (!resolve_path(file_id, path, sizeof(path), &kind)) return ESP_ERR_NOT_FOUND;

    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;

    if (out_size) *out_size = (uint32_t)st.st_size;
    if (out_kind) *out_kind = kind;
    if (out_capture_time) *out_capture_time = (uint32_t)st.st_mtime;
    return ESP_OK;
}

esp_err_t cam_storage_open_read(uint32_t file_id, FILE **out_fp, uint32_t *out_size)
{
    char path[64];
    if (!resolve_path(file_id, path, sizeof(path), NULL)) return ESP_ERR_NOT_FOUND;

    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_ERR_NOT_FOUND;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);
    if (size < 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    *out_fp   = fp;
    *out_size = (uint32_t)size;
    return ESP_OK;
}

esp_err_t cam_storage_delete(uint32_t file_id)
{
    char path[64];
    if (!resolve_path(file_id, path, sizeof(path), NULL)) return ESP_ERR_NOT_FOUND;
    if (remove(path) != 0) return ESP_FAIL;
    return ESP_OK;
}

/* 2026-08-21 — DELETE_ALL_RECEIVED(esp_now_cam.c)이 삭제 시작 "전"에 개수를 먼저 알려야 해서
 * 신설. cam_storage_delete_all()과 완전히 같은 필터로 세기만 함(파일 열기/삭제 없음, readdir만
 * 도니까 빠름) — Cntl이 이 개수로 완료 대기 예산을 계산하므로 실제로 지워질 개수와 반드시
 * 일치해야 함(필터 로직 수정 시 두 함수 같이 맞출 것) */
int cam_storage_count_files(void)
{
    DIR *dir = opendir(BSP_CAM_SD_MOUNT_POINT);
    if (!dir) return -1;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len != CAM_STORAGE_FNAME_LEN) continue;
        if (!is_valid_kind(ent->d_name[0])) continue;
        if (strcmp(ent->d_name + 1 + CAM_STORAGE_SEQ_DIGITS, ".jpg") != 0) continue;
        count++;
    }
    closedir(dir);
    return count;
}

int cam_storage_delete_all(void)
{
    DIR *dir = opendir(BSP_CAM_SD_MOUNT_POINT);
    if (!dir) return -1;

    int deleted = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len != CAM_STORAGE_FNAME_LEN) continue;
        if (!is_valid_kind(ent->d_name[0])) continue;
        if (strcmp(ent->d_name + 1 + CAM_STORAGE_SEQ_DIGITS, ".jpg") != 0) continue;

        char path[64];
        snprintf(path, sizeof(path), "%s/%.*s", BSP_CAM_SD_MOUNT_POINT, CAM_STORAGE_FNAME_LEN, ent->d_name);
        if (remove(path) == 0) deleted++;
    }
    closedir(dir);
    return deleted;
}
