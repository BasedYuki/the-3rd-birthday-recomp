"""Import saves made on a real PSP or in PPSSPP (encrypted) into the port (plain files).

    python tools/save_import.py <folder> [--install]

<folder> is a PSP SAVEDATA folder holding ULUS10567... save folders, or one save folder. The
port cannot decrypt PSP saves itself, so PPSSPP (PPSSPPWindows64.exe and the game ISO in the
project folder) does it: for each save, PPSSPP boots the game off-screen with only that save on
its memory stick (its own saves are put back afterwards), and the decrypted data is read from
the game's memory when the game loads it (tools/oracle_save_dump.py). Saves that are already
plain (port or PPSSPP "unencrypted" saves) are copied as they are.

The result goes to build/ext_saves/imported_<time>/; --install also copies it into the game's
memory stick (build/ULUS10567/memstick/PSP/SAVEDATA), after backing up the saves there to
build/ULUS10567/save_backup_<time>/.

The other way round needs nothing: the port's saves are plain, and PPSSPP loads plain saves
(PARAM.SFO SAVEDATA_PARAMS = 0), so copy a port save folder into PPSSPP's SAVEDATA folder.
A real PSP only accepts encrypted saves, which this project cannot produce.
"""

import base64
import json
import os
import shutil
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
from ppsspp_ws import Dbg

STREAM_READER = 0x08B1072C          # the game's memory-stream reader; s1 = stream {+8 base, +0xc size}
GAME_SIZE, SYS_SIZE = 0x20000, 0x4000
GAME_FILE, SYS_FILE = "3RDDATA.BIN", "SYSDATA.BIN"


def sfo_zero_params(path):
    """Mark a PARAM.SFO as not encrypted (SAVEDATA_PARAMS all zero), as PPSSPP does."""
    b = bytearray(open(path, "rb").read())
    if b[:4] != b"\0PSF": return
    keys, data, n = struct.unpack_from("<III", b, 8)
    for i in range(n):
        ko, fmt, ln, mx, do = struct.unpack_from("<HHIII", b, 20 + i * 16)
        name = b[keys + ko:b.index(0, keys + ko)].decode("ascii", "replace")
        if name == "SAVEDATA_PARAMS":
            b[data + do:data + do + mx] = bytes(mx)
    open(path, "wb").write(bytes(b))


def is_plain(path, size, head):
    if not os.path.isfile(path) or os.path.getsize(path) != size: return False
    return struct.unpack("<I", open(path, "rb").read(4))[0] == head


def decrypt_with_ppsspp(src_dir, want, scratch):
    """Boot PPSSPP with the save folder(s) in src_dir and return {kind: bytes} for the reads
    wanted ("game", "sys") as the game loads them."""
    got = {}
    g = drive.Ppsspp(scratch, src_dir)
    try:
        d = Dbg(45679)
        def rd(addr, n):
            buf = bytearray()
            for a in range(addr, addr + n, 0x10000):
                k = min(0x10000, addr + n - a)
                buf += base64.b64decode(d.request("memory.read", address=a, size=k, timeout=20)["base64"])
            return bytes(buf)
        for _ in range(50):
            try: d.request("cpu.breakpoint.add", address=STREAM_READER, enabled=True); break
            except RuntimeError: time.sleep(0.2)
        end, i = time.time() + 300, 0
        while time.time() < end and set(want) - set(got):
            st = d.request("cpu.status")
            if not st.get("stepping"):
                if True:                                   # walk the title menu into Load Game
                    d.ticket += 1
                    d._send(json.dumps(dict(event="input.buttons.press", ticket=str(d.ticket),
                                            button="cross" if i % 3 else "start", duration=6)))
                    i += 1
                time.sleep(0.7)
                continue
            regs = d.request("cpu.getAllRegs")["categories"][0]
            s1 = dict(zip(regs["registerNames"], regs["uintValues"]))["s1"]
            base, size = struct.unpack("<II", rd(s1 + 8, 8))
            head = struct.unpack("<I", rd(base, 4))[0] if size else 0
            if size == GAME_SIZE and head == 0x14 and "game" in want: got["game"] = rd(base, size)
            if size == SYS_SIZE and head == 0x5 and "sys" in want and "sys" not in got: got["sys"] = rd(base, size)
            d.request("cpu.resume", expect="cpu.resume")
        try: d.request("cpu.breakpoint.remove", address=STREAM_READER)
        except Exception: pass
    finally:
        g.close()
    return got


