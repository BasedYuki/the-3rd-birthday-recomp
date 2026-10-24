"""Cheats test: load a fight, fire with the cheats off, switch them on in the Esc menu (real key
messages, like a player), fire again; screenshots of the HUD and the memory the cheats touch.

    python tools/cheats_test.py [--scene 25] [--save user]
Writes scratchpad/cheats/report.txt and screenshots.
"""

import os
import shutil
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
from controls_test import Keys, hwnd_of, log_since

OUT = os.path.join(drive.SCRATCH, "cheats")
LIBERATION, BP = 0x09CAB10C, 0x09CABF04   # versions/ULUS10567.toml


def main():
    scene = int(sys.argv[sys.argv.index("--scene") + 1]) if "--scene" in sys.argv else 25
    save = sys.argv[sys.argv.index("--save") + 1] if "--save" in sys.argv else "user"
    shutil.rmtree(OUT, ignore_errors=True); os.makedirs(OUT)
    src = drive.SAVES[save][0]                       # a copy with the scene index patched (drive.py --scene)
    tmp = os.path.join(drive.BUILD, "save_scene")
    shutil.rmtree(tmp, ignore_errors=True); shutil.copytree(src, tmp)
    f = os.path.join(tmp, "ULUS10567DATA00", "3RDDATA.BIN")
    d = bytearray(open(f, "rb").read()); struct.pack_into("<I", d, 0xB9D8, 0xFFFF0000 | scene); open(f, "wb").write(d)
    g = drive.Port(OUT, tmp, extra_env={"SR_KEYLOG": "0"})
    errp = os.path.join(OUT, "port.err")
    rep = []
    def check(label, ok, detail=""):
        rep.append(f"{'PASS' if ok else 'FAIL'}  {label}  {detail}"); print(rep[-1], flush=True)
    def fire(seconds, mid=None):
        end, shot = time.time() + seconds, False
        while time.time() < end:
            g._cmd("btn 300 20"); time.sleep(0.4)      # hold L (aim) + R (fire) for 20 frames
            if mid and not shot and time.time() > end - seconds / 2:
                g._cmd("btn 300 20"); g.shot(mid); shot = True
    def f32(a): return struct.unpack("<f", g.read(a, 4))[0]
    def u32(a): return struct.unpack("<I", g.read(a, 4))[0]
    try:
        # title -> Load Game -> slot 1 (drive.scenarios.scene_quick), then into the fight
        import scenarios
        scenarios.to_title_menu(g)
        g.press("cross"); g.wait(4); g.press("cross"); g.wait(25)
        g.shot("a_start")
        lib0, bp0 = f32(LIBERATION), u32(BP)
        fire(6, "b_mid_off"); g.shot("b_fired_cheats_off")
        h = hwnd_of(g.p.pid); kb = Keys(h)
        _, pos = log_since(errp, 0)
        kb.tap("esc"); time.sleep(0.6); kb.tap("down"); kb.tap("down"); kb.tap("enter"); time.sleep(0.5)
        for i in range(5):                            # every cheat on
            kb.tap("enter"); time.sleep(0.2); kb.tap("down")
        time.sleep(0.3); g.shot("c_cheats_page")
        kb.tap("esc"); time.sleep(0.3); kb.tap("esc"); time.sleep(0.8)
        t, pos = log_since(errp, pos)
        on = [l.split()[1] for l in t.splitlines() if l.startswith("cheats:") and l.endswith(" on")]
        check("Esc > Cheats switches all five on", len(on) == 5, f"{on}")
        txt = open(os.path.join(OUT, "cheats.cfg")).read()
        check("cheats.cfg saved", all(f"{k} = 1" in txt for k in ("no_damage", "inf_ammo", "inf_grenades", "inf_liberation", "max_bp")))
        g.shot("d_before_fire"); fire(12, "e_mid_on"); g.shot("e_fired_cheats_on")
        check("Liberation gauge held full", abs(f32(LIBERATION) - 1000.0) < 0.5, f"{lib0:.1f} -> {f32(LIBERATION):.1f}")
        check("BP held at 999999", u32(BP) == 999999, f"{bp0} -> {u32(BP)}")
        g.wait(20); g.shot("f_after_20s")
    finally:
        g.close()
    open(os.path.join(OUT, "report.txt"), "w").write("\n".join(rep) + "\n")


if __name__ == "__main__":
    main()
