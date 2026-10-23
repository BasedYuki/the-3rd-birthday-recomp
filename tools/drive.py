"""Drive the port and PPSSPP through the same scenario and compare their screenshots.

    python tools/drive.py <scenario> [port|ppsspp|both] [--save clear|user]

A scenario is a function in tools/scenarios.py taking a backend `g` with:
    g.press("cross", frames=6)        buttons: cross circle square triangle start select up down
                                      left right l r (several: "l+r")
    g.stick(x, y, frames)             left stick, 0..255 (128 = centre), held for frames
    g.wait(seconds)
    g.shot("name")                    screenshot -> <out>/<backend>/<name>.png
    g.read(addr, n) -> bytes          guest memory (same layout in both)
Output: scratchpad drive/<scenario>/{port,ppsspp}/*.png and compare.png (side by side + diff).

The port runs at real speed with the PC extras off (SR_TWINSTICK=0, SR_INTERP=0, SR_SKIPINTRO=0)
from a temporary memory stick; PPSSPP uses its own memstick with the save swapped in and restored.
"""

import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import time

ROOT = r"K:\The 3rd Birthday project"
BUILD = os.path.join(ROOT, "build", "ULUS10567")
SCRATCH = r"C:\Users\LOQ\AppData\Local\Temp\claude\K--The-3rd-Birthday-project\ed4bad6e-8b50-44e2-bff4-ec36a72c8f8a\scratchpad"
SAVES = {
    # port: plaintext saves; ppsspp: the matching encrypted originals
    "clear": (os.path.join(ROOT, r"build\ext_saves\ch29444\port_save"),
              os.path.join(ROOT, r"build\ext_saves\ch29444\x\The 3rd Birthday (US)")),
    "user": (os.path.join(BUILD, r"memstick\PSP\SAVEDATA"), None),
    "none": (None, None),
    "two": (os.path.join(ROOT, "build", "ext_saves", "two_slots"), None),   # clear save in slot 1, the user's in slot 2
    "installed": (os.path.join(ROOT, "build", "ext_saves", "user_installed"), None),   # the user's saves + Data Install output
}
BTN = {"select": 0x1, "start": 0x8, "up": 0x10, "right": 0x20, "down": 0x40, "left": 0x80,
       "l": 0x100, "r": 0x200, "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000}
PP_BTN = {"l": "ltrigger", "r": "rtrigger"}

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


class Port:
    name = "port"

    def __init__(self, out, save, exe="ULUS10567_new", extra_env=None):
        extra_env = dict(extra_env or {})
        for kv in os.environ.get("PORT_ENV", "").split():   # PORT_ENV="SR_DLGLOG=1 ...": extra debug env
            k, _, v = kv.partition("="); extra_env.setdefault(k, v)
        self.out = out
        ms = os.path.join(BUILD, "memstick_drive")
        shutil.rmtree(ms, ignore_errors=True)
        os.makedirs(os.path.join(ms, "PSP", "SAVEDATA"))
        if save:
            for d in os.listdir(save):
                if d.startswith("ULUS10567"):
                    shutil.copytree(os.path.join(save, d), os.path.join(ms, "PSP", "SAVEDATA", d))
        for attempt in range(30):          # a previous run's process may still hold the file
            try:
                shutil.copy(os.path.join(BUILD, exe + ".exe"), os.path.join(BUILD, "ULUS10567_drive.exe")); break
            except PermissionError:
                subprocess.run(["taskkill", "/F", "/IM", "ULUS10567_drive.exe"], capture_output=True)
                time.sleep(1)
        env = dict(os.environ)
        for k in list(env):
            if k.startswith("SR_"): del env[k]
        env.update(PSP_ISO=next(os.path.join(ROOT, f) for f in os.listdir(ROOT) if f.endswith(".iso")),
                   SR_MEMSTICK=ms, PSP_VFPU_TABLES=os.path.join(ROOT, "assets", "vfpu").replace("\\", "/"),
                   SR_CTRL="45680", SR_OFFSCREEN="1", SR_CONTROLS_CFG=os.path.join(out, "controls.cfg"), SR_SHOTDIR=out, SR_TWINSTICK="0", SR_INTERP="0", SR_SKIPINTRO="0", SR_NOINPUT="1",
                   PATH=r"K:\msys64\ucrt64\bin;" + env.get("PATH", ""))
        if os.path.isdir(os.path.join(ROOT, "ppsspp", "assets", "vfpu")):
            env["PSP_VFPU_TABLES"] = os.path.join(ROOT, "ppsspp", "assets", "vfpu").replace("\\", "/")
        env.update(extra_env or {})
        self.err = open(os.path.join(out, "port.err"), "w")
        self.p = subprocess.Popen([os.path.join(BUILD, "ULUS10567_drive.exe"), "--image", "image.bin", "08804000",
                                   "08AF9774", "init.trace", "none", "--gui"], cwd=BUILD, env=env,
                                  stdout=subprocess.DEVNULL, stderr=self.err)
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", 45680), timeout=10); break
            except OSError:
                time.sleep(0.2)
        self.f = self.s.makefile("rw")

    def _cmd(self, line):
        self.f.write(line + "\n"); self.f.flush()
        r = self.f.readline().strip()
        if not r.startswith("ok"): raise RuntimeError(f"port: {line!r} -> {r!r}")
        return r[3:]

    def press(self, buttons, frames=6):
        m = 0
        for b in buttons.split("+"): m |= BTN[b]
        self._cmd(f"btn {m:x} {frames}")
        time.sleep(frames / 60.0 + 0.05)

    def stick(self, x, y, frames):
        self._cmd(f"stick {x} {y} {frames}")

    def wait(self, s): time.sleep(s)

    def shot(self, name):
        # The window's client area, like PPSSPP (the overlay dialogs/toasts included);
        # shot_game() saves the internal-resolution game image alone.
        path = os.path.join(self.out, name + ".png")
        subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(ROOT, "tools", "capture_window.ps1"),
                        "-Process", "ULUS10567_drive", "-Out", path, "-Client"], capture_output=True)

    def shot_game(self, name):
        path = os.path.join(self.out, name + ".png")
        if os.path.exists(path): os.remove(path)
        self._cmd(f"shot {path}")
        last = -1
        for _ in range(80):   # written after the next present: wait for a complete file
            time.sleep(0.1)
            if os.path.exists(path):
                sz = os.path.getsize(path)
                if sz > 0 and sz == last: break
                last = sz

    def read(self, addr, n):
        return bytes.fromhex(self._cmd(f"read {addr:x} {n}"))

    def close(self):
        try: self.p.kill()
        except Exception: pass
        self.p.wait(); self.err.close()


