"""Esc menu visibility (issue #6): boot to the title, press Esc, capture the window itself and the
game's screenshot, with 60 fps off and on. Writes scratchpad/menu/<interp>/*.png.

    python tools/menu_test.py
"""

import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
import scenarios
from controls_test import Keys, hwnd_of, log_since


def run(interp):
    out = os.path.join(drive.SCRATCH, "menu", f"interp{interp}")
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    g = drive.Port(out, None, extra_env={"SR_INTERP": interp, "SR_MENULOG": "1"})
    try:
        scenarios.to_title_menu(g)
        kb = Keys(hwnd_of(g.p.pid))
        kb.tap("esc"); time.sleep(1.5)
        g.shot("game_shot")
        subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(drive.ROOT, "tools", "capture_window.ps1"),
                        "-Process", "ULUS10567_drive", "-Out", os.path.join(out, "window.png"), "-Client"], capture_output=True, timeout=20)
        kb.tap("down"); time.sleep(0.5)
        subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(drive.ROOT, "tools", "capture_window.ps1"),
                        "-Process", "ULUS10567_drive", "-Out", os.path.join(out, "window2.png"), "-Client"], capture_output=True, timeout=20)
        kb.tap("esc"); time.sleep(0.5)
    finally:
        g.close()
    log = open(os.path.join(out, "port.err"), errors="replace").read()
    print(interp, [l for l in log.splitlines() if "menu" in l.lower()][:8])


for i in ("0", "1"):
    run(i)
