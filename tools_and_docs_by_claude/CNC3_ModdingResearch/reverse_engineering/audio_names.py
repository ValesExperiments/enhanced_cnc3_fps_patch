"""List C&C3 audio asset names from a stream manifest.

C&C3 stores audio in `EnglishAudio.big` as hash-named `.cdata` entries with no
readable names, which is why the sound feature initially looked impractical.

The names are not lost, though — they live in the accompanying `.manifest`,
which is **RefPack compressed** (magic `0x10FB`). Decompressing it yields a
string table of entries like:

    AudioEvent:BuildingCaptured
    AudioEvent:Geva_MissionFailed

RefPack decompression comes from pyBIG, which the user supplied.

Usage
-----
    python audio_names.py <path to EnglishAudio.big> [search regex]

Examples
--------
    python audio_names.py ...\\EnglishAudio.big captur
    python audio_names.py ...\\EnglishAudio.big "MissionFailed"
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# pyBIG ships the EA RefPack implementation.
# Walk up to the project root instead of counting parents - see the same
# note in big_extract.py; this folder has moved once already.
PYBIG = next((d / ".tools_and_docs_by_user" / "pyBIG-master"
              for d in Path(__file__).resolve().parents
              if (d / ".tools_and_docs_by_user").is_dir()),
             Path("pyBIG-master-not-found"))
if PYBIG.is_dir():
    sys.path.insert(0, str(PYBIG))

try:
    from pyBIG import refpack
except ImportError:  # pragma: no cover
    sys.exit(f"pyBIG not found; expected it at {PYBIG}")

sys.path.insert(0, str(Path(__file__).parent))
from big_extract import read_index  # noqa: E402

NAME_RE = re.compile(rb"[A-Za-z][A-Za-z0-9_:]{4,60}")


def manifest_names(big_path: Path) -> list[str]:
    """Every asset name in the archive's manifest(s)."""
    entries, _size = read_index(big_path)
    names: set[str] = set()

    for name, offset, size in entries:
        if not name.lower().endswith(".manifest"):
            continue
        with big_path.open("rb") as fh:
            fh.seek(offset)
            blob = fh.read(size)
        if refpack.has_refpack_header(blob):
            blob = refpack.decompress(blob)
        for m in NAME_RE.finditer(blob):
            names.add(m.group().decode("latin-1"))
    return sorted(names)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    big = Path(sys.argv[1])
    pattern = sys.argv[2] if len(sys.argv) > 2 else None

    names = manifest_names(big)
    if pattern:
        rx = re.compile(pattern, re.I)
        names = [n for n in names if rx.search(n)]

    for n in names[:400]:
        print("  " + n)
    print(f"\n{len(names)} name(s)"
          + (f" matching {pattern!r}" if pattern else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
