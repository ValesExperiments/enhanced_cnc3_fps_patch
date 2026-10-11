"""List the DLLs cnc3game.dat imports, to choose a proxy-DLL target.

A proxy DLL only loads if the game imports it by name from its own folder.
dinput8.dll is already claimed by cnc3_fps_patch, so AllianceSwitchTool needs a
different module to coexist with it.
"""
import struct
import sys
from pathlib import Path

BINARY = Path(__file__).parent / "unpacked" / "cnc3game_1.10.dat"

DIRECTORY_IMPORT = 1
DIRECTORY_DELAY_IMPORT = 13


def rva_to_off(rva, sections):
    for _name, va, vsize, praw, psize in sections:
        if va <= rva < va + max(vsize, psize):
            return praw + (rva - va)
    return None


def read_sections(data, pe, opt, opt_size):
    sections = []
    sec_off = opt + opt_size
    nsections = struct.unpack_from("<H", data, pe + 6)[0]
    for i in range(nsections):
        s = sec_off + i * 40
        name = data[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, psize, praw = struct.unpack_from("<IIII", data, s + 8)
        sections.append((name, va, vsize, praw, psize))
    return sections


def cstring(data, off):
    return data[off:data.index(b"\0", off)].decode("ascii", "replace")


def list_descriptors(data, sections, dir_rva, stride, name_field):
    """Walk an import-descriptor array, yielding module names."""
    off = rva_to_off(dir_rva, sections)
    if off is None:
        return
    i = 0
    while True:
        entry = off + i * stride
        fields = struct.unpack_from("<%dI" % (stride // 4), data, entry)
        if not any(fields):
            return
        name_off = rva_to_off(fields[name_field], sections)
        if name_off is None:
            return
        yield cstring(data, name_off)
        i += 1


def main() -> int:
    data = BINARY.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        print("not a PE", file=sys.stderr)
        return 1
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    sections = read_sections(data, pe, opt, opt_size)

    # DataDirectory begins at +96 (PE32) or +112 (PE32+); each entry is 8 bytes.
    dd = opt + (112 if magic == 0x20b else 96)
    imp_rva = struct.unpack_from("<I", data, dd + DIRECTORY_IMPORT * 8)[0]
    delay_rva = struct.unpack_from("<I", data, dd + DIRECTORY_DELAY_IMPORT * 8)[0]

    print(f"PE32{'+' if magic == 0x20b else ''}, {len(sections)} sections\n")

    print("Normal imports:")
    names = list(list_descriptors(data, sections, imp_rva, 20, 3))
    for n in names:
        print(f"  {n}")

    print("\nDelay-load imports:")
    # IMAGE_DELAYLOAD_DESCRIPTOR: Attributes, DllNameRVA, ... -> name at index 1
    delayed = list(list_descriptors(data, sections, delay_rva, 32, 1)) if delay_rva else []
    for n in delayed:
        print(f"  {n}")
    if not delayed:
        print("  (none)")

    print(f"\n{len(names)} normal + {len(delayed)} delay-load")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
