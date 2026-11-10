#!/usr/bin/env python3
"""Offline checks for fs-throttle-probe.py: log parsing and verdicts."""

import importlib.util
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "probe", Path(__file__).with_name("fs-throttle-probe.py"))
probe = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = probe  # dataclasses resolve annotations through sys.modules
spec.loader.exec_module(probe)

LOG = """\
wineserver: server slow op=open_create ms=408 name=/data/forever/x/a.png_
wine: title slow create pid=122 ms=408 lookup=0 open=408 disp=5 status=0 scans=266 entries=142310 path=/data/forever/x/a.png_
wine: title slow write pid=122 ms=400 bytes=5057 offset=0 size=0->5057 allocated=0->65536 path=/data/forever/x/b.png_
wineserver: server slow op=unlink ms=937 name=/data/forever/x/c.old
wineserver: server slow op=unlink ms=941 name=/data/forever/x/d.old
wineserver: server slow op=unlink ms=946 name=/data/forever/x/e.old
wine: title slow create pid=122 ms=556 lookup=0 open=556 disp=5 status=0 scans=82 entries=9 path=/data/forever/x/f
wine: title write route small=broker reason=local-slow pid=122
"""


def outside(create, rename, delete):
    samples = probe.Samples()
    for name, ms in (("create", create), ("rename", rename), ("delete", delete)):
        for _ in range(5):
            samples.add(name, ms)
    return samples


title = probe.parse_title_log(LOG)
assert sorted(title.values) == ["create", "unlink", "write"], title.values
assert title.values["create"] == [408.0, 556.0]      # open_create not double-counted
assert title.values["unlink"] == [937.0, 941.0, 946.0]
assert title.values["write"] == [400.0]

assert probe.verdict(title, outside(2, 3, 2))[0] == "SUPPORTED"
assert probe.verdict(title, outside(900, 3, 940))[0] == "REJECTED"
assert probe.verdict(title, outside(120, 3, 2))[0] == "MIXED"
quiet = probe.parse_title_log("wine: title slow create pid=1 ms=20 path=x\n")
assert probe.verdict(quiet, outside(2, 3, 2))[0] == "INCONCLUSIVE"

# Window summaries: 7151 ms over max 934 ms = 7 stalled renames; 3149/938 = 3
# stalled deletes; a fast window adds nothing.
window = probe.parse_title_log(
    "wineserver: server time window_ms=10560 handler_ms=10342 top_ms "
    "set_fd_name_info=153/7151ms/max934ms close_handle=228/3149ms/max938ms "
    "create_file=229/37ms/max0ms get_handle_fd=153/1ms/max0ms\n"
    "wineserver: server time window_ms=10000 handler_ms=50 top_ms "
    "set_fd_name_info=10/20ms/max5ms close_handle=10/9ms/max2ms\n")
assert window.values == {"rename": [934.0] * 7, "unlink": [938.0] * 3}, window.values
assert probe.verdict(window, outside(2, 3, 2))[0] == "SUPPORTED"
print("PASS test-fs-throttle-probe")
