"""Locate the ownership-transfer cases inside ScriptActions::executeAction.

Reasoning
---------
In the script-action registration function, the template entries for
PLAYER_TRANSFER_OWNERSHIP_PLAYER and NAMED_TRANSFER_OWNERSHIP_PLAYER sit at
struct offsets 0x2bbc and 0x2c3c. The entry stride is 0x80, so the two actions
have *adjacent* enum values.

Both actions transfer ownership -- one for a single named object, one for every
object of a player -- so both should reach the same underlying routine.

So: parse the decompiled switch, and look for consecutive case labels whose
bodies share a called function. That pair is the strong candidate, and the
shared callee is the transfer routine we want.
"""

from __future__ import annotations

import collections
import re
from pathlib import Path

SOURCE = Path(__file__).parent / "notes" / "execute_action.c"

CASE_RE = re.compile(r"^\s*case (0x[0-9a-fA-F]+|\d+):", re.M)
CALL_RE = re.compile(r"\b(FUN_[0-9a-f]{8})\b")


def parse_cases(text: str) -> dict[int, str]:
    """Map case value -> body text (up to the next case label)."""
    matches = list(CASE_RE.finditer(text))
    cases: dict[int, str] = {}
    for i, m in enumerate(matches):
        value = int(m.group(1), 0)
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        # Several labels can share one body; keep the longest body seen.
        body = text[m.end():end]
        if value not in cases or len(body) > len(cases[value]):
            cases[value] = body
    return cases


def main() -> int:
    if not SOURCE.exists():
        print(f"missing {SOURCE}")
        return 1
    text = SOURCE.read_text(encoding="utf-8", errors="replace")
    cases = parse_cases(text)
    print(f"parsed {len(cases)} case labels\n")

    calls = {v: set(CALL_RE.findall(body)) for v, body in cases.items()}

    # How often is each function called across all cases? A routine shared by
    # only a couple of cases is far more interesting than a ubiquitous helper.
    freq: collections.Counter = collections.Counter()
    for s in calls.values():
        for fn in s:
            freq[fn] += 1

    values = sorted(cases)
    print("Adjacent case pairs sharing a rarely-used callee:")
    print(f"{'cases':>13}  {'shared callee':<14} {'used by N cases':>15}")
    print("-" * 50)

    hits = []
    for a, b in zip(values, values[1:]):
        if b != a + 1:
            continue
        shared = calls[a] & calls[b]
        for fn in shared:
            # Ignore helpers smeared across the whole switch.
            if freq[fn] <= 6:
                hits.append((freq[fn], a, b, fn))

    hits.sort()
    for n, a, b, fn in hits[:40]:
        print(f"{a:>5}/{b:<7}  {fn:<14} {n:>15}")

    if not hits:
        print("(no adjacent pair shares a rare callee)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
