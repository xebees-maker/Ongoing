#!/usr/bin/env python3
"""초점 거리 탐색용 — shot(콘솔 직접촬영) -> ls로 방금 찍은 id 찾기 -> get -> 선명도 점수.
   한 번 실행에 한 라운드(연결/리셋 1회). cam_get.py를 모듈로 재사용."""
import sys
import re
import numpy as np
from PIL import Image

sys.path.insert(0, ".")
import cam_get  # noqa: E402

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
LABEL = sys.argv[2] if len(sys.argv) > 2 else "?"
OUT_DIR = "_focus_test"

import os
os.makedirs(OUT_DIR, exist_ok=True)

ser = cam_get.connect_and_sync(PORT)
try:
    ok = cam_get.cmd_shot(ser, [])
    if not ok:
        print("FAIL: shot 실패")
        sys.exit(1)

    raw = cam_get.send_command(ser, "ls", timeout=15.0)
    m = re.search(r"BEGIN_LS\r?\n(.*?)END_LS", raw, re.DOTALL)
    if not m:
        print("FAIL: ls 파싱 실패")
        sys.exit(1)
    lines = [l for l in m.group(1).strip().splitlines() if l.strip()]
    if not lines:
        print("FAIL: 목록 비어있음")
        sys.exit(1)
    # "M <id>  <시각>  <바이트수>" 형식, 마지막 줄이 방금 찍은 것(가장 최근)
    last_id = lines[-1].split()[1]

    out_path = f"{OUT_DIR}/dist_{LABEL}_id{last_id}.jpg"
    if not cam_get.cmd_get(ser, last_id, out_path):
        print("FAIL: get 실패")
        sys.exit(1)
finally:
    ser.close()

img = Image.open(out_path).convert("L")
arr = np.asarray(img, dtype=np.float64)
# 3x3 라플라시안 커널로 컨볼루션(엣지 강도) -> 분산이 클수록 선명(초점 잘 맞음)
kernel = np.array([[0, 1, 0], [1, -4, 1], [0, 1, 0]])
h, w = arr.shape
lap = np.zeros((h - 2, w - 2))
for dy in range(3):
    for dx in range(3):
        if kernel[dy, dx] == 0:
            continue
        lap += kernel[dy, dx] * arr[dy:dy + h - 2, dx:dx + w - 2]
sharpness = float(lap.var())

print(f"거리={LABEL}  파일={out_path}  선명도점수={sharpness:.1f}")
