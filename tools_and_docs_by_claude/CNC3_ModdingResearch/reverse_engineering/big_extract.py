"""List and extract files from a C&C BIG archive.

Used to pull the game's own sound effects out of EnglishAudio.big so the tool
can play them, rather than calling the engine's audio subsystem. Playing a
file is zero risk; invoking engine functions for a cosmetic feature is not.

BIGF layout (mixed endianness, which is the usual trap):
    0x00  'BIGF'
    0x04  archive size          (little endian)
    0x08  entry count           (BIG endian)
    0x0C  header size           (BIG endian)
    0x10  entries: offset(BE), size(BE), NUL-terminated name

RefPack (important)
    Some entries are stored **RefPack compressed** (magic `0x10FB`) - in
    EnglishAudio.big that is `.manifest`, `.bin` and `.relo`, the exact set
    `audiodump.exe` reads. Writing them out as stored produces a dump that
    looks complete but makes those tools parse a compressed blob, which is
    what crashed audiodump on the first attempt here. Extraction therefore
    decompresses them by default; pass `--raw` to keep the stored bytes.
    All four RefPack header forms are handled (10/11/90/91 FB) - large
    entries such as static_common.bin use 90 FB.

Usage:
    python big_extract.py <archive.big> --list [pattern]
    python big_extract.py <archive.big> --extract <name> <outfile> [--raw]
    python big_extract.py <archive.big> --extract-all <outdir> [--raw]
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path


def read_index(path: Path) -> tuple[list[tuple[str, int, int]], int]:
    """Return [(name, offset, size)] and the archive size."""
    with path.open("rb") as fh:
        head = fh.read(16)
        # C&C3 uses 'BIG4'; older titles use 'BIGF'. Same layout.
        if head[:4] not in (b"BIGF", b"BIG4"):
            raise SystemExit(f"{path.name}: not a BIG archive (got {head[:4]!r})")
        archive_size = struct.unpack_from("<I", head, 4)[0]
        count, _header_size = struct.unpack_from(">II", head, 8)

        entries = []
        # The index is a contiguous run of records right after the header.
        blob = fh.read(min(64 * 1024 * 1024, archive_size))
        pos = 0
        for _ in range(count):
            if pos + 8 > len(blob):
                break
            offset, size = struct.unpack_from(">II", blob, pos)
            pos += 8
            end = blob.find(b"\0", pos)
            if end < 0:
                break
            name = blob[pos:end].decode("latin-1")
            pos = end + 1
            entries.append((name, offset, size))
    return entries, archive_size


# pyBIG ships the EA RefPack decoder; extraction still works without it.
#
# Found by walking UP to the project root rather than counting parents: a
# fixed parents[2] broke when this folder moved one level deeper into
# .tools_and_docs_by_claude/, and the toolset layout is expected to change
# again.
_PYBIG = next((d / ".tools_and_docs_by_user" / "pyBIG-master"
               for d in Path(__file__).resolve().parents
               if (d / ".tools_and_docs_by_user").is_dir()),
              Path("pyBIG-master-not-found"))
if _PYBIG.is_dir():
    sys.path.insert(0, str(_PYBIG))
try:
    from pyBIG import refpack
except ImportError:  # pragma: no cover
    refpack = None


REFPACK_MAGIC = bytes((0x10, 0xFB))   # the common form; see refpack_header()


def refpack_header(data: bytes) -> tuple[int, int] | None:
    """(header length, declared uncompressed size), or None if not RefPack.

    The first byte is a flags byte, the second always 0xFB:
        0x80  sizes are 4 bytes instead of 3 (entries of 16 MB and up)
        0x01  a compressed-size field precedes the uncompressed size
    so there are four forms: 10 FB, 11 FB, 90 FB, 91 FB. pyBIG only strips
    the 10 FB header; the others used to come out still compressed - e.g.
    StaticStream.big's 148 MB static_common.bin, which is 90 FB.
    """
    if len(data) < 5 or data[1] != 0xFB or data[0] not in (0x10, 0x11, 0x90, 0x91):
        return None
    size_len = 4 if data[0] & 0x80 else 3
    fields = 2 if data[0] & 0x01 else 1
    header = 2 + size_len * fields
    if len(data) < header:
        return None
    declared = int.from_bytes(data[header - size_len:header], "big")
    return header, declared


def maybe_decompress(data: bytes, raw: bool) -> tuple[bytes, bool]:
    """Decompress a RefPack entry. Returns (bytes, was_decompressed)."""
    head = None if raw or refpack is None else refpack_header(data)
    if head is None:
        return data, False
    header, declared = head
    try:
        out = bytes(refpack.decompress(data[header:], ignore_mismatch=True))
    except Exception:
        return data, False
    if len(out) != declared:
        # A stored entry that merely starts with RefPack-looking bytes; keep it.
        return data, False
    return out, True


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    raw = "--raw" in sys.argv
    argv = [a for a in sys.argv if a != "--raw"]
    archive = Path(argv[1])
    mode = argv[2]
    entries, archive_size = read_index(archive)

    if mode == "--list":
        pattern = argv[3] if len(argv) > 3 else None
        rx = re.compile(pattern, re.I) if pattern else None
        shown = 0
        for name, offset, size in entries:
            if rx and not rx.search(name):
                continue
            print(f"  {size:>9}  {name}")
            shown += 1
            if shown >= 200:
                print("  ... (truncated)")
                break
        print(f"\n{shown} shown of {len(entries)} entries "
              f"({archive_size / 1e6:.0f} MB archive)")
        return 0

    if mode == "--extract-all":
        # audiodump.exe expects a manifest with a sibling cdata\ folder, so
        # unpack preserving the archive's own paths.
        outdir = Path(argv[3])
        written = unpacked = 0
        with archive.open("rb") as fh:
            for name, offset, size in entries:
                dest = outdir / name.replace("\\", "/")
                dest.parent.mkdir(parents=True, exist_ok=True)
                fh.seek(offset)
                data, did = maybe_decompress(fh.read(size), raw)
                dest.write_bytes(data)
                written += 1
                unpacked += did
                if written % 500 == 0:
                    print(f"  {written}/{len(entries)}...")
        print(f"extracted {written} file(s) -> {outdir}")
        if unpacked:
            print(f"  {unpacked} RefPack entry/entries decompressed")
        elif not raw and refpack is None:
            print("  WARNING: pyBIG not found, RefPack entries left compressed")
        return 0

    if mode == "--extract":
        wanted, out = argv[3], Path(argv[4])
        for name, offset, size in entries:
            if name.lower() == wanted.lower():
                with archive.open("rb") as fh:
                    fh.seek(offset)
                    data = fh.read(size)
                data, did = maybe_decompress(data, raw)
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(data)
                note = f", RefPack -> {len(data)} bytes" if did else ""
                print(f"extracted {name} ({size} bytes{note}) -> {out}")
                print(f"  first bytes: {data[:16].hex()}  {data[:4]!r}")
                return 0
        print(f"not found: {wanted}")
        return 1

    print(__doc__)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
