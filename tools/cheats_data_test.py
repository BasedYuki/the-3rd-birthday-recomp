"""Data cheats checked against what the game shows: Max BP on the status screen (CTI hub) and
the Liberation gauge in a fight (scene 25), each without and with the cheat.

    python tools/cheats_data_test.py
Writes scratchpad/cheats_data/<run>/*.png.
"""

import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
import scenarios

LIBERATION, BP = 0x09CAB10C, 0x09CABF04   # versions/ULUS10567.toml


def port(tag, cheats, scene=None):
    out = os.path.join(drive.SCRATCH, "cheats_data", tag)
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    if cheats: open(os.path.join(out, "cheats.cfg"), "w").write(cheats)
    save = drive.SAVES["user"][0]
    if scene is not None:
        tmp = os.path.join(drive.BUILD, "save_scene")
        shutil.rmtree(tmp, ignore_errors=True); shutil.copytree(save, tmp)
        f = os.path.join(tmp, "ULUS10567DATA00", "3RDDATA.BIN")
        d = bytearray(open(f, "rb").read()); struct.pack_into("<I", d, 0xB9D8, 0xFFFF0000 | scene); open(f, "wb").write(d)
        save = tmp
    return drive.Port(out, save)


def hub(tag, cheats):
    g = port(tag, cheats)
    try:
        scenarios.to_title_menu(g)
        g.press("cross"); g.wait(4); g.press("cross"); g.wait(20)
        for _ in range(4): g.press("start"); g.wait(4); g.press("cross"); g.wait(4)
        for _ in range(3): g.press("circle"); g.wait(1.5)
        g.press("start"); g.wait(2.5); g.shot("status")           # the status screen shows BP
        print(f"{tag}: BP in memory {struct.unpack('<I', g.read(BP, 4))[0]}", flush=True)
    finally:
        g.close()


def fight(tag, cheats):
    g = port(tag, cheats, 25)
    try:
        scenarios.to_title_menu(g)
        g.press("cross"); g.wait(4); g.press("cross"); g.wait(25)
        vals = []
        for _ in range(4):
            vals.append(round(struct.unpack("<f", g.read(LIBERATION, 4))[0], 1)); g.wait(2)
        g.shot("fight")
        print(f"{tag}: Liberation gauge {vals}", flush=True)
    finally:
        g.close()


hub("hub_off", "")
hub("hub_bp", "max_bp = 1\n")
fight("fight_off", "")
fight("fight_lib", "inf_liberation = 1\n")
