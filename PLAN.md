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
| Rendering | GE emulated at runtime (Vulkan). Widescreen/FOV and camera patched in game code. HUD kept 4:3 separately. |
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
4. Widescreen + upscaling
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

### Remaining for step 0
- [ ] Install PPSSPP, enable *Settings → Tools → Developer tools → Dump decrypted EBOOT.BIN on game boot*, boot the game once, and copy the dump into `dumps/` (gitignored).
- [ ] Record the decrypted ELF's sha256 in `versions/ULUS10567.toml`.
- [ ] In PPSSPP, measure the logic tick (30 vs 60) and note which system modules the game imports.

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
