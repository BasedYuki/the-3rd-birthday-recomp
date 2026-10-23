"""No-damage check on Aya's HP itself: SR_PCPROBE at the low-HP check (0x0896D6A4, a0 = Aya)
gives her object, the control port sets her HP to half; without the cheat it stays there, with
no_damage the patched check stores max HP back.

    python tools/cheats_hp_test.py
"""

import os
import re
import shutil
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
import scenarios


def run(tag, cheats):
    out = os.path.join(drive.SCRATCH, "cheats_hp", tag)
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    if cheats: open(os.path.join(out, "cheats.cfg"), "w").write(cheats)
    tmp = os.path.join(drive.BUILD, "save_scene")
    shutil.rmtree(tmp, ignore_errors=True); shutil.copytree(drive.SAVES["user"][0], tmp)
    f = os.path.join(tmp, "ULUS10567DATA00", "3RDDATA.BIN")
    d = bytearray(open(f, "rb").read()); struct.pack_into("<I", d, 0xB9D8, 0xFFFF0000 | 25); open(f, "wb").write(d)
    g = drive.Port(out, tmp, extra_env={"SR_PCPROBE": "0896d6a4:1"})
    try:
        scenarios.to_title_menu(g)
        g.press("cross"); g.wait(4); g.press("cross"); g.wait(25)
        for _ in range(30):                          # the log is buffered: wait for the register line
            err = open(os.path.join(out, "port.err"), errors="replace").read()
            m = re.search("PCPROBE pc=0x0896d6a4.*?r4=([0-9a-f]{8})", err, re.S)
            if m: break
            time.sleep(1)
        if not m:
            print(tag, "no probe output"); return
        aya = int(m.group(1), 16)
        hp = lambda: struct.unpack("<f", g.read(aya + 0x36C, 4))[0]
        mx = struct.unpack("<f", g.read(aya + 0x370, 4))[0]
        print(f"{tag}: Aya object {aya:08x}, HP {hp():.1f} / {mx:.1f}", flush=True)
        g._cmd("write %x %x" % (aya + 0x36C, struct.unpack("<I", struct.pack("<f", mx / 2))[0]))
        time.sleep(0.05); a = hp(); time.sleep(2); b = hp()
        print(f"{tag}: HP set to half -> {a:.1f}, 2 s later {b:.1f}", flush=True)
    finally:
        g.close()


run("off", "")
run("on", "no_damage = 1\n")
