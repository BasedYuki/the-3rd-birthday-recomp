#!/bin/bash
# Full pipeline: decrypted EBOOT -> generated C -> native exe.
# Run from an MSYS2 UCRT64 shell (scripts/build.ps1 does that for you).
#
#   GAME=ULUS10567 JOBS=6 bash scripts/build.sh [--regen]
#
# Output goes to build/$GAME/ (gitignored: it contains code derived from the game).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

GAME=${GAME:-ULUS10567}
JOBS=${JOBS:-6}
BASE=0x08804000
ELF="$ROOT/dumps/${GAME}_EBOOT.elf"
RT="$ROOT/runtime"
OUT="$ROOT/build/$GAME"
GEN="$OUT/gen"
REGEN=0
[ "${1:-}" = "--regen" ] && REGEN=1

[ -f "$ELF" ] || { echo "missing $ELF (dump it with PPSSPP, see PLAN.md step 0)"; exit 1; }
mkdir -p "$OUT" "$GEN"

# 1. Relocate, map imports, discover functions, generate C.
# Hooked function entries: the game's (versions/<game>.toml) and the enabled code mods'. The
# code is regenerated only when this list changes (a mod hooking an already hooked function,
# or a change to a mod's C code, just relinks).
HOOKS="$(python "$ROOT/tools/toml_addrs.py" --hooks "$ROOT/versions/$GAME.toml"),$(python "$ROOT/tools/mods_gen.py" --hooks)"
HOOKS="$(echo "$HOOKS" | tr ',' '\n' | grep . | sort -u | paste -sd, -)"
if [ $REGEN = 1 ] || [ ! -f "$OUT/recomp.c" ] || [ "$ELF" -nt "$OUT/recomp.c" ] \
   || [ "$RT/tools/codegen.py" -nt "$OUT/recomp.c" ] || [ "$ROOT/versions/$GAME.toml" -nt "$OUT/recomp.c" ] \
   || [ "$HOOKS" != "$(cat "$OUT/hooks.txt" 2>/dev/null)" ]; then
  python "$RT/tools/prxload.py"  "$ELF" $BASE --out="$OUT/image.bin"
  python "$RT/tools/imports.py"  "$ELF" $BASE --toml="$OUT/imports.toml" > /dev/null
  python "$RT/tools/analyze.py"  "$ELF" --base=$BASE --toml="$OUT/functions.toml" --quiet
  python "$RT/tools/codegen.py"  "$ELF" "$OUT/recomp.c" --base=$BASE --toml="$OUT/functions.toml" \
    --hooks="$HOOKS" \
    | grep -v '^DEBUG'
  rm -rf "$GEN"
  python "$ROOT/tools/split_recomp.py" "$OUT/recomp.c" "$GEN"
  echo "$HOOKS" > "$OUT/hooks.txt"
fi

# 2. Compile the generated chunks in parallel (the single file needs >16 GB RAM).
CFLAGS=(-O1 -foptimize-sibling-calls -I"$RT/src/rt" -I"$GEN" -DSR_SDL3VK -w)
export OUT
export RTH="$RT/src/rt/recomp.h"
ls "$GEN"/recomp_*.c | xargs -P "$JOBS" -I{} bash -c '
  c="$1"; o="${c%.c}.o"; shift
  [ "$o" -nt "$c" ] && [ "$o" -nt "$RTH" ] || gcc "$@" -c "$c" -o "$o"' _ {} "${CFLAGS[@]}"

# 3. Runtime + link.
{ [ "$OUT/ge.o" -nt "$RT/src/rt/ge.c" ] && [ "$OUT/ge.o" -nt "$RT/src/rt/recomp.h" ]; } 2>/dev/null || \
  gcc -O2 -g -fno-math-errno -w -I"$RT/src/rt" -c "$RT/src/rt/ge.c" -o "$OUT/ge.o"

# ATRAC3/3+ decoder (FFmpeg via PPSSPP's at3_standalone, C++): compiled once, libstdc++ linked
# statically so the exe needs no extra DLL.
AT3="$RT/third_party/at3_standalone"
mkdir -p "$OUT/at3"
for c in atrac atrac3 atrac3plus atrac3plusdec atrac3plusdsp get_bits compat fft mem; do
  [ "$OUT/at3/$c.o" -nt "$AT3/$c.cpp" ] || \
    g++ -O2 -w -I"$AT3" -I"$RT/third_party/at3_shim" -c "$AT3/$c.cpp" -o "$OUT/at3/$c.o"
done
g++ -O2 -Wall -I"$AT3" -I"$RT/src/rt" -c "$RT/src/rt/at3_bridge.cpp" -o "$OUT/at3/at3_bridge.o"

