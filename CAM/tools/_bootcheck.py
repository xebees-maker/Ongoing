import sys, time
sys.path.insert(0, ".")
import serial
from cam_get import BAUD, read_until_prompt

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
ser = serial.Serial(port, BAUD, timeout=0.1)
print(f"{port} 연결 — 리셋 후 부팅 대기 중...")
# reset_input_buffer()를 리셋 직후 바로 부르면 부팅 초반 로그(카메라 초기화 등)가
# 이미 도착해 있던 걸 그대로 날려버릴 수 있어서 뺌 — 이번엔 처음부터 다 보고 싶음
ser.write(b"\n")
raw = read_until_prompt(ser, overall_timeout=20.0)
ser.close()
print(raw)
