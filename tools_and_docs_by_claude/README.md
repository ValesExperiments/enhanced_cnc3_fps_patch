# tools_and_docs_by_claude

Tools Claude set up for this project. Everything runs from inside the project
folder; nothing is installed system-wide.

| Path | Tracked in git? | What it is |
|---|---|---|
| `build.sh` | yes | Builds the patch with the local toolchain. `sh tools_and_docs_by_claude/build.sh [release\|debug\|clean\|package]` |
| `zig/` | no | Zig 0.16.0 (same as upstream CI) |
| `make/` | no | GNU make 4.4.1 (ezwinports build) |
| `venv/` | no | Python 3.14 venv: JPype 1.7.1, PyGhidra 3.1.0, MCP SDK, meson, ninja |
| `layout_check/` | yes | Offline check that the patch resolves all its hook sites in a given game executable |
| `ghidra_tools/` | yes | Headless Ghidra helpers and the Ghidra MCP server |
| `dxvk_cursorlock/` | partly | DXVK 3.1.1 `d3d9.dll` with a fullscreen cursor lock: patch, build script, README (source copy and output are gitignored) |
| `gitignored/dxvk/` | no | Upstream DXVK v3.1.1 reference clone (read-only; fetch/pull only) |
| `ghidra_data/` | no | Copied game executables, the Ghidra project `projects/CnC3.gpr`, and Ghidra settings/cache |

## Offline resolver check

```sh
sh tools_and_docs_by_claude/layout_check/run.sh "$(cygpath -w "<path to cnc3game.dat>")" ...
```

It maps each executable at 0x00400000 inside a 64-bit host and runs every
stage of `resolve_game_layout`. A 32-bit host can't do this, because its own
startup mappings sit inside the game's address range.

## Ghidra

- Ghidra 12.1.3 is the user's system install
  (`C:\Users\Kellan\UsefulSSD\ToolsSSD\ghidra_12.1.3_PUBLIC`), driven
  headless through PyGhidra.
- `ghidra_tools/ghidra_env.py` starts Ghidra with settings, cache, temp
  files and Java detection all kept inside the project (no `%APPDATA%`
  writes).
- `ghidra_tools/import_analyze.py [program...]` imports
  `ghidra_data/binaries/*` into `ghidra_data/projects/CnC3.gpr` and runs
  auto-analysis. Safe to rerun.
  - Programs: `TW_1.10_cnc3game.dat`, `KW_1.03_cnc3ep1.dat` (copies of the
    game's executables).
- `ghidra_tools/ghidra_mcp_server.py` is a stdio MCP server, registered as
  `ghidra` in the repo's `.mcp.json`. Its tools: `list_programs`,
  `program_info`, `function_info`, `decompile`, `disassemble`, `xrefs_to`,
  `xrefs_from`, `callers`, `callees`, `search_functions`, `search_symbols`,
  `search_strings`, `search_bytes`, `search_instructions`, `read_memory`,
  `rename_function`, `create_label`, `set_comment`, `save_program`, and
  `run_python` (an escape hatch).
- **Only one process may hold the Ghidra project.** Close the Ghidra GUI on
  `CnC3.gpr` (and don't run `import_analyze.py`) while the MCP server is in
  use, and the other way round.
- The project can also be opened in the Ghidra GUI. It's a normal project:
  File → Open Project → `ghidra_data/projects/CnC3.gpr`.

### Version note
PyGhidra 3.1.0 pins `JPype1==1.5.2`, which has no Python 3.14 wheel.
PyGhidra was therefore installed with `--no-deps` on JPype 1.7.1, which has
3.14 wheels and works with Ghidra 12.1.3. If something JPype-related breaks,
the fallback is a project-local Python 3.13 with JPype 1.5.2.
