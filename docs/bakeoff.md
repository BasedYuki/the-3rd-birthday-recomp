# Framework bake-off

Input: `dumps/ULUS10567_EBOOT.elf` (sha256 `33cd4fa6…51d9`). Checkouts live in `bakeoff/` (gitignored).

| | [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) (+ OverkillLabs forks) | [sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp) | [AljandrOrtega/PSP-recompilation-project](https://github.com/AljandrOrtega/PSP-recompilation-project) |
|---|---|---|---|
| License | MIT | MIT (strict: no PPSSPP code) | GPL-2.0+ (mpeg ported from PPSSPP) |
| Last commit (checked 2026-09-26) | 2026-08-13 | 2026-07-20 | 2026-09-23 |
| Proven game | GTA: Vice City Stories (playable profile) | WTF: Work Time Fun (stalls in startup, nothing on screen) | not stated in README |
| Toolchain | CMake + C++20, VS 2022 | CMake + C compiler | Python 3 + MSYS2 UCRT64 gcc, SDL3, Vulkan loader, lld |
| Renderer | DX12 (VCS profile). Vulkan in the OverkillLabs fork | Software rasterizer; transformed geometry not drawn | Own SDL3 + Vulkan GE backend |
| Threads | yes (VCS runs) | not started (run-to-completion) | `sched.c` scheduler |
| Media / saves | per profile | sascore only | mpeg (PPSSPP port), h264 via Media Foundation, savedata, PGF fonts |
| Oracle tooling | profile tests | tracing, write-watch, stack audit, PPSSPP as oracle | tracediff, funcdiff, ppmdiff, vfpu fuzz vs PPSSPPHeadless |

## Check 1: lift coverage

### AljandrOrtega — ran 2026-09-26
Pipeline: `prxload.py` → `imports.py` → `analyze.py` → `codegen.py`, all Python, done in about 15 s total.
- `prxload`: rebased to `0x08804000`, **140,186 relocations applied**, entry `0x08AF9774`.
- `imports`: all 273 imports mapped to stub addresses.
- `analyze`: **19,305 functions** discovered.
- `codegen`: 158 MB of C, **19,305 / 19,305 functions emitted, 138 (0.7%) as trapping stubs** because of unhandled instructions:

| Count | Unhandled | Likely meaning |
|---:|---|---|
| 52 | VFPU4 jump 0x01 | a subgroup of less common VFPU conversion/misc ops |
| 45 | cop2 sub 8 | `bvf`/`bvt`: **branches on VFPU condition flags** (control flow, so fix first) |
| 15 | VFPU opcode 0x3c sub 0x4 | VFPU6 matrix-group subop |
| 10 | SPECIAL3 funct 0x20 | BSHFL group (`seb`/`seh`/`wsbh`/`bitrev`), probably Allegrex `bitrev` |
| 5 | VFPU9 op 0x06 | VFPU misc (sync/flush class) |
| 4 | opcode 0x2f | `cache`: safe to no-op |
| 3 | VFPU4 jump 0x11 | VFPU conversion |
| 2 | VFPU4 jump 0x14 | VFPU conversion |
| 2 | VFPUMatrix1 which 0 | VFPU matrix op |

The trapping functions cluster in `0x08B14000–0x08C00000`, the last 1 MB of code, which is probably the math/engine library.

Also: the README omits the `analyze.py` step, and `codegen.py` defaults `--toml` to `build/mygame/functions.toml`.

### sp00nznet: ran 2026-09-26 (MinGW gcc 16.2 build)
- `cover`: 98.7% of words decode, and 1.03% are unknown (partly data).
- `funcs`: **37,494 "functions"**. It seeds from every relocation pointer, so data gets traced as code: 19,619 functions have no `jr $ra`. That's roughly twice the real count.
- **0 imports resolved.** It depends on section names, which this EBOOT has stripped.
- `emit`: 123 MB of C, with **2,134 VFPU instructions emitted as traps**.
- Runtime: no thread scheduler, and transformed geometry isn't drawn yet. Its only proven target doesn't reach the screen.

### PSPRecomp (original): ran 2026-09-26 (framework-only build with MinGW g++)
- `psp_analyze`: all 140,186 relocations are fine, and it found 26,204 functions.
- **Module info not found, 0 imports.** Same stripped-section-names problem.
- The load base has to be written as `0x08804000`. Without the `0x`, it errors with "Guest memory access outside PSP RAM".
- Everything past the recompiler (HLE, renderer, threads) lives in the game-specific VCS profile, not the framework. A new game starts with almost no runtime.

## Check 2: build and boot (AljandrOrtega only; the others have no usable runtime for this game)
- **The whole generated file can't be compiled on 16 GB of RAM:** cc1 reached 6.7 GB at `-O1` before we stopped it. `tools/split_recomp.py` splits it into 38 chunks of about 4 MB, each compiling in about 13 s. The full build takes about 2 minutes with 6 jobs (`bakeoff/aljandrortega/build_t3b.sh`).
- Also needed `mingw-w64-ucrt-x86_64-vulkan-headers`, which the README doesn't mention.
- The `--image` run needs a real init file (`# init r28=0 r4=0 r5=0`), not `none`, plus `PSP_VFPU_TABLES` pointing at PPSSPP's `assets/vfpu`.
- **Bug fixed locally:** `codegen.py` finds import stubs via the `.sceStub.text` section name. On this stripped EBOOT, every stub compiled to `jr $ra; nop`, so all system calls silently did nothing (`hit_hle=0`). The fix falls back to the stub addresses from `parse_imports`. After the fix, 273/273 stubs call `sr_syscall`. This is worth sending upstream.
- **Result:** the Vulkan device initialized (Radeon 780M, 960×544 swapchain), the game code ran on the scheduler, and it made HLE calls until it stopped at the first unimplemented one: `sceKernelSetCompiledSdkVersion603_605`.

## Check 3: HLE breadth (`tools/hle_coverage.py`, full list in `docs/hle_coverage_aljandr.txt`)
**148 / 273 of the game's imports are implemented** (the runtime registers 199 handlers in total). Of the 125 missing:
- **About 45 are networking** (sceHttp 20, sceNetApctl 6, sceNetAdhocctl 4, sceNet/Inet/Resolver 6, Netconf 4, sceOpenPSID, sceParseHttp). These can be stubbed to "offline".
- **Real work:** sceSasCore 17 (effects, reverb, ADSR), ThreadManForUser 12 (Fpl, LwMutex, alarms, callbacks), sceUtility MsgDialog 6 plus Load/UnloadModule, IoFileMgr 9 (directories, ioctl), sceGe_user 7 (ListSync, stall address, EnQueueHead, break/continue), sceAudioOutput2 5, sceAtrac3plus 5, Kernel_Library 4, scePower 4, UtilsForUser 3 (cache ops can be no-ops), sceRtc 3, sceDisplay 2, sceMpeg 2, sceUmd 2, SysMem 1.

## Verdict
**AljandrOrtega wins** under the kill rule. It's the only candidate that lifts the whole game (99.3% of functions), builds, and runs game code through HLE. The other two can't resolve a single import on this EBOOT, and neither has a general-purpose runtime for a new game. Its GPL license matches our decision, and SDL3 + Vulkan matches the platform decision.

Worth taking from the others:
- sp00nznet's bring-up instruments (write-watch, stack-balance check, loop back-edge recording) and its decoder coverage report.
- PSPRecomp's profile layout, which keeps game-specific code out of the framework.
