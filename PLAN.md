# The 3rd Birthday — native PC port (static recompilation)

## Goals
- Playable improvements: widescreen, high framerate, modern controls (twin-stick camera is the first real milestone)
- Learning RE/recomp skills; the game is the vehicle

## Decisions
| Area | Decision |
|---|---|
| Scope | 3rd Birthday only, full focus |
| Approach | Static recomp + hybrid function replacement. No matching decomp. |
| License | GPL-2.0+ (PPSSPP code may be reused) |
| Distribution | Prebuilt exe + player's own ISO. No assets, ISO hash check, no monetization. Runtime kept separate from generated code so a build-on-player-machine installer is an easy fallback after a takedown. |
| Framework | 2-weekend bake-off with fixed pass/fail checks (below) |
| Rendering | GE emulated at runtime (Vulkan). Widescreen/FOV and camera patched in game code. HUD anchored to screen edges for ultrawide (PSP is already ~16:9). |
| High FPS | Logic stays at native tick, render interpolation (Zelda64Recomp-style matrix tagging). After twin-stick. Camera updates at display rate. Skip interpolation on cuts, teleports, Overdive. |
| Camera | Milestone: free right-stick orbit, original lock-on/aim untouched. Later: auto-return toggle. Not now: free-aim redesign. |
| Debugging | Syscall trace, GE list dumps, function enter/exit tracing, all diffable against PPSSPP, from the first commit. Ghidra DB exported to text and versioned. |
| Media | Stub PMF/ATRAC until stable at native rate, then FFmpeg-based HLE. Native high-res video replacement is a later feature. |
| Platform | SDL3 (window, input, audio, timing) + Vulkan. Windows first, Linux/Steam Deck in CI early. |
| Version | NA `ULUS10567`, hash-locked. All addresses in `versions/<game-id>.toml`. |
| Saves | Auto-answer stubs first, then PSP-layout saves (PPSSPP import, savedata crypto) with Dear ImGui dialogs, stored in user data dir. |
| Visibility | Public repo from day one, quiet. One announcement once widescreen + twin-stick gameplay video exists. Devlog in `docs/`. |

## Milestones
0. **Version check + decrypted EBOOT** — version check done (see below)
1. **Framework bake-off**: done. AljandrOrtega wins (see `docs/bakeoff.md`)
2. Boot
3. Stable at native rate
4. Ultrawide + upscaling
5. Twin-stick camera
6. Audio + FMV
7. Interpolated high FPS
8. Release

