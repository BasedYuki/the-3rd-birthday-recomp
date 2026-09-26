"""Inspect a decrypted PSP executable (ELF / PRX).

Prints sections, sceModuleInfo, imported libraries (with NID counts) and
exports, plus a rough count of VFPU instructions in executable sections.

Usage:
    python tools/elf_info.py <decrypted EBOOT.BIN> [--nids OUT.txt]

--nids writes every imported library/NID pair to a text file, which is the
checklist of HLE functions a recomp runtime has to implement.
"""

import argparse
import collections
import hashlib
import struct
import sys
from pathlib import Path

SHF_EXECINSTR = 0x4

# Primary opcodes (top 6 bits) that belong to the Allegrex VFPU.
VFPU_OPCODES = {
    0x12: "cop2 (mfv/mtv/bvf)",
    0x18: "vfpu0", 0x19: "vfpu1", 0x1B: "vfpu3",
    0x32: "lv.s", 0x34: "vfpu4", 0x35: "lvl/lvr.q", 0x36: "lv.q",
    0x37: "vfpu5", 0x3A: "sv.s", 0x3C: "vfpu6", 0x3D: "svl/svr.q",
    0x3E: "sv.q", 0x3F: "vfpu7",
}


class Elf:
    def __init__(self, data):
        self.d = data
        if data[:4] != b"\x7fELF":
            sys.exit("not an ELF (still encrypted?)")
        (self.type, self.machine, _ver, self.entry, self.phoff, self.shoff,
         self.flags, _ehsize, _phentsize, self.phnum, _shentsize, self.shnum,
         self.shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)

        self.phdrs = [struct.unpack_from("<IIIIIIII", data, self.phoff + i * 32)
                      for i in range(self.phnum)]
        raw = [struct.unpack_from("<IIIIIIIIII", data, self.shoff + i * 40)
               for i in range(self.shnum)]
        strtab = raw[self.shstrndx]
        self.sections = []
        for name, typ, flags, addr, off, size, *_ in raw:
            s = data[strtab[4] + name:].split(b"\x00", 1)[0].decode()
            self.sections.append((s, typ, flags, addr, off, size))

    def vaddr_to_off(self, va):
        for _t, off, vaddr, _pa, filesz, *_ in self.phdrs:
            if vaddr <= va < vaddr + filesz:
                return off + va - vaddr
        raise ValueError(f"vaddr 0x{va:08X} not in any segment")

    def u32(self, va):
        return struct.unpack_from("<I", self.d, self.vaddr_to_off(va))[0]

    def cstr(self, va):
        o = self.vaddr_to_off(va)
        return self.d[o:self.d.index(b"\x00", o)].decode("ascii", "replace")

    def module_info_off(self):
        """File offset of sceModuleInfo."""
        for name, _t, _f, _a, off, _s in self.sections:
            if name == ".rodata.sceModuleInfo":
                return off
        # PRX convention: first program header's p_paddr holds its file offset
        return self.phdrs[0][3] & 0x7FFFFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--nids")
    args = ap.parse_args()

    data = Path(args.elf).read_bytes()
    elf = Elf(data)

    print(f"file    {Path(args.elf).name}  ({len(data)} bytes)")
    print(f"sha256  {hashlib.sha256(data).hexdigest()}")
    kind = {0xFFA0: "PSP PRX (relocatable)", 2: "static EXEC"}.get(elf.type, hex(elf.type))
    print(f"type    0x{elf.type:04X} {kind}, machine {elf.machine}, entry 0x{elf.entry:08X}")
    print(f"flags   0x{elf.flags:08X}\n")

    print("Segments")
    for typ, off, vaddr, paddr, filesz, memsz, flg, align in elf.phdrs:
        print(f"  type=0x{typ:08X} off=0x{off:06X} vaddr=0x{vaddr:08X} "
              f"filesz=0x{filesz:06X} memsz=0x{memsz:06X} flags={flg}")

    print("\nSections (X = executable; names may be stripped)")
    for i, (name, typ, flags, addr, off, size) in enumerate(elf.sections):
        if typ:
            x = "X" if flags & SHF_EXECINSTR else " "
            print(f"  {i:>2} {x} {name or '-':<24} type=0x{typ:08X} "
                  f"addr=0x{addr:08X} size=0x{size:06X}")

    mi = elf.module_info_off()
    attr, ver = struct.unpack_from("<HH", data, mi)
    name = data[mi + 4:mi + 32].split(b"\x00", 1)[0].decode()
    gp, ent, ent_end, stub, stub_end = struct.unpack_from("<IIIII", data, mi + 32)
    print(f"\nsceModuleInfo  name={name!r} attr=0x{attr:04X} ver=0x{ver:04X} gp=0x{gp:08X}")

    print("\nImports")
    imports = []
    va = stub
    while va < stub_end:
        lib_name_va = elf.u32(va)
        lib_ver, lib_flags = struct.unpack_from("<HH", data, elf.vaddr_to_off(va + 4))
        size, vcount, fcount = struct.unpack_from("<BBH", data, elf.vaddr_to_off(va + 8))
        nid_va = elf.u32(va + 12)
        lib = elf.cstr(lib_name_va)
        nids = [elf.u32(nid_va + i * 4) for i in range(fcount)]
        imports.append((lib, nids))
        print(f"  {lib:<28} {fcount:>4} funcs {vcount:>2} vars")
        va += size * 4
    total = sum(len(n) for _l, n in imports)
    print(f"  {len(imports)} libraries, {total} imported functions")

    print("\nExports")
    va = ent
    while va < ent_end:
        name_va = elf.u32(va)
        size, vcount, fcount = struct.unpack_from("<BBH", data, elf.vaddr_to_off(va + 8))
        lib = elf.cstr(name_va) if name_va else "(module_start/syslib)"
        print(f"  {lib:<28} {fcount:>4} funcs {vcount:>2} vars")
        va += size * 4

    counts = collections.Counter()
    code_words = 0
    for name, _t, flags, addr, off, size in elf.sections:
        if flags & SHF_EXECINSTR:
            for (w,) in struct.iter_unpack("<I", data[off:off + size - size % 4]):
                code_words += 1
                op = w >> 26
                if op in VFPU_OPCODES:
                    counts[VFPU_OPCODES[op]] += 1
    vf = sum(counts.values())
    print(f"\nCode: {code_words} instructions, ~{vf} VFPU ({100 * vf / max(code_words, 1):.1f}%, "
          "rough: counts every word with a VFPU primary opcode)")
    for k, v in counts.most_common():
        print(f"  {k:<20} {v}")

    if args.nids:
        with open(args.nids, "w") as f:
            for lib, nids in imports:
                for nid in nids:
                    f.write(f"{lib} 0x{nid:08X}\n")
        print(f"\nwrote {total} NIDs to {args.nids}")


if __name__ == "__main__":
    main()