class Ppsspp:
    name = "ppsspp"

    def __init__(self, out, save):
        from ppsspp_ws import Dbg
        self.out = out
        sd = os.path.join(ROOT, "memstick", "PSP", "SAVEDATA")
        self.backup = os.path.join(ROOT, r"build\ext_saves\ppsspp_savedata_backup")
        if not os.path.isdir(self.backup): shutil.copytree(sd, self.backup)
        shutil.rmtree(sd); os.makedirs(sd)
        if save:
            for d in os.listdir(save):
                if d.startswith("ULUS10567"): shutil.copytree(os.path.join(save, d), os.path.join(sd, d))
        iso = next(os.path.join(ROOT, f) for f in os.listdir(ROOT) if f.endswith(".iso"))
        ini = os.path.join(ROOT, "memstick", "PSP", "SYSTEM", "ppsspp.ini")
        self.ini_text = open(ini, encoding="utf-8", newline="").read()
        import re
        t = re.sub(r"(?m)^WindowX = .*$", "WindowX = -4000", self.ini_text)   # off the desktop while driven
        open(ini, "w", encoding="utf-8", newline="").write(t)
        si = subprocess.STARTUPINFO(); si.dwFlags = subprocess.STARTF_USESHOWWINDOW; si.wShowWindow = 4   # SW_SHOWNOACTIVATE
        self.p = subprocess.Popen([os.path.join(ROOT, "PPSSPPWindows64.exe"), iso], cwd=ROOT, startupinfo=si)
        for _ in range(100):
            try:
                self.d = Dbg(45679); break
            except OSError:
                time.sleep(0.2)
        self.held = None
        self.patcher = None

    def patch_save(self, patches):
        """Patch u32s into the decrypted 128 KB game save whenever the game's stream reader
        (0x08b1072c) reads it -- the title screen's peek and the real load alike."""
        import threading, struct as st
        from ppsspp_ws import Dbg
        BP = 0x08B1072C
        stop = threading.Event()

        def run():
            d2 = Dbg(45679)
            while not stop.is_set():           # "CPU not started" until the game boots
                try:
                    d2.request("cpu.breakpoint.add", address=BP, enabled=True); break
                except RuntimeError:
                    time.sleep(0.2)
            while not stop.is_set():
                try:
                    s_ = d2.request("cpu.status")
                    if s_.get("stepping") and s_.get("pc") == BP:
                        c = d2.request("cpu.getAllRegs")["categories"][0]
                        s1 = dict(zip(c["registerNames"], c["uintValues"]))["s1"]
                        rd = lambda a, n: base64.b64decode(d2.request("memory.read", address=a, size=n)["base64"])
                        base, size = st.unpack("<II", rd(s1 + 8, 8))
                        if size == 0x20000 and st.unpack("<I", rd(base, 4))[0] == 0x14:
                            for off, val in patches:
                                d2.request("memory.write", address=base + off,
                                           base64=base64.b64encode(st.pack("<I", val)).decode())
                        d2.request("cpu.resume", expect="cpu.resume")
                    time.sleep(0.03)
                except Exception:
                    time.sleep(0.1)
        self.patcher = (threading.Thread(target=run, daemon=True), stop)
        self.patcher[0].start()

    def _send(self, event, **kw):
        self.d.ticket += 1
        self.d._send(json.dumps(dict(event=event, ticket=str(self.d.ticket), **kw)))

    def press(self, buttons, frames=6):
        for b in buttons.split("+"):
            self._send("input.buttons.press", button=PP_BTN.get(b, b), duration=frames)
        time.sleep(frames / 60.0 + 0.05)

    def stick(self, x, y, frames):
        self._send("input.analog.send", stick="left", x=(x - 128) / 127.0, y=(y - 128) / 127.0)
        time.sleep(frames / 60.0)
        self._send("input.analog.send", stick="left", x=0.0, y=0.0)

    def wait(self, s): time.sleep(s)

    def shot(self, name):
        # PPSSPP 1.20's debugger screenshot fails on every backend ("Could not download output"):
        # capture the window's client area, which PPSSPP fills with the PSP image.
        path = os.path.join(self.out, name + ".png")
        subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(ROOT, "tools", "capture_window.ps1"),
                        "-Process", "PPSSPPWindows64", "-Out", path, "-Client"], capture_output=True)

    def read(self, addr, n):
        return base64.b64decode(self.d.request("memory.read", address=addr, size=n)["base64"])

    def close(self):
        if self.patcher: self.patcher[1].set()
        try: self.p.kill()
        except Exception: pass
        self.p.wait()
        sd = os.path.join(ROOT, "memstick", "PSP", "SAVEDATA")
        time.sleep(1)
        open(os.path.join(ROOT, "memstick", "PSP", "SYSTEM", "ppsspp.ini"), "w", encoding="utf-8", newline="").write(self.ini_text)
        shutil.rmtree(sd, ignore_errors=True)
        shutil.copytree(self.backup, sd)