## Step 0 findings (2026-09-26)
- Disc: `ULUS10567`, v1.00, requires firmware 6.37. Hashes in `versions/ULUS10567.toml`.
- `EBOOT.BIN` is encrypted (`~PSP`, module name `pe`, tag `0xD91613F0`, 5,663,725-byte ELF inside). `BOOT.BIN` is an all-zero dummy, so no free plaintext copy.
- **No game PRX modules on the disc.** The whole game is one executable, which makes recomp simpler. Still to check: whether modules are loaded from inside `3rd.pkg` at runtime (watch `sceKernelLoadModule*` in PPSSPP's log).
- All assets are in `USRDIR/3rd.pkg` (1.3 GB) with `3rd.fsd` (7.9 KB) as the likely index. The runtime reads assets through this one archive.
- `SYSDIR/OPNSSMP.BIN` (880 bytes) is also present. `SYSDIR/UPDATE/` is a firmware updater and can be ignored.

### Decrypted EBOOT (`dumps/ULUS10567_EBOOT.elf`, from `tools/elf_info.py`)
- Relocatable PRX (`0xFFA0`), module name `pe`, entry `0x2F5774` relative to the load base. The recompiler must apply relocations or pick a fixed base.
- The section name table is stripped (zeroed), so tools must work from segments and `sceModuleInfo`, not section names.
- About 1.07M words in the executable section (4.2 MB). Some of that is probably data, so treat it as an upper bound on the function count.
- **VFPU is used heavily:** about 38.5k VFPU-opcode words (a rough count), mostly `lv.s`/`sv.s`/`lv.q`/`sv.q`. VFPU coverage is a hard bake-off requirement.
- **273 imports across 32 libraries** (full NID list: `docs/ULUS10567_imports.txt`). This is the HLE checklist. Largest: ThreadManForUser 37, sceSasCore 27, sceMpeg 25, IoFileMgrForUser 24, sceHttp 20, sceUtility 15, sceAudio 14, sceAtrac3plus 13, sceGe_user 11.
- Network libraries (sceNet*, sceHttp, sceParseHttp, sceWlanDrv, sceNetAdhocctl, sceOpenPSID) can probably all be stubbed to "offline".
- `ModuleMgrForUser` (7 funcs) is imported, so the game loads modules at runtime. Next, check PPSSPP's log for which ones (probably the firmware's mpeg/atrac/sas PRXs, which HLE replaces).
- `sceSasCore` (the PSP's hardware audio mixer) handles all in-game sound effects, so it needs real HLE early, not a silence stub.

### Strings and runtime behavior
- File paths: assets come from `disc0:/PSP_GAME/USRDIR/3rd.pkg` plus `3rd.fsd`. Saves go to `ms0:/PSP/SAVEDATA/ULUS10567DATA/`.
- **`3RDINS.BIN` on the memory stick is an optional data install** (a cache copied from the UMD to cut loading times). The runtime can skip it or fake it, since reads come from a PC drive anyway.
- Leftover dev paths: `host0:` and `disc0:/PSP_GAME/USRDIR/data/` (loose data folder). Probably unused in retail, but a hint that the pkg holds a normal file tree.
- No `.prx` path strings. System modules come in through `sceUtilityLoadModule` (sas/atrac/mpeg/net), which HLE replaces. `sceKernelLoadModule` is imported, but it's unclear whether it's ever called.
- sceUtility imports: LoadModule/UnloadModule, Savedata (4), MsgDialog (5), Netconf (4). No on-screen keyboard. Netconf can be stubbed to "cancelled".
- **Early invalid kernel calls** (from PPSSPP's log at boot): `sceKernelReferThreadStatus` with a bad struct size (544), and `sceKernelWaitSema`/`sceKernelSignalSema` on semaphore id -1. The runtime must return the same error codes as real hardware (use PPSSPP as the reference), not assert.
- **Frame rate: confirmed a locked 30 FPS in gameplay** (PPSSPP at full speed, first mission). The logic tick is 30 Hz.
- **Frame rate target:** 60 FPS minimum, with higher rates (120/144) supported. Reached through render interpolation over the 30 Hz logic (see Decisions). This is required for release, not an optional extra.
- **Correction on widescreen:** the PSP screen is 480×272 (about 16:9), not 4:3, so the game is already widescreen. The "widescreen" work is really:
  - exact 16:9 (a small FOV adjustment from 1.765 to 1.778)
  - ultrawide (21:9, 32:9), which is where FOV, culling and HUD anchoring matter
  - HUD elements anchored to the screen edges, not pillarboxed

### Remaining for step 0
- [x] Install PPSSPP and dump the decrypted EBOOT
- [x] Record the decrypted ELF's sha256 in `versions/ULUS10567.toml`
- [ ] Move PPSSPP out of the repo root into `ppsspp/`
- [ ] From PPSSPP's log: which modules get loaded at runtime (`sceKernelLoadModule*`, `sceUtilityLoadModule`)
- [x] Measure the logic tick: 30 Hz

## Boot milestone: status (2026-09-26)
- **All 273 imports are handled** (`tools/hle_coverage.py` reports 255 because the 17 sceHttp stubs register from a table it doesn't parse). New handlers are in `runtime/src/rt/hle_ext.c`, with small hooks in `hle.c`.
- **All 19,281 functions translate** (0 trapping stubs). Code generator fixes: `cache`, `wsbw`/`bitrev`, `vnop`/`vsync`/`vflush`, `bvf`/`bvt` branches, branch targets that land in a delay slot, and a runtime `sr_vfpu_ext` for vf2i*/vi2f/vrnd*/vx2i/vi2x/vfad/vavg/vmscl/vmmov (PPSSPP semantics).
- **Runtime additions:** raw UMD block device (`umd1:` reads in sectors; the game streams `3rd.pkg` this way), resumable GE lists with stall addresses plus a proper list queue, `sceKernelSetAlarm` in the scheduler, FPL/LwMutex, SAS PCM/noise/pause, Output2 audio, and a message dialog that auto-answers.
- **Where it stands (updated):** the boot notice, the Square Enix logo, the title menu (New Game / Load Game / Extra) and the difficulty select all display correctly. The game runs its attract loop stably, and New Game reaches an in-game 3D scene. Verified with the software renderer and scripted input (`SR_PADSCRIPT`).
- **Fixed since:**
  - `sceDisplayGetFrameBuf` didn't return width/format (the black-screen cause)
  - thread stacks overlapped the heap: stacks now come from the top of the user partition with a kernel-RAM fallback, the interrupt stack lives in kernel RAM, and deleted threads' slots and stacks are reused
  - the entry thread ran with `sp = 0`
  - `analyze.py` ignored every vtable/jump-table pointer on stripped EBOOTs (19,281 → 19,471 functions)
- **Update: the cutscene blocker is fixed.** The movie player waited for the UMD drive-status callback to report READY, and the runtime had no kernel callbacks. With callbacks in place, movies decode, and New Game reaches the first mission's street scene. Open issues: movie frames show macroblock corruption, Aya and the HUD aren't drawn, and the scene looks static (waiting on audio or an in-engine cutscene?). **PPSSPP oracle:** `RemoteDebuggerOnStartup`/`RemoteISOPort = 45679` in `memstick/PSP/SYSTEM/ppsspp.ini` (backup in `ppsspp.ini.bak`), plus `tools/ppsspp_ws.py` and `tools/oracle_newgame.py`. Same load base, so RAM diffs line up (`SR_RAMDUMP`).
- **Fixed (2026-09-26, later):**
  - Aya was invisible because the PSP scratchpad (0x00010000, where the game builds bone matrices) wasn't mapped.
  - Movies were corrupted because the low-latency H.264 decoder was fed partial frames (it now gets whole access units), and they ran at half speed because the ring buffer freed one packet per frame (it now tracks the decoder's unconsumed data).
  - A crash at the end of long movies (`es_compact`).
  - GE signal handlers now run.
  - Aya walks and runs under scripted analog input, and the Vulkan renderer displays correctly in the game window (`tools/capture_window.ps1` can capture it, even when covered).
  - With a fresh save the opening is unskippable and very long; with an old save, ✕ skips it. The earlier "barricade scene without HUD" came from skipping.
- **HUD fixed (2026-09-26):** a FINISH that follows a SYNC signal (behavior 8) doesn't end a GE list (PPSSPP Execute_End). The game emits SIGNAL(SYNC)/END, then FINISH/END, then its whole HUD, and our interpreter stopped at that FINISH. Found by breaking on the HUD vertex write in PPSSPP (tools/oracle_watch_write.py), confirming the same call chain runs in the port (SR_PCCOUNT), and comparing where the list ends. The HUD, radar, radio dialogue box and ammo now render. **New issue:** gameplay looks washed out and hazy compared to PPSSPP, probably a full-screen effect pass in that newly executed part of the list blending wrong.
- **Haze fixed (2026-09-27):** the "haze" was the game's glow post-effect. It downsamples the 8888 framebuffer into a 128x64 4444 buffer at VRAM 0x154000 by reading it as CLUT32 (green>>12 through a 16-entry palette), blurs that buffer by reading it as CLUT16, then adds it over the frame (texfunc ADD, material 0x0064FF / alpha 0x6E). The CPU texture sampler (ge.c sample_tex) had no CLUT16/CLUT32 case and returned white, so the glow buffer was solid 0xFFFF and whitened the whole screen. Also fixed: the Vulkan texture cache now keys CLUT16/32 by palette and hashes CLUT32 at 32bpp. New debug switches: SR_DRAWLOG=<frame,...> (per-prim state), SR_GPU_SKIPTEX=<vram offset> (drop draws sampling that address), and SR_FBDUMP now syncs GPU targets into edram.bin. Still open: PPSSPP keeps destination alpha when the stencil test is off, while we add it (ge_gpu.c alpha blend ONE/ONE).
- **HUD history:** after the full opening (fresh save, 10,411 movie frames), mission 1 starts at the same spot as PPSSPP but **no HUD or MISSION popup is drawn**. The HUD isn't drawn-but-invisible: in gameplay the only screen-space (through-mode) draws are 2 per frame, and the new transform-mode vtypes (0x11e, 0x102) are world-space effects. So the game decides not to submit the HUD. There are no failed file opens and no out-of-range memory accesses. Next: diff game state against PPSSPP in gameplay (same RAM layout), e.g. locate the HUD draw code via its textures and find the gate.
- **Was open:** does gameplay after the full opening show the HUD and "MISSION" popup like PPSSPP? Also: a stray-pixel column at the right edge and a strip at the bottom of Vulkan output, and silent ATRAC/voice audio.
- **Aya invisible, narrowed down (2026-09-26):** her skinned draws (vtypes 0x522/0x4522/0x8522/0xc522) are submitted, but every bone matrix is zero. The GE runs the 0x2A/0x2B bone commands, and all 158 bone blocks in RAM hold zero data, so the game's own code emits zeros. Timing is ruled out. The emitter is the function at 0x08b52cd0 (unrelocated 0x34ECD0): `lv.q` loads two matrices, `vmmul`, `sv.q` to a scratch buffer, then packs `0x2B000000 | float>>8`. Next: compare `vmmul`/`lv.q`/`sv.q` codegen (register mapping, matrix transpose) against PPSSPP on this sequence, or diff the source matrices at `*(obj+0x3C4)+0x40` with PPSSPP. New debug switches: `SR_VTSTAT`, `SR_SKINDBG=<frame>`, `tools/ppsspp_shot.py` (capture doesn't work on v1.20.4).
- **Previous blocker (fixed):** after New Game, the 3D scene rendered behind a full-screen black quad at alpha 255 (a fade that never starts). The opening movie's MPEG player is created (`sceMpegCreate`, `InitAu`) but never fed (`ringPut=0`), so the game presumably waits for the cutscene. Next steps: find out why the player never calls `sceMpegRingbufferPut` (check how PPSSPP proceeds after New Game), then do real MPEG/ATRAC decoding per the plan.
- **Debug switches added:** `SR_REGWATCH=<reg>:<hex>` (first instruction that sees a GPR value), `SR_WATCH_VAL` (filter for `SR_WATCH`), `SR_DMALOG`, `SR_CALLCOUNT=2` (all calls), `SR_STACK_ARENA` (removed again: the arena is now kernel RAM).

## Bake-off checklist (per framework)
- [ ] Lift coverage: the whole ELF translates without unknown instructions (VFPU especially)
- [ ] Module/relocation handling
- [ ] HLE breadth: number of missing syscalls before the first crash
- [ ] GE path: a real renderer or just a stub
- [ ] Health: recent commits, responsive issues, other games booting

Kill rule: the framework that gets closest to showing anything wins. If none gets past the lift step, write our own recompiler and take the runtime from the best candidate.

## Open, not blocking
- Controller remapping and config UI
- Mod/patch format for other people's function replacements
- Audio resampling vs. keeping the PSP's native rate
