#!/usr/bin/env python3
"""Regenerate assets_root/time_sync.txt with the current PC wall-clock time.

Cntl's RTC seeding (main/rtc_sync.c) has no timezone concept: every value it
reads/writes is raw local-clock digits, never passed through a real UTC
offset (2026-08-01: doing that once made the on-device clock read 9h off,
KST vs UTC). So the local wall-clock time here is encoded with
calendar.timegm() -- which does NOT apply any timezone shift -- instead of
the normal UTC-epoch path, to match that convention.
"""
import calendar
import datetime
import pathlib
import sys


def main() -> int:
    if len(sys.argv) > 1:
        out_path = pathlib.Path(sys.argv[1])
    else:
        out_path = pathlib.Path(__file__).resolve().parent.parent / "assets_root" / "time_sync.txt"

    naive_unix = calendar.timegm(datetime.datetime.now().timetuple())
    out_path.write_text(str(naive_unix))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
