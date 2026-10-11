"""Extract ASCII strings from cnc3game.dat and search them.

Used to locate MSVC RTTI type descriptors (".?AVClassName@@") and other
anchors for the ownership-transfer reverse engineering.

Usage:
    python scan_strings.py <pattern> [pattern ...]
    python scan_strings.py --rtti
"""
import re
import sys
from pathlib import Path

BINARY = Path(__file__).parent / "unpacked" / "cnc3game_1.10.dat"
MIN_LEN = 5


def iter_strings(data: bytes, min_len: int = MIN_LEN):
    """Yield (file_offset, text) for each printable ASCII run."""
    for match in re.finditer(rb"[\x20-\x7e]{%d,}" % min_len, data):
        yield match.start(), match.group().decode("ascii")


def main() -> int:
    if not BINARY.exists():
        print(f"missing binary: {BINARY}", file=sys.stderr)
        return 1
    data = BINARY.read_bytes()
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1

    if args[0] == "--rtti":
        # MSVC mangles RTTI type descriptors as .?AVFoo@@ / .?AUFoo@@
        names = sorted({
            text for _, text in iter_strings(data, 6)
            if text.startswith(".?A")
        })
        print(f"{len(names)} RTTI type descriptors")
        for name in names:
            print(name)
        return 0

    patterns = [re.compile(a, re.IGNORECASE) for a in args]
    for offset, text in iter_strings(data):
        if any(p.search(text) for p in patterns):
            print(f"0x{offset:08x}  {text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
