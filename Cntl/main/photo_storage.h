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

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
