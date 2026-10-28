#include "photo_storage.h"
#include "sd_storage.h"
#include "storage_mgr.h"
#include "sensor_registry.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <stddef.h>
#include "sd_health.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "PHOTO";

/* 2026-10-28(사용자 설계 — 고정 자리, sensor_registry.h) — 캠 보드 주소 → 그 캠 자리의 저장 키. 대치된 캠은 옛 캠의 사진 폴더를
 * 이어 씀. 주소를 받는 공개 함수마다 첫머리에서(키를 다시 넣어도 그대로 — 키 주소의 보드는 붙어 있지 않음) */
#define SLOT_KEY(mac) uint8_t slot_key_[6]; sreg_cam_storage_mac((mac), slot_key_); (mac) = slot_key_

/* 파일명: <kind 1글자><8자리 십진수 순번>.jpg — 카메라 폴더 하나당 최대 99,999,999장
 * (콘은 CAM의 예전 500장 링버퍼와 달리 영구 저장소라 base36 4자리보다 훨씬 넉넉하게 잡음,
 * 십진수라 별도 인코더 없이 snprintf/strtoul로 충분) */
#define PHOTO_SEQ_DIGITS  8
#define PHOTO_FNAME_LEN   (1 + PHOTO_SEQ_DIGITS + 4)  /* kind + 8자리 + ".jpg" */

/* 2026-09-26(SD 안전정책 — 쓰다가 리셋돼도 반쯤 쓴 파일이 사진으로 안 보이게) — 먼저 이 확장자로
 * 다 쓰고 fsync+close한 뒤 최종 이름(.jpg)으로 rename. 재스캔 때 남아있는 .tmp는 지움 */
#define PHOTO_TMP_SUFFIX  ".tmp"

/* 2026-09-26(재스캔 방어) — 사진 한 장이 이보다 크면 손상 의심(OV5640 최대 약 1MB — 여유 있게) */
#define PHOTO_MAX_SANE_BYTES (16u * 1024u * 1024u)

/* ---- RAM 상태(2026-09-26, storage_mgr.h 참고) — 사용량 합계와 카메라별 다음 순번.
 * 예전엔 사용량을 매번 폴더 전체 스캔으로 구했고(LVGL 태스크에서 약 765ms), 저장할 때마다
 * next_seq_in_dir()로 카메라 폴더를 스캔했음. 이제 재스캔 때 한 번만 훑고 이후엔 여기서 관리 */
/* 2026-10-01(할 일 H — 사진 목록 폴더 스캔을 LVGL 태스크 밖으로) — 카메라별 사진 색인(순번 오름차순). 예전엔 카메라
 * 팝업이 열리거나 갱신될 때마다 LVGL 태스크가 그 카메라 폴더를 통째로 훑었음(get_count 1번 + read_page 2번 + 정렬).
 * 이제 재스캔(마운트/재연결/포맷 직후, 파일처리 태스크) 때 한 번 만들고, 저장/삭제/정리 때 더하고 뺌 — 화면은 RAM만
 * 읽음(stats_store의 RAM 색인과 같은 방식). 예전 "카메라별 다음 순번 캐시"도 여기로 합침. 전부 PSRAM */
#define PHOTO_IDX_CAM_CAP 16  /* 사진 폴더가 있는 카메라 수 상한(지금까지 2대) */
typedef struct {
    uint32_t seq;
    uint32_t size;
    uint32_t mtime;
    uint8_t  kind;
    uint16_t nslots;   /* 2026-10-15(3단계) — 칸 사진이면 차지한 칸 수 */
    uint32_t slot;     /* 2026-10-15 — 칸 사진이면 첫 칸 번호(전체), 예전 .jpg 파일이면 UINT32_MAX */
    uint32_t crc;      /* 2026-10-15 — 칸 사진의 JPEG CRC32(읽을 때 확인) */
} photo_idx_entry_t;

typedef struct {
    bool               used;
    uint8_t            mac[6];
    uint32_t           next_seq;
    photo_idx_entry_t *items;   /* 순번 오름차순 */
    uint32_t           count;
    uint32_t           cap;
} photo_idx_cam_t;

static SemaphoreHandle_t s_mutex = NULL;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_used_bytes = 0;
static photo_idx_cam_t *s_cams = NULL;  /* PHOTO_IDX_CAM_CAP개, 처음 쓸 때 PSRAM 할당 */

static void lock(void)
{
    if (!s_mutex) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_mutex_init_lock);
        if (!s_mutex) { s_mutex = m; m = NULL; }
        taskEXIT_CRITICAL(&s_mutex_init_lock);
        if (m) vSemaphoreDelete(m);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static void used_add(int64_t delta)
{
    lock();
    if (delta < 0 && (uint64_t)(-delta) > s_used_bytes) s_used_bytes = 0;
    else s_used_bytes = (uint64_t)((int64_t)s_used_bytes + delta);
    unlock();
}

static void mac_to_hex(const uint8_t mac[6], char out[13])
{
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* mac_to_hex()의 역변환 — 폴더명(12자리 소문자 hex)이 아니면 false */
static bool hex_to_mac(const char *hex, uint8_t out[6])
{
    if (strlen(hex) != 12) return false;
    for (int i = 0; i < 6; i++) {
        unsigned byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return false;
        out[i] = (uint8_t)byte;
    }
    return true;
}

static void camera_dir_path(const uint8_t mac[6], char *out, size_t out_len)
{
    char mac_hex[13];
    mac_to_hex(mac, mac_hex);
    snprintf(out, out_len, "%s/photos/%s", SD_STORAGE_MOUNT_POINT, mac_hex);
}

static bool is_valid_kind(uint8_t kind)
{
    return kind == 'M' || kind == 'T';
}

static void file_path_for(const uint8_t mac[6], uint8_t kind, uint32_t seq, char *out, size_t out_len)
{
    char dir_path[64];
    camera_dir_path(mac, dir_path, sizeof(dir_path));
    snprintf(out, out_len, "%s/%c%0*u.jpg", dir_path, (char)kind, PHOTO_SEQ_DIGITS, (unsigned)seq);
}

/* 파일명("<kind><8자리>.jpg")을 파싱 — 형식이 아니면 false */
static bool parse_fname(const char *d_name, uint8_t *out_kind, uint32_t *out_seq)
{
    size_t len = strlen(d_name);
    if (len != PHOTO_FNAME_LEN) return false;
    if (!is_valid_kind((uint8_t)d_name[0])) return false;
    if (strcmp(d_name + 1 + PHOTO_SEQ_DIGITS, ".jpg") != 0) return false;

    char seq_str[PHOTO_SEQ_DIGITS + 1];
    memcpy(seq_str, d_name + 1, PHOTO_SEQ_DIGITS);
    seq_str[PHOTO_SEQ_DIGITS] = '\0';
    char *end = NULL;
    uint32_t seq = (uint32_t)strtoul(seq_str, &end, 10);
    if (end != seq_str + PHOTO_SEQ_DIGITS) return false;

    *out_kind = (uint8_t)d_name[0];
    *out_seq = seq;
    return true;
}

/* 이 카메라 폴더 안에서 가장 큰 순번+1(비어있으면 0) — 폴더가 아예 없어도 0(첫 저장 때
 * mkdir로 새로 만듦) */
/* 2026-10-21(사용자 — 웹 사진 목록이 주기·수동 촬영 뒤 자동으로 안 바뀜) — 사진 목록이 바뀔 때마다(저장·지우기·재스캔) 늘어나는 번호.
 * 주화면 내용(웹 WebSocket)에 실어 웹 카메라 팝업이 목록을 다시 받게 함(기록 표의 rec과 같은 방식) */
static volatile uint32_t s_list_gen = 0;
uint32_t photo_storage_list_gen(void) { return s_list_gen; }
static void photo_list_changed(void)
{
    s_list_gen++;
    storage_mgr_notify_changed();
}

static uint32_t next_seq_in_dir(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    uint32_t max_seq_plus_one = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len != PHOTO_FNAME_LEN) continue;
        if (!is_valid_kind((uint8_t)ent->d_name[0])) continue;
        if (strcmp(ent->d_name + 1 + PHOTO_SEQ_DIGITS, ".jpg") != 0) continue;

        char seq_str[PHOTO_SEQ_DIGITS + 1];
        memcpy(seq_str, ent->d_name + 1, PHOTO_SEQ_DIGITS);
        seq_str[PHOTO_SEQ_DIGITS] = '\0';
        char *end = NULL;
        uint32_t seq = (uint32_t)strtoul(seq_str, &end, 10);
        if (end != seq_str + PHOTO_SEQ_DIGITS) continue;  /* 자릿수 전부 숫자가 아니었음 */

        if (seq + 1 > max_seq_plus_one) max_seq_plus_one = seq + 1;
    }
    closedir(dir);
    return max_seq_plus_one;
}

