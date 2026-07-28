#pragma once

/**
 * 개발 단계 전용 진단 콘솔(UART REPL) — shot(즉시 촬영) / ls(SD 파일 목록) /
 * get <id>(파일을 base64로 덤프, tools/cam_get.py로 로컬에 저장). idf.py monitor나
 * VS Code 시리얼 터미널에서 그대로 사용. 운영 빌드에서는 제거 대상(TODO).
 */
void dev_console_start(void);

/**
 * @brief shot/ls/get 중 하나라도 최근에 쓰였으면 true — cam_node.c의 자동 촬영 타이머가
 *        이걸 보고 이번 주기를 건너뛴다. ls로 본 파일이 자동 촬영의 순환삭제(오래된 것부터
 *        삭제)로 get 하기 전에 사라지는 걸 막기 위함.
 */
bool dev_console_auto_capture_paused(void);
