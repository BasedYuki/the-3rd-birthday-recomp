"""List the call sites (jal) of imported functions in a relocated image.

Usage:
    python tools/find_callers.py <build dir> <regex on library or function name>

<build dir> holds image.bin and imports.toml from scripts/build.sh. Function names come from
runtime/src/rt/nid_names.h. Example:
    python tools/find_callers.py build/ULUS10567 "sceMpeg|sceAtrac"
"""

import re
import struct
import sys
from pathlib import Path

BASE = 0x08804000
ROOT = Path(__file__).resolve().parent.parent


def main():
    bdir, pat = Path(sys.argv[1]), re.compile(sys.argv[2])
    img = (bdir / "image.bin").read_bytes()
    names = {int(n, 16): s for n, s in re.findall(
        r'\{0x([0-9a-f]{8})u, "(\w+)"\}', (ROOT / "runtime/src/rt/nid_names.h").read_text(errors="replace"))}
    stubs = {}
    for stub, lib, nid in re.findall(r'stub = 0x([0-9a-f]+)\s+lib = "(\w+)"\s+nid = 0x([0-9a-f]+)',
                                     (bdir / "imports.toml").read_text()):
        name = names.get(int(nid, 16), f"{lib}_{nid}")
        if pat.search(lib) or pat.search(name):
            stubs[int(stub, 16)] = name
    calls = {}
    for off in range(0, len(img) - 3, 4):
        w = struct.unpack_from("<I", img, off)[0]
        if w >> 26 == 3:
            target = ((BASE + off) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if target in stubs:
                calls.setdefault(stubs[target], []).append(BASE + off)
    for name in sorted(set(stubs.values())):
        sites = calls.get(name, [])
        print(f"{name:40} {len(sites):3}  " + " ".join(f"0x{a:08x}" for a in sites[:12]))


if __name__ == "__main__":
    main()
