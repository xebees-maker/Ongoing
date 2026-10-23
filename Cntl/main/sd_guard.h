#pragma once
/**
 * @file    sd_guard.h
 * @brief   SD 쓰기 보호(2026-10-22, 사용자 지시 — "최대한 방어 및 복구 코드를 넣어 시험하고, 얼마나 유효했는지 보고").
 *
 * 10-22 02:35 실측: 콘이 FAT 한 섹터를 쓰는 동안 카드가 47ms 붙잡고, 같은 8KB(16섹터) 안의 다른 15섹터를 콘 쓰기 없이 바꿨음
 * (10-21도 8KB 단위, 32섹터 떨어진 블록까지). 그래서 FatFs 디스크 계층(diskio)의 쓰기를 감싸서:
 *  1) 쓰기 전 — 쓰는 섹터가 걸친 8KB 블록과 앞뒤 SDG_NEIGH 블록 중 이번에 쓰지 않는 섹터를 읽어 둠(쓰기 전 내용)
 *  2) 쓴 뒤 — 그 범위를 다시 읽어 쓴 섹터는 새 내용과, 나머지는 쓰기 전 내용과 비교
 *  3) 다르면 — 그 블록 전체를 맞는 내용으로 다시 쓰고 다시 확인(최대 SDG_ROUNDS번). 끝내 안 되면 SD 이상(5009)·쓰기 실패
 * 효과를 보고할 수 있게 날짜별·누적 집계와 최근 사건을 LittleFS(sd_guard.bin)에 남김.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

/* 마운트·포맷 직후(sd_storage) — 이 카드의 FatFs 디스크 쓰기를 보호 쓰기로 바꿔 끼움. 파티션 표가 듀얼이면 이중화 켬 */
void sd_guard_attach(sdmmc_card_t *card);

/* ════════════════════════════════════════════════════════════
 * 2026-10-22(사용자 결정 — "SD를 파티션 2개로 나눠서 듀얼로 저장", "정상인 곳에서 비정상인 곳을 메꾸고, 알리고",
 * "사용자는 한쪽이 망가지면 복구할 준비", "메꿔지는지도 봐야") — 듀얼 파티션 이중화.
 * 카드 = MBR + 같은 크기 FAT32 파티션 A·B(B는 A 바로 뒤). FatFs는 A만 마운트하고, 이 계층이 A 영역에 쓰는 모든 섹터를
 * B 영역 같은 자리(+delta)에도 씀(양쪽 다 쓰기 보호). 읽기는 A(A를 못 쓰게 되면 B), 읽기 실패면 다른 쪽.
 * 망가짐은 지금 감시들이 알려 줌(쓰기 보호, FAT 감시·검사, 부트 영역 감시, FAT 읽기 확인) — 그 쪽 섹터를 다른 쪽 내용으로
 * 메꾸고 다시 읽어 확인, 경고 6005. 메꾸지 못한 쪽은 쓰지 않고 경고 6006(카드 교체 준비). 둘 다 못 쓰면 SD 이상(5009).
 * 듀얼이 아닌 카드는 예전처럼 한 벌(경고 6004 — 포맷하면 듀얼).
 * ════════════════════════════════════════════════════════════ */
/* 듀얼 A 시작 섹터(감시가 MBR 없이 배치를 앎) */
uint32_t sd_guard_dual_a0(void);
/* 마운트가 실패한 카드(초기화만) — 0번 섹터가 망가졌고 A·B 부트 섹터가 멀쩡하면 파티션 표를 다시 씀(true면 마운트 재시도) */
bool sd_guard_repair_mbr(sdmmc_card_t *card);
/* 2026-10-23 — 언마운트(꺼내기·재연결·포맷) 전에: 진행 중인 카드 명령이 끝나길 기다려 카드를 놓음(그 뒤 요청은 "준비 안 됨") */
void sd_guard_detach(void);
/* 마운트 안 된 카드(초기화만)를 듀얼로 포맷 */
esp_err_t sd_guard_format_dual(sdmmc_card_t *card);
/* 듀얼이면 true, delta = B 시작 - A 시작 */
bool sd_guard_dual_info(uint32_t *delta);
/* 볼륨 v(0 = A, 1 = B)를 쓰는 중인지 */
bool sd_guard_vol_usable(int v);
/* 볼륨 v의 섹터 읽기(lba_a = A 기준 절대 섹터) */
esp_err_t sd_guard_read_vol(int v, void *dst, uint32_t lba_a, uint32_t n);
/* bad_v 쪽 [lba_a, lba_a+n)를 다른 쪽 내용으로 메꾸고 다시 읽어 확인(want_crc가 있으면 다른 쪽이 그 CRC일 때만). 집계·경고 포함 */
bool sd_guard_fix_from_other(int bad_v, uint32_t lba_a, uint32_t n, const uint32_t *want_crc, const char *what);
/* src_v·src_lba_a 한 섹터를 dst_v·dst_lba_a로 복사, 다시 읽어 확인(want_crc가 있으면 원본이 그 CRC일 때만). 집계 없음 */
bool sd_guard_copy_sector(int src_v, uint32_t src_lba_a, int dst_v, uint32_t dst_lba_a, const uint32_t *want_crc);
/* 메꾼 결과 집계·로그(성공이면 경고 6005) */
void sd_guard_note_fix(int v, uint32_t sectors, bool ok, uint32_t lba_a, const char *what);
/* 볼륨 v를 쓰지 않게 함(메꾸지 못함) — 경고 6006, 둘 다면 SD 이상 */
void sd_guard_vol_fail(int v, const char *why, uint32_t lba_a);
/* 쓰기 보호 없이 절대 섹터에 씀(MBR 되살리기) */
esp_err_t sd_guard_write_raw(const void *src, uint32_t lba, uint32_t n);

/* 포맷 중엔 끔(f_mkfs가 쓰는 대량 쓰기까지 확인하면 너무 오래 걸림) */
void sd_guard_set_enabled(bool on);

/* 부팅 때 한 번(LittleFS 뒤) — 저장된 집계를 읽음 */
void sd_guard_init(void);

/* 10분 FAT 감시가 "콘 쓰기 없이 바뀜"을 찾았을 때 — 보호 범위 밖 손상으로 셈(sd_health) */
void sd_guard_note_outside(uint32_t sectors);

/* 매시(sd_health) — 날짜 넘김, 바뀐 것 저장, 한 줄 요약 로그 */
void sd_guard_hourly(void);

/* /api/sd/health에 붙일 JSON 조각("guard":{...}) — 길이 반환 */
int sd_guard_json(char *buf, size_t cap);
