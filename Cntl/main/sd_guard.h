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
#include "sdmmc_cmd.h"

/* 마운트·포맷 직후(sd_storage) — 이 카드의 FatFs 디스크 쓰기를 보호 쓰기로 바꿔 끼움 */
void sd_guard_attach(sdmmc_card_t *card);

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
