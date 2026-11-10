#!/usr/bin/env python3
"""Test whether the title's slow file creates and deletes are a per-title throttle.

Hypothesis: once a title has written enough, the console stalls that title's
file operations (~0.95 s each, see include/wine/ps5_writebroker.h in the Wine
fork), while a process outside the title -- the write broker, or the FTP
server this script uses -- is not stalled. If so, moving create, delete and
rename into the broker would remove the stalls.

Run it while a title is busy writing (for example Steam unpacking an update)
and while the kernel log is being captured to a file:

    fs-throttle-probe.py --host PS5-ADDRESS --klog forever-klog.txt

It creates, renames and deletes small files over FTP (a process outside the
title) in a scratch folder under /data/forever, times each operation net of
the FTP round trip, and compares that with the title's own `slow op=` and
`slow create`/`slow write` lines logged during the same window.
"""

from __future__ import annotations

import argparse
import io
import os
import re
import statistics
import sys
import time
from dataclasses import dataclass, field
from ftplib import FTP, all_errors
from pathlib import Path

SLOW_RE = re.compile(
    r"(?:wineserver: server slow op=(?P<op>\w+)|wine: title slow (?P<kind>create|write)\b)"
    r".*?\bms=(?P<ms>\d+)")

# Verdict thresholds (milliseconds).
THROTTLED_MS = 300   # a title operation this slow counts as stalled
FREE_MS = 50         # an outside operation under this is not stalled
MIN_TITLE_SAMPLES = 5


@dataclass
class Samples:
    values: dict[str, list[float]] = field(default_factory=dict)

    def add(self, name: str, ms: float) -> None:
        self.values.setdefault(name, []).append(ms)

    def median(self, name: str) -> float | None:
        values = self.values.get(name)
        return statistics.median(values) if values else None


# Once stalls are routine the server stops logging each one and only reports
# 10 s windows: "server time window_ms=… top_ms set_fd_name_info=153/7151ms/max934ms
# close_handle=228/3149ms/max938ms …". A window whose max is a stall is
# counted as total//max stalls of the max's length (an estimate).
WINDOW_RE = re.compile(r"wineserver: server time window_ms=\d+.*")
WINDOW_OPS = {"set_fd_name_info": "rename", "close_handle": "unlink"}
WINDOW_FIELD_RE = re.compile(r"\b(\w+)=(\d+)/(\d+)ms/max(\d+)ms")


