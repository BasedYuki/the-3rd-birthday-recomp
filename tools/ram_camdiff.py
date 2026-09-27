"""Find the camera angle in RAM dumps taken around scripted camera input (see pad_cam.txt).

    python tools/ram_camdiff.py <dir with ram_<frame>.bin>

Dumps sorted by frame are expected as: idle A, idle B, LEFT-during, LEFT-end, after-left,
RIGHT-during, RIGHT-end, after-right. Reports 32-bit floats and 16/32-bit ints that stay put
while idle, move one way during LEFT and the other way during RIGHT.
"""

import glob
import os
import re
import struct
import sys

import numpy as np

d = sys.argv[1]
files = sorted(glob.glob(os.path.join(d, "ram_*.bin")), key=lambda p: int(re.findall(r"\d+", os.path.basename(p))[0]))
print("dumps:", [os.path.basename(f) for f in files])
raw = [open(f, "rb").read() for f in files]
A, B, L1, L2, L3, R1, R2, R3 = raw[:8]
BASE = 0x08800000


def report(kind, arrs, stride):
    a, b, l1, l2, l3, r1, r2, r3 = arrs
    with np.errstate(all="ignore"):
        idle = a == b
        dl = l2 - b
        dr = r2 - l3
        cand = idle & (l1 != b) & (l2 != b) & np.isfinite(dl) & np.isfinite(dr) & (dl != 0) & (dr != 0)
        cand &= np.sign(dl) == -np.sign(dr)
        cand &= np.sign(l1 - b) == np.sign(dl)          # moved the same way during the hold
        cand &= np.sign(r1 - l3) == np.sign(dr)
        cand &= np.abs(l1 - b) < np.abs(dl)             # mid-hold value lies between the ends
        cand &= np.abs(r1 - l3) < np.abs(dr)
        cand &= np.abs(l3 - l2) <= np.abs(dl) * 0.5     # holds (mostly) after release
        cand &= np.abs(r3 - r2) <= np.abs(dr) * 0.5
        addr = BASE + np.arange(len(a)) * stride
        cand &= ~((addr >= 0x08c50000) & (addr < 0x08c70000))   # GE display lists
        if kind == "f32":
            cand &= (np.abs(b) < 1e5) & (np.abs(dl) < 1e4)
    idx = np.nonzero(cand)[0]
    print(f"--- {kind}: {len(idx)} candidates")
    for i in idx[:60]:
        addr = BASE + i * stride
        print(f"  {addr:08x}  idle {a[i]!r:>12} L {l1[i]!r:>12} {l2[i]!r:>12} after {l3[i]!r:>12}"
              f"  R {r1[i]!r:>12} {r2[i]!r:>12} after {r3[i]!r:>12}")


report("f32", [np.frombuffer(x, dtype="<f4").astype(np.float64) for x in (A, B, L1, L2, L3, R1, R2, R3)], 4)
report("i32", [np.frombuffer(x, dtype="<i4").astype(np.int64) for x in (A, B, L1, L2, L3, R1, R2, R3)], 4)
report("i16", [np.frombuffer(x, dtype="<i2").astype(np.int64) for x in (A, B, L1, L2, L3, R1, R2, R3)], 2)
