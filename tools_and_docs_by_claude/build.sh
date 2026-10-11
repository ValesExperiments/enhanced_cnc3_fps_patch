#!/usr/bin/env sh
# Build the patch with the project-local Zig and GNU make (Git Bash on Windows).
# Usage: tools_and_docs_by_claude/build.sh [make targets...]   (default: release)
# Every cache stays inside the project folder (see CLAUDE.md).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/tools_and_docs_by_claude"
export PATH="$TOOLS/zig/zig-x86_64-windows-0.16.0:$TOOLS/make/bin:$PATH"
export ZIG_GLOBAL_CACHE_DIR="$ROOT/.zig-cache/global"
export ZIG_LOCAL_CACHE_DIR="$ROOT/.zig-cache/local"
cd "$ROOT"
if [ "$#" -eq 0 ]; then set -- release; fi
exec make PYTHON=python "$@"
