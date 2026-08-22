#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_now_link.h"  /* photo_request_mode_t */

#ifdef __cplusplus
extern "C" {
#endif

/* SD카드(SDMMC 1-bit)에 사진을 순환 저장. 파일명은 <kind><4자리 base36 순번>.jpg
 * (2026-08-01 재설계 — 예전엔 10자리 유닉스 타임스탬프라 파일명이 길었고 Cntl이 그걸
 * 촬영시각으로 역해석하는 편법을 씀). 순번은 M/T가 공유, 별도 카운터 파일 없이 SD를 훑어서
 * 가장 최근(mtime) 파일의 seq+1부터 이어감. 정렬/최근시간 필터/순환삭제는 전부 파일의
 * FAT mtime 기준(cam_storage_stat의 out_capture_time으로 조회) — file_id 자체엔 이제
 * 시간 순서 정보가 없음. 저장 전 파일 개수가 CAM_STORAGE_MAX_FILES를 넘으면 가장 오래된
 * 파일부터 지운다(FIFO 순환). */

#define CAM_STORAGE_MAX_FILES  500   /* TODO: 실기로 SD 용량/평균 파일크기 확인 후 재조정 */

/* 파일명 접두사로 수동(M)/자동(T) 촬영을 구분. file_id만으로 여는 API들은 두 접두사를
 * 순서대로 시도해서 찾는다(cam_storage.c의 resolve_path 참고) — M/T가 순번을 공유해서
 * 같은 seq가 두 kind에 동시에 존재할 수 없으므로 결과는 항상 유일함. */
typedef enum {
    CAM_CAPTURE_KIND_MANUAL = 'M',
    CAM_CAPTURE_KIND_AUTO   = 'T',
} cam_capture_kind_t;

esp_err_t cam_storage_init(void);

/**
 * @brief 캡처된 JPEG 1장을 새 파일로 저장(파일명=<kind><4자리 base36 순번>.jpg). 저장 전
 *        파일 개수가 한도를 넘으면 오래된 것부터 삭제.
 * @param out_file_id 저장된 파일의 식별자(=순번, M/T 공유) 출력
 */
esp_err_t cam_storage_save_capture(const uint8_t *jpeg_data, size_t len, cam_capture_kind_t kind, uint32_t *out_file_id);

/**
 * @brief 요청 모드에 맞는 파일 id 목록을 오래된 것부터(FAT mtime 기준) 반환
 * @return 채운 개수(0 이상), 실패 시 음수
 */
int cam_storage_list(photo_request_mode_t mode, uint32_t param, uint32_t *out_file_ids, int max);

/** @brief file_id에 해당하는 파일의 크기/종류(M|T)/촬영시각(FAT mtime)을 조회(열지 않음) */
esp_err_t cam_storage_stat(uint32_t file_id, uint32_t *out_size, char *out_kind, uint32_t *out_capture_time);

/**
 * @brief 저장된 모든 사진의 전체 정보(id/종류/촬영시각/크기)를 file_id 오름차순으로
 *        한 번에 채움 — 목록 전송(PHOTO_LIST) 전용, 파일마다 별도 cam_storage_stat()
 *        호출이 필요 없음(2026-08-11, scan_all_files가 애초에 stat()으로 갖고 있던
 *        정보를 그대로 재사용 — 예전엔 이 정보를 파일마다 다시 조회해서 이중으로 느렸음)
 * @return 채운 개수(0 이상), 실패 시 음수
 */
int cam_storage_list_full(esp_now_photo_list_item_t *out_items, int max);

/**
 * @brief file_id에 해당하는 파일을 읽기 모드로 열고 크기를 알려줌 — 호출자가 청크 단위로
 *        fread()해서 ESP-NOW로 보내면 됨. 다 쓰면 fclose() 호출.
 */
esp_err_t cam_storage_open_read(uint32_t file_id, FILE **out_fp, uint32_t *out_size);

/** @brief cam_storage_delete_all()이 지울 파일 개수를 미리 셈(삭제 없이 readdir만) — CAM이
 *  삭제 시작 전 DELETE_ALL_RECEIVED로 이 개수를 먼저 알림 @return 개수, 실패 시 음수 */
int cam_storage_count_files(void);

/** @brief 저장된 모든 사진을 삭제 (개발용 clear 명령) @return 삭제한 개수, 실패 시 음수 */
int cam_storage_delete_all(void);

/** @brief file_id에 해당하는 파일 하나만 삭제(목록에서 삭제 버튼 눌렀을 때) */
esp_err_t cam_storage_delete(uint32_t file_id);

/** @brief SD카드 전체/사용 용량(KB) — Cntl 목록 화면에 사용량 %로 보여주려고 추가
 *  (2026-08-01). 실패하면 out_total_kb/out_used_kb를 0으로 채우고 에러 반환 */
esp_err_t cam_storage_get_sd_usage(uint32_t *out_total_kb, uint32_t *out_used_kb);

#ifdef __cplusplus
}
#endif
