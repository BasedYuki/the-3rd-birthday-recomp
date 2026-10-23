"""Game speed with 60 fps frame interpolation on a slow PC, simulated: SR_SLOWINTERP=<ms> makes
every in-between frame cost that much more. Loads a fight (scene 25), walks around for 40 s and
prints the PACE lines (vblanks per 5 s: 300 = full speed) and the interpolation's fallbacks.

    python tools/pace_test.py [slow_ms ...]       e.g. 0 25
"""

import os
import shutil
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
import scenarios


def run(slow, adapt="1"):
    out = os.path.join(drive.SCRATCH, "pace", f"slow{slow}_adapt{adapt}")
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    tmp = os.path.join(drive.BUILD, "save_scene")
    shutil.rmtree(tmp, ignore_errors=True); shutil.copytree(drive.SAVES["user"][0], tmp)
    f = os.path.join(tmp, "ULUS10567DATA00", "3RDDATA.BIN")
    d = bytearray(open(f, "rb").read()); struct.pack_into("<I", d, 0xB9D8, 0xFFFF0000 | 25); open(f, "wb").write(d)
    g = drive.Port(out, tmp, extra_env={"SR_INTERP": "1", "SR_PACELOG": "1", "SR_SLOWINTERP": str(slow), "SR_INTERP_ADAPT": adapt})
    try:
        scenarios.to_title_menu(g)
        g.press("cross"); g.wait(4); g.press("cross"); g.wait(20)
        for i in range(8):                       # walk around in a square, 40 s
            g.stick((1, 128, 255, 128)[i % 4], (128, 1, 128, 255)[i % 4], 150); g.wait(5)
    finally:
        g.close()
    for line in open(os.path.join(out, "port.err"), errors="replace"):
        if line.startswith("PACE") or line.startswith("interp:"):
            print(f"slow{slow} adapt{adapt}: {line.rstrip()}")


# arguments: slow_ms[:0] (":0" = without the fallback)
for a in (sys.argv[1:] or ["0", "25"]):
    ms, _, ad = a.partition(":")
    run(int(ms), ad or "1")
