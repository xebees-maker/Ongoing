"""보드 앱 플래시(0x10000만 — 전체 플래시는 LittleFS 설정을 지움). 포트가 나타날 때까지 기다렸다 플래시.

사용: python wait_flash.py <포트> <bin> <대기초> [칩(기본 esp32s3)]
2026-10-14(사용자 지시) — 콘 포트(CNTL_PORT)면 플래시 전에 반드시 Tools/cntl_predump.py로 콘 RAM 로그를 받아 두고,
실패하면 플래시하지 않음(재시작하면 RAM 로그가 지워짐 — 사후 분석 자료). 상시 로그 수집기가 포트를 쥐고 있으면
<로그 폴더>/<이름>.pause 를 먼저 만들 것.
"""
import os, subprocess, sys, time
from serial.tools import list_ports

CNTL_PORT = 'COM19'
HERE = os.path.dirname(os.path.abspath(__file__))
PY = r"C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe"

port = sys.argv[1]
binpath = sys.argv[2]
CHIP = sys.argv[4] if len(sys.argv) > 4 else "esp32s3"
deadline = time.time() + float(sys.argv[3])

if port.upper() == CNTL_PORT:
    r = subprocess.run([sys.executable, os.path.join(HERE, 'cntl_predump.py')], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode != 0:
        print("FLASH_ABORTED - Cntl RAM log dump failed (do not reset Cntl before the logs are saved)")
        sys.exit(2)

while time.time() < deadline:
    if any(p.device == port for p in list_ports.comports()):
        r = subprocess.run([PY, "-m", "esptool", "--chip", CHIP, "-p", port, "-b", "460800",
                            "write-flash", "0x10000", binpath], capture_output=True, text=True)
        out = r.stdout + r.stderr
        print(out[-1500:])
        if r.returncode == 0 and "Hash of data verified" in out:
            print("FLASH_OK", time.strftime("%H:%M:%S"))
            sys.exit(0)
        print("flash attempt failed, retrying")
    time.sleep(0.5)
print("TIMEOUT - port never appeared")
sys.exit(1)
