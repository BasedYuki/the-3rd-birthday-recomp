"""Play the port step by step from the command line (it keeps running between calls).

    python tools/portctl.py start [clear|user|two|none|<save dir>] [extra ENV=VAL ...]
    python tools/portctl.py press cross [frames]        (buttons as in drive.py, "l+r" for several)
    python tools/portctl.py stick <x> <y> <frames>      (0..255, 128 centre; y 0 = up)
    python tools/portctl.py seq "<cmd>; <cmd>; ..."     e.g. "stick 128 0 120; wait 2; press r; shot a"
    python tools/portctl.py shot <name>                 -> scratchpad/play/<name>.png (window client area)
    python tools/portctl.py read <hexaddr> <n>
    python tools/portctl.py stop
"""

import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive

OUT = os.path.join(drive.SCRATCH, "play")


def conn():
    s = socket.create_connection(("127.0.0.1", 45680), timeout=10)
    return s, s.makefile("rw")


def cmd(f, line):
    f.write(line + "\n"); f.flush()
    r = f.readline().strip()
    if not r.startswith("ok"): raise SystemExit(f"{line!r} -> {r!r}")
    return r[3:]


def shot(name):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + ".png")
    subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(drive.ROOT, "tools", "capture_window.ps1"),
                    "-Process", "ULUS10567_drive", "-Out", path, "-Client"], capture_output=True)
    print(path)


def run(f, words):
    op = words[0]
    if op == "press":
        m = 0
        for b in words[1].split("+"): m |= drive.BTN[b]
        n = int(words[2]) if len(words) > 2 else 6
        cmd(f, f"btn {m:x} {n}"); time.sleep(n / 60.0 + 0.05)
    elif op == "hold":                     # hold buttons while the stick moves: "hold r 120"
        m = 0
        for b in words[1].split("+"): m |= drive.BTN[b]
        cmd(f, f"btn {m:x} {int(words[2])}")
    elif op == "stick":
        n = int(words[3]); cmd(f, f"stick {words[1]} {words[2]} {n}"); time.sleep(n / 60.0 + 0.05)
    elif op == "wait":
        time.sleep(float(words[1]))
    elif op == "shot":
        shot(words[1])
    elif op == "read":
        print(cmd(f, f"read {words[1]} {words[2]}"))
    elif op == "drawlog":                  # log the draws of the next frame into port.err (DRAW lines)
        cmd(f, "drawlog")
    elif op == "vcount":
        print(cmd(f, "vcount"))
    else:
        raise SystemExit(f"unknown op {op}")


def main():
    a = sys.argv[1:]
    if a[0] == "start":
        save = a[1] if len(a) > 1 else "clear"
        extra = dict(kv.split("=", 1) for kv in a[2:])
        os.makedirs(OUT, exist_ok=True)
        p = drive.Port(OUT, drive.SAVES[save][0] if save in drive.SAVES else save, extra_env=extra)
        p.s.close()                        # leave the game running
        print("started pid", p.p.pid)
        return
    if a[0] == "stop":
        subprocess.run(["taskkill", "/F", "/IM", "ULUS10567_drive.exe"], capture_output=True)
        print("stopped"); return
    s, f = conn()
    if a[0] == "seq":
        for c in a[1].split(";"):
            if c.strip(): run(f, c.split())
    else:
        run(f, a)
    s.close()


if __name__ == "__main__":
    main()
