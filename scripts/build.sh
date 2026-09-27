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
if [ $REGEN = 1 ] || [ ! -f "$OUT/recomp.c" ] || [ "$ELF" -nt "$OUT/recomp.c" ] \
   || [ "$RT/tools/codegen.py" -nt "$OUT/recomp.c" ] || [ "$ROOT/versions/$GAME.toml" -nt "$OUT/recomp.c" ]; then
  python "$RT/tools/prxload.py"  "$ELF" $BASE --out="$OUT/image.bin"
  python "$RT/tools/imports.py"  "$ELF" $BASE --toml="$OUT/imports.toml" > /dev/null
  python "$RT/tools/analyze.py"  "$ELF" --base=$BASE --toml="$OUT/functions.toml" --quiet
  python "$RT/tools/codegen.py"  "$ELF" "$OUT/recomp.c" --base=$BASE --toml="$OUT/functions.toml" \
    --hooks="$(python "$ROOT/tools/toml_addrs.py" --hooks "$ROOT/versions/$GAME.toml")" \
    | grep -v '^DEBUG'
  rm -rf "$GEN"
  python "$ROOT/tools/split_recomp.py" "$OUT/recomp.c" "$GEN"
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
  gcc -O2 -fno-math-errno -w -I"$RT/src/rt" -c "$RT/src/rt/ge.c" -o "$OUT/ge.o"

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
for st in vert frag; do
  if [ "$SH/shaders/psp.$st" -nt "$SH/psp_$st.inc" ]; then
    if command -v glslc >/dev/null; then glslc -O -mfmt=c -o "$SH/psp_$st.inc" "$SH/shaders/psp.$st"
    else echo "warning: shaders/psp.$st changed but glslc is missing; using the old psp_$st.inc"; fi
  fi
done

# Game addresses: versions/<game>.toml [addresses] -> ADDR_* defines (nothing hardcoded in C).
python "$ROOT/tools/toml_addrs.py" "$ROOT/versions/$GAME.toml" "$OUT/game_addrs.h"

RT_SRCS=(recomp.c vfpu_interp.c hle.c hle_ext.c sched.c iso.c mpeg.c pgf.c gui.c audio.c h264_mf.c
         savedata.c osk_win.c driver.c atrac_hle.c sas_hle.c twinstick.c gpu_sdl3vk/sdl3vk.c gpu_sdl3vk/ge_gpu.c)
gcc "${CFLAGS[@]}" -I"$OUT" -fuse-ld=lld -o "$OUT/$GAME.exe" "$GEN"/*.o "$OUT/ge.o" "$OUT"/at3/*.o \
  "${RT_SRCS[@]/#/$RT/src/rt/}" \
  -lSDL3 -lvulkan-1 -lmfplat -lgdi32 -ldinput8 -ldxguid -lole32 -lwinmm \
  -static-libgcc -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic

cp /ucrt64/bin/SDL3.dll "$OUT/"
cp -r "$RT/font" "$OUT/"
echo "# init r28=0 r4=0 r5=0" > "$OUT/init.trace"
echo "built $OUT/$GAME.exe"
