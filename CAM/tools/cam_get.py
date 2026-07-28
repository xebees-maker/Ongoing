#!/usr/bin/env python3
"""
CAM dev_console(main/dev_console.c, UART REPL의 shot/ls/get) 원격 클라이언트.
VS Code 터미널에서 pyserial로 시리얼 포트를 직접 열어 명령을 보내고 응답을 파싱한다.

주의:
  - idf.py monitor(또는 VS Code ESP-IDF 확장의 모니터)와 같은 포트를 동시에 열 수 없다.
    이 스크립트를 쓸 때는 monitor를 먼저 닫을 것.
  - 포트를 열면 대부분의 ESP32 보드에서 DTR/RTS 토글로 하드웨어 리셋이 걸린다(esptool/
    monitor와 동일 동작) — 부팅 로그가 끝나고 "cam>" 프롬프트가 뜰 때까지 몇 초 기다린다.
  - pyserial 필요: ESP-IDF 파이썬 venv(C:\\Espressif\\tools\\python\\v6.0.1\\venv\\Scripts\\
    python.exe)에 이미 포함되어 있음. 별도 파이썬이면 `pip install pyserial`.

사용법:
    python cam_get.py <PORT>                       # 대화형 모드 — 한 번만 연결/리셋하고
                                                    # 이후 명령은 리셋 없이 즉시 처리
    python cam_get.py <PORT> shot [5m|qvga|vga]    # 단발 모드 — 매번 새로 연결(=매번 리셋)
                                                    # 해상도 인자 주면 그걸로 바꾼 뒤 촬영
                                                    # (이후 촬영에도 계속 적용됨)
    python cam_get.py <PORT> ls
    python cam_get.py <PORT> get <id> [저장경로]

예:
    python cam_get.py COM5                         # 대화형: shot / ls / get 1737359123 / exit
    python cam_get.py COM5 shot
    python cam_get.py COM5 shot vga
    python cam_get.py COM5 get 1737359123 ./out/1737359123.jpg
"""
import argparse
import re
import sys
import time

import serial

BAUD = 115200
# 실기에서 실제 프롬프트가 "cam>  "(스페이스 2개, esp_console/linenoise가 붙이는 여분의
# 스페이스 포함)로 나오는 걸 확인 — 정확한 공백 개수에 의존하면 깨지기 쉬우니 뒤쪽 공백은
# rstrip으로 다 걷어내고 "cam>" 자체만 매치한다.
PROMPT = "cam>"


def read_until_prompt(ser, overall_timeout, quiet_window=0.3):
    deadline = time.time() + overall_timeout
    buf = b""
    last_recv = time.time()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            last_recv = time.time()
        elif buf.rstrip().endswith(PROMPT.encode()) and (time.time() - last_recv) > quiet_window:
            break
        else:
            time.sleep(0.02)
    return buf.decode("utf-8", errors="replace")


def send_command(ser, cmd, timeout):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()
    return read_until_prompt(ser, overall_timeout=timeout)


def strip_echo(raw, cmd):
    """echo된 입력 줄과 프롬프트 줄을 걷어내고 실제 출력만 남긴다."""
    keep = []
    for line in raw.splitlines():
        stripped = line.strip()
        if stripped in (cmd.strip(), PROMPT.strip(), ""):
            continue
        keep.append(line)
    return "\n".join(keep).strip()


def cmd_shot(ser, args):
    # SHOT_OK/SHOT_FAIL 토큰은 결과 판정용으로 정규식으로 뽑되, 그 사이에 찍히는 카메라
    # 드라이버 로그(NO-SOI, timeout 등)도 디버깅에 필요해서 원본을 그대로 다 보여준다
    # (이전엔 토큰만 출력하고 나머지를 버려서 로그가 안 보인다는 피드백을 받음 — 2026-07-20)
    cmd = "shot" if not args else f"shot {args[0]}"
    raw = send_command(ser, cmd, timeout=30.0)
    print(strip_echo(raw, cmd))
    m = re.search(r"SHOT_(OK|FAIL)", raw)
    if not m:
        return False
    return m.group(1) == "OK"


