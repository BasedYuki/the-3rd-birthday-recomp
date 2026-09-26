"""Split one huge generated recomp C file into many translation units.

A single 150+ MB C file needs more RAM than a 16 GB machine has when gcc
compiles it (cc1 passed 6.7 GB at -O1 before we stopped it). Splitting it
lets the pieces compile in parallel with bounded memory.

Layout expected (AljandrOrtega codegen.py output):
    preamble      includes, static helpers, forward declarations
    bodies        top-level `void f_XXXXXXXX(CpuState *s) {` definitions
    tail          anything after the bodies (e.g. sr_register_all)

Output:
    <out>/recomp_prelude.h     preamble (static helpers made static inline)
    <out>/recomp_NNN.c         #include prelude + a slice of bodies
    <out>/recomp_tail.c        #include prelude + the tail

Usage:
    python tools/split_recomp.py <recomp.c> <outdir> [--chunk-mb 4]
"""

import argparse
import re
from pathlib import Path

BODY = re.compile(r"^void f_[0-9a-fA-F]+\(CpuState \*s\)\s*\{")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--chunk-mb", type=float, default=4.0)
    args = ap.parse_args()

    lines = Path(args.src).read_text(encoding="utf-8").split("\n")
    first = next(i for i, l in enumerate(lines) if BODY.match(l))

    # Walk top-level definitions by brace depth.
    bodies, tail_start = [], None
    depth, start = 0, first
    for i in range(first, len(lines)):
        line = lines[i]
        if depth == 0 and line.strip():
            if not BODY.match(line):
                tail_start = i
                break
            start = i
        depth += line.count("{") - line.count("}")
        if depth == 0 and line.startswith("}"):
            bodies.append((start, i + 1))
    if depth != 0:
        raise SystemExit(f"unbalanced braces near line {start}")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("recomp_*.c"):
        old.unlink()

    prelude = "\n".join(lines[:first])
    prelude = re.sub(r"^static (?!inline)", "static inline ", prelude, flags=re.M)
    (out / "recomp_prelude.h").write_text(
        "#pragma once\n" + prelude + "\n", encoding="utf-8")

    header = '#include "recomp_prelude.h"\n\n'
    limit = int(args.chunk_mb * 1024 * 1024)
    chunk, size, n = [], 0, 0

    def flush():
        nonlocal chunk, size, n
        if chunk:
            (out / f"recomp_{n:03d}.c").write_text(header + "\n".join(chunk) + "\n",
                                                   encoding="utf-8")
            n += 1
            chunk, size = [], 0

    for s, e in bodies:
        text = "\n".join(lines[s:e])
        chunk.append(text)
        size += len(text)
        if size >= limit:
            flush()
    flush()

    tail = "\n".join(lines[tail_start:]) if tail_start is not None else ""
    (out / "recomp_tail.c").write_text(header + tail + "\n", encoding="utf-8")
    print(f"{len(bodies)} functions -> {n} chunks + tail in {out}")


if __name__ == "__main__":
    main()
