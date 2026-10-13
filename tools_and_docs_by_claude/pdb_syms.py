"""Name the functions containing RVAs of our dinput8.dll from its PDB.

usage: pdb_syms.py <dll> <pdb> <hex rva> [...]
Brute-force scans the PDB for S_GPROC32 (0x1110) / S_LPROC32 (0x110F)
records (zig's linker writes no map file) and converts their
section:offset to RVAs with the DLL's section table.
"""
import struct
import sys

import pefile


def procs(pdb, sections):
    out = []
    for kind in (0x1110, 0x110F):
        pat = struct.pack("<H", kind)
        i = pdb.find(pat, 2)
        while i != -1:
            start = i - 2
            try:
                reclen = struct.unpack_from("<H", pdb, start)[0]
                (_parent, _end, _next, length, _ds, _de, _typ, off, seg) = struct.unpack_from(
                    "<IIIIIIIIH", pdb, i + 2)
                name_at = i + 2 + 34 + 1
                name_end = pdb.index(b"\0", name_at)
                name = pdb[name_at:name_end].decode("ascii")
                if (8 <= reclen <= 0x400 and name_end <= start + 2 + reclen and
                        1 <= seg <= len(sections) and 0 < length < 0x100000 and name.isprintable()):
                    out.append((sections[seg - 1] + off, length, name))
            except (struct.error, ValueError, UnicodeDecodeError):
                pass
            i = pdb.find(pat, i + 1)
    return sorted(set(out))


def main():
    pe = pefile.PE(sys.argv[1], fast_load=True)
    sections = [s.VirtualAddress for s in pe.sections]
    table = procs(open(sys.argv[2], "rb").read(), sections)
    for arg in sys.argv[3:]:
        rva = int(arg, 16)
        hits = [(a, n, name) for a, n, name in table if a <= rva < a + n]
        if hits:
            a, n, name = hits[0]
            print("%X -> %s+0x%X (fn %X, %d bytes)" % (rva, name, rva - a, a, n))
        else:
            print("%X -> ?" % rva)


if __name__ == "__main__":
    main()
