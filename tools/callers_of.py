"""List direct callers (jal) of the functions containing given addresses.

    python tools/callers_of.py <build dir> <hexaddr> [<hexaddr> ...]

Function boundaries come from functions.toml (analyze.py output).
"""

import bisect
import re
import struct
import sys
from pathlib import Path

BASE = 0x08804000

bdir = Path(sys.argv[1])
img = (bdir / "image.bin").read_bytes()
starts = sorted(int(a, 16) for a in re.findall(r"addr = 0x([0-9a-f]+)", (bdir / "functions.toml").read_text()))


def func_of(a):
    i = bisect.bisect_right(starts, a) - 1
    return starts[i] if i >= 0 else None


targets = {func_of(int(x, 16)) for x in sys.argv[2:]}
calls = {t: [] for t in targets}
for off in range(0, len(img) - 3, 4):
    w = struct.unpack_from("<I", img, off)[0]
    if w >> 26 == 3:
        t = ((BASE + off) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        if t in calls:
            calls[t].append(BASE + off)
for t in sorted(calls):
    sites = calls[t]
    print(f"function 0x{t:08x}: {len(sites)} callers")
    for s in sites:
        print(f"    call at 0x{s:08x} in function 0x{func_of(s):08x}")