/* ---- 색인(호출부가 lock() 잡은 상태) ---- */

/* 이 카메라의 색인을 찾음. create면 없을 때 새로 만들고 다음 순번은 dir_path 폴더를 한 번 읽어 정함(재스캔이 끝나기 전에
 * 사진이 와도 기존 파일을 덮어쓰지 않게 — 예전 순번 캐시와 같은 처리). 꽉 찼거나 할당 실패면 NULL */
static photo_idx_cam_t *cam_find_locked(const uint8_t mac[6], bool create, const char *dir_path)
{
    if (!s_cams) {
        if (!create) return NULL;
        s_cams = heap_caps_calloc(PHOTO_IDX_CAM_CAP, sizeof(photo_idx_cam_t), MALLOC_CAP_SPIRAM);
        if (!s_cams) { ESP_LOGE(TAG, "Photo index alloc failed"); return NULL; }
    }
    int free_slot = -1;
    for (int i = 0; i < PHOTO_IDX_CAM_CAP; i++) {
        if (s_cams[i].used && memcmp(s_cams[i].mac, mac, 6) == 0) return &s_cams[i];
        if (!s_cams[i].used && free_slot < 0) free_slot = i;
    }
    if (!create) return NULL;
    if (free_slot < 0) { ESP_LOGW(TAG, "Photo index full (%d cameras)", PHOTO_IDX_CAM_CAP); return NULL; }
    photo_idx_cam_t *c = &s_cams[free_slot];
    memset(c, 0, sizeof(*c));
    c->used = true;
    memcpy(c->mac, mac, 6);
    c->next_seq = dir_path ? next_seq_in_dir(dir_path) : 0;
    return c;
}

/* 맨 뒤에 붙임(순서 신경 안 씀 — 재스캔은 다 넣고 cam_sort_locked로 한 번에 정렬) */
static bool cam_push_locked(photo_idx_cam_t *c, const photo_idx_entry_t *e)
{
    if (c->count == c->cap) {
        uint32_t ncap = c->cap ? c->cap * 2 : 64;
        photo_idx_entry_t *n = heap_caps_realloc(c->items, (size_t)ncap * sizeof(*n), MALLOC_CAP_SPIRAM);
        if (!n) { ESP_LOGE(TAG, "Photo index grow failed (%u items)", (unsigned)ncap); return false; }
        c->items = n;
        c->cap = ncap;
    }
    c->items[c->count++] = *e;
    return true;
}

static int cmp_entry_seq_asc(const void *a, const void *b)
{
    uint32_t sa = ((const photo_idx_entry_t *)a)->seq, sb = ((const photo_idx_entry_t *)b)->seq;
    return (sa > sb) - (sa < sb);
}

static void cam_sort_locked(photo_idx_cam_t *c)
{
    if (c->count > 1) qsort(c->items, c->count, sizeof(c->items[0]), cmp_entry_seq_asc);
}

/* 순번 오름차순을 지키며 추가(저장할 때 — 순번이 늘어나므로 보통 맨 뒤) */
static bool cam_add_locked(photo_idx_cam_t *c, const photo_idx_entry_t *e)
{
    if (!cam_push_locked(c, e)) return false;
    uint32_t pos = c->count - 1;
    while (pos > 0 && c->items[pos - 1].seq > e->seq) {
        c->items[pos] = c->items[pos - 1];
        pos--;
    }
    c->items[pos] = *e;
    return true;
}

static void cam_remove_locked(photo_idx_cam_t *c, uint8_t kind, uint32_t seq)
{
    for (uint32_t i = 0; i < c->count; i++) {
        if (c->items[i].seq == seq && c->items[i].kind == kind) {
            memmove(&c->items[i], &c->items[i + 1], (size_t)(c->count - i - 1) * sizeof(c->items[0]));
            c->count--;
            return;
        }
    }
}

static void index_remove(const uint8_t mac[6], uint8_t kind, uint32_t seq)
{
    lock();
    photo_idx_cam_t *c = cam_find_locked(mac, false, NULL);
    if (c) cam_remove_locked(c, kind, seq);
    unlock();
}

/* 이 카메라의 다음 순번을 꺼내고 1 올림 */
static uint32_t take_next_seq_locked(const uint8_t mac[6], const char *dir_path)
{
    photo_idx_cam_t *c = cam_find_locked(mac, true, dir_path);
    if (!c) return next_seq_in_dir(dir_path);  /* 색인 꽉 참(실제로는 안 옴) — 예전처럼 매번 폴더에서 */
    return c->next_seq++;
}

/* ════════════════════════════════════════════════════════════
 * 2026-10-15(SD 개선 3단계, 사용자 설계 — "사진칸", 512K) — 사진 칸 파일.
 * 예전엔 사진마다 .tmp 파일을 만들고 쓰고 이름을 바꿨음 — 사진 한 장마다 디렉터리 항목 생성·이름 바꾸기·새 클러스터
 * 할당이 일어났고, MPIO 카드 손상(10-15)은 사진 폴더가 새 클러스터를 받은 직후에 생겼음(Docs/한일 153).
 * 이제 /sdcard/pslot/sNNNN.bin(512KB 칸 64개 = 32MB)을 링으로 돌려 씀: 파일은 처음 한 바퀴 동안 필요할 때 하나씩
 * 만들고(데이터는 안 쓰고 클러스터만 잡음), 그 뒤로는 가장 오래된 칸부터 덮어씀 — 파일 생성·지우기·클러스터 할당·
 * 디렉터리 항목 추가가 없음. 칸 머리(첫 512바이트)가 사진 정보를 들고 있어 재스캔은 머리만 읽음.
 * 쓰는 순서: "쓰는 중" 머리 → 데이터 → "유효" 머리 → fsync(도중에 리셋되면 "쓰는 중"으로 남아 무시됨).
 * 읽을 때 머리·데이터 CRC를 확인 — 다르면 SD 이상(쓴 뒤 바뀐 손상도 여기서 잡힘).
 * 예전 .jpg 파일(photos/<mac>/)은 그대로 읽고 지우고 정리함(새 사진만 칸에 씀).
 * ════════════════════════════════════════════════════════════ */
#define SLOT_DIR        SD_STORAGE_MOUNT_POINT "/pslot"
#define SLOT_RING_PATH  SLOT_DIR "/ring.bin"
#define SLOT_BYTES      (512u * 1024u)
#define SLOT_PER_FILE   64u
#define SLOT_FILE_BYTES (SLOT_BYTES * SLOT_PER_FILE)   /* 32 MB */
#define SLOT_MAX_FILES  512u                           /* 16 GB */
#define SLOT_HDR_BYTES  512u
#define SLOT_MAGIC      0x31534C50u   /* "PLS1" */
#define SLOT_RING_MAGIC 0x31474E52u   /* "RNG1" */
#define SLOT_RING_PCT   67u           /* 카드의 67%(사진 예산 90%의 75% — 정리 문턱 90% 아래) */
#define SLOT_NONE       UINT32_MAX

enum { SLOT_ST_FREE = 0, SLOT_ST_WRITING = 1, SLOT_ST_VALID = 2 };

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t ring_id;   /* 이 링을 만들 때 정한 난수 — 예전 링·포맷 전 찌꺼기 머리를 사진으로 보지 않게 */
    uint8_t  state;
    uint8_t  kind;
    uint8_t  mac[6];
    uint32_t seq;
    uint32_t size;      /* JPEG 바이트 */
    uint32_t crc;       /* JPEG CRC32 */
    uint32_t mtime;
    uint16_t nslots;    /* 이 사진이 차지한 칸 수(머리는 첫 칸에만) */
    uint16_t rsv;
    uint32_t gen;       /* 쓴 순서(링에서 다음 칸을 찾는 데 씀) */
    uint32_t hcrc;      /* 위 필드들의 CRC32 */
} slot_hdr_t;

