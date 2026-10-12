#!/usr/bin/env sh
# Build the offline resolver check and run it on the given game executables.
# Usage: tools_and_docs_by_claude/layout_check/run.sh <cnc3game.dat|cnc3ep1.dat> [...]
# Pass Windows-style paths (e.g. via `cygpath -w`) when calling from Git Bash.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ZIG="$ROOT/tools_and_docs_by_claude/zig/zig-x86_64-windows-0.16.0/zig.exe"
OUT="$ROOT/temp/layout_check"
export ZIG_GLOBAL_CACHE_DIR="$ROOT/.zig-cache/global"
export ZIG_LOCAL_CACHE_DIR="$ROOT/.zig-cache/local"
mkdir -p "$OUT"
# 64-bit host with high-entropy ASLR keeps 0x00400000 free for the game image.
"$ZIG" cc -target x86_64-windows-gnu -std=c11 -O1 -Wall -I"$ROOT/src_fps_patch" \
    -Wl,--high-entropy-va -Wl,--dynamicbase \
    "$ROOT/tools_and_docs_by_claude/layout_check/layout_check.c" \
    -lkernel32 -o "$OUT/layout_check.exe"
exec "$OUT/layout_check.exe" "$@"
