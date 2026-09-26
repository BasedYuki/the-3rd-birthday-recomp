"""Step 0 inspection of a PSP UMD ISO.

Prints the ISO hashes, PARAM.SFO fields, the state of EBOOT.BIN / BOOT.BIN
(encrypted ~PSP vs plain ELF), and every PRX module on the disc.

Usage:
    python tools/iso_info.py <game.iso> [--extract DIR]

--extract copies PARAM.SFO, EBOOT.BIN, BOOT.BIN and all .prx files into DIR
(keep DIR out of git: it contains Square Enix code).
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

SECTOR = 2048


class Iso:
    def __init__(self, path):
        self.f = open(path, "rb")

    def read(self, lba, size):
        self.f.seek(lba * SECTOR)
        return self.f.read(size)

    def walk(self):
        """Yield (path, lba, size) for every file on the disc."""
        pvd = self.read(16, SECTOR)
        if pvd[1:6] != b"CD001":
            sys.exit("not an ISO9660 image")
        root = pvd[156:156 + 34]
        yield from self._walk_dir(*struct.unpack_from("<I", root, 2),
                                  struct.unpack_from("<I", root, 10)[0], "")

    def _walk_dir(self, lba, size, prefix):
        data = self.read(lba, size)
        pos = 0
        while pos < len(data):
            rec_len = data[pos]
            if rec_len == 0:
                # records never cross sector boundaries; skip to next sector
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            ext_lba, = struct.unpack_from("<I", data, pos + 2)
            ext_size, = struct.unpack_from("<I", data, pos + 10)
            flags = data[pos + 25]
            name_len = data[pos + 32]
            name = data[pos + 33:pos + 33 + name_len]
            pos += rec_len
            if name in (b"\x00", b"\x01"):
                continue
            name = name.decode("ascii", "replace").split(";")[0]
            path = f"{prefix}/{name}"
            if flags & 2:
                yield from self._walk_dir(ext_lba, ext_size, path)
            else:
                yield path, ext_lba, ext_size


def parse_sfo(data):
    if data[:4] != b"\x00PSF":
        return {}
    key_tab, data_tab, count = struct.unpack_from("<III", data, 8)
    out = {}
    for i in range(count):
        key_off, fmt, length, _max, data_off = struct.unpack_from("<HHIII", data, 0x14 + i * 16)
        key = data[key_tab + key_off:].split(b"\x00", 1)[0].decode()
        raw = data[data_tab + data_off:data_tab + data_off + length]
        if fmt == 0x0404:
            out[key] = struct.unpack("<I", raw[:4])[0]
        else:
            out[key] = raw.split(b"\x00", 1)[0].decode("utf-8", "replace")
    return out


def describe_module(data):
    if data[:4] == b"~PSP":
        name = data[0xA:0xA + 28].split(b"\x00", 1)[0].decode("ascii", "replace")
        elf_size, psp_size = struct.unpack_from("<II", data, 0x28)
        tag, = struct.unpack_from("<I", data, 0xD0)
        return f"ENCRYPTED ~PSP  module={name!r} elf_size={elf_size} tag=0x{tag:08X}"
    if data[:4] == b"\x7fELF":
        return "plain ELF (already decrypted)"
    if not data.strip(b"\x00"):
        return "all zeros (dummy)"
    return f"unknown header {data[:8].hex()}"


def hash_file(path):
    sha, md5 = hashlib.sha256(), hashlib.md5()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 22):
            sha.update(chunk)
            md5.update(chunk)
    return sha.hexdigest(), md5.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("--extract")
    args = ap.parse_args()

    iso_path = Path(args.iso)
    sha, md5 = hash_file(iso_path)
    print(f"ISO     {iso_path.name}")
    print(f"size    {iso_path.stat().st_size}")
    print(f"sha256  {sha}")
    print(f"md5     {md5}\n")

    iso = Iso(iso_path)
    files = list(iso.walk())
    by_path = {p.upper(): (p, lba, size) for p, lba, size in files}

    sfo = by_path.get("/PSP_GAME/PARAM.SFO")
    if sfo:
        print("PARAM.SFO")
        for k, v in parse_sfo(iso.read(sfo[1], sfo[2])).items():
            print(f"  {k:<18} {v}")
        print()

    wanted = []
    for key in ("/PSP_GAME/SYSDIR/EBOOT.BIN", "/PSP_GAME/SYSDIR/BOOT.BIN"):
        if key in by_path:
            wanted.append(by_path[key])
    wanted += [f for f in files if f[0].upper().endswith(".PRX")]

    print("Executables / modules")
    for path, lba, size in wanted:
        head = iso.read(lba, min(size, 0x100))
        if path.upper().endswith("BOOT.BIN") and not head.strip(b"\x00"):
            full = iso.read(lba, size)
            desc = describe_module(full)
        else:
            desc = describe_module(head)
        print(f"  {path:<45} {size:>10}  {desc}")

    print(f"\n{len(files)} files on disc")

    if args.extract:
        out = Path(args.extract)
        for path, lba, size in ([sfo] if sfo else []) + wanted:
            dest = out / path.lstrip("/")
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(iso.read(lba, size))
        print(f"extracted {len(wanted) + bool(sfo)} files to {out}")


if __name__ == "__main__":
    main()