static uint32_t s_ring_id = 0;
static uint32_t s_slot_files = 0;        /* 만든 칸 파일 수 */
static uint32_t s_slot_files_max = 0;    /* 링 크기(파일 수) — 재스캔 때 카드 크기로 정함 */
static uint32_t s_next_slot = 0;         /* 다음에 쓸 칸(전체 번호) */
static uint32_t s_gen = 0;
static bool     s_slot_scanned = false;  /* 이 마운트에서 칸 재스캔을 했는지(안 했으면 저장 전에 먼저 — 링 상태 없이 칸 파일을 다시 만들지 않게) */
static void slot_rescan_locked(uint64_t sd_total);   /* 아래 */
static uint8_t *s_slot_io = NULL;        /* PSRAM, 재스캔의 머리 읽기용 512바이트(재스캔은 두 잠금을 다 잡음) */

static uint32_t hdr_crc(const slot_hdr_t *h)
{
    return esp_rom_crc32_le(0, (const uint8_t *)h, offsetof(slot_hdr_t, hcrc));
}

static void slot_file_path(uint32_t file_no, char *out, size_t n)
{
    snprintf(out, n, SLOT_DIR "/s%04u.bin", (unsigned)file_no);
}

static uint32_t nslots_for(uint32_t size)
{
    return (SLOT_HDR_BYTES + size + SLOT_BYTES - 1) / SLOT_BYTES;
}

/* 저장기 — 받는 동안 PSRAM에 모았다가 finish에서 한 번에 칸에 씀(같은 칸 파일을 두 곳에서 열지 않게 — FATFS_FS_LOCK=0) */
struct photo_storage_writer {
    uint8_t  *buf;       /* PSRAM */
    size_t    len;
    size_t    cap;
    uint8_t   mac[6];
    uint8_t   kind;
    bool      failed;
    bool      ext;       /* 2026-10-20 — buf가 호출부 것(캠별 고정 버퍼) — 늘리지도 놓지도 않음 */
};

/* 2026-09-28 — 재마운트/포맷 동안 사진 쓰기·읽기를 막는 잠금(잠금 순서: s_wmutex -> s_mutex(photo)) */
static SemaphoreHandle_t s_wmutex = NULL;
static portMUX_TYPE s_wmutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static void wlock(void)
{
    if (!s_wmutex) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_wmutex_init_lock);
        if (!s_wmutex) { s_wmutex = m; m = NULL; }
        taskEXIT_CRITICAL(&s_wmutex_init_lock);
        if (m) vSemaphoreDelete(m);
    }
    xSemaphoreTake(s_wmutex, portMAX_DELAY);
}

static void wunlock(void)
{
    xSemaphoreGive(s_wmutex);
}

/* 2026-10-14 — 색인에 이 사진이 있는지(읽기 실패가 SD 이상인지 판단용). 있으면 칸 정보도 */
static bool index_find(const uint8_t mac[6], uint8_t kind, uint32_t seq, photo_idx_entry_t *out)
{
    bool has = false;
    lock();
    photo_idx_cam_t *c = cam_find_locked(mac, false, NULL);
    for (uint32_t i = 0; c && i < c->count; i++) {
        if (c->items[i].seq == seq && c->items[i].kind == kind) { has = true; if (out) *out = c->items[i]; break; }
    }
    unlock();
    return has;
}

static bool index_has(const uint8_t mac[6], uint8_t kind, uint32_t seq)
{
    return index_find(mac, kind, seq, NULL);
}

static photo_storage_writer_t *begin_common(const uint8_t mac[6], uint8_t kind, uint8_t *ext_buf, size_t ext_cap);

photo_storage_writer_t *photo_storage_begin(const uint8_t mac[6], uint8_t kind)
{
    return begin_common(mac, kind, NULL, 0);
}

/* 2026-10-20(사용자 결정 — 사진 버퍼 재설계) — 캠별로 미리 잡아 둔 버퍼에 모음. 받는 중 재할당이 없음(예전 128K→256K→512K
 * 재할당이 힙 잠금 안에서 수백 KB를 복사하는 동안 인터럽트가 막혀 CAN 수신 FIFO가 넘쳤음) */
photo_storage_writer_t *photo_storage_begin_ext(const uint8_t mac[6], uint8_t kind, uint8_t *buf, size_t cap)
{
    if (!buf || cap == 0) return NULL;
    return begin_common(mac, kind, buf, cap);
}

static photo_storage_writer_t *begin_common(const uint8_t mac[6], uint8_t kind, uint8_t *ext_buf, size_t ext_cap)
{
    SLOT_KEY(mac);
    if (!sd_storage_is_mounted()) {
        ESP_LOGW(TAG, "Save start failed - SD not mounted");
        return NULL;
    }
    if (!is_valid_kind(kind)) {
        ESP_LOGW(TAG, "Save start failed - bad kind=%c", (char)kind);
        return NULL;
    }
    if (!sd_health_writes_allowed()) {   /* 2026-10-14 — SD 고장 상태면 쓰지 않음(재연결/포맷 검증 뒤 풀림) */
        ESP_LOGW(TAG, "Save start refused - SD faulted");
        return NULL;
    }
    photo_storage_writer_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM);
    if (!w) return NULL;
    if (ext_buf) {
        w->ext = true;
        w->buf = ext_buf;
        w->cap = ext_cap;
    } else {
        w->cap = 128 * 1024;
        w->buf = heap_caps_malloc(w->cap, MALLOC_CAP_SPIRAM);
        if (!w->buf) { heap_caps_free(w); return NULL; }
    }
    memcpy(w->mac, mac, 6);
    w->kind = kind;
    return w;
}

bool photo_storage_append(photo_storage_writer_t *w, const uint8_t *data, size_t len)
{
    if (!w || w->failed) return false;
    if (len == 0) return true;
    if (w->len + len > PHOTO_MAX_SANE_BYTES) { w->failed = true; ESP_LOGW(TAG, "Photo too large (%u bytes)", (unsigned)(w->len + len)); return false; }
    if (w->len + len > w->cap && w->ext) {
        w->failed = true;
        ESP_LOGW(TAG, "Photo larger than its fixed buffer (%u > %u)", (unsigned)(w->len + len), (unsigned)w->cap);
        return false;
    }
    if (w->len + len > w->cap) {
        size_t ncap = w->cap;
        while (ncap < w->len + len) ncap *= 2;
        uint8_t *nb = heap_caps_realloc(w->buf, ncap, MALLOC_CAP_SPIRAM);
        if (!nb) { w->failed = true; ESP_LOGW(TAG, "Photo buffer grow failed (%u bytes)", (unsigned)ncap); return false; }
        w->buf = nb;
        w->cap = ncap;
    }
    memcpy(w->buf + w->len, data, len);
    w->len += len;
    return true;
}

void photo_storage_abort(photo_storage_writer_t *w)
{
    if (!w) return;
    if (!w->ext) heap_caps_free(w->buf);
    heap_caps_free(w);
}

