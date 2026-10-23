"""Prepare the game on a player's PC: build game.dll from their own disc image.

    python prepare.py --iso <game.iso> --game-dir <folder with ULUS10567.exe> [--kit <kit folder>]

Steps (each prints "PROGRESS <percent> <text>" for the launcher):
  1. decrypt the game program (PSP_GAME/SYSDIR/EBOOT.BIN) from the ISO (eboot_decrypt.exe)
  2. relocate it into image.bin, find its imports and functions, translate them to C
  3. compile the C (in parallel) and link game.dll with the bundled compiler
  4. write launch.cfg and game_build.txt (what this game.dll was built from)

Only the player's disc is used; nothing is downloaded. The kit folder holds the compiler
(kit/mingw), the translation tools (kit/tools), the headers (kit/rt) and versions/<game>.toml.
Dev use: --kit . with MSYS2's gcc on PATH works too (--gcc gcc).
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

GAME = "ULUS10567"
BASE = 0x08804000


def progress(pct, text):
    print(f"PROGRESS {pct:3d} {text}", flush=True)


def run(cmd, cwd=None, quiet=True):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace",
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if p.returncode != 0:
        sys.stderr.write(p.stdout[-4000:] + p.stderr[-4000:])
        raise SystemExit(f"failed: {' '.join(os.path.basename(str(c)) for c in cmd[:3])} (exit {p.returncode})")
    return p.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--kit", default=None, help="kit folder (default: this script's parent folder)")
    ap.add_argument("--gcc", default=None, help="gcc to use (default: the kit's)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    a = ap.parse_args()
    kit = os.path.abspath(a.kit or os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    game_dir = os.path.abspath(a.game_dir)
    tools = os.path.join(kit, "tools")
    rt = os.path.join(kit, "rt")
    py = sys.executable
    gcc = a.gcc or os.path.join(kit, "mingw", "bin", "gcc.exe")
    # the bundled compiler is relocated: name its system headers and libraries explicitly
    sysinc = [] if a.gcc else ["-isystem", os.path.join(kit, "mingw", "include")]
    syslib = [] if a.gcc else ["-L" + os.path.join(kit, "mingw", "lib")]
    if not a.gcc:   # the kit's compiler finds its own DLLs and programs through PATH
        os.environ["PATH"] = os.path.join(kit, "mingw", "bin") + os.pathsep + os.environ.get("PATH", "")
    toml = os.path.join(kit, "versions", f"{GAME}.toml")
    work = os.path.join(game_dir, "build_tmp")
    gen = os.path.join(work, "gen")
    t0 = time.time()
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(gen)

    progress(2, "Reading the game program from your disc image")
    elf = os.path.join(work, "EBOOT.elf")
    run([os.path.join(tools, "eboot_decrypt.exe"), a.iso, elf])

    progress(6, "Translating the game program")
    run([py, os.path.join(tools, "prxload.py"), elf, hex(BASE), "--out=" + os.path.join(game_dir, "image.bin")])
    run([py, os.path.join(tools, "imports.py"), elf, hex(BASE), "--toml=" + os.path.join(work, "imports.toml")])
    run([py, os.path.join(tools, "analyze.py"), elf, f"--base={BASE:#x}", "--toml=" + os.path.join(work, "functions.toml"),
         "--quiet"])
    hooks = run([py, os.path.join(tools, "toml_addrs.py"), "--hooks", toml]).strip()
    mods_dir = os.path.join(game_dir, "mods")
    env_mods = dict(os.environ, SR_MODS_DIR=mods_dir)
    mod_hooks = subprocess.run([py, os.path.join(tools, "mods_gen.py"), "--hooks"], capture_output=True, text=True,
                               env=env_mods, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout.strip()
    allhooks = ",".join(sorted({h for h in (hooks + "," + mod_hooks).split(",") if h}))
    progress(12, "Translating the game program to C")
    run([py, os.path.join(tools, "codegen.py"), elf, os.path.join(work, "recomp.c"), f"--base={BASE:#x}",
         "--toml=" + os.path.join(work, "functions.toml"), "--hooks=" + allhooks])
    run([py, os.path.join(tools, "split_recomp.py"), os.path.join(work, "recomp.c"), gen])
    os.remove(os.path.join(work, "recomp.c"))

    chunks = sorted(f for f in os.listdir(gen) if f.startswith("recomp_") and f.endswith(".c"))
    flags = ["-O1", "-foptimize-sibling-calls", "-I" + rt, "-I" + gen, "-DSR_SDL3VK", "-DSR_GAME_DLL", "-w"] + sysinc
    done = [0]

    def compile_one(c):
        src = os.path.join(gen, c)
        run([gcc] + flags + ["-c", src, "-o", src[:-2] + ".o"])
        done[0] += 1
        progress(20 + 70 * done[0] // len(chunks), f"Compiling the game ({done[0]}/{len(chunks)})")

    progress(20, f"Compiling the game (0/{len(chunks)}) - this takes a few minutes")
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        list(ex.map(compile_one, chunks))

    progress(91, "Linking game.dll")
    objs = [os.path.join(gen, c[:-2] + ".o") for c in chunks]
    run([gcc, "-O2", "-DSR_GAME_DLL", "-I" + rt] + sysinc + ["-c", os.path.join(rt, "game_glue.c"), "-o", os.path.join(work, "game_glue.o")])
    p = subprocess.run([py, os.path.join(tools, "mods_gen.py"), "--gen", work], env=env_mods, capture_output=True,
                       text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if p.returncode: raise SystemExit("mods: " + p.stderr[-2000:] + p.stdout[-2000:])
    run([gcc, "-O2", "-DSR_GAME_DLL", "-I" + rt] + sysinc + ["-c", os.path.join(work, "mods_gen.c"), "-o", os.path.join(work, "mods_gen.o")])
    objs += [os.path.join(work, "game_glue.o"), os.path.join(work, "mods_gen.o")]
    for line in open(os.path.join(work, "mods_srcs.txt")).read().splitlines():
        mid, src = line.split(" ", 1)
        o = os.path.join(work, f"mod_{mid}_{os.path.basename(src)[:-2]}.o")
        run([gcc, "-O2", "-DSR_GAME_DLL", "-I" + rt, "-I" + work] + sysinc + ["-include", os.path.join(work, "mods_hooks.h"),
             f"-Dmod_hook=mod_{mid}_hook", "-c", src, "-o", o])
        objs.append(o)
    dll_tmp = os.path.join(work, "game.dll")
    run([gcc, "-shared", "-s", "-o", dll_tmp] + objs + syslib + ["-static-libgcc"])
    shutil.move(dll_tmp, os.path.join(game_dir, "game.dll"))

    progress(97, "Finishing")
    run([py, os.path.join(tools, "write_launch_cfg.py"), elf, hex(BASE), os.path.join(game_dir, "launch.cfg")])
    open(os.path.join(game_dir, "init.trace"), "w").write("# init r28=0 r4=0 r5=0\n")
    kit_id = open(os.path.join(kit, "kit_id.txt")).read().strip() if os.path.exists(os.path.join(kit, "kit_id.txt")) else "dev"
    with open(os.path.join(game_dir, "game_build.txt"), "w") as f:
        f.write(f"kit={kit_id}\nhooks={hashlib.sha1(allhooks.encode()).hexdigest()}\n"
                f"built={time.strftime('%Y-%m-%d %H:%M')}\nseconds={int(time.time() - t0)}\n")
    shutil.rmtree(work, ignore_errors=True)
    progress(100, f"Done in {int(time.time() - t0) // 60} min {int(time.time() - t0) % 60} s")


if __name__ == "__main__":
    main()