def compare(base):
    from PIL import Image, ImageChops, ImageDraw
    pd, qd = os.path.join(base, "port"), os.path.join(base, "ppsspp")
    names = sorted(n for n in os.listdir(pd) if n.endswith(".png") and os.path.exists(os.path.join(qd, n)))
    if not names: return
    W, H = 480, 272
    sheet = Image.new("RGB", (W * 3, H * len(names)))
    lines = []
    ok = []
    for n in names:
        try:
            Image.open(os.path.join(pd, n)).verify(); Image.open(os.path.join(qd, n)).verify(); ok.append(n)
        except Exception:
            pass
    names = ok
    sheet = Image.new("RGB", (W * 3, H * max(1, len(names))))
    for i, n in enumerate(names):
        a = Image.open(os.path.join(pd, n)).convert("RGB").resize((W, H), Image.BILINEAR)
        b = Image.open(os.path.join(qd, n)).convert("RGB").resize((W, H), Image.BILINEAR)
        d = ImageChops.difference(a, b)
        score = sum(sum(px) for px in d.resize((60, 34)).getdata()) / (60 * 34 * 3)
        lines.append(f"{n}: mean diff {score:.1f}")
        for j, im in enumerate((a, b, d.point(lambda v: min(255, v * 3)))):
            sheet.paste(im, (j * W, i * H))
        ImageDraw.Draw(sheet).text((4, i * H + 4), f"{n}  port | ppsspp | diff x3  ({score:.1f})", fill="yellow")
    sheet.save(os.path.join(base, "compare.png"))
    open(os.path.join(base, "compare.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


def main():
    import scenarios
    scen = sys.argv[1]
    which = sys.argv[2] if len(sys.argv) > 2 else "both"
    save = "clear"
    if "--save" in sys.argv: save = sys.argv[sys.argv.index("--save") + 1]
    scene = None
    if "--scene" in sys.argv: scene = int(sys.argv[sys.argv.index("--scene") + 1], 0)
    fn = getattr(scenarios, scen)
    base = os.path.join(SCRATCH, "drive", scen + (f"_{scene}" if scene is not None else ""))
    for kind in (("port", "ppsspp") if which == "both" else (which,)):
        out = os.path.join(base, kind)
        shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
        port_save, pp_save = SAVES[save]
        patches = [(0xB9D8, 0xFFFF0000 | scene)] if scene is not None else []
        if patches and kind == "port":           # the port reads plaintext: patch a copy
            import struct
            tmp = os.path.join(BUILD, "save_scene")
            shutil.rmtree(tmp, ignore_errors=True); shutil.copytree(port_save, tmp)
            f_ = os.path.join(tmp, "ULUS10567DATA00", "3RDDATA.BIN")
            d_ = bytearray(open(f_, "rb").read())
            for off, val in patches: struct.pack_into("<I", d_, off, val)
            open(f_, "wb").write(d_); port_save = tmp
        g = Port(out, port_save) if kind == "port" else Ppsspp(out, pp_save or port_save)
        if patches and kind == "ppsspp": g.patch_save(patches)
        t0 = time.time()
        try:
            fn(g)
        finally:
            g.close()
        print(f"{kind}: {time.time() - t0:.0f} s", flush=True)
    if which == "both": compare(base)


if __name__ == "__main__":
    main()