/* 호출부가 lock() — 링 기술 파일(ring.bin)을 읽거나 새로 만듦. 성공하면 s_ring_id 설정 */
static bool ring_load_or_create_locked(void)
{
    if (mkdir(SLOT_DIR, 0777) != 0 && errno != EEXIST) { ESP_LOGW(TAG, "Slot folder create failed (errno=%d)", errno); return false; }
    uint32_t r[3] = { 0 };
    FILE *f = fopen(SLOT_RING_PATH, "rb");
    if (f) {
        bool ok = fread(r, sizeof(r), 1, f) == 1;
        fclose(f);
        if (ok && r[0] == SLOT_RING_MAGIC && r[2] == esp_rom_crc32_le(0, (const uint8_t *)r, 8) && r[1] != 0) {
            s_ring_id = r[1];
            return true;
        }
        ESP_LOGW(TAG, "Slot ring descriptor bad - new ring (old slot photos not shown)");
    }
    r[0] = SLOT_RING_MAGIC;
    r[1] = esp_random() | 1u;
    r[2] = esp_rom_crc32_le(0, (const uint8_t *)r, 8);
    if (!sd_storage_chain_ok(SLOT_RING_PATH)) return false;   /* 2026-10-21 — 클러스터 연결이 끊긴 파일에 쓰면 FatFs가 빈 클러스터를 붙임(sd_storage_chain_ok) */
    f = fopen(SLOT_RING_PATH, "wb");
    bool ok = f && fwrite(r, sizeof(r), 1, f) == 1;
    if (f && (fflush(f) != 0 || fsync(fileno(f)) != 0)) ok = false;
    if (f) fclose(f);
    if (!ok) { ESP_LOGW(TAG, "Slot ring descriptor write failed (errno=%d)", errno); return false; }
    sd_health_count(SDH_C_CREATE, 1);
    s_ring_id = r[1];
    s_slot_files = 0;
    s_next_slot = 0;
    ESP_LOGI(TAG, "Slot ring created (id %08X)", (unsigned)s_ring_id);
    return true;
}

/* 칸 파일 file_no를 만듦 — 데이터는 쓰지 않고 크기만 늘려 클러스터를 잡음(FatFs f_lseek가 쓰기 모드에서 파일을 늘림) */
static bool slot_file_create(uint32_t file_no)
{
    char path[48];
    slot_file_path(file_no, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) == 0 && (uint64_t)st.st_size == SLOT_FILE_BYTES) return true;   /* 이미 있음 — 다시 만들지 않음(지우지 않게) */
    int64_t t0 = esp_timer_get_time();
    if (!sd_storage_chain_ok(path)) return false;   /* 2026-10-21 — 클러스터 연결이 끊긴 파일에 쓰면 FatFs가 빈 클러스터를 붙임(sd_storage_chain_ok) */
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "Slot file create failed: %s (errno=%d)", path, errno); return false; }
    uint8_t z = 0;
    bool ok = fseek(f, (long)SLOT_FILE_BYTES - 1, SEEK_SET) == 0 && fwrite(&z, 1, 1, f) == 1;
    if (ok) ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!ok) { ESP_LOGW(TAG, "Slot file extend failed: %s (errno=%d)", path, errno); unlink(path); return false; }
    sd_health_count(SDH_C_CREATE, 1);
    sd_health_count(SDH_C_FSYNC, 1);
    ESP_LOGI(TAG, "Slot file created: %s (%u MB, %ums)", path, (unsigned)(SLOT_FILE_BYTES >> 20),
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    return true;
}

/* 호출부가 lock() — [first, first+n) 칸과 겹치는 칸 사진을 색인에서 뺌. 겹치는 사진의 첫 칸이 범위 앞에 있으면(머리가 안 덮임)
 * 그 머리를 따로 지워야 하므로 그 칸 번호를 돌려줌(없으면 SLOT_NONE) */
static uint32_t slot_evict_locked(uint32_t first, uint32_t n)
{
    uint32_t stale_head = SLOT_NONE;
    for (int ci = 0; s_cams && ci < PHOTO_IDX_CAM_CAP; ci++) {
        photo_idx_cam_t *c = &s_cams[ci];
        if (!c->used) continue;
        uint32_t k = 0;
        for (uint32_t i = 0; i < c->count; i++) {
            const photo_idx_entry_t *e = &c->items[i];
            bool overlap = e->slot != SLOT_NONE && e->slot < first + n && e->slot + e->nslots > first;
            if (overlap) {
                if (e->slot < first) stale_head = e->slot;
                continue;
            }
            c->items[k++] = *e;
        }
        c->count = k;
    }
    return stale_head;
}

/* 칸 머리 하나를 씀(열린 칸 파일 f, 파일 안 칸 번호 idx) — 512바이트 버퍼는 그때그때(PSRAM, 스택에 안 둠) */
static bool slot_write_hdr(FILE *f, uint32_t idx, const slot_hdr_t *h)
{
    uint8_t *b = heap_caps_calloc(1, SLOT_HDR_BYTES, MALLOC_CAP_SPIRAM);
    if (!b) return false;
    memcpy(b, h, sizeof(*h));
    bool ok = fseek(f, (long)(idx * SLOT_BYTES), SEEK_SET) == 0 && fwrite(b, 1, SLOT_HDR_BYTES, f) == SLOT_HDR_BYTES;
    heap_caps_free(b);
    return ok;
}

/* 칸 하나의 머리를 "빈 칸"으로(지우기·덮어쓸 사진의 앞 칸) */
static void slot_free_hdr(uint32_t slot)
{
    char path[48];
    slot_file_path(slot / SLOT_PER_FILE, path, sizeof(path));
    if (!sd_storage_chain_ok(path)) return;   /* 2026-10-21 — 클러스터 연결이 끊긴 파일에 쓰면 FatFs가 빈 클러스터를 붙임(sd_storage_chain_ok) */
    FILE *f = fopen(path, "r+b");
    if (!f) { ESP_LOGW(TAG, "Slot free: open failed %s (errno=%d)", path, errno); return; }
    slot_hdr_t h = { .magic = SLOT_MAGIC, .ring_id = s_ring_id, .state = SLOT_ST_FREE };
    h.hcrc = hdr_crc(&h);
    bool ok = slot_write_hdr(f, slot % SLOT_PER_FILE, &h) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (ok) sd_health_count(SDH_C_FSYNC, 1);
    else ESP_LOGW(TAG, "Slot free: header write failed (slot %u)", (unsigned)slot);
}

/* 칸 사진 데이터를 다시 읽어 CRC 비교. 1 같음, 0 다름, -1 확인 못 함 */
static int slot_verify(FILE *f, uint32_t idx, uint32_t size, uint32_t crc)
{
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) return -1;
    if (fseek(f, (long)(idx * SLOT_BYTES + SLOT_HDR_BYTES), SEEK_SET) != 0) { heap_caps_free(buf); return 0; }
    uint32_t c = 0, left = size;
    while (left > 0) {
        size_t n = left > 4096 ? 4096 : left;
        if (fread(buf, 1, n, f) != n) { heap_caps_free(buf); return 0; }
        c = esp_rom_crc32_le(c, buf, n);
        left -= n;
    }
    heap_caps_free(buf);
    return c == crc ? 1 : 0;
}

