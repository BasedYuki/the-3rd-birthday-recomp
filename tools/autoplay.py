"""A simple autoplayer for the running port (start it with tools/portctl.py start ...).

Reads the radar in the window capture: red dots = enemies (hold L to lock on + R to fire), the
green triangle on the ring = the objective (steer the stick toward it; the radar is Aya-relative,
up = her facing). Presses CROSS when a dialog/"Skip?" box seems to wait. Logs each step and keeps
every Nth capture in scratchpad/play/auto/.

    python tools/autoplay.py <steps> [shot_every]
"""

import math
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
import portctl
from PIL import Image

OUT = os.path.join(portctl.OUT, "auto")
os.makedirs(OUT, exist_ok=True)
CAP = os.path.join(OUT, "_cur.png")


def capture():
    subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(drive.ROOT, "tools", "capture_window.ps1"),
                    "-Process", "ULUS10567_drive", "-Out", CAP, "-Client"], capture_output=True)
    return Image.open(CAP).convert("RGB")


def radar(im):
    w, h = im.size
    cx, cy, r = 0.922 * w, 0.118 * h, 0.067 * w
    px = im.load()
    reds, greens, orange, blue = [], [], 0, 0
    for y in range(int(cy - r), int(cy + r)):
        for x in range(int(cx - r), int(cx + r)):
            dx, dy = x - cx, y - cy
            d = math.hypot(dx, dy) / r
            if d > 1.08: continue
            R, G, B = px[x, y]
            if R > 190 and G < 70 and B < 70 and d < 0.95: reds.append((dx, dy))
            elif G > 150 and R < 120 and B < 140 and 0.55 < d < 1.08: greens.append((dx, dy))
            if 0.8 < d < 1.0:
                if R > 150 and G > 70 and B < 80: orange += 1
                elif B > 120 and R < 80: blue += 1
    def mean_angle(pts):   # 0 = up, clockwise, degrees
        if not pts: return None
        sx = sum(p[0] for p in pts) / len(pts); sy = sum(p[1] for p in pts) / len(pts)
        return math.degrees(math.atan2(sx, -sy)) % 360
    return {"enemies": len(reds), "enemy_dir": mean_angle(reds), "goal": mean_angle(greens),
            "alert": orange > blue}


def small(im):
    return list(im.resize((32, 18)).convert("L").tobytes())


def stick_dir(deg):
    a = math.radians(deg)
    return int(128 + 127 * math.sin(a)), int(128 - 127 * math.cos(a))


def main():
    steps = int(sys.argv[1]); every = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    s, f = portctl.conn()
    prev = None; stuck = 0; still = 0; log = open(os.path.join(OUT, "log.txt"), "a")
    for i in range(steps):
        im = capture()
        rd = radar(im)
        cur = small(im)
        diff = sum(abs(a - b) for a, b in zip(cur, prev)) / len(cur) if prev else 99
        prev = cur
        if rd["enemies"] or rd["alert"]:
            act = "fight"
            if rd["enemy_dir"] is not None and 45 < rd["enemy_dir"] < 315 and rd["enemies"]:
                x, y = stick_dir(rd["enemy_dir"]); portctl.cmd(f, f"stick {x} {y} 12"); time.sleep(0.25)
            portctl.cmd(f, "btn 300 100"); time.sleep(1.8)            # L + R
            stuck = 0
        elif rd["goal"] is not None:
            act = f"goal {rd['goal']:.0f}"
            x, y = stick_dir(rd["goal"])
            if diff < 2.0: stuck += 1
            else: stuck = 0
            if stuck >= 2:                                             # wall: sidestep
                x, y = stick_dir((rd["goal"] + (90 if stuck % 4 < 2 else -90)) % 360); act += " sidestep"
            portctl.cmd(f, f"stick {x} {y} 90"); time.sleep(1.6)
        else:
            still = still + 1 if diff < 3.0 else 0
            if still >= 3:                         # nothing moves: a dialog / prompt waits for X
                act = "still: cross"; still = 0
                portctl.cmd(f, "btn 4000 6"); time.sleep(1.2)
            else:
                act = "no radar: wait"; time.sleep(1.5)
        line = f"{i:4d} {act:24s} enemies={rd['enemies']} alert={rd['alert']} diff={diff:.1f}"
        print(line, flush=True); log.write(line + "\n"); log.flush()
        if i % every == 0:
            im.save(os.path.join(OUT, f"a{int(time.time())}_{i:04d}.png"))
    s.close()


if __name__ == "__main__":
    main()
