import sys, time
sys.path.insert(0, ".")
from cam_get import connect_and_sync, send_command, strip_echo, PROMPT
import re

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
n = int(sys.argv[2]) if len(sys.argv) > 2 else 3

ser = connect_and_sync(port)
print("연결됨 — 같은 부팅 세션에서 shot 반복 테스트")

for i in range(n):
    raw = send_command(ser, "shot", timeout=15.0)
    out = strip_echo(raw, "shot")
    m = re.search(r"SHOT_(OK|FAIL)", raw)
    result = m.group(1) if m else "UNKNOWN"
    print(f"--- shot #{i+1}: {result} ---")
    print(out)
    time.sleep(1)

ser.close()
