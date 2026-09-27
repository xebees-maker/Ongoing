# Ongoing — Claude 작업 규칙

사용자는 두 PC(임시 PC / 원래 PC)를 오가며 작업함. 한쪽에서 푸시가 빠지거나 git 밖 파일(메모리, sdkconfig)이
안 옮겨지는 일을 막기 위해 아래를 **Claude가 스스로** 함(사용자에게 시키지 않음).

## 세션 시작 시(먼저 할 것)
1. `git fetch` 후 로컬과 원격 비교 — 로컬에만 있는 커밋이나 커밋 안 된 변경이 있으면 **pull 전에** 사용자에게 알림.
2. pull 후 최신 인수인계 문서(`Docs/한일_*.txt` 중 가장 최근)의 맨 위 **"PC 이동 기록"**을 읽음.
3. 거기 적힌 메모리 zip(`Docs/claude_memory_<날짜>.zip`)이 지금 메모리보다 새것이면: 기존 메모리 폴더
   (`~/.claude/projects/C--Projects-Ongoing/memory/`)부터 확인하고, **덮어쓰기 전에 사용자에게 물은 뒤** 풂.
4. 기록된 git 밖 항목(예: sdkconfig 옵션)을 반영하고, 한 일/남은 일을 사용자에게 보고.

## PC를 떠날 때(사용자가 이동한다고 하거나 작업을 마칠 때)
1. 커밋 안 된 변경·푸시 안 된 커밋(`git log origin/<브랜치>..HEAD`) 목록을 보여 주고 푸시 여부를 물음.
2. git 밖 항목 점검: 각 프로젝트 sdkconfig와 sdkconfig.defaults 차이(sdkconfig는 git 제외), 남길 도구(Tools/로).
3. 메모리를 zip으로 다시 묶어 `Docs/claude_memory_<날짜>.zip`에 넣음.
4. 인수인계 문서 맨 위 "PC 이동 기록" 갱신(어느 PC에서, 언제, 브랜치·마지막 커밋, 푸시 여부, 도착해서 할 git 밖 항목).
5. 커밋·푸시(푸시는 사용자가 말했을 때만).

## 그 밖
- 푸시는 사용자가 말할 때만. 스키매틱(References/)은 커밋하지 않음.
- 콘은 앱만 플래시(0x10000) — 전체 플래시는 LittleFS의 설정 파일을 지움.
