"""Recover every class vftable from an MSVC x86 binary via its RTTI - no Ghidra.

    python rtti_vftables.py <binary> [--json out.json] [--names-from vftables.json]

MSVC x86 layout, read straight from the file:

    vftable[-1]                     -> RTTICompleteObjectLocator (COL)
    COL      { sig=0, offset, cdOffset, TypeDescriptor*, ClassHierarchyDescriptor* }
    TypeDescriptor { type_info vftable*, spare, ".?AVName@@\\0" }

So: find every TypeDescriptor by its ".?AV"/".?AU" name, every COL that points
at one, and every pointer to a COL; the slot after that pointer is a vftable.

Why this exists: Ghidra needed a long import-and-analyse pass to produce
vftables.json for 1.10. Supporting another game version (1.9) needs the same
map for that binary, and reading RTTI directly takes seconds. It is validated
by reproducing the Ghidra-built 1.10 map before being trusted on anything else.

--names-from: MSVC mangled names are reproduced as the demangled names Ghidra
chose for the same class in an existing map, so the output matches it exactly.
"""

from __future__ import annotations

import json
import re
import struct
import sys
from pathlib import Path

import pefile


def load(path: Path):
    pe = pefile.PE(str(path), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    return pe, base, img


def simple_demangle(mangled: str) -> str:
    """'.?AVImpl@Button@Tactical@@' -> 'Tactical::Button::Impl'. Templates kept raw."""
    m = re.match(r"\.\?A[VU](.+?)@@$", mangled)
    if not m or "?$" in mangled:
        return mangled
    return "::".join(reversed(m.group(1).split("@")))


def scan(path: Path) -> dict[int, str]:
    """vftable VA -> mangled class name."""
    pe, base, img = load(path)
    lo, hi = base, base + len(img)

    # 1. TypeDescriptors: name string at +8.
    tds = {}
    for m in re.finditer(rb"\.\?A[VU][^\x00]{1,400}?@@\x00", img):
        td = base + m.start() - 8
        tds[td] = m.group()[:-1].decode("latin-1")

    # 2. COLs: signature 0, TypeDescriptor* in the set, CHD* inside the image.
    cols = {}
    words = memoryview(img).cast("B")
    for off in range(0, len(img) - 20, 4):
        sig, _o, _cd, ptd, pchd = struct.unpack_from("<5I", img, off)
        if sig == 0 and ptd in tds and lo <= pchd < hi:
            cols[base + off] = tds[ptd]

    # 3. vftables: a pointer to a COL; the vftable starts in the next slot.
    #    The first entry must itself point into the image (a function).
    vft = {}
    for off in range(0, len(img) - 8, 4):
        p = struct.unpack_from("<I", img, off)[0]
        if p in cols:
            first = struct.unpack_from("<I", img, off + 4)[0]
            if lo <= first < hi:
                vft[base + off + 4] = cols[p]
    return vft


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = Path(sys.argv[1])
    out = None
    names_from = None
    if "--json" in sys.argv:
        out = Path(sys.argv[sys.argv.index("--json") + 1])
    if "--names-from" in sys.argv:
        names_from = Path(sys.argv[sys.argv.index("--names-from") + 1])

    vft = scan(path)
    print(f"{path.name}: {len(vft)} vftables, "
          f"{len(set(vft.values()))} distinct classes")

    if out:
        rename = {}
        if names_from:
            # mangled -> Ghidra's demangled name, learned from the reference
            # binary's own map (the reference map must match that binary).
            ref_bin = Path(sys.argv[sys.argv.index("--names-from") + 2])
            ref_vft = scan(ref_bin)
            ref_names = {int(k, 16): v for k, v in
                         json.loads(names_from.read_text(encoding="utf-8")).items()}
            for addr, mangled in ref_vft.items():
                if addr in ref_names:
                    rename.setdefault(mangled, ref_names[addr])
        data = {f"0x{a:08x}": rename.get(m, simple_demangle(m))
                for a, m in sorted(vft.items())}
        out.write_text(json.dumps(data, indent=1), encoding="utf-8")
        print(f"wrote {out} ({len(data)} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