# Vulkan shaders: GLSL -> embedded SPIR-V (psp_*.inc) when the source is newer. Needs glslc
# (pacman -S mingw-w64-ucrt-x86_64-shaderc); without it the committed .inc files are used.
SH="$RT/src/rt/gpu_sdl3vk"
if [ "$SH/shaders/psp.frag" -nt "$SH/psp_frag_dual.inc" ]; then   # the dual-source blend variant
  if command -v glslc >/dev/null; then glslc -O -DDUAL_SRC -mfmt=c -o "$SH/psp_frag_dual.inc" "$SH/shaders/psp.frag"
  else echo "warning: shaders/psp.frag changed but glslc is missing; using the old psp_frag_dual.inc"; fi
fi
for st in vert frag; do
  if [ "$SH/shaders/psp.$st" -nt "$SH/psp_$st.inc" ]; then
    if command -v glslc >/dev/null; then glslc -O -mfmt=c -o "$SH/psp_$st.inc" "$SH/shaders/psp.$st"
    else echo "warning: shaders/psp.$st changed but glslc is missing; using the old psp_$st.inc"; fi
  fi
done

# Game addresses: versions/<game>.toml [addresses] -> ADDR_* defines (nothing hardcoded in C).
python "$ROOT/tools/toml_addrs.py" "$ROOT/versions/$GAME.toml" "$OUT/game_addrs.h"

RT_SRCS=(recomp.c vfpu_interp.c hle.c hle_ext.c sched.c iso.c mpeg.c pgf.c gui.c audio.c h264_mf.c
         savedata.c osk_win.c driver.c ctrl_server.c atrac_hle.c sas_hle.c twinstick.c mods.c update.c gpu_sdl3vk/sdl3vk.c gpu_sdl3vk/ge_gpu.c)
# Code mods (mods/<name>/mod.toml + *.c, see mods/README.md): each mod's mod_hook gets its own name.
python "$ROOT/tools/mods_gen.py" --gen "$OUT"
MOD_OBJS=("$OUT/mods_gen.c")
mkdir -p "$OUT/mods_obj"
while read -r id src; do
  [ -n "$id" ] || continue
  o="$OUT/mods_obj/${id}_$(basename "$src" .c).o"
  gcc -O2 -g -I"$RT/src/rt" -I"$OUT" -include "$OUT/mods_hooks.h" -Dmod_hook="mod_${id}_hook" -c "$src" -o "$o"
  MOD_OBJS+=("$o")
done < "$OUT/mods_srcs.txt"
# The runtime (GE backend, HLE, scheduler) at -O2; the later flag overrides CFLAGS' -O1, which
# is only there for the huge generated chunks.
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"   # release version (window title, update check)
gcc "${CFLAGS[@]}" -O2 -g -fno-math-errno -DSR_VERSION="\"$VERSION\"" -I"$OUT" -fuse-ld=lld -o "$OUT/${EXE_NAME:-$GAME}.exe" "$GEN"/*.o "$OUT/ge.o" "$OUT"/at3/*.o \
  "${RT_SRCS[@]/#/$RT/src/rt/}" "${MOD_OBJS[@]}" \
  -lSDL3 -lvulkan-1 -lmfplat -lgdi32 -lgdiplus -lws2_32 -lwinhttp -ldinput8 -ldxguid -lole32 -lwinmm \
  -static-libgcc -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic

cp /ucrt64/bin/SDL3.dll "$OUT/" 2>/dev/null || echo "note: SDL3.dll in use (game running), kept the existing copy"
# SDL3's own dependency and the Vulkan loader next to the exe: a double-click start (no MSYS2 on
# PATH) found neither and fell back to the slow GDI path.
for d in libiconv-2.dll vulkan-1.dll; do cp /ucrt64/bin/$d "$OUT/" 2>/dev/null || true; done
cp -r "$RT/font" "$OUT/"
echo "# init r28=0 r4=0 r5=0" > "$OUT/init.trace"
# Double-click launch (driver.c launch_defaults): the image, its base and entry point; the
# VFPU tables next to the executable.
python "$ROOT/tools/write_launch_cfg.py" "$ELF" "$BASE" "$OUT/launch.cfg"
mkdir -p "$OUT/assets" && cp -r "$RT/assets/vfpu" "$OUT/assets/"
# The launcher (settings, saves, mods, update notice, Play) next to the game.
LSRC="$RT/src/launcher"
(cd "$LSRC" && windres launcher.rc -O coff -o "$OUT/launcher_res.o")
gcc -O2 -mwindows -DSR_VERSION="\"$VERSION\"" -o "$OUT/The 3rd Birthday.exe" "$LSRC/launcher.c" "$OUT/launcher_res.o" \
  -lwinhttp -lcomdlg32 -lshell32 -lole32 -luuid -static-libgcc \
  || echo "note: launcher not rebuilt (in use?)"
echo "built $OUT/${EXE_NAME:-$GAME}.exe"
