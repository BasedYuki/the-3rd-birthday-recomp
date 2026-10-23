"""Emit versions/<game>.toml [addresses], [hooks] and [patches] as C #defines (ADDR_<NAME>,
HOOK_<NAME>, PATCH_<NAME> = index), or with --hooks print the list codegen takes: hook addresses
and the instruction patches as p<index>@<address>=<word>.

    python tools/toml_addrs.py versions/ULUS10567.toml build/ULUS10567/game_addrs.h
    python tools/toml_addrs.py --hooks versions/ULUS10567.toml
"""

import sys
import tomllib

args = [a for a in sys.argv[1:] if not a.startswith("--")]
with open(args[0], "rb") as f:
    data = tomllib.load(f)
if "--hooks" in sys.argv:
    items = ["0x%08x" % v for v in data.get("hooks", {}).values()]
    for i, words in enumerate(data.get("patches", {}).values()):
        for aw in words:
            addr, word = (int(x, 16) for x in aw.split("="))
            items.append("p%d@0x%08x=0x%08x" % (i, addr, word))
    print(",".join(items))
    sys.exit(0)
lines = ["/* Generated from %s by tools/toml_addrs.py -- do not edit. */" % args[0].replace("\\", "/"),
         "#pragma once"]
for name, value in data.get("addresses", {}).items():
    lines.append("#define ADDR_%s 0x%08Xu" % (name.upper(), value))
for name, value in data.get("hooks", {}).items():
    lines.append("#define HOOK_%s 0x%08Xu" % (name.upper(), value))
patches = list(data.get("patches", {}))
for i, name in enumerate(patches):
    lines.append("#define PATCH_%s %d" % (name.upper(), i))
lines.append("#define PATCH_COUNT %d" % len(patches))
open(args[1], "w").write("\n".join(lines) + "\n")
