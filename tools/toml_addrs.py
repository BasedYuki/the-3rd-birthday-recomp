"""Emit versions/<game>.toml [addresses] and [hooks] as C #defines (ADDR_<NAME>, HOOK_<NAME>),
or with --hooks print the hook addresses as a comma list for codegen.

    python tools/toml_addrs.py versions/ULUS10567.toml build/ULUS10567/game_addrs.h
    python tools/toml_addrs.py --hooks versions/ULUS10567.toml
"""

import sys
import tomllib

args = [a for a in sys.argv[1:] if not a.startswith("--")]
with open(args[0], "rb") as f:
    data = tomllib.load(f)
if "--hooks" in sys.argv:
    print(",".join("0x%08x" % v for v in data.get("hooks", {}).values()))
    sys.exit(0)
lines = ["/* Generated from %s by tools/toml_addrs.py -- do not edit. */" % args[0].replace("\\", "/"),
         "#pragma once"]
for name, value in data.get("addresses", {}).items():
    lines.append("#define ADDR_%s 0x%08Xu" % (name.upper(), value))
for name, value in data.get("hooks", {}).items():
    lines.append("#define HOOK_%s 0x%08Xu" % (name.upper(), value))
open(args[1], "w").write("\n".join(lines) + "\n")
