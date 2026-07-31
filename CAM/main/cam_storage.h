#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_now_link.h"  /* photo_request_mode_t */

#ifdef __cplusplus
extern "C" {
#endif

/* SD카드(SDMMC 1-bit)에 사진을 순환 저장 — 파일명은 유닉스 타임스탬프(초) 기준이라
 * 정렬만으로 오래된/최신 파일 판별 가능, 별도 인덱스 파일 불필요. 저장 전 파일 개수가
 * CAM_STORAGE_MAX_FILES를 넘으면 가장 오래된 파일부터 지운다(FIFO 순환). */

#define CAM_STORAGE_MAX_FILES  500   /* TODO: 실기로 SD 용량/평균 파일크기 확인 후 재조정 */

/* 파일명 접두사로 수동(M)/자동(T) 촬영을 구분 — file_id(타임스탬프) 자체는 그대로 두고
 * 파일명만 "<M|T><10자리 id>.jpg"로 저장. file_id만으로 여는 API들은 두 접두사를 순서대로
 * 시도해서 찾는다(cam_storage.c의 resolve_path 참고). */
typedef enum {
    CAM_CAPTURE_KIND_MANUAL = 'M',
    CAM_CAPTURE_KIND_AUTO   = 'T',
} cam_capture_kind_t;

esp_err_t cam_storage_init(void);

/**
 * @brief 캡처된 JPEG 1장을 새 파일로 저장(파일명=<kind><현재 유닉스 타임스탬프>.jpg). 저장 전
 *        파일 개수가 한도를 넘으면 오래된 것부터 삭제.
 * @param out_file_id 저장된 파일의 식별자(=타임스탬프) 출력
 */
esp_err_t cam_storage_save_capture(const uint8_t *jpeg_data, size_t len, cam_capture_kind_t kind, uint32_t *out_file_id);

/**
 * @brief 요청 모드에 맞는 파일 id 목록을 오름차순(오래된 것부터)으로 반환
 * @return 채운 개수(0 이상), 실패 시 음수
 */
int cam_storage_list(photo_request_mode_t mode, uint32_t param, uint32_t *out_file_ids, int max);

/** @brief file_id에 해당하는 파일의 크기/종류(M|T)만 조회(열지 않음) — ls 표시용 */
esp_err_t cam_storage_stat(uint32_t file_id, uint32_t *out_size, char *out_kind);

/**
 * @brief file_id에 해당하는 파일을 읽기 모드로 열고 크기를 알려줌 — 호출자가 청크 단위로
 *        fread()해서 ESP-NOW로 보내면 됨. 다 쓰면 fclose() 호출.
 */
esp_err_t cam_storage_open_read(uint32_t file_id, FILE **out_fp, uint32_t *out_size);

/** @brief 저장된 모든 사진을 삭제 (개발용 clear 명령) @return 삭제한 개수, 실패 시 음수 */
int cam_storage_delete_all(void);

/** @brief file_id에 해당하는 파일 하나만 삭제(목록에서 삭제 버튼 눌렀을 때) */
esp_err_t cam_storage_delete(uint32_t file_id);

#ifdef __cplusplus
}
#endif
