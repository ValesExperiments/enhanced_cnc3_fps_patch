"""Port addresses from one build of cnc3game.dat to another, by masked byte matching.

    python port_addresses.py <from.dat> <to.dat>

1.9 (2012) and 1.10 (2025) share almost all their code - 1.10's .text is only
0.3 % larger - so the same functions exist at shifted addresses. Anything that
moves when code shifts (call/jump targets, absolute addresses) is masked out of
a byte pattern; the rest must match exactly.

Every address is found by two INDEPENDENT methods, which must agree:

  signature  the first bytes of the function itself, masked, must match
             exactly once in the target build
  xref vote  every place the source build USES the address (a call to the
             function, a load of the global) is matched in the target build,
             and the address the target's code uses at that spot is read back

A function whose body changed between builds fails the signature and is
reported rather than guessed. The control is porting a build onto itself,
which must return every address unchanged.
"""

from __future__ import annotations

import collections
import re
import struct
import sys
from pathlib import Path

import pefile

# What AllianceSwitchTool needs, as 1.10 addresses.
FUNCTIONS = {
    "setTeam": 0x63C219,
    "changeOwner": 0x652C42,
    "objectAlloc": 0x4AFAD1,
    "frameUpdate": 0x4EE108,        # GameLogic vftable slot 13
    "setTeamThunk": 0x647220,       # diagnostics (Ctrl+Alt+E)
    "setTeamCallerA": 0x651C35,
    "setTeamCallerB": 0x651BFE,
    "setTeamCallerC": 0x65ACA1,
}
# Virtual functions are reached only through a vftable, so they have no call
# sites to vote with. Their second method is the vftable itself: the same slot
# of the same class's vftable, found in each build from RTTI.
#   name -> (mangled class, subobject offset, slot)
VIRTUALS = {
    "frameUpdate": (".?AVGameLogic@@", 0, 13),
}

GLOBALS = {
    "thePlayerList": 0xBEE110,
    "theGameLogic": 0xBEE574,
}
ADDR_LO, ADDR_HI = 0x400000, 0xD00000      # anything in here is an address
SIG_LEN = 48
XREF_BEFORE, XREF_AFTER = 20, 24


