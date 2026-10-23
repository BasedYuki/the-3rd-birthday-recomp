"""Write build/<game>/launch.cfg for a double-click start (runtime/src/rt/driver.c
launch_defaults): the relocated image, its load base and the EBOOT's entry point. A user's
iso= line is kept.

    python tools/write_launch_cfg.py <EBOOT.elf> <base-hex> <launch.cfg>
"""

import os
import struct
import sys

elf, base, out = sys.argv[1], int(sys.argv[2], 16), sys.argv[3]
head = open(elf, "rb").read(64)
etype = struct.unpack_from("<H", head, 16)[0]
entry = struct.unpack_from("<I", head, 24)[0]
if etype == 0xFFA0:                     # PRX: the entry is relative to the load base
    entry += base
keep = []
if os.path.exists(out):
    keep = [l for l in open(out).read().splitlines() if l.startswith("iso=")]
lines = ["image=image.bin", f"base={base:08X}", f"entry={entry:08X}"] + keep
open(out, "w").write("\n".join(lines) + "\n")
