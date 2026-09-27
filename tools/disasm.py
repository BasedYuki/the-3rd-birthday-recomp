"""Disassemble the relocated game image (build/<game>/image.bin, base 0x08804000).

    PYTHONPATH="Vs assets/pylib" python tools/disasm.py <hexstart> <hexend> [image.bin]

Uses capstone (installed into Vs assets/pylib). VFPU/Allegrex-only opcodes print as .word.
"""

import struct
import sys

import capstone

BASE = 0x08804000
start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
path = sys.argv[3] if len(sys.argv) > 3 else "build/ULUS10567/image.bin"
img = open(path, "rb").read()
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
a = start
while a < end:
    w = img[a - BASE:a - BASE + 4]
    ins = list(md.disasm(w, a))
    word = struct.unpack("<I", w)[0]
    if ins:
        print(f"{a:08x}: {word:08x}  {ins[0].mnemonic:8} {ins[0].op_str}")
    else:
        print(f"{a:08x}: {word:08x}  .word")
    a += 4