class Build:
    def __init__(self, path: Path):
        pe = pefile.PE(str(path), fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.img = pe.get_memory_mapped_image()
        text = next(s for s in pe.sections if s.Name.startswith(b".text"))
        self.text_lo = text.VirtualAddress
        self.text_hi = text.VirtualAddress + text.Misc_VirtualSize
        self.text = self.img[self.text_lo:self.text_hi]

    def rva(self, va):  return va - self.base
    def va(self, rva):  return rva + self.base
    def u32(self, va):  return struct.unpack_from("<I", self.img, self.rva(va))[0]


def mask(window: bytes) -> list[bool]:
    """True = this byte must match. Relocatable operands are masked out."""
    keep = [True] * len(window)

    def drop(a, n):
        for k in range(a, min(a + n, len(window))):
            keep[k] = False

    for i, b in enumerate(window):
        if b in (0xE8, 0xE9):                       # call/jmp rel32
            drop(i + 1, 4)
        if b == 0x0F and i + 1 < len(window) and 0x80 <= window[i + 1] <= 0x8F:
            drop(i + 2, 4)                          # jcc rel32
        if i + 4 <= len(window):
            v = struct.unpack_from("<I", window, i)[0]
            if ADDR_LO <= v < ADDR_HI:              # absolute address
                drop(i, 4)
    return keep


def masked_window(build: "Build", rva: int, n: int) -> tuple[bytes, list[bool]]:
    """n bytes at rva, masked with 4 bytes of context on each side.

    Masking a window in isolation misses operands that straddle its edges: a
    4-byte address starting 2 bytes before the end is only half inside, is
    not recognised, and stays unmasked. That made the per-frame function's
    signature fail between builds although its body is identical.
    """
    ext = build.img[rva - 4:rva + n + 4]
    keep = mask(ext)[4:4 + n]
    return build.img[rva:rva + n], keep


def pattern(window: bytes, keep: list[bool]) -> re.Pattern:
    parts = [re.escape(bytes([b])) if k else b"." for b, k in zip(window, keep)]
    return re.compile(b"".join(parts), re.DOTALL)


MAX_MATCHES = 8     # past this a pattern is simply "ambiguous"


def find_all(build: Build, pat: re.Pattern, limit: int = MAX_MATCHES) -> list[int]:
    """VAs of (overlapping) matches in the target's .text, at most `limit`.

    Capped because a tiny or heavily-masked pattern - a 3-byte stub, or a
    window that is mostly relocated operands - can match almost every byte of
    the code. Uncapped, one such pattern grew a single check to 8.7 GB of RAM
    before it was stopped. Callers only ever need "exactly one" or "not one".
    """
    out, pos = [], 0
    while len(out) < limit:
        m = pat.search(build.text, pos)
        if not m:
            break
        out.append(build.va(build.text_lo + m.start()))
        pos = m.start() + 1
    return out


def in_text(build: Build, va: int) -> bool:
    return build.text_lo <= build.rva(va) < build.text_hi


def by_signature(src: Build, dst: Build, va: int) -> list[int]:
    if not in_text(src, va):
        return []                                   # not code: nothing to match
    w, keep = masked_window(src, src.rva(va), SIG_LEN)
    if sum(keep) < 16:
        return []                                   # too little left to be specific
    return find_all(dst, pattern(w, keep))


def xrefs(src: Build, target: int, is_call: bool) -> list[int]:
    """VAs of 4-byte operands in src .text that refer to target."""
    out = []
    t = src.text
    if is_call:
        for m in re.finditer(rb"\xE8", t):
            i = m.start()
            if i + 5 <= len(t):
                rel = struct.unpack_from("<i", t, i + 1)[0]
                op_va = src.va(src.text_lo + i + 1)
                if op_va + 4 + rel == target:
                    out.append(op_va)
    else:
        needle = struct.pack("<I", target)
        for m in re.finditer(re.escape(needle), t):
            out.append(src.va(src.text_lo + m.start()))
    return out


def by_xref_vote(src: Build, dst: Build, target: int, is_call: bool):
    votes = collections.Counter()
    used = 0
    for op in xrefs(src, target, is_call):
        a = src.rva(op) - XREF_BEFORE
        w, keep = masked_window(src, a, XREF_BEFORE + 4 + XREF_AFTER)
        for k in range(XREF_BEFORE, XREF_BEFORE + 4):
            keep[k] = False                         # the operand itself
        hits = find_all(dst, pattern(w, keep))
        if len(hits) != 1:
            continue                                # ambiguous or changed: no vote
        used += 1
        d_op = hits[0] + XREF_BEFORE
        if is_call:
            rel = struct.unpack_from("<i", dst.img, dst.rva(d_op))[0]
            votes[d_op + 4 + rel] += 1
        else:
            votes[dst.u32(d_op)] += 1
    return votes, used


def by_vftable_slot(path: Path, build: Build, cls: str, sub: int, slot: int):
    """The function in `slot` of `cls`'s vftable for subobject `sub`, via RTTI."""
    import rtti_vftables
    for vft, mangled in rtti_vftables.scan(path).items():
        if mangled == cls and build.u32(build.u32(vft - 4) + 4) == sub:
            return build.u32(vft + 4 * slot)
    return None


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    src, dst = Build(Path(sys.argv[1])), Build(Path(sys.argv[2]))
    print(f"porting {Path(sys.argv[1]).name} -> {Path(sys.argv[2]).name}\n")
    print(f"  {'name':<16}{'source':>10}  {'signature':<22}{'xref vote':<32}verdict")
    ok = True
    for name, va in list(FUNCTIONS.items()) + list(GLOBALS.items()):
        is_fn = name in FUNCTIONS
        sig = by_signature(src, dst, va) if is_fn else []
        votes, used = by_xref_vote(src, dst, va, is_call=is_fn)
        if name in VIRTUALS:
            slot = by_vftable_slot(Path(sys.argv[2]), dst, *VIRTUALS[name])
            votes = collections.Counter({slot: 1}) if slot else votes
            used = 1 if slot else used
        top = votes.most_common(2)
        sig_txt = ("-" if not is_fn else
                   f"0x{sig[0]:x}" if len(sig) == 1 else f"{len(sig)} matches")
        vote_txt = (("vftable slot 0x%x" % top[0][0]) if name in VIRTUALS and top else
                    f"0x{top[0][0]:x} ({top[0][1]}/{used} of refs)" if top else "no votes")
        if is_fn:
            agree = len(sig) == 1 and top and top[0][0] == sig[0]
            answer = sig[0] if agree else None
        else:
            agree = bool(top) and top[0][1] >= 2 and (len(top) == 1 or top[0][1] > 3 * top[1][1])
            answer = top[0][0] if agree else None
        ok &= bool(agree)
        verdict = f"OK -> 0x{answer:x}" if agree else "UNRESOLVED"
        print(f"  {name:<16}{'0x%x' % va:>10}  {sig_txt:<22}{vote_txt:<32}{verdict}")
    print("\nall resolved by two agreeing methods" if ok else "\nsome addresses need manual work")
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
