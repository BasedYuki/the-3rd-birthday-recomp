#!/bin/bash
# Assemble the player kit: everything a player's PC needs to build game.dll from their own disc
# image, without MSYS2 (tools/prepare.py, run by the launcher). Run from an MSYS2 UCRT64 shell:
#   bash scripts/make_kit.sh <out-dir>
# Contents: mingw/ (gcc, cc1, as, ld and the libraries a DLL link needs), python/ (MSYS2's
# Python with a trimmed standard library), tools/ (translation scripts, eboot_decrypt.exe),
# rt/ (headers the generated code compiles against), versions/, licenses/. No game code.
set -eu
cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="${1:?usage: make_kit.sh <out-dir>}"
U=/ucrt64
GV=$(ls $U/lib/gcc/x86_64-w64-mingw32 | head -1)
GL="lib/gcc/x86_64-w64-mingw32/$GV"
rm -rf "$OUT"; mkdir -p "$OUT"

# --- compiler (same relative layout as /ucrt64, so gcc finds its parts) ---
M="$OUT/mingw"
mkdir -p "$M/bin" "$M/$GL" "$M/x86_64-w64-mingw32/bin" "$M/lib" "$M/include"
cp $U/bin/gcc.exe "$M/bin/"
for f in cc1.exe collect2.exe lto-wrapper.exe liblto_plugin.dll crtbegin.o crtend.o libgcc.a libgcc_eh.a; do cp "$U/$GL/$f" "$M/$GL/"; done
cp -r "$U/$GL/include" "$M/$GL/"
cp $U/x86_64-w64-mingw32/bin/as.exe $U/x86_64-w64-mingw32/bin/ld.exe "$M/x86_64-w64-mingw32/bin/"
for f in dllcrt2.o crt2.o libadvapi32.a libkernel32.a libmingw32.a libmingwex.a libmsvcrt.a libucrt.a libucrtbase.a \
         libpthread.a libpthread.dll.a libwinpthread.a libwinpthread.dll.a libshell32.a libuser32.a libmoldname.a; do
  [ -f "$U/lib/$f" ] && cp "$U/lib/$f" "$M/lib/"
done
# headers the generated code, game_glue.c and code mods use (plus their includes)
HDRS="_mingw.h _mingw_mac.h _mingw_off_t.h _mingw_secapi.h _mingw_stdarg.h corecrt.h corecrt_malloc.h corecrt_memory.h
      corecrt_stdio_config.h corecrt_wstdio.h corecrt_wstdlib.h corecrt_wstring.h corecrt_startup.h crtdefs.h errno.h
      limits.h malloc.h math.h setjmp.h stddef.h stdint.h stdio.h stdlib.h string.h vadefs.h stdarg.h ctype.h
      inttypes.h stdbool.h float.h assert.h time.h swprintf.inl sec_api"
for h in $HDRS; do [ -e "$U/include/$h" ] && cp -r "$U/include/$h" "$M/include/"; done
# the DLLs those programs need
for e in "$M/bin/gcc.exe" "$M/$GL/cc1.exe" "$M/$GL/collect2.exe" "$M/$GL/lto-wrapper.exe" \
         "$M/x86_64-w64-mingw32/bin/as.exe" "$M/x86_64-w64-mingw32/bin/ld.exe"; do
  ldd "$e" | awk '{print $3}' | grep -i "ucrt64" | while read -r d; do cp -n "$d" "$M/bin/"; done
done
# as/ld and cc1 look for their DLLs next to themselves first
cp -n "$M"/bin/*.dll "$M/x86_64-w64-mingw32/bin/"; cp -n "$M"/bin/*.dll "$M/$GL/"

# --- Python (MSYS2 UCRT64 build) with a trimmed standard library ---
P="$OUT/python"
mkdir -p "$P/lib"
cp $U/bin/python3.14.exe "$P/python.exe"
cp $U/bin/libpython3.14.dll "$P/"
ldd $U/bin/python3.14.exe | awk '{print $3}' | grep -i ucrt64 | while read -r d; do cp -n "$d" "$P/"; done
PL=$U/lib/python3.14
mkdir -p "$P/lib/python3.14"
( cd "$PL" && tar cf - --exclude=test --exclude=idlelib --exclude=tkinter --exclude=turtledemo --exclude=ensurepip \
    --exclude=site-packages --exclude=pydoc_data --exclude='config-*' --exclude=__pycache__ --exclude=lib2to3 \
    --exclude=unittest --exclude=sqlite3 --exclude=curses --exclude=xmlrpc --exclude=email --exclude=http \
    --exclude=asyncio --exclude=multiprocessing --exclude=distutils --exclude=venv --exclude=wsgiref \
    --exclude='_tkinter*' . ) | ( cd "$P/lib/python3.14" && tar xf - )
rm -f "$P/lib/python3.14/lib-dynload"/{_test,_xxtest,xxlimited,xxsubtype,_ctypes_test,_curses,_sqlite3,_tkinter,_ssl,_remote_debugging}*.pyd
for d in $(ls "$P/lib/python3.14/lib-dynload"/*.pyd); do
  ldd "$d" 2>/dev/null | awk '{print $3}' | grep -i ucrt64 | while read -r x; do cp -n "$x" "$P/"; done || true
done

# --- tools, headers, game data description ---
mkdir -p "$OUT/tools" "$OUT/rt" "$OUT/versions" "$OUT/licenses"
cp runtime/tools/{prxload,imports,analyze,codegen}.py tools/{split_recomp,toml_addrs,mods_gen,write_launch_cfg,prepare}.py "$OUT/tools/"
ED=runtime/third_party/eboot_decrypt
TMPO=$(mktemp -d)
for c in AES SHA1 bn ec kirk_engine amctrl; do gcc -O2 -w -c $ED/libkirk/$c.c -o $TMPO/$c.o; done
g++ -std=c++17 -O2 -w -I$ED/shim -I$ED $ED/eboot_decrypt.cpp $ED/PrxDecrypter.cpp $TMPO/*.o -static -s     -o "$OUT/tools/eboot_decrypt.exe"
rm -rf $TMPO
cp runtime/src/rt/recomp.h runtime/src/rt/game_glue.c "$OUT/rt/"
cp versions/ULUS10567.toml "$OUT/versions/"
cp runtime/LICENSE "$OUT/licenses/THIS_PROJECT_GPL2.txt"
cp $U/share/licenses/gcc/COPYING* "$OUT/licenses/" 2>/dev/null || true
cat > "$OUT/licenses/README.txt" <<'EOF'
gcc, binutils (as, ld) and the MinGW-w64 runtime: GPL / LGPL / MinGW-w64 licenses, from the
MSYS2 project (https://www.msys2.org). Python: PSF License. eboot_decrypt.exe: GPL-3.0-or-later
(contains PPSSPP's PrxDecrypter and libkirk, https://github.com/hrydgard/ppsspp).
This project: GPL-2.0-or-later (THIS_PROJECT_GPL2.txt).
EOF
# kit id: changes when the translation tools, headers or the game description change
( cat "$OUT"/tools/*.py "$OUT"/rt/* "$OUT"/versions/*; ) | sha1sum | cut -c1-16 > "$OUT/kit_id.txt"
du -sh "$OUT" "$M" "$P"
