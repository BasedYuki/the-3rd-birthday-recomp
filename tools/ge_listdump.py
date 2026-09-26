"""Decode a GE display list from a RAM dump (base 0x08800000), following JUMP/CALL/RET.

    python tools/ge_listdump.py <ram.bin> <hexstart> [<hexend>] [--draws]

--draws prints only PRIMs (with through-mode rectangles) plus the blend/texture state that
applies to each.
"""

import struct
import sys

B = 0x08800000
NAMES = {0x00: "NOP", 0x01: "VADDR", 0x02: "IADDR", 0x04: "PRIM", 0x08: "JUMP", 0x0a: "CALL",
         0x0b: "RET", 0x0c: "END", 0x0e: "SIGNAL", 0x0f: "FINISH", 0x10: "BASE", 0x12: "VTYPE",
         0x1e: "TEXEN", 0x21: "BLENDEN", 0x22: "ATESTEN", 0x23: "ZTESTEN", 0x1d: "LIGHTEN",
         0x1f: "FOGEN", 0x9c: "FBPTR", 0x9d: "FBW", 0xa0: "TEXADDR0", 0xa8: "TEXBUFW0",
         0xb8: "TEXSIZE0", 0xc2: "TEXMODE", 0xc3: "TEXFMT", 0xc5: "CLUTFMT", 0xc9: "TEXFUNC",
         0xca: "TEXENVCOL", 0xdb: "ATEST", 0xdf: "BLENDMODE", 0xe0: "BLENDFIXA", 0xe1: "BLENDFIXB",
         0xd2: "CLEARMODE", 0xde: "ZTEST", 0xe7: "ZMASK", 0xe8: "MASKRGB", 0xe9: "MASKA",
         0x55: "MATAMB", 0x58: "MATALPHA", 0x56: "MATDIFF", 0x57: "MATSPEC", 0xd3: "CLEAR?"}

args = [a for a in sys.argv[1:] if not a.startswith("--")]
draws_only = "--draws" in sys.argv
m = open(args[0], "rb").read()
start = int(args[1], 16)
end = int(args[2], 16) if len(args) > 2 else start + 0x4000
r32 = lambda a: struct.unpack_from("<I", m, (a & 0x0FFFFFFF) - B)[0]
r16 = lambda a: struct.unpack_from("<h", m, (a & 0x0FFFFFFF) - B)[0]

pc, base, stack, st, vaddr, vtype = start, 0, [], {}, 0, 0
for _ in range(20000):
    if not (start <= (pc & 0x0FFFFFFF) < end) and not stack:
        break
    w = r32(pc); c, d = w >> 24, w & 0xFFFFFF
    nxt = pc + 4
    if c == 0x10: base = d
    elif c == 0x01: vaddr = ((base & 0xFF0000) << 8) | d
    elif c == 0x12: vtype = d
    elif c == 0x08: nxt = ((base & 0xFF0000) << 8) | (d & 0xFFFFFC)
    elif c == 0x0a: stack.append(pc + 4); nxt = ((base & 0xFF0000) << 8) | (d & 0xFFFFFC)
    elif c == 0x0b and stack: nxt = stack.pop()
    st[c] = d
    if c == 0x04:
        typ, cnt = (d >> 16) & 7, d & 0xFFFF
        line = f"{pc:08x}: PRIM type={typ} count={cnt} vtype={vtype:06x} vaddr={vaddr:08x}"
        if vtype & 0x800000 and (vtype >> 7) & 3 == 2 and typ == 6 and cnt >= 2:
            tc = 4 if vtype & 3 == 2 else (2 if vtype & 3 == 1 else (8 if vtype & 3 == 3 else 0))
            col = {4: 2, 5: 2, 6: 2, 7: 4}.get((vtype >> 2) & 7, 0)
            off = tc + col
            off = (off + 1) & ~1
            stride = ((off + 6) + 3) & ~3 if col == 4 or tc == 8 else (off + 6)
            x0, y0 = r16(vaddr + off), r16(vaddr + off + 2)
            x1, y1 = r16(vaddr + stride + off), r16(vaddr + stride + off + 2)
            line += f" rect=({x0},{y0})-({x1},{y1})"
        line += (f" | tex={st.get(0x1e, 0) & 1} texaddr={st.get(0xa0, 0):06x} texfmt={st.get(0xc3, 0)}"
                 f" texfunc={st.get(0xc9, 0):06x} blend={st.get(0x21, 0) & 1} bmode={st.get(0xdf, 0):06x}"
                 f" fixa={st.get(0xe0, 0):06x} fixb={st.get(0xe1, 0):06x} atest={st.get(0x22, 0) & 1}/{st.get(0xdb, 0):06x}"
                 f" ztest={st.get(0x23, 0) & 1} mat={st.get(0x55, 0):06x}/{st.get(0x58, 0):02x}"
                 f" clutfmt={st.get(0xc5, 0):06x} cbp={st.get(0xb1, 0):06x}{st.get(0xb0, 0):06x} load={st.get(0xc4, 0):06x} fbp={st.get(0x9d, 0):06x}{st.get(0x9c, 0):06x} fbfmt={st.get(0xd2, 0)}")
        print(line)
    elif not draws_only:
        print(f"{pc:08x}: {NAMES.get(c, f'{c:02x}'):9} {d:06x}")
    if c == 0x0c and st.get(0x0f) is not None and not stack and draws_only is False:
        pass
    pc = nxt
