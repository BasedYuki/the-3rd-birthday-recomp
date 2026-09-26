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
1. Framework bake-off
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
