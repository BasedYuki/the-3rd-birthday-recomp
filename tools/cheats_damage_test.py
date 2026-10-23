"""No-damage check: stand in a fight (scene 25) for 45 s and let the enemies shoot, once without
cheats and once with only no_damage (cheats.cfg written before the start); compare the LIFE bar.

    python tools/cheats_damage_test.py
Writes scratchpad/cheats_damage/{off,on}/*.png.
"""

import os
import shutil
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
import scenarios


def run(tag, cheats):
    out = os.path.join(drive.SCRATCH, "cheats_damage", tag)
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    if cheats: open(os.path.join(out, "cheats.cfg"), "w").write(cheats)
    tmp = os.path.join(drive.BUILD, "save_scene")
    shutil.rmtree(tmp, ignore_errors=True); shutil.copytree(drive.SAVES["user"][0], tmp)
    f = os.path.join(tmp, "ULUS10567DATA00", "3RDDATA.BIN")
    d = bytearray(open(f, "rb").read()); struct.pack_into("<I", d, 0xB9D8, 0xFFFF0000 | 25); open(f, "wb").write(d)
    g = drive.Port(out, tmp)
    try:
        scenarios.to_title_menu(g)
        g.press("cross"); g.wait(4); g.press("cross"); g.wait(25)
        g.shot("t00")
        g.stick(128, 1, 90); g.wait(2)            # walk towards the soldiers a little
        for t in (15, 30, 45):
            g.wait(15); g.shot(f"t{t}")
    finally:
        g.close()


run("off", "")
run("on", "no_damage = 1\n")
