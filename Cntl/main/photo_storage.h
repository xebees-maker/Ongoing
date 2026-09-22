/**
 * @file    photo_storage.h
 * @brief   2026-09-18(SD 제거 재설계 — "캠은 사진을 자체 보관 안 하고, 찍을 때마다 콘에
 *          가져와서 콘의 SD에 저장") — CAM이 ESP-NOW로 푸시한 사진을 콘 SD의
 *          /sdcard/photos/<카메라 mac 16진수>/ 밑에 영구 저장. stats_store.c(고정크기
 *          레코드 하나의 파일)와 달리 사진은 가변크기 개별 파일이라 저장 방식이 다름 —
 *          대신 "오래된 것부터 정리" 정책은 stats_store_trim_to()와 동일 원칙을 따름.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 2026-09-19(사진목록 UI 로컬화) — 목록 한 행에 필요한 정보. file_id 같은 불투명 값
 * 대신 kind/seq를 그대로 노출(이 파일명 자체가 이미 kind+seq라 인코딩할 이유가 없음,
 * device_config.h의 "ID에 의미를 담지 않는다" 원칙과는 다른 상황 — 여기 kind/seq는
 * 애초부터 각각 독립된 필드이지 하나의 ID를 쪼갠 게 아님) */
typedef struct {
    uint8_t  kind;       /* 'M'(즉시촬영) 또는 'T'(주기촬영) */
    uint32_t seq;         /* 카메라 폴더 안에서 유일 — photo_storage_save()의 out_seq와 동일 값 */
    time_t   mtime;       /* 파일 저장(수정) 시각 */
    size_t   file_size;   /* 바이트 */
} photo_storage_item_t;

/* 사진 1장을 해당 카메라(mac) 전용 폴더에 저장 — 폴더가 없으면 생성. kind는
 * cam_capture_kind_t 값을 그대로 받음('M'=수동/즉시, 'T'=자동/주기, esp_now_link.h의
 * esp_now_photo_meta_t.kind와 동일) — 헤더 의존을 피하려고 여기선 uint8_t로 받음.
 * 파일명은 그 카메라 폴더 안에서만 유일하면 되는 순번(이 함수가 스캔해서 자동 부여,
 * out_seq에 기록) — CAM은 더 이상 영구 파일명을 갖지 않으므로(SD 제거) file_id를 그대로
 * 쓰지 않고 콘이 처음부터 다시 부여함. SD 미마운트 등으로 실패하면 false */
bool photo_storage_save(const uint8_t mac[6], uint8_t kind, const uint8_t *jpeg, size_t len,
                         uint32_t *out_seq);

/* 모든 카메라 폴더를 합산한 총 사용 바이트 — 주화면 Storage 표시의 Picture 항목
 * (ui_main.c refresh_storage_status_label)용. SD 미마운트/폴더 없음 등이면 0 */
uint64_t photo_storage_get_used_bytes(void);

/* 카메라 구분 없이 전체에서 가장 오래된(mtime 기준) 파일부터 지워서 총 사용량이
 * target_bytes 이하가 되게 함 — stats_store_trim_to()와 동일한 호출 패턴(90% 도달 시 80%
 * 목표로 호출). 카메라마다 독립된 순번(seq)이라 순번으로는 기기 간 시간 순서를 알 수
 * 없어서 mtime을 씀(같은 카메라 내 연속촬영처럼 짧은 간격이 아니라 서로 다른 기기의
 * 전송이라 FAT mtime 해상도 문제가 실질적으로 없음 — cam_storage.c가 seq를 쓰는 이유와는
 * 다른 상황). 실제로 지운 파일 수 반환 */
uint32_t photo_storage_trim_to(uint64_t target_bytes);

/* 2026-09-19(사진목록 UI 로컬화) — 이 카메라 폴더의 전체 사진 수(페이지 계산용).
 * stats_store_get_count()와 동일 관례 */
uint32_t photo_storage_get_count(const uint8_t mac[6]);

/* page_index=0이 가장 최근(seq 큰 것부터) 페이지 — stats_store_read_page()와 동일 관례.
 * out에 최대 out_cap개 채우고 실제 채운 개수 반환 */
uint32_t photo_storage_read_page(const uint8_t mac[6], uint32_t page_index, uint32_t page_size,
                                  photo_storage_item_t *out, uint32_t out_cap);

/* 사진 1장의 원본 JPEG 바이트를 통째로 읽음(디코드는 호출부 책임, 기존
 * decode_jpeg_scaled()에 그대로 넘길 원본 바이트가 필요해서). buf_cap보다 파일이 크면
 * 실패(false) — 잘린 채로 디코드 시도하지 않음 */
bool photo_storage_read_file(const uint8_t mac[6], uint8_t kind, uint32_t seq,
                              uint8_t *out_buf, size_t buf_cap, size_t *out_len);

/* 사진 1장 삭제 — 성공/실패(이미 없음 포함) 반환 */
bool photo_storage_delete(const uint8_t mac[6], uint8_t kind, uint32_t seq);

/* 이 카메라 폴더의 모든 사진 삭제(폴더 자체는 남김) — 실제로 지운 개수 반환 */
uint32_t photo_storage_delete_all(const uint8_t mac[6]);

/* 2026-09-22(사용자 지시 — 카메라 미연결 상태에서도 팝업에서 과거 촬영 이력을 볼 수 있어야
 * 함) — /sdcard/photos/ 밑에 폴더가 존재하는 카메라 MAC 전부(=사진을 한 번이라도 저장한
 * 적 있는 카메라) 나열. out_cap개까지 채우고 실제 채운 개수 반환. SD 미마운트/폴더 없음이면 0 */
uint32_t photo_storage_list_camera_macs(uint8_t out_macs[][6], uint32_t out_cap);

#ifdef __cplusplus
}
#endif