bool photo_storage_finish(photo_storage_writer_t *w, uint32_t *out_seq)
{
    if (!w) return false;
    if (w->failed || w->len == 0) {
        ESP_LOGW(TAG, "Photo not saved (%u bytes, failed=%d)", (unsigned)w->len, w->failed ? 1 : 0);
        photo_storage_abort(w);
        return false;
    }
    if (!sd_storage_is_mounted() || !sd_health_writes_allowed()) {
        ESP_LOGW(TAG, "Photo not saved - SD %s", sd_storage_is_mounted() ? "faulted" : "not mounted");
        photo_storage_abort(w);
        return false;
    }
    uint32_t crc = esp_rom_crc32_le(0, w->buf, w->len);
    uint32_t n = nslots_for((uint32_t)w->len);
    if (n > SLOT_PER_FILE) { ESP_LOGW(TAG, "Photo too large for slots (%u bytes)", (unsigned)w->len); photo_storage_abort(w); return false; }

    wlock();
    lock();
    if (!s_slot_scanned) {   /* 부팅·재연결 직후 파일처리 태스크의 재스캔보다 사진이 먼저 온 경우 */
        uint64_t total = 0, freeb = 0;
        sd_storage_get_capacity(&total, &freeb);
        slot_rescan_locked(total);
    }
    bool ok = s_slot_files_max > 0 && (s_ring_id != 0 || ring_load_or_create_locked());
    /* 자리 정하기 — 칸 파일 경계를 넘지 않게, 링 끝이면 처음으로 */
    uint32_t slot = s_next_slot;
    if (ok && (slot % SLOT_PER_FILE) + n > SLOT_PER_FILE) slot = (slot / SLOT_PER_FILE + 1) * SLOT_PER_FILE;
    if (ok && slot / SLOT_PER_FILE >= s_slot_files_max) slot = 0;
    uint32_t file_no = slot / SLOT_PER_FILE, idx = slot % SLOT_PER_FILE;
    if (ok && file_no >= s_slot_files) {
        ok = (file_no == s_slot_files) && slot_file_create(file_no);   /* 처음 한 바퀴 — 차례로 하나씩 */
        if (ok) s_slot_files++;
    }
    uint32_t seq = 0, stale_head = SLOT_NONE;
    if (ok) {
        stale_head = slot_evict_locked(slot, n);   /* 덮어쓸 오래된 사진을 색인에서 뺌 */
        char dir_path[64];
        camera_dir_path(w->mac, dir_path, sizeof(dir_path));
        seq = take_next_seq_locked(w->mac, dir_path);
    }
    uint32_t gen = ++s_gen;
    unlock();

    int64_t t0 = esp_timer_get_time();
    if (ok && stale_head != SLOT_NONE) slot_free_hdr(stale_head);
    char path[48];
    slot_file_path(file_no, path, sizeof(path));
    if (ok && !sd_storage_chain_ok(path)) ok = false;   /* 2026-10-21 — 클러스터 연결이 끊긴 파일에 쓰면 FatFs가 빈 클러스터를 붙임(sd_storage_chain_ok) */
    FILE *f = ok ? fopen(path, "r+b") : NULL;
    if (ok && !f) { ESP_LOGW(TAG, "Slot open failed: %s (errno=%d)", path, errno); ok = false; }
    slot_hdr_t h = { .magic = SLOT_MAGIC, .ring_id = s_ring_id, .state = SLOT_ST_WRITING, .kind = w->kind, .seq = seq,
                     .size = (uint32_t)w->len, .crc = crc, .mtime = (uint32_t)time(NULL), .nslots = (uint16_t)n, .gen = gen };
    memcpy(h.mac, w->mac, 6);
    h.hcrc = hdr_crc(&h);
    if (ok) ok = slot_write_hdr(f, idx, &h) && fwrite(w->buf, 1, w->len, f) == w->len;
    if (ok) {
        h.state = SLOT_ST_VALID;
        h.hcrc = hdr_crc(&h);
        ok = slot_write_hdr(f, idx, &h) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    }
    /* 2026-10-22(10-22 02:35 실측 — 쓰는 도중 FAT이 상해 FatFs가 이 파일에 빈 클러스터 1,376개를 붙였음) — 쓴 뒤 연결 다시 확인 */
    if (ok && !sd_storage_chain_ok(path)) ok = false;
    int v = -1;
    if (ok) {
        sd_health_count(SDH_C_FSYNC, 1);
        sd_health_count(SDH_C_BYTES, (uint32_t)(w->len + 2 * SLOT_HDR_BYTES));
        v = slot_verify(f, idx, (uint32_t)w->len, crc);   /* 2026-10-14(SD 개선 1단계) — 다시 읽어 비교 */
    }
    if (f) fclose(f);
    if (v == 0) sd_health_report(SDH_EV_VERIFY_PHOTO, path, slot);
    else if (v > 0) sd_health_count(SDH_C_VERIFY_OK, 1);
    if (!ok) ESP_LOGW(TAG, "Photo slot write failed: %s slot %u (errno=%d)", path, (unsigned)idx, errno);

    lock();
    if (ok) {
        s_next_slot = slot + n;
        photo_idx_cam_t *c = cam_find_locked(w->mac, true, NULL);
        if (c) {
            photo_idx_entry_t e = { .seq = seq, .size = (uint32_t)w->len, .mtime = h.mtime, .kind = w->kind,
                                    .slot = slot, .nslots = (uint16_t)n, .crc = crc };
            cam_add_locked(c, &e);
        }
    }
    unlock();
    wunlock();
    if (ok) {
        photo_list_changed();
        if (out_seq) *out_seq = seq;
        ESP_LOGD(TAG, "Photo saved: slot %u (%s #%u) %c%08u %u bytes, %ums", (unsigned)slot, path, (unsigned)idx, (char)w->kind,
                 (unsigned)seq, (unsigned)w->len, (unsigned)((esp_timer_get_time() - t0) / 1000));
    }
    photo_storage_abort(w);
    return ok;
}

void photo_storage_io_suspend(void)
{
    wlock();   /* 칸에 쓰는 중이면 끝날 때까지 기다림(받는 중인 사진은 PSRAM에만 있어 닫을 파일이 없음) */
    lock();
    s_slot_scanned = false;   /* 재연결·포맷 — 다른 카드일 수 있으니 칸 상태를 다시 읽기 전엔 쓰지 않음 */
    s_ring_id = 0;
}

void photo_storage_io_resume(void)
{
    unlock();
    wunlock();
}

void photo_storage_pause(void) { wlock(); lock(); }
void photo_storage_unpause(void) { unlock(); wunlock(); }