def cmd_ls(ser):
    raw = send_command(ser, "ls", timeout=15.0)
    m = re.search(r"BEGIN_LS\r?\n(.*?)END_LS", raw, re.DOTALL)
    if not m:
        print("FAIL: BEGIN_LS/END_LS 마커를 못 찾음 — 원본 출력:")
        print(raw)
        return False
    print(m.group(1).strip())
    return True


BIN_HEADER_RE = re.compile(rb"BEGIN_BIN id=(\d+) size=(\d+)\r?\n")


def _recv_bin(ser, send_cmd, out_path, not_found_msg):
    """base64 왕복 없이 raw 바이너리를 그대로 받는다 — USB가 이미 패킷 레벨에서 오류검출을
    하므로 전송 중 변조는 따로 안 따지고, 크기가 정확히 맞는지만 확인한다."""
    ser.reset_input_buffer()
    ser.write((send_cmd + "\n").encode())
    ser.flush()

    # 1) "BEGIN_BIN id=.. size=.." 헤더 줄이 나올 때까지는 텍스트로 읽는다
    buf = b""
    deadline = time.time() + 15.0
    m = None
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            m = BIN_HEADER_RE.search(buf)
            if m:
                break
        else:
            time.sleep(0.01)

    if not m:
        text = buf.decode("utf-8", errors="replace")
        if not_found_msg and "파일 없음" in text:
            print(not_found_msg)
        elif "저장된 캡처 없음" in text:
            print("FAIL: 플래시에 저장된 캡처 없음")
        else:
            print("FAIL: BEGIN_BIN 헤더를 못 찾음 — 원본 출력:")
            print(text)
        return False

    preamble = buf[:m.start()].decode("utf-8", errors="replace")
    for line in preamble.splitlines():
        if line.strip():
            print("  [dev]", line.strip())

    size = int(m.group(2))
    body_start = m.end()
    data = bytearray(buf[body_start:body_start + size])
    leftover = buf[body_start + size:]  # 헤더 뒤에 이미 같이 도착해있던 나머지(END_BIN/프롬프트 일부)

    # 2) 헤더 뒤부터는 정확히 size바이트를 raw로 채울 때까지 읽는다(텍스트 디코드 안 함)
    deadline = time.time() + 60.0
    while len(data) < size and time.time() < deadline:
        chunk = ser.read(min(4096, size - len(data)))
        if chunk:
            data += chunk
        else:
            time.sleep(0.005)

    if len(data) != size:
        print(f"FAIL: 크기 불일치 (기대={size}, 수신={len(data)}) — 전송 중단/유실, 재시도 요망")
        return False

    # 3) 남은 꼬리(END_BIN + 프롬프트)는 내용 안 쓰고 그냥 흘려보내서 연결 상태만 정리
    tail = leftover
    deadline = time.time() + 5.0
    while not tail.rstrip().endswith(PROMPT.encode()) and time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            tail += chunk
        else:
            time.sleep(0.01)

    with open(out_path, "wb") as f:
        f.write(bytes(data))
    print(f"OK: {out_path} ({len(data)} bytes)")
    return True


def cmd_get(ser, file_id, out_path):
    return _recv_bin(ser, f"get {file_id}", out_path,
                      not_found_msg=f"FAIL: id={file_id} 파일이 SD에 없음 — ls로 다시 확인 후 재시도")


def cmd_clear(ser):
    raw = send_command(ser, "clear", timeout=15.0)
    m = re.search(r"CLEARED (\d+)", raw)
    if not m:
        print("FAIL: 응답 파싱 실패 — 원본 출력:")
        print(raw)
        return False
    print(f"삭제됨: {m.group(1)}개")
    return True