def main():
    if len(sys.argv) < 2: print(__doc__); return 1
    src = os.path.abspath(sys.argv[1])
    install = "--install" in sys.argv
    saves = [os.path.join(src, d) for d in sorted(os.listdir(src)) if d.startswith("ULUS10567")
             and os.path.isfile(os.path.join(src, d, "PARAM.SFO"))]
    if not saves and os.path.basename(src).startswith("ULUS10567"): saves = [src]
    if not saves: print("no ULUS10567 save folders found in", src); return 1
    saves.sort(key=lambda s: s.endswith("SYSDATA"))   # game saves first: SYSDATA is decrypted with them
    stamp = time.strftime("%Y%m%d_%H%M%S")
    out = os.path.join(drive.ROOT, "build", "ext_saves", "imported_" + stamp)
    scratch = os.path.join(drive.SCRATCH, "save_import")
    os.makedirs(scratch, exist_ok=True)
    sysdir = next((s for s in saves if s.endswith("SYSDATA")), None)
    sys_plain = None                       # SYSDATA decrypted alongside the first game save
    for s in saves:
        name = os.path.basename(s)
        dst = os.path.join(out, name)
        shutil.copytree(s, dst)
        kind, fn, size, head = ("sys", SYS_FILE, SYS_SIZE, 0x5) if name.endswith("SYSDATA") else ("game", GAME_FILE, GAME_SIZE, 0x14)
        if kind == "sys" and sys_plain:
            open(os.path.join(dst, fn), "wb").write(sys_plain)
            sfo_zero_params(os.path.join(dst, "PARAM.SFO"))
            print(f"{name}: decrypted ({len(sys_plain)} bytes)"); continue
        if is_plain(os.path.join(s, fn), size, head):
            print(f"{name}: already plain, copied")
            sfo_zero_params(os.path.join(dst, "PARAM.SFO"))
            continue
        # PPSSPP sees only this save (plus the system data, which the game reads at boot)
        tmp = os.path.join(scratch, "one")
        shutil.rmtree(tmp, ignore_errors=True); os.makedirs(tmp)
        shutil.copytree(s, os.path.join(tmp, name))
        if sysdir and kind == "game": shutil.copytree(sysdir, os.path.join(tmp, os.path.basename(sysdir)))
        print(f"{name}: decrypting with PPSSPP (off-screen, up to 5 minutes)...", flush=True)
        want_sys = kind == "game" and sysdir and not sys_plain and not is_plain(os.path.join(sysdir, SYS_FILE), SYS_SIZE, 0x5)
        got = decrypt_with_ppsspp(tmp, [kind] + (["sys"] if want_sys else []), scratch)
        if "sys" in got: sys_plain = got["sys"]
        if kind not in got:
            print(f"{name}: FAILED (the game never loaded it); left encrypted in the output"); continue
        open(os.path.join(dst, fn), "wb").write(got[kind])
        sfo_zero_params(os.path.join(dst, "PARAM.SFO"))
        print(f"{name}: decrypted ({len(got[kind])} bytes)")
    print("imported saves:", out)
    if install:
        ms = os.path.join(drive.BUILD, "memstick", "PSP", "SAVEDATA")
        bak = os.path.join(drive.BUILD, "save_backup_" + stamp)
        if os.path.isdir(ms): shutil.copytree(ms, bak); print("backed up the current saves to", bak)
        os.makedirs(ms, exist_ok=True)
        for d in os.listdir(out):
            shutil.rmtree(os.path.join(ms, d), ignore_errors=True)
            shutil.copytree(os.path.join(out, d), os.path.join(ms, d))
        print("installed into", ms)
    return 0


if __name__ == "__main__":
    sys.exit(main())