/* 호출부가 lock() — 재스캔: 칸 파일들의 머리를 읽어 색인에 넣고, 다음 칸·세대·파일 수를 정함 */
static void slot_rescan_locked(uint64_t sd_total)
{
    s_slot_scanned = true;
    s_ring_id = 0;
    s_slot_files = 0;
    s_next_slot = 0;
    s_gen = 0;
    uint64_t ring_bytes = sd_total * SLOT_RING_PCT / 100;
    s_slot_files_max = (uint32_t)(ring_bytes / SLOT_FILE_BYTES);
    if (s_slot_files_max < 1) s_slot_files_max = 1;
    if (s_slot_files_max > SLOT_MAX_FILES) s_slot_files_max = SLOT_MAX_FILES;
    if (!s_slot_io) s_slot_io = heap_caps_malloc(SLOT_HDR_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_slot_io) return;
    struct stat st;
    if (stat(SLOT_DIR, &st) != 0) return;   /* 칸 사진이 아직 없음 — 첫 저장 때 링을 만듦 */
    if (!ring_load_or_create_locked()) return;
    int64_t t0 = esp_timer_get_time();
    uint32_t found = 0, newest_gen = 0, newest_end = 0;
    for (uint32_t fno = 0; fno < SLOT_MAX_FILES; fno++) {
        char path[48];
        slot_file_path(fno, path, sizeof(path));
        if (stat(path, &st) != 0) break;   /* 칸 파일은 0번부터 차례로 만듦 */
        if ((uint64_t)st.st_size != SLOT_FILE_BYTES) { ESP_LOGW(TAG, "Slot file size bad (%ld): %s - not used", (long)st.st_size, path); break; }
        s_slot_files = fno + 1;
        FILE *f = fopen(path, "rb");
        if (!f) break;
        for (uint32_t i = 0; i < SLOT_PER_FILE; ) {
            slot_hdr_t h;
            if (fseek(f, (long)(i * SLOT_BYTES), SEEK_SET) != 0 || fread(s_slot_io, 1, SLOT_HDR_BYTES, f) != SLOT_HDR_BYTES) break;
            memcpy(&h, s_slot_io, sizeof(h));
            bool hv = h.magic == SLOT_MAGIC && h.ring_id == s_ring_id && h.hcrc == hdr_crc(&h);
            if (hv && h.gen > s_gen) s_gen = h.gen;
            if (hv && h.state == SLOT_ST_VALID && is_valid_kind(h.kind) && h.nslots >= 1 && i + h.nslots <= SLOT_PER_FILE &&
                h.nslots == nslots_for(h.size)) {
                uint32_t slot = fno * SLOT_PER_FILE + i;
                photo_idx_cam_t *cam = cam_find_locked(h.mac, true, NULL);
                if (cam) {
                    photo_idx_entry_t e = { .seq = h.seq, .size = h.size, .mtime = h.mtime, .kind = h.kind,
                                            .slot = slot, .nslots = h.nslots, .crc = h.crc };
                    cam_push_locked(cam, &e);
                    if (cam->next_seq <= h.seq) cam->next_seq = h.seq + 1;
                }
                if (h.gen >= newest_gen) { newest_gen = h.gen; newest_end = slot + h.nslots; }
                found++;
                i += h.nslots;
            } else {
                if (hv && h.state == SLOT_ST_WRITING && h.gen >= newest_gen) { newest_gen = h.gen; newest_end = fno * SLOT_PER_FILE + i + (h.nslots ? h.nslots : 1); }
                i++;
            }
        }
        fclose(f);
    }
    if (s_slot_files > s_slot_files_max) s_slot_files_max = s_slot_files;   /* 예전에 더 크게 만든 링이면 그대로 씀 */
    s_next_slot = newest_end;
    ESP_LOGI(TAG, "Slot rescan: %u files (ring max %u), %u photos, next slot %u, gen %u, %ums", (unsigned)s_slot_files,
             (unsigned)s_slot_files_max, (unsigned)found, (unsigned)s_next_slot, (unsigned)s_gen,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
}

bool photo_storage_save(const uint8_t mac[6], uint8_t kind, const uint8_t *jpeg, size_t len,
                         uint32_t *out_seq)
{
    if (!jpeg || len == 0) {
        ESP_LOGW(TAG, "Save failed - empty data (len=%u)", (unsigned)len);
        return false;
    }
    photo_storage_writer_t *w = photo_storage_begin(mac, kind);
    if (!w) return false;
    if (!photo_storage_append(w, jpeg, len)) {
        photo_storage_abort(w);
        return false;
    }
    return photo_storage_finish(w, out_seq);
}

uint64_t photo_storage_get_used_bytes(void)
{
    lock();
    uint64_t v = s_used_bytes + (uint64_t)s_slot_files * SLOT_FILE_BYTES;   /* 2026-10-15 — 예전 .jpg 합계 + 칸 파일 */
    unlock();
    return v;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

/* 손상 의심 이름 진단용 — 이름 앞 8바이트를 16진수로(깨진 이름이 사진 데이터인지 보려고, JPEG면 FFD8...) */
static void name_hex8(const char *name, char out[17])
{
    size_t n = strnlen(name, 8);
    for (size_t i = 0; i < 8; i++) {
        if (i < n) snprintf(out + i * 2, 3, "%02X", (unsigned char)name[i]);
        else { out[i * 2] = '-'; out[i * 2 + 1] = '-'; }
    }
    out[16] = '\0';
}

void photo_storage_rescan(uint64_t sd_total, uint32_t *out_bad_entries)
{
    uint32_t bad = 0;
    uint64_t total = 0;
    uint64_t sane_limit = PHOTO_MAX_SANE_BYTES;
    if (sd_total > 0 && sd_total < sane_limit) sane_limit = sd_total;

    /* 스캔 동안 저장(카메라 사진 도착)이 끼어들어 이중으로 세지 않게 전체를 잠금 —
     * 마운트/재연결/포맷 직후에만 도는 일이라 저장이 잠깐 기다리는 건 허용. 2026-10-15 — 칸에 쓰는 중과도 겹치지 않게 쓰기 잠금도 */
    wlock();
    lock();
    if (s_cams) {  /* 색인을 처음부터 다시 만듦(항목 배열은 재사용하지 않고 비움) */
        for (int i = 0; i < PHOTO_IDX_CAM_CAP; i++) heap_caps_free(s_cams[i].items);
        memset(s_cams, 0, PHOTO_IDX_CAM_CAP * sizeof(photo_idx_cam_t));
    }

    char photos_root[32];
    snprintf(photos_root, sizeof(photos_root), "%s/photos", SD_STORAGE_MOUNT_POINT);
    DIR *root = opendir(photos_root);
    if (root) {
        struct dirent *cam_ent;
        while ((cam_ent = readdir(root)) != NULL) {
            if (cam_ent->d_name[0] == '.') continue;  /* "."/".." 건너뜀 */
            uint8_t cam_mac[6];
            if (!hex_to_mac(cam_ent->d_name, cam_mac)) {
                char hx[17]; name_hex8(cam_ent->d_name, hx);
                ESP_LOGW(TAG, "Rescan: unknown entry (not a camera folder) skipped in %s: %.40s [%s]", photos_root, cam_ent->d_name, hx);
                bad++;
                continue;
            }
            char cam_dir[64];
            camera_dir_path(cam_mac, cam_dir, sizeof(cam_dir));
            DIR *dir = opendir(cam_dir);
            if (!dir) continue;
            photo_idx_cam_t *cam = cam_find_locked(cam_mac, true, NULL);  /* 사진이 없는 폴더도 목록엔 나옴(예전과 같음) */

            uint32_t max_seq_plus_one = 0;
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char path[96 + 16];
                if (ends_with(ent->d_name, PHOTO_TMP_SUFFIX) && strlen(ent->d_name) < 32) {
                    /* 저장 도중 리셋으로 남은 반쯤 쓴 임시파일 — 지움 */
                    snprintf(path, sizeof(path), "%s/%.31s", cam_dir, ent->d_name);
                    if (unlink(path) == 0) ESP_LOGW(TAG, "Rescan: leftover temp file deleted: %s", path);
                    continue;
                }
                uint8_t kind; uint32_t seq;
                if (!parse_fname(ent->d_name, &kind, &seq)) {
                    char hx[17]; name_hex8(ent->d_name, hx);
                    ESP_LOGW(TAG, "Rescan: malformed name skipped (possibly corrupt) in %s: %.40s [%s]", cam_dir, ent->d_name, hx);
                    bad++;
                    continue;
                }
                file_path_for(cam_mac, kind, seq, path, sizeof(path));  /* parse_fname 통과 = 고정 길이 이름 */
                struct stat st;
                if (stat(path, &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size > sane_limit) {
                    ESP_LOGW(TAG, "Rescan: bad size skipped (possibly corrupt): %s", path);
                    bad++;
                    continue;
                }
                total += (uint64_t)st.st_size;
                if (seq + 1 > max_seq_plus_one) max_seq_plus_one = seq + 1;
                if (cam) {
                    photo_idx_entry_t e = { .seq = seq, .size = (uint32_t)st.st_size, .mtime = (uint32_t)st.st_mtime, .kind = kind,
                                            .slot = SLOT_NONE };
                    cam_push_locked(cam, &e);
                }
            }
            closedir(dir);
            if (cam) {
                cam_sort_locked(cam);
                cam->next_seq = max_seq_plus_one;
            }
        }
        closedir(root);
    }
    s_used_bytes = total;
    slot_rescan_locked(sd_total);   /* 2026-10-15(3단계) — 칸 사진 */
    for (int i = 0; s_cams && i < PHOTO_IDX_CAM_CAP; i++) {
        if (s_cams[i].used) cam_sort_locked(&s_cams[i]);
    }
    unlock();
    wunlock();

    if (out_bad_entries) *out_bad_entries = bad;
}

/* ---- 정리 — 가장 오래된 것부터. 한 번 훑을 때 오래된 후보를 TRIM_BATCH개씩 모아서 지움
 * (예전엔 한 장 지울 때마다 전체를 두 번 훑었음 — get_used_bytes()+find_oldest_file()) ---- */
#define TRIM_BATCH 64
typedef struct {
    time_t   mtime;
    uint32_t size;
    uint32_t seq;
    uint8_t  kind;
    uint8_t  mac[6];
} trim_cand_t;

/* cands(오름차순, mtime 오래된 것부터)에 후보 하나를 끼워 넣음 — 꽉 찼으면 가장 새로운 걸 밀어냄 */
static void cand_insert(trim_cand_t *cands, int *count, const trim_cand_t *c)
{
    int n = *count;
    if (n == TRIM_BATCH && c->mtime >= cands[n - 1].mtime) return;
    int pos = (n < TRIM_BATCH) ? n : TRIM_BATCH - 1;
    while (pos > 0 && cands[pos - 1].mtime > c->mtime) {
        cands[pos] = cands[pos - 1];
        pos--;
    }
    cands[pos] = *c;
    if (n < TRIM_BATCH) *count = n + 1;
}

static int collect_oldest(trim_cand_t *cands, uint64_t sane_limit)
{
    int count = 0;
    char photos_root[32];
    snprintf(photos_root, sizeof(photos_root), "%s/photos", SD_STORAGE_MOUNT_POINT);
    DIR *root = opendir(photos_root);
    if (!root) return 0;
    struct dirent *cam_ent;
    while ((cam_ent = readdir(root)) != NULL) {
        uint8_t cam_mac[6];
        if (cam_ent->d_name[0] == '.' || !hex_to_mac(cam_ent->d_name, cam_mac)) continue;
        char cam_dir[64];
        camera_dir_path(cam_mac, cam_dir, sizeof(cam_dir));
        DIR *dir = opendir(cam_dir);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            trim_cand_t c;
            if (!parse_fname(ent->d_name, &c.kind, &c.seq)) continue;  /* 손상 의심 항목은 정리 대상 아님 */
            char path[96];
            file_path_for(cam_mac, c.kind, c.seq, path, sizeof(path));
            struct stat st;
            if (stat(path, &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size > sane_limit) continue;
            c.mtime = st.st_mtime;
            c.size = (uint32_t)st.st_size;
            memcpy(c.mac, cam_mac, 6);
            cand_insert(cands, &count, &c);
        }
        closedir(dir);
    }
    closedir(root);
    return count;
}

uint32_t photo_storage_trim_to(uint64_t target_bytes)
{
    if (!sd_storage_is_mounted()) return 0;

    static trim_cand_t *s_cands = NULL;  /* 파일처리 태스크 전용, PSRAM 한 번만 */
    if (!s_cands) {
        s_cands = heap_caps_malloc(sizeof(trim_cand_t) * TRIM_BATCH, MALLOC_CAP_SPIRAM);
        if (!s_cands) { ESP_LOGE(TAG, "Trim candidate buffer alloc failed - not trimming"); return 0; }
    }

    uint32_t deleted = 0;
    while (photo_storage_get_used_bytes() > target_bytes) {
        int n = collect_oldest(s_cands, PHOTO_MAX_SANE_BYTES);
        if (n == 0) break;  /* 더 지울 게 없음 */
        bool progress = false;
        for (int i = 0; i < n && photo_storage_get_used_bytes() > target_bytes; i++) {
            char path[96];
            file_path_for(s_cands[i].mac, s_cands[i].kind, s_cands[i].seq, path, sizeof(path));
            if (unlink(path) != 0) {
                ESP_LOGW(TAG, "Trim delete failed (errno=%d): %s", errno, path);
                continue;
            }
            used_add(-(int64_t)s_cands[i].size);
            index_remove(s_cands[i].mac, s_cands[i].kind, s_cands[i].seq);
            deleted++;
            progress = true;
        }
        if (!progress) break;  /* 이번 묶음을 하나도 못 지움 — 무한루프 방지 */
    }
    if (deleted > 0) ESP_LOGI(TAG, "Trim: %u photos deleted (oldest first)", (unsigned)deleted);
    return deleted;
}

/* 2026-10-01(할 일 H) — 아래 세 함수는 화면(LVGL 태스크)이 부르므로 폴더를 읽지 않고 색인만 봄 */
uint32_t photo_storage_get_count(const uint8_t mac[6])
{
    SLOT_KEY(mac);
    if (!sd_storage_is_mounted()) return 0;  /* 예전(폴더 열기 실패 = 0)과 같게 */
    lock();
    photo_idx_cam_t *c = cam_find_locked(mac, false, NULL);
    uint32_t count = c ? c->count : 0;
    unlock();
    return count;
}

uint32_t photo_storage_read_page(const uint8_t mac[6], uint32_t page_index, uint32_t page_size,
                                  photo_storage_item_t *out, uint32_t out_cap)
{
    SLOT_KEY(mac);
    uint32_t got = 0;
    if (!sd_storage_is_mounted()) return 0;
    lock();
    photo_idx_cam_t *c = cam_find_locked(mac, false, NULL);
    if (c) {
        /* 색인은 순번 오름차순 — 최신(순번 큰 것)부터 page_index * page_size개를 건너뜀 */
        uint64_t start = (uint64_t)page_index * page_size;
        for (uint64_t k = start; k < c->count && got < page_size && got < out_cap; k++) {
            const photo_idx_entry_t *e = &c->items[c->count - 1 - k];
            out[got].kind = e->kind;
            out[got].seq = e->seq;
            out[got].mtime = (time_t)e->mtime;
            out[got].file_size = e->size;
            got++;
        }
    }
    unlock();
    return got;
}

/* 2026-10-15(3단계) — 칸 사진 읽기: 머리(유효·같은 사진인지)와 데이터 CRC를 확인. 다르면 SD 이상(쓴 뒤 바뀐 손상) */
static bool slot_read(const photo_idx_entry_t *e, const uint8_t mac[6], uint8_t *out_buf, size_t buf_cap, size_t *out_len)
{
    if (e->size > buf_cap) {
        ESP_LOGW(TAG, "read_file: buffer too small (photo %u > buf %u)", (unsigned)e->size, (unsigned)buf_cap);
        return false;
    }
    char path[48];
    slot_file_path(e->slot / SLOT_PER_FILE, path, sizeof(path));
    uint32_t idx = e->slot % SLOT_PER_FILE;
    wlock();   /* 칸에 쓰는 중과 겹치지 않게(덮어쓰는 중인 칸을 읽지 않음) */
    FILE *f = fopen(path, "rb");
    bool ok = false;
    const char *why = "open";
    slot_hdr_t h;
    if (f) {
        uint8_t *hb = heap_caps_malloc(SLOT_HDR_BYTES, MALLOC_CAP_SPIRAM);
        why = "header read";
        if (hb && fseek(f, (long)(idx * SLOT_BYTES), SEEK_SET) == 0 && fread(hb, 1, SLOT_HDR_BYTES, f) == SLOT_HDR_BYTES) {
            memcpy(&h, hb, sizeof(h));
            why = "header";
            if (h.magic == SLOT_MAGIC && h.hcrc == hdr_crc(&h) && h.state == SLOT_ST_VALID && h.seq == e->seq && h.kind == e->kind &&
                memcmp(h.mac, mac, 6) == 0 && h.size == e->size) {
                why = "data read";
                if (fread(out_buf, 1, e->size, f) == e->size) {
                    why = "data crc";
                    ok = esp_rom_crc32_le(0, out_buf, e->size) == e->crc;
                }
            }
        }
        heap_caps_free(hb);
        fclose(f);
    }
    wunlock();
    if (!ok) {
        ESP_LOGE(TAG, "read_file: slot photo %c%08u bad (%s) at %s #%u", (char)e->kind, (unsigned)e->seq, why, path, (unsigned)idx);
        if (sd_storage_is_mounted() && index_has(mac, e->kind, e->seq)) sd_health_report(SDH_EV_VERIFY_PHOTO, path, e->slot);
        return false;
    }
    if (out_len) *out_len = e->size;
    return true;
}

bool photo_storage_read_file(const uint8_t mac[6], uint8_t kind, uint32_t seq,
                              uint8_t *out_buf, size_t buf_cap, size_t *out_len)
{
    SLOT_KEY(mac);
    photo_idx_entry_t ie;
    if (index_find(mac, kind, seq, &ie) && ie.slot != SLOT_NONE) return slot_read(&ie, mac, out_buf, buf_cap, out_len);
    char file_path[96];
    file_path_for(mac, kind, seq, file_path, sizeof(file_path));

    struct stat st;
    if (stat(file_path, &st) != 0) {
        ESP_LOGW(TAG, "read_file: not found: %s", file_path);
        /* 2026-10-14(SD 개선 1단계) — 목록엔 있는데 파일이 없으면 SD 이상. 지우는 중(파일 먼저 지우고 색인을 나중에 뺌)과
         * 겹친 것일 수 있어 잠깐 뒤 다시 확인 */
        vTaskDelay(pdMS_TO_TICKS(100));
        if (index_has(mac, kind, seq) && stat(file_path, &st) != 0) sd_health_report(SDH_EV_MISSING_PHOTO, file_path, seq);
        return false;
    }
    if ((size_t)st.st_size > buf_cap) {
        ESP_LOGW(TAG, "read_file: buffer too small (file %u > buf %u): %s",
                 (unsigned)st.st_size, (unsigned)buf_cap, file_path);
        return false;
    }

    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "read_file: open failed: %s", file_path);
        return false;
    }
    size_t read_len = fread(out_buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (read_len != (size_t)st.st_size) {
        ESP_LOGW(TAG, "read_file: short read (%u/%u): %s", (unsigned)read_len, (unsigned)st.st_size, file_path);
        return false;
    }

    if (out_len) *out_len = read_len;
    return true;
}

bool photo_storage_get_size(const uint8_t mac[6], uint8_t kind, uint32_t seq, size_t *out_size)
{
    SLOT_KEY(mac);
    photo_idx_entry_t ie;
    if (index_find(mac, kind, seq, &ie) && ie.slot != SLOT_NONE) { *out_size = ie.size; return true; }
    char file_path[96];
    file_path_for(mac, kind, seq, file_path, sizeof(file_path));
    struct stat st;
    if (stat(file_path, &st) != 0) return false;
    *out_size = (size_t)st.st_size;
    return true;
}

bool photo_storage_read_part(const uint8_t mac[6], uint8_t kind, uint32_t seq, size_t offset,
                             uint8_t *out_buf, size_t cap, size_t *out_len, size_t *out_total)
{
    SLOT_KEY(mac);
    *out_len = 0;
    photo_idx_entry_t ie;
    if (index_find(mac, kind, seq, &ie) && ie.slot != SLOT_NONE) {
        *out_total = ie.size;
        if (offset >= ie.size) return offset == ie.size;
        size_t n = ie.size - offset;
        if (n > cap) n = cap;
        char path[48];
        slot_file_path(ie.slot / SLOT_PER_FILE, path, sizeof(path));
        uint32_t idx = ie.slot % SLOT_PER_FILE;
        bool ok = false;
        wlock();   /* 칸에 쓰는 중과 겹치지 않게(slot_read와 같음) */
        FILE *f = fopen(path, "rb");
        if (f) {
            slot_hdr_t h;
            uint8_t *hb = heap_caps_malloc(SLOT_HDR_BYTES, MALLOC_CAP_SPIRAM);
            if (hb && fseek(f, (long)(idx * SLOT_BYTES), SEEK_SET) == 0 && fread(hb, 1, SLOT_HDR_BYTES, f) == SLOT_HDR_BYTES) {
                memcpy(&h, hb, sizeof(h));
                if (h.magic == SLOT_MAGIC && h.hcrc == hdr_crc(&h) && h.state == SLOT_ST_VALID && h.seq == ie.seq &&
                    h.kind == ie.kind && memcmp(h.mac, mac, 6) == 0 && h.size == ie.size &&
                    fseek(f, (long)(idx * SLOT_BYTES + SLOT_HDR_BYTES + offset), SEEK_SET) == 0 &&
                    fread(out_buf, 1, n, f) == n) {
                    ok = true;
                }
            }
            heap_caps_free(hb);
            fclose(f);
        }
        wunlock();
        if (!ok) { ESP_LOGW(TAG, "read_part: slot photo %c%08u read failed at %u", (char)kind, (unsigned)seq, (unsigned)offset); return false; }
        *out_len = n;
        return true;
    }
    char file_path[96];
    file_path_for(mac, kind, seq, file_path, sizeof(file_path));
    struct stat st;
    if (stat(file_path, &st) != 0) return false;
    *out_total = (size_t)st.st_size;
    if (offset >= (size_t)st.st_size) return offset == (size_t)st.st_size;
    FILE *fp = fopen(file_path, "rb");
    if (!fp) return false;
    size_t n = (size_t)st.st_size - offset;
    if (n > cap) n = cap;
    bool ok = fseek(fp, (long)offset, SEEK_SET) == 0 && fread(out_buf, 1, n, fp) == n;
    fclose(fp);
    if (ok) *out_len = n;
    return ok;
}

bool photo_storage_delete(const uint8_t mac[6], uint8_t kind, uint32_t seq)
{
    SLOT_KEY(mac);
    photo_idx_entry_t ie;
    if (index_find(mac, kind, seq, &ie) && ie.slot != SLOT_NONE) {   /* 2026-10-15 — 칸 사진: 머리를 빈 칸으로 */
        wlock();
        slot_free_hdr(ie.slot);
        index_remove(mac, kind, seq);
        wunlock();
        photo_list_changed();
        ESP_LOGI(TAG, "delete: slot %u (%c%08u)", (unsigned)ie.slot, (char)kind, (unsigned)seq);
        return true;
    }
    char file_path[96];
    file_path_for(mac, kind, seq, file_path, sizeof(file_path));
    struct stat st;
    int64_t size = (stat(file_path, &st) == 0 && st.st_size > 0) ? (int64_t)st.st_size : 0;
    if (unlink(file_path) != 0) {
        ESP_LOGW(TAG, "delete: failed (errno=%d): %s", errno, file_path);
        return false;
    }
    used_add(-size);  /* 삭제가 성공했을 때만 뺌 */
    index_remove(mac, kind, seq);
    photo_list_changed();
    ESP_LOGI(TAG, "delete: %s", file_path);
    return true;
}

static uint32_t delete_all_key(const uint8_t mac[6]);

uint32_t photo_storage_delete_all(const uint8_t mac[6])
{
    SLOT_KEY(mac);
    return delete_all_key(mac);
}

uint32_t photo_storage_delete_all_key(const uint8_t key[6])
{
    return delete_all_key(key);
}

static uint32_t delete_all_key(const uint8_t mac[6])
{
    char dir_path[64];
    camera_dir_path(mac, dir_path, sizeof(dir_path));
    DIR *dir = opendir(dir_path);

    uint32_t deleted = 0;
    int64_t freed = 0;
    struct dirent *ent;
    uint8_t kind; uint32_t seq;
    while (dir && (ent = readdir(dir)) != NULL) {
        if (!parse_fname(ent->d_name, &kind, &seq)) continue;
        /* parse_fname()이 PHOTO_FNAME_LEN 고정 길이만 통과시키므로 file_path_for()로 다시 만듦
         * (큰 스택 버퍼 없이) */
        char file_path[96];
        file_path_for(mac, kind, seq, file_path, sizeof(file_path));
        struct stat st;
        int64_t size = (stat(file_path, &st) == 0 && st.st_size > 0) ? (int64_t)st.st_size : 0;
        if (unlink(file_path) == 0) { deleted++; freed += size; }
        else ESP_LOGW(TAG, "delete_all: failed (errno=%d): %s", errno, file_path);
    }
    if (dir) closedir(dir);
    used_add(-freed);
    /* 2026-10-15(3단계) — 이 카메라의 칸 사진도 머리를 빈 칸으로 */
    wlock();
    for (;;) {
        uint32_t slot = SLOT_NONE;
        lock();
        photo_idx_cam_t *cc = cam_find_locked(mac, false, NULL);
        for (uint32_t i = 0; cc && i < cc->count; i++) {
            if (cc->items[i].slot != SLOT_NONE) { slot = cc->items[i].slot; cam_remove_locked(cc, cc->items[i].kind, cc->items[i].seq); break; }
        }
        unlock();
        if (slot == SLOT_NONE) break;
        slot_free_hdr(slot);
        deleted++;
    }
    wunlock();
    lock();
    photo_idx_cam_t *c = cam_find_locked(mac, false, NULL);
    if (c) c->count = 0;  /* 순번(next_seq)은 그대로 — 지운 번호를 다시 쓰지 않음 */
    unlock();
    photo_list_changed();
    ESP_LOGI(TAG, "delete_all: %u deleted (%s)", (unsigned)deleted, dir_path);
    return deleted;
}

uint32_t photo_storage_list_camera_macs(uint8_t out_macs[][6], uint32_t out_cap)
{
    if (!sd_storage_is_mounted()) return 0;

    uint32_t count = 0;
    lock();
    for (int i = 0; s_cams && i < PHOTO_IDX_CAM_CAP && count < out_cap; i++) {
        if (s_cams[i].used) memcpy(out_macs[count++], s_cams[i].mac, 6);
    }
    unlock();
    return count;
}
