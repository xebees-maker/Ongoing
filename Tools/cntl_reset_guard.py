"""Claude Code PreToolUse 훅(Bash/PowerShell) — 콘 RAM 로그를 받기 전에 콘을 리셋하는 명령을 막음.

2026-10-14(사용자 지시) — 막는 것: 콘 포트(COM19)에 쓰는 플래시(esptool/idf.py, Tools/wait_flash.py 제외 — 그 스크립트는
스스로 먼저 덤프함), 콘 웹 API 중 리셋 계열(/api/restart, /api/sd/format, /api/sd/reconnect).
통과 조건: <로그 폴더>/cntl_dumps/LAST_OK 가 10분 안에 갱신됨(Tools/cntl_predump.py 성공).
"""
import json, os, re, sys, time

MARKER = r'C:/Projects/serial_logs/cntl_dumps/LAST_OK'
FRESH_SEC = 600

try:
    cmd = json.load(sys.stdin).get('tool_input', {}).get('command', '') or ''
except Exception:
    sys.exit(0)

flash_cntl = re.search(r'COM19\b', cmd, re.I) and re.search(r'esptool|idf\.py|write[-_]flash|wait_flash', cmd, re.I) \
    and not re.search(r'Tools[/\\]+wait_flash\.py', cmd, re.I)
reset_api = re.search(r'/api/(restart|sd/format|sd/reconnect)\b', cmd) \
    and re.search(r'curl|wget|Invoke-WebRequest|Invoke-RestMethod|iwr\b|irm\b|urlopen|requests\.|http\.client', cmd, re.I)
if not (flash_cntl or reset_api):
    sys.exit(0)

fresh = os.path.exists(MARKER) and (time.time() - os.path.getmtime(MARKER)) < FRESH_SEC
if fresh:
    sys.exit(0)
print(json.dumps({"hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "deny",
    "permissionDecisionReason": "콘을 리셋하기 전에 RAM 로그부터 받으세요: python C:/Projects/Ongoing/Tools/cntl_predump.py "
                                "(성공하면 10분 안에 다시 시도). 콘 플래시는 Tools/wait_flash.py를 쓰면 자동으로 받습니다."}},
    ensure_ascii=True))
sys.exit(0)
