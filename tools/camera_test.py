"""Camera styles side by side: load a fight (scene 25) in the classic and the modern camera style,
turn with the mouse, walk, aim; screenshots of each step (scratchpad/camera/<style>/).

    python tools/camera_test.py [classic|modern|both]
"""

import os
import shutil
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
import scenarios


def run(style):
    out = os.path.join(drive.SCRATCH, "camera", style)
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    tmp = os.path.join(drive.BUILD, "save_scene")
    shutil.rmtree(tmp, ignore_errors=True); shutil.copytree(drive.SAVES["user"][0], tmp)
    f = os.path.join(tmp, "ULUS10567DATA00", "3RDDATA.BIN")
    d = bytearray(open(f, "rb").read()); struct.pack_into("<I", d, 0xB9D8, 0xFFFF0000 | 25); open(f, "wb").write(d)
    g = drive.Port(out, tmp, extra_env={"SR_TWINSTICK": "1", "SR_CAMSTYLE": "1" if style == "modern" else "0"})
    try:
        scenarios.to_title_menu(g)
        g.press("cross"); g.wait(4); g.press("cross"); g.wait(25)
        g.shot("a_start")
        for _ in range(10): g._cmd("mouse 40 0"); time.sleep(0.05)      # turn right ~50 degrees
        g.wait(1); g.shot("b_turned")
        g.stick(128, 1, 75); g.wait(0.5); g.shot("c_walking")              # walk forward 2.5 s
        g.wait(2); g.shot("d_after_walk")
        g._cmd("btn 100 300"); g.wait(1.5); g.shot("e_aiming")              # hold L (aim) 5 s
        for _ in range(30): g._cmd("mouse 12 0"); time.sleep(0.033)        # mouse right 1 s: aim moves right
        g.wait(0.3); g.shot("f_aim_moved")
        g.wait(4); g.shot("g_after_aim")
    finally:
        g.close()


which = sys.argv[1] if len(sys.argv) > 1 else "both"
for st in (("classic", "modern") if which == "both" else (which,)):
    run(st)