def cmd_auto(ser, args):
    if not args:
        print("사용법: auto <on|off>")
        return False
    raw = send_command(ser, f"auto {args[0]}", timeout=15.0)
    if "AUTO_ON" in raw:
        print("자동촬영: on")
        return True
    if "AUTO_OFF" in raw:
        print("자동촬영: off")
        return True
    print("FAIL: 응답 파싱 실패 — 원본 출력:")
    print(raw)
    return False


def cmd_q(ser, args):
    if not args:
        print("사용법: q <0~63>")
        return False
    raw = send_command(ser, f"q {args[0]}", timeout=15.0)
    if "Q_OK" in raw:
        print(f"화질 변경됨: {args[0]}")
        return True
    if "Q_FAIL" in raw:
        print("FAIL: 화질 변경 실패 — 원본 출력:")
        print(raw)
        return False
    print("FAIL: 응답 파싱 실패 — 원본 출력:")
    print(raw)
    return False


def cmd_xclk(ser, args):
    if not args:
        print("사용법: xclk <1~40> (MHz)")
        return False
    raw = send_command(ser, f"xclk {args[0]}", timeout=15.0)
    if "XCLK_OK" in raw:
        print(f"XCLK 변경됨: {args[0]}MHz")
        return True
    if "XCLK_FAIL" in raw:
        print("FAIL: XCLK 변경 실패 — 원본 출력:")
        print(raw)
        return False
    print("FAIL: 응답 파싱 실패 — 원본 출력:")
    print(raw)
    return False


def dispatch(ser, command, args):
    """성공 여부를 bool로 반환 — 대화형 모드에서는 실패해도 세션을 죽이지 않고 계속 진행."""
    if command == "shot":
        return cmd_shot(ser, args)
    elif command == "ls":
        return cmd_ls(ser)
    elif command == "get":
        if not args:
            print("사용법: get <id> [저장경로]")
            return False
        file_id = args[0]
        out_path = args[1] if len(args) > 1 else f"{file_id}.jpg"
        return cmd_get(ser, file_id, out_path)
    elif command == "clear":
        return cmd_clear(ser)
    elif command == "auto":
        return cmd_auto(ser, args)
    elif command == "q":
        return cmd_q(ser, args)
    elif command == "xclk":
        return cmd_xclk(ser, args)
    else:
        print(f"모르는 명령: {command} (shot / ls / get <id> [경로] / clear / auto <on|off> / q <0-63> / xclk <1-40>)")
        return False


def connect_and_sync(port):
    ser = serial.Serial(port, BAUD, timeout=0.1)
    print(f"{port} 연결 — 리셋 후 부팅 대기 중...")
    ser.reset_input_buffer()
    ser.write(b"\n")
    read_until_prompt(ser, overall_timeout=20.0)  # 부팅 로그(카메라/SD/ESP-NOW 초기화) 흘려보내고 동기화
    return ser


def interactive(port):
    ser = connect_and_sync(port)
    print("연결됨 — shot / ls / get <id> [경로] / clear / auto <on|off> / exit")
    try:
        while True:
            try:
                line = input("> ").strip().lstrip("﻿")
            except EOFError:
                break
            if not line:
                continue
            parts = line.split()
            command, args = parts[0], parts[1:]
            if command in ("exit", "quit"):
                break
            dispatch(ser, command, args)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()


def main():
    ap = argparse.ArgumentParser(description="CAM dev_console 클라이언트")
    ap.add_argument("port", help="시리얼 포트 (예: COM5)")
    ap.add_argument("command", nargs="?", choices=["shot", "ls", "get", "clear", "auto", "q", "xclk"],
                     help="생략하면 대화형 모드로 진입 (한 번만 리셋)")
    ap.add_argument("args", nargs="*")
    ns = ap.parse_args()

    if ns.command is None:
        interactive(ns.port)
        return

    ser = connect_and_sync(ns.port)
    try:
        ok = dispatch(ser, ns.command, ns.args)
    finally:
        ser.close()
    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