def parse_title_log(text: str) -> Samples:
    """Collect the title's slow file operations from kernel log text."""
    samples = Samples()
    for match in SLOW_RE.finditer(text):
        name = match.group("op") or match.group("kind")
        if name == "open_create":
            continue  # the same event is also logged as "title slow create"
        samples.add(name, float(match.group("ms")))
    for line in WINDOW_RE.findall(text):
        for request, count, total, worst in WINDOW_FIELD_RE.findall(line):
            name = WINDOW_OPS.get(request)
            worst_ms = float(worst)
            if name and worst_ms >= THROTTLED_MS:
                for _ in range(min(int(count), int(float(total) // worst_ms))):
                    samples.add(name, worst_ms)
    return samples


def verdict(title: Samples, outside: Samples) -> tuple[str, str]:
    stalled = [ms for name in ("create", "unlink", "rename") for ms in title.values.get(name, [])
               if ms >= THROTTLED_MS]
    if len(stalled) < MIN_TITLE_SAMPLES:
        return ("INCONCLUSIVE",
                "the title logged fewer than %d stalled creates/unlinks during the probe; "
                "rerun while it is visibly slow" % MIN_TITLE_SAMPLES)
    worst = max(outside.median(name) or 0.0 for name in ("create", "rename", "delete"))
    if worst < FREE_MS:
        return ("SUPPORTED",
                "the title's file operations stalled (median %.0f ms) while the same "
                "operations outside the title took at most %.1f ms (median); a broker-side "
                "create/delete/rename would escape the throttle"
                % (statistics.median(stalled), worst))
    if worst >= THROTTLED_MS:
        return ("REJECTED",
                "operations outside the title stalled too (worst median %.0f ms): the "
                "slowdown is device-wide, so routing through the broker would not help" % worst)
    return ("MIXED",
            "outside operations were slower than %d ms but not stalled (worst median %.0f ms); "
            "rerun, and check the console's network and disk load" % (FREE_MS, worst))


def measure_outside(ftp: FTP, folder: str, count: int, size: int) -> Samples:
    samples = Samples()
    rtt = []
    for _ in range(20):
        start = time.perf_counter()
        ftp.voidcmd("NOOP")
        rtt.append((time.perf_counter() - start) * 1000)
    base = statistics.median(rtt)
    samples.add("rtt", base)
    payload = os.urandom(size)

    def timed(name: str, call) -> None:
        start = time.perf_counter()
        call()
        samples.add(name, max(0.0, (time.perf_counter() - start) * 1000 - base))

    for i in range(count):
        path = f"{folder}/f{i:04d}.bin"
        # STOR needs a data connection: about one more round trip than NOOP.
        timed("create", lambda: ftp.storbinary("STOR " + path, io.BytesIO(payload)))
        timed("rename", lambda: (ftp.sendcmd("RNFR " + path),
                                 ftp.voidcmd("RNTO " + path + ".old")))
        timed("delete", lambda: ftp.delete(path + ".old"))
    for i in range(min(count, 10)):
        timed("mkdir", lambda: ftp.mkd(f"{folder}/d{i:02d}"))
        timed("rmdir", lambda: ftp.rmd(f"{folder}/d{i:02d}"))
    return samples


def report(title: Samples, outside: Samples) -> None:
    print("outside the title (FTP server process), net of %.1f ms round trip:"
          % (outside.median("rtt") or 0.0))
    for name in ("create", "rename", "delete", "mkdir", "rmdir"):
        values = outside.values.get(name, [])
        if values:
            stalled = sum(ms >= THROTTLED_MS for ms in values)
            print("  %-7s n=%3d median=%7.1f ms  max=%7.1f ms  stalled=%d"
                  % (name, len(values), statistics.median(values), max(values), stalled))
    print("inside the title (kernel log, same window):")
    if not title.values:
        print("  no slow file operations logged")
    for name, values in sorted(title.values.items()):
        print("  %-7s n=%3d median=%7.1f ms  max=%7.1f ms"
              % (name, len(values), statistics.median(values), max(values)))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", required=True, help="PS5 address")
    parser.add_argument("--port", type=int, default=2120, help="FTP port (default 2120)")
    parser.add_argument("--klog", type=Path, required=True,
                        help="file the kernel log is being captured to")
    parser.add_argument("--count", type=int, default=300,
                        help="files to create (default 300, longer than one 10 s log window)")
    parser.add_argument("--size", type=int, default=4096, help="bytes per file (default 4096)")
    parser.add_argument("--settle", type=float, default=12.0,
                        help="seconds to wait for log lines after the probe (default 12, "
                             "one log window)")
    args = parser.parse_args(argv)

    folder = "/data/forever/throttle-probe-%d" % os.getpid()
    start_offset = args.klog.stat().st_size
    ftp = FTP()
    ftp.connect(args.host, args.port, timeout=30)
    ftp.login()
    try:
        ftp.mkd(folder)
        try:
            outside = measure_outside(ftp, folder, args.count, args.size)
        finally:
            try:
                for name in ftp.nlst(folder):
                    ftp.delete(name if name.startswith("/") else f"{folder}/{name}")
            except all_errors:
                pass
            try:
                ftp.rmd(folder)
            except all_errors as error:
                print("warning: could not remove %s: %s" % (folder, error), file=sys.stderr)
    finally:
        ftp.quit()
    time.sleep(args.settle)
    with args.klog.open("rb") as log:
        log.seek(start_offset)
        title = parse_title_log(log.read().decode("utf-8", "replace"))

    report(title, outside)
    result, reason = verdict(title, outside)
    print("%s: %s" % (result, reason))
    return 0 if result in ("SUPPORTED", "REJECTED") else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
