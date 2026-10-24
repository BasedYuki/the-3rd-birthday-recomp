"""Widescreen check (issue #7): load the user's save (the CTI hub), widescreen on/off in a given
window size, screenshots of Aya. Writes scratchpad/ws/<tag>/*.png.

    python tools/ws_test.py [WxH ...]      default 1918x1027 2520x1080
"""

import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
import scenarios


def run(size, ws):
    tag = f"{size}_ws{ws}"
    out = os.path.join(drive.SCRATCH, "ws", tag)
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    g = drive.Port(out, drive.SAVES["user"][0], extra_env={"SR_WIDESCREEN": ws, "SR_WINSIZE": size})
    try:
        scenarios.to_title_menu(g)
        g.press("cross"); g.wait(4); g.press("cross"); g.wait(20)
        for _ in range(4):                                      # skip the movie / talk into the hub
            g.press("start"); g.wait(4); g.press("cross"); g.wait(4)
        for _ in range(3): g.press("circle"); g.wait(1.5)      # close the status menu Start opened
        g.shot("a")
        g.stick(1, 128, 20); g.wait(1.5); g.shot("b")          # turn a little
        g.stick(255, 128, 40); g.wait(1.5); g.shot("c")
    finally:
        g.close()


for s in (sys.argv[1:] or ["1918x1027", "2520x1080"]):
    for ws in ("0", "1"):
        run(s, ws)
