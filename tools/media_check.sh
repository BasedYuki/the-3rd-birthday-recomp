#!/bin/sh
# Build tools/media_check.exe (MSYS2 UCRT64) against the port's decoders and run it on the disc.
# Needs the at3 objects from a normal scripts/build.sh run.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/ULUS10567"
gcc -O2 -I"$ROOT/runtime/src/rt" -o "$ROOT/build/media_check.exe" "$ROOT/tools/media_check.c" \
    "$ROOT/runtime/src/rt/h264_mf.c" "$OUT"/at3/*.o -lmfplat -lole32 \
    -static-libgcc -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic
echo built
