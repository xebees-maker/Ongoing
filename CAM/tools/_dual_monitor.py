import serial, threading, time, sys

CNTL_PORT = sys.argv[1] if len(sys.argv) > 1 else "COM18"
CAM_PORT  = sys.argv[2] if len(sys.argv) > 2 else "COM5"
DURATION  = float(sys.argv[3]) if len(sys.argv) > 3 else 35.0

t0 = time.time()
lines = []  # (elapsed, tag, text)
lock = threading.Lock()

def open_port(port):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.dtr = False
    ser.rts = False
    ser.timeout = 0.2
    ser.open()
    return ser

def reader(port, tag):
    # COM5(CAM)는 끊기면 1초 주기로 죽었다 살았다 하는 게 정상 동작(2026-08-21, 사용자 확인
    # — 실제 무선 불안정과 무관) — 여기서 죽으면 전체 스레드가 죽어서 그 뒤 로그를 통째로
    # 놓쳤었음. 포트 예외를 잡아서 재연결하며 캡처를 이어감
    ser = open_port(port)
    buf = b""
    end = t0 + DURATION
    while time.time() < end:
        try:
            data = ser.read(4096)
        except serial.SerialException:
            with lock:
                lines.append((time.time() - t0, tag, "[포트 끊김 — 재연결 시도]"))
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(0.3)
            try:
                ser = open_port(port)
            except Exception:
                time.sleep(0.5)
            continue
        if data:
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                txt = line.decode(errors="replace").rstrip("\r")
                with lock:
                    lines.append((time.time() - t0, tag, txt))
    try:
        ser.close()
    except Exception:
        pass

t1 = threading.Thread(target=reader, args=(CNTL_PORT, "CNTL"))
t2 = threading.Thread(target=reader, args=(CAM_PORT, "CAM "))
t1.start(); t2.start()
t1.join(); t2.join()

lines.sort(key=lambda x: x[0])
with open("tools/_dual_capture.log", "w", encoding="utf-8") as f:
    for elapsed, tag, txt in lines:
        f.write(f"[{elapsed:6.2f}s][{tag}] {txt}\n")

print(f"captured {len(lines)} lines over {DURATION}s -> tools/_dual_capture.log")
