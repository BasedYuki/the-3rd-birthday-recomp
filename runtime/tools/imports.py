#!/usr/bin/env python3
# Parse a PSP PRX import table from the rebased+relocated image and map each import stub
# address to (library name, NID). Ports PPSSPP's KernelImportModuleFuncs walk over the
# PspLibStubEntry array between module-info libstub..libstubend.
#
# Each stub at firstSymAddr + i*8 imports nidData[i] from the named library; the loader
# normally patches a syscall into the stub's delay slot. For static recompilation we instead
# emit a call to the matching HLE handler.
#
# Usage: imports.py <prx-elf> <base-hex> [--toml out.toml]

import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from analyze import Elf


def parse_imports(elf):
    libstub = None
    libstubend = None

    # 1. Intentar la vía estándar por sección ELF
    mi = elf.sec(".rodata.sceModuleInfo")
    if mi:
        b = elf.read_at_vaddr(mi["addr"], 52)
        libstub, libstubend = struct.unpack("<2I", b[44:52])
    else:
        # 2. Fallback: Buscar la firma de PspLibStubEntry en la memoria ejecutable
        sys.stderr.write("Aviso: .rodata.sceModuleInfo no encontrada. Escaneando memoria ejecutable...\n")

        # Obtener rango de direcciones virtuales del ELF
        min_vaddr = None
        max_vaddr = None

        # Recorrer Program Headers para conocer los límites de memoria
        if hasattr(elf, 'ph') and elf.ph:
            for ph in elf.ph:
                if ph.get('p_type') == 1: # PT_LOAD
                    vaddr = ph['p_vaddr']
                    memsz = ph['p_memsz']
                    if min_vaddr is None or vaddr < min_vaddr:
                        min_vaddr = vaddr
                    if max_vaddr is None or (vaddr + memsz) > max_vaddr:
                        max_vaddr = vaddr + memsz

        # Si no hay PH, recurrir a las secciones
        if min_vaddr is None and hasattr(elf, 'sections'):
            for sec in elf.sections:
                if sec['addr'] > 0 and sec['size'] > 0:
                    if min_vaddr is None or sec['addr'] < min_vaddr:
                        min_vaddr = sec['addr']
                    if max_vaddr is None or (sec['addr'] + sec['size']) > max_vaddr:
                        max_vaddr = sec['addr'] + sec['size']

        if min_vaddr is None:
            min_vaddr = 0x08804040
            max_vaddr = min_vaddr + len(getattr(elf, 'data', getattr(elf, 'raw_bytes', b'')))

        found_stubs = []
        # Escanear alineado a 4 bytes
        for addr in range(min_vaddr, max_vaddr - 28, 4):
            try:
                e = elf.read_at_vaddr(addr, 28)
                if not e or len(e) < 28:
                    continue
                name_ptr, ver, flags, size, numVars, numFuncs, nidData, firstSym = struct.unpack("<IHHBBHII", e[:20])

                # Validar valores típicos de PspLibStubEntry
                if size == 5 and 0 < numFuncs < 2048:
                    if (min_vaddr <= name_ptr < max_vaddr) and (min_vaddr <= nidData < max_vaddr) and (min_vaddr <= firstSym < max_vaddr):
                        found_stubs.append(addr)
            except Exception:
                continue

        if not found_stubs:
            raise SystemExit("Error: No se encontraron tablas de importación PspLibStubEntry en la memoria del ELF.")

        libstub = found_stubs[0]
        libstubend = found_stubs[-1] + 20

    def r32(a):
        return struct.unpack("<I", elf.read_at_vaddr(a, 4))[0]

    def cstr(a):
        out = bytearray()
        while True:
            ch = elf.read_at_vaddr(a, 1)
            if not ch or ch[0] == 0:
                break
            out.append(ch[0]); a += 1
        return out.decode("latin1")

    stubs = {}        # stubAddr -> (libname, nid)
    pos = libstub
    while pos < libstubend:
        e = elf.read_at_vaddr(pos, 28)
        if not e or len(e) < 28:
            break
        name_ptr, ver, flags, size, numVars, numFuncs, nidData, firstSym = struct.unpack(
            "<IHHBBHII", e[:20])
        if size == 0:
            break
        libname = cstr(name_ptr) if name_ptr else "(null)"
        if numFuncs > 0 and nidData != 0:
            for i in range(numFuncs):
                nid = r32(nidData + i * 4)
                stubs[firstSym + i * 8] = (libname, nid)
        pos += size * 4
    return stubs


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        sys.stderr.write("usage: imports.py <prx-elf> <base-hex> [--toml out.toml]\n")
        return 2
    elf = Elf(args[0], base=int(args[1], 16))
    stubs = parse_imports(elf)

    by_lib = {}
    for addr, (lib, nid) in stubs.items():
        by_lib.setdefault(lib, []).append((addr, nid))
    print(f"imports: {len(stubs)} stubs across {len(by_lib)} libraries")
    for lib in sorted(by_lib):
        print(f"  {lib}: {len(by_lib[lib])}")

    out = None
    for a in argv[1:]:
        if a.startswith("--toml"):
            out = a.split("=", 1)[1] if "=" in a else argv[argv.index(a) + 1]
    if out:
        lines = ["# Import map emitted by tools/imports.py: stub address -> (library, NID).", ""]
        for addr in sorted(stubs):
            lib, nid = stubs[addr]
            lines.append("[[import]]")
            lines.append(f"stub = 0x{addr:08x}")
            lines.append(f'lib = "{lib}"')
            lines.append(f"nid = 0x{nid:08x}")
            lines.append("")
        open(out, "w", encoding="ascii", newline="\n").write("\n".join(lines))
        print("wrote", out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
