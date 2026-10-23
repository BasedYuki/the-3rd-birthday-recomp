# Mods

Three kinds, from simplest to most powerful.

## 1. Texture packs (no rebuild)

Replace any texture the game draws with your own PNG, at any resolution.

1. Set `texture_dump=1` in `graphics.cfg` (next to the game's executable) and play the part of
   the game you want to change. Every texture the game uses is written once to
   `textures/dump/<hash>.png`.
2. Copy the ones you want into `textures/` (keep the file name), edit them. They can be
   larger than the original (for example 4x for an HD pack); keep the same proportions.
3. Set `texture_dump=0` again and restart. The log (`run.err`) says how many replacements
   were found.

The name is a hash of the texture's decoded pixels and size, so a replacement applies wherever
the game uses that image. Textures the game changes every frame (render targets, effects drawn
by the game) are not files and cannot be replaced.

## 2. Memory patches (no rebuild)

Text files in `patches/` next to the executable change game data in memory:

```
# address   type  value       when
08C3C700    u32   0x00000001  always    # written every frame (the default)
08C3C704    f32   1.5         once      # written once, when the game starts
```

Types: `u8`, `u16`, `u32`, `f32`. Only data can be patched this way: the game's code is
recompiled to native code, so patching instruction words (as CWCheat code patches do)
changes nothing. For code, write a code mod.

## 3. Code mods (rebuild)

A folder here, `mods/<name>/`, with a `mod.toml` and C files, compiled into the game by
`scripts/build.ps1` / `scripts/build.sh`. A mod hooks game functions by address: its
`mod_hook` runs at the entry of each one, before the game's code, and can change arguments,
memory, or replace the function completely.

```toml
name = "Wider field of view"
enabled = true

[hooks]
cam_setup = 0x08B435B8        # becomes MODHOOK_CAM_SETUP in your C code
```

```c
#include "recomp.h"

int mod_hook(CpuState *s, uint32_t addr) {
    if (addr == MODHOOK_CAM_SETUP) s->f[12] *= 1.15f;   /* $f12 = fov argument */
    return 0;   /* 0: the game's function runs; 1: it is skipped (set s->r[2] = return value) */
}
```

`CpuState` is the game CPU: `s->r[4..7]` are the arguments a0-a3, `s->r[2]` the return value
v0, `s->f[12..]` the float arguments, `s->r[29]` the stack pointer. `MEM_R32(addr)` /
`MEM_W32(addr, v)` (and 8/16-bit forms) read and write game memory. Several mods can hook
the same function; they run in folder-name order, and the first one returning 1 replaces it.

Finding functions: `tools/disasm.py` disassembles the game, `tools/callers_of.py` lists
callers, and the debug switches in `PLAN.md` (`SR_PCPROBE`, `SR_WATCH`, ...) show what the
game does at run time. `example_wider_fov/` is a working example (disabled; set
`enabled = true` and rebuild).

Adding or changing hooks regenerates the recompiled code (a full rebuild, a few minutes);
changing only a mod's C code just relinks.
