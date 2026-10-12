"""Summarise fps_profile.log (src_multicore profiler) per function.

Usage:
    python analyze.py fps_profile.log [--map functions.tsv] [--windows 3-7] [--top 40]

functions.tsv: one function per line, "entry_rva<TAB>end_rva<TAB>name" (hex RVAs),
exported from Ghidra (see export_function_map() below, run through the Ghidra MCP
`run_python` tool). Without a map, raw RVAs are shown.
"""
import argparse
import bisect
import collections
import re

ZONES = ("client", "logic", "effects")


def parse(path):
    windows = []
    current = None
    header = ""
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\r\n")
        if line.startswith("fps_profile"):
            header = line
            continue
        m = re.match(r"=== window (\d+) t=(\d+)s length=([\d.]+)s samples=(\d+) failed=(\d+) "
                     r"frames=(\d+) \(([\d.]+) FPS\) rate=(-?\d+) ===", line)
        if m:
            current = {
                "index": int(m.group(1)), "t": int(m.group(2)), "length": float(m.group(3)),
                "samples": int(m.group(4)), "fps": float(m.group(7)), "rate": int(m.group(8)),
                "zones": {}, "modules": [], "threads": [],
                "excl": {z: collections.Counter() for z in ZONES},
                "incl": collections.Counter(), "ext": collections.Counter(),
            }
            windows.append(current)
            continue
        if current is None:
            continue
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "zone":
            fields = dict(p.split("=") for p in parts[2:])
            current["zones"][parts[1]] = {k: int(v) for k, v in fields.items()}
        elif parts[0] == "module":
            fields = dict(p.split("=") for p in parts[2:])
            current["modules"].append((parts[1], {k: int(v) for k, v in fields.items()}))
        elif parts[0] == "thread":
            current["threads"].append(line)
        elif parts[0].startswith("excl_") and parts[1] != "dropped":
            current["excl"][parts[0][5:]][int(parts[1], 16)] += int(parts[2])
        elif parts[0] == "incl" and parts[1] != "dropped":
            current["incl"][int(parts[1], 16)] += int(parts[2])
        elif parts[0] == "ext" and parts[1] != "dropped":
            current["ext"][int(parts[1], 16)] += int(parts[2])
    return header, windows


class FunctionMap:
    def __init__(self, path):
        self.starts, self.ends, self.names = [], [], []
        if path:
            rows = []
            for line in open(path, encoding="utf-8"):
                entry, end, name = line.rstrip("\n").split("\t")
                rows.append((int(entry, 16), int(end, 16), name))
            rows.sort()
            for entry, end, name in rows:
                self.starts.append(entry)
                self.ends.append(end)
                self.names.append(name)

    def name(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and rva <= self.ends[i]:
            return "%s (0x%X)" % (self.names[i], self.starts[i])
        return "0x%06X" % rva


def by_function(counter, fmap):
    totals = collections.Counter()
    for rva, count in counter.items():
        totals[fmap.name(rva)] += count
    return totals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--map")
    ap.add_argument("--windows", help="e.g. 3-7 (inclusive)")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()

    header, windows = parse(args.log)
    print(header)
    print("%-7s %6s %7s %7s %7s %7s %7s" % ("window", "t(s)", "FPS", "samples", "client", "logic",
                                             "effects"))
    for w in windows:
        z = w["zones"]
        print("%-7d %6d %7.1f %7d %6.0f%% %6.0f%% %6.0f%%" % (
            w["index"], w["t"], w["fps"], w["samples"],
            *(100.0 * z.get(n, {}).get("samples", 0) / max(1, w["samples"]) for n in ZONES)))

    if args.windows:
        lo, hi = (int(x) for x in args.windows.split("-"))
        windows = [w for w in windows if lo <= w["index"] <= hi]
    if not windows:
        return
    fmap = FunctionMap(args.map)
    total = sum(w["samples"] for w in windows)
    print("\nSelected windows: %s, %d samples" % ([w["index"] for w in windows], total))

    modules = collections.Counter()
    for w in windows:
        for name, fields in w["modules"]:
            modules[name] += sum(fields.values())
    print("\nOutside the game image:")
    for name, count in modules.most_common(12):
        print("  %5.1f%%  %s" % (100.0 * count / total, name))

    for zone in ZONES:
        excl = collections.Counter()
        for w in windows:
            excl.update(w["excl"][zone])
        print("\nExclusive, zone %s (share of all samples):" % zone)
        for name, count in by_function(excl, fmap).most_common(args.top):
            print("  %5.1f%%  %s" % (100.0 * count / total, name))

    incl = collections.Counter()
    for w in windows:
        incl.update(w["incl"])
    # One sample can hold several addresses of the same function; this is an upper bound.
    print("\nInclusive (function on the stack; upper bound):")
    for name, count in by_function(incl, fmap).most_common(args.top * 2):
        print("  %5.1f%%  %s" % (100.0 * count / total, name))

    ext = collections.Counter()
    for w in windows:
        ext.update(w["ext"])
    print("\nGame call sites leading into other modules:")
    for name, count in by_function(ext, fmap).most_common(args.top):
        print("  %5.1f%%  %s" % (100.0 * count / total, name))


# Run inside the Ghidra MCP `run_python` tool to write functions.tsv for the
# current program (RVAs relative to the image base):
#
# fm = currentProgram.getFunctionManager(); base = currentProgram.getImageBase().getOffset()
# out = open(r"<path>\\functions_<build>.tsv", "w")
# for f in fm.getFunctions(True):
#     body = f.getBody()
#     out.write("%X\t%X\t%s\n" % (f.getEntryPoint().getOffset() - base,
#               body.getMaxAddress().getOffset() - base, f.getName()))
# out.close()

if __name__ == "__main__":
    main()
