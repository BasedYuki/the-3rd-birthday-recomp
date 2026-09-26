"""Report which of the game's imported NIDs a runtime implements.

Usage:
    python tools/hle_coverage.py <imports.txt> <runtime src dir> [--names nid_names.h]

<imports.txt> is the "LIB 0xNID" list written by tools/elf_info.py --nids.
Handlers are found by scanning *.c for sr_hle_register(0xNID, ...).
"""

import argparse
import collections
import re
from pathlib import Path

REG = re.compile(r"sr_hle_register\(\s*0x([0-9a-fA-F]+)")
NAME = re.compile(r'\{0x([0-9a-fA-F]{8})u?,\s*"(\w+)"\}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("imports")
    ap.add_argument("src")
    ap.add_argument("--names")
    ap.add_argument("--nids", action="store_true", help="print the NID next to each missing name")
    args = ap.parse_args()

    impl = set()
    for f in Path(args.src).rglob("*.c"):
        impl.update(int(m, 16) for m in REG.findall(f.read_text(errors="replace")))

    names = {}
    if args.names:
        names = {int(n, 16): s for n, s in NAME.findall(Path(args.names).read_text(errors="replace"))}

    total, have = collections.Counter(), collections.Counter()
    missing = collections.defaultdict(list)
    for line in Path(args.imports).read_text().splitlines():
        lib, nid = line.split()
        nid = int(nid, 16)
        total[lib] += 1
        if nid in impl:
            have[lib] += 1
        else:
            missing[lib].append(f"{names.get(nid, '?')}=0x{nid:08X}" if args.nids
                                else names.get(nid, f"0x{nid:08X}"))

    print(f"covered {sum(have.values())}/{sum(total.values())} imports "
          f"({len(impl)} handlers registered in runtime)\n")
    for lib in sorted(total, key=lambda l: (have[l] - total[l], l)):
        mark = "ok " if not missing[lib] else "   "
        print(f"{mark}{lib:<22} {have[lib]:>3}/{total[lib]:<3} {', '.join(missing[lib])}")


if __name__ == "__main__":
    main()
