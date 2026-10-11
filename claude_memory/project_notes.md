# Project notes (Claude's memory for future sessions)

Quick facts that took some digging to find. Keep this file short and accurate.
Fix or remove anything that turns out to be wrong.

## Where things are
- **Game install (read-only!):**
  `D:\SteamLibrary\steamapps\common\Command and Conquer 3 Tiberium Wars`.
  Kane's Wrath is also installed at
  `D:\SteamLibrary\steamapps\common\Command and Conquer 3 - Kane's Wrath`
  (versions `RetailExe\1.2`, `1.3`).
- The active version is **TW 1.10**: `CNC3_english_1.10.SkuDef` →
  `RetailExe\1.10\cnc3game.dat`. The patch DLL (`dinput8.dll` +
  `fps_patch.ini`) goes next to `cnc3game.dat`, not next to `CNC3.exe`. The
  user installs it themselves; we never write into the game folder.
- 1.10 `cnc3game.dat` differs from its `.bak` only by the PE
  Large-Address-Aware flag (1 byte at offset 0x12E). That's harmless.
- TW `1.9\cnc3game.dat` and KW `1.2\cnc3ep1.dat` are the 2012 Steam builds.
  Their bigger `.bak` files are the 2007/2008 retail originals. The patch
  doesn't resolve on those, and nothing launches them.
- The user's own WIP DLSS `d3d9.dll` mod (with a DXVK backend) is in the game
  folders. It has no frame generation and is currently off.
- The user wants Kane's Wrath to keep working. KW differs only slightly from
  TW.

## Git
- `origin` is the user's fork: `ValesExperiments/enhanced_cnc3_fps_patch`.
- The original upstream is `CNCStuff/cnc3_fps_patch`. **Never push there.**

## Build (works since 2026-10-11)
- Run `sh tools_and_docs_by_claude/build.sh` (default target `release`; also
  `debug`, `clean`, `package`). Output: `zig-out/bin/dinput8.dll` plus
  `zig-out/bin/fps_patch.ini`, which is copied from `fps_patch.ini.example`.
  The `build` target copies it, so the bin folder is install-ready.
- Local tools (gitignored): `tools_and_docs_by_claude/zig/` (Zig 0.16.0, the
  same version as upstream CI) and `tools_and_docs_by_claude/make/` (GNU make
  4.4.1 from ezwinports). Python 3.14 is system-wide.
- **Always set `ZIG_GLOBAL_CACHE_DIR`/`ZIG_LOCAL_CACHE_DIR` inside the
  project** (build.sh does this). Otherwise Zig writes to
  `%LOCALAPPDATA%\zig`, which is outside the project and against CLAUDE.md.
  A 72 MB cache was accidentally created there once; Claude wasn't allowed
  to delete it, so the user was told about it.

## Offline resolver check
- `sh tools_and_docs_by_claude/layout_check/run.sh "$(cygpath -w <exe>)" ...`
  maps the game exe at 0x00400000 and runs each `resolve_game_layout` stage.
- It **must be a 64-bit host**. A 32-bit process already has WOW64/stack/heap
  mappings between 0x00700000 and 0x01A00000 at creation (even when
  suspended), so 16-24 MB game images can't be placed at their base. The game
  images have no relocations.

## Ghidra (since 2026-10-11)
- Project: `tools_and_docs_by_claude/ghidra_data/projects/CnC3.gpr` with
  programs `TW_1.10_cnc3game.dat` and `KW_1.03_cnc3ep1.dat`. These are
  gitignored copies of the game executables; image base 0x00400000.
- MCP server `ghidra` (`.mcp.json` → `tools_and_docs_by_claude/ghidra_tools/ghidra_mcp_server.py`).
  It only shows up in a Claude Code session started after `.mcp.json` was
  approved. Without it, write PyGhidra scripts that call
  `ghidra_env.start_ghidra()`.
- Only one process can open the project at a time: the MCP server, the GUI,
  or a script.
- Always start Ghidra via `ghidra_env.start_ghidra()`. It keeps settings,
  cache and temp files in the project and sets `java_home` itself, because
  PyGhidra's default Java detection writes `%APPDATA%\ghidra\...\java_home.save`.
- PyGhidra 3.1.0 is installed `--no-deps` on JPype 1.7.1, because the pinned
  JPype 1.5.2 has no Python 3.14 wheel.
- See `tools_and_docs_by_claude/README.md` for the tool list.

## Useful resources the user provided (2026-10-11)
- `tools_and_docs_by_user/CnC_Modding_Support/Tiberium Wars/` (and `Kanes Wrath/`)
  hold EA's GPL **source XML data, schemas, shaders and map scripts** for the
  game. Grep these for INI/XML field names (locomotor suspension, UI
  settings, FX) instead of digging through `.big` files.
- `tools_and_docs_by_user/pyBIG-master` and `CC-BIG-Extractor` can read `.big`
  archives.
- `tools_and_docs_by_claude/AllianceToolSwitchNotes/` and
  `CNC3_ModdingResearch/` are notes from an earlier CnC3 modding project
  (AllianceSwitchTool): ownership model, RTTI, TW 1.10 addresses, and old
  Ghidra scripts.

## Engine facts learned
- The binary keeps **full MSVC RTTI**, so `search_symbols` on class names
  (`...::vftable`) is the fastest way in.
- **APT (Flash UI) runs on real time, not client frames.** `AptPlayer_Update`
  (TW 1.10 0x44B7BC) passes `timeGetTime` deltas (clamped to 60 ms) to a
  fixed-step accumulator (0x8CC57F) that advances movie frames at the movie's
  rate (~33 ms). Any UI logic that waits for an ActionScript callback
  therefore lags by up to 33 ms at high FPS. `g_aptPlayer` (VA 0xBE6C58) +
  0x1E9 = "force an APT tick before the next APT render".
- On-map objective markers are APT clips managed by
  `TacticalObjectiveIndicatorStageImpl` (B1; see docs/goals_detailed.md).
- **Common pattern:** many client systems guard themselves with
  "if (stored == GameClient->getFrame()) return" (vtable +0x78 on
  `g_theGameClient`, TW 1.10 VA 0xBEAC00). Redirecting that one read to the
  patch's synthetic 30 Hz frame throttles the system to the retail rate. If
  the caller needs a value every frame, add a cache/blend wrapper (B2's
  `calc_physics_xform_interpolated`).
- **Measure, don't guess:** static reasoning about the frame/phase ordering
  went in circles for B4. A one-run in-game trace (`debug_motion_frames` in
  fps_patch.ini → `M` lines in fps_patch.log) found the cause immediately.
  Add a targeted diagnostic early when timing is unclear. Tell the user to
  *edit* an existing INI key, never add a duplicate, because
  GetPrivateProfileInt reads the first occurrence.
- **C++ → APT traffic is deferred** to ~30 Hz APT ticks (pending variable
  table with last-value-wins, queued calls). APT gadgets that forward values
  "only when changed" can lose updates at high FPS (B6 "Unit Name"). Prefer
  re-sending over trusting change checks.
- **The user's display path:** their own DXVK 3.1.1-based DLSS mod
  (`RetailExe/1.10/d3d9.dll`, logs in `RetailExe/1.10/logs/`), exclusive
  fullscreen, FIFO/V-Sync, and DXVK's built-in limiter active. G-Sync doesn't
  engage, so judder at 90 FPS on 160 Hz is expected. Check the DXVK log before
  blaming the patch for pacing issues.
- **DXVK:** the user wants DXVK builds from source (stock DXVK doesn't lock
  the cursor in fullscreen, which breaks edge scrolling). Reference clone:
  `tools_and_docs_by_claude/gitignored/dxvk` (v3.1.1, never modified). Our
  copy, patch and build live in `tools_and_docs_by_claude/dxvk_cursorlock/`
  (MSVC VS 2026 x86 via `build_dxvk.bat`, about 2 min). DXVK's own limiter in
  fullscreen + V-Sync caps at the display refresh rate; that's normal and not
  a pacing bug. "Prefer layered on DXGI swapchain" didn't make G-Sync engage.
  **User preference: borderless, not full-screen exclusive.** Don't enable
  `dxvk.allowFse`. Upstream DXVK disallows FSE by default and notes that FSE is
  required for VRR/HDR on Windows; borderless VRR then depends on the NVIDIA
  driver's layered-DXGI path (independent flip). **Solved:** our build passes
  `VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT` (option `dxvk.fseDriverDefault`,
  default on) instead of DISALLOWED. G-Sync then works borderless with
  windowed G-Sync off (the user keeps it off because a 60 Hz second monitor
  breaks it).
- **Patch-write budget:** `PATCH_TRANSACTION_CAPACITY` (src/memory_patch.h)
  limits how many static writes one install may make (raised from 32 to 64
  on 2026-10-11, when 33 writes silently pushed the game back to 30 FPS).
  layout_check only tests signature resolution, not the install step, so
  check `Static patch writes installed=` in fps_patch.log after adding hooks.
- Hard-coded client-frame counts that assumed 2 client frames per logic tick
  (e.g. B4's `< 6` interpolation windows) must scale to `3 × R`.
- TW and KW differ in some struct offsets (e.g. Object→AI is 0x2E8 in TW and
  0x2F4 in KW). Wildcard such bytes and always run layout_check on all four
  builds: one failing stage disables the whole patch.
- **Watch for `timeGetTime` + truncation patterns.** Code like `elapsed =
  now - stamp; stamp += elapsed; step = ftol(elapsed * rate)` (e.g. the
  shroud fades) is "time-based" but loses the fraction every frame. At high
  FPS small rates truncate to 0 and stall. `FUN_009D97D0` (TW) is the CRT
  `_ftol2` and truncates. Fix: move the stamp back by the unused ms.
- The game's `memcpy` is at RVA 0xA2CE (TW 1.10). Redirecting one specific
  `CALL memcpy` site is a cheap way to substitute what gets uploaded (used
  for the fog texture rows).
- MinGW's `<windows.h>` already defines `CDECL` (empty); use `__cdecl`.
- **"Right speed but still steppy" can be quantisation, not timing.** The
  grey fog of war is an ARGB4444 texture (16 shades), so slow fades step
  visibly at any FPS. Check texture formats (`FUN_0075609A(w, h, D3DFMT, …)`
  in TW) before chasing timing.
- Unit names: Buzzers are **Scrin**, not Nod.
- **Adaptive pacing (F2): never use `g_config->target_fps` for per-frame
  conversions.** Use `g_rate` (the live effective rate). Anything computed
  once from the rate must also be rewritten in `retime_client_rate`, and
  absolute `frame × 30 / rate` mappings must be running counters (see
  `sync_retail_frame`). Frame "work" seen by the CPU includes waiting in
  Present only when the GPU is the bottleneck, so it can't show GPU headroom:
  remember failed rises as a ceiling and probe gently (monitor round 3).
  `g_config->target_fps <= 30` stays only as the
  "patch active" guard. The game's integer rate globals must never go below
  15 (stock code divides by `clientFPS/15`).
- **Replacing a leaf function via a JMP at its entry is not ABI-safe.**
  MSVC callers may keep "volatile" registers (ECX, EDX, EAX, XMM) live across
  calls to leaves that don't touch them (e.g. GameEngine_update relies on
  UpdatePhaseInterpolation keeping ECX). Route such hooks through a naked
  stub that saves what the original leaf preserved.
- **Don't predict the mouse from UI events.** CnC3's APT UI samples the
  cursor some time before its events reach C++ code, so extrapolating from
  live `GetCursorPos` stair-steps. For UI-driven 30 Hz camera moves, ease
  each step (short exponential follow, ~18 ms) instead (B10).
- **Diagnosing a DLL crash:** Windows Application log (event 1000) gives the
  fault offset in DINPUT8.dll; disassemble the shipped DLL at that RVA with
  capstone + pefile (installed in the project venv).
- **Logic scheduling (F1):** `GameEngine_update` (TW 0x54B0CE) does
  `phase++` then dispatches via vtable slot 0x94 (`DispatchLogicPhase`, live
  vtable is `Win32GameEngine`). Wrap to phase 1 = new logic tick (may stall:
  `GameClient+0xD4` cleared, phase reverts). Above ratio 6,
  `IsClientFrameBoundary` is `phase == 1`, so phase 1 must occupy exactly one
  frame and phase 6 must land on the tick's last frame. Simulate scheduler
  changes offline (Python) before shipping.
- **Per-call stepping systems** (TintEnvelope colour flashes, etc.) are
  called once per client frame from `Drawable_updateDrawable` (TW
  0x48353F). Check its callees for other "one step per frame" systems.
- **Check "is it vanilla?" first.** The user is happy to test without the DLL
  at 30 FPS; the smoke pop and the shroud-return shadow blink both turned out
  to be vanilla. Ask early when a symptom might be stock behaviour.
- Diagnostics in `fps_patch.ini` (all need `logging=1`, 0 = off):
  `debug_motion_frames`, `debug_frame_times`, `debug_trail_draws` (per
  trail-system draw: frame, step, blend fraction, emitter position, per-trail
  point count/alive/life/head). When a fix "does nothing", ask the user for a
  log run instead of guessing again (the trail log found the spark-death jump
  in one run).
- Branch scans: a function's callers can include tail `JMP`s (E9), not only
  `CALL`s (E8). Ghidra reports both as calls.

## Lessons from tooling problems
- A PyGhidra/JVM process does **not** exit when its MCP client disconnects,
  because JVM non-daemon threads keep it alive. Stale servers keep the Ghidra
  project lock. Only stop processes whose command line points into this
  project. The user also runs other projects' Ghidra MCP servers (e.g.
  Uncharted); never touch those.
- The `ghidra` MCP server works once Claude Code has loaded `.mcp.json` (a
  new session/restart is needed after changing it).

## Architecture in 30 seconds
- Logic is 15 Hz with 6 phases per tick. Client FPS = `target_fps`, and the
  ratio `R = target/15` is currently limited to 3..6 (45–90 FPS). **R ≤ 6 is
  the core limit for >90 FPS.**
- Per-frame legacy systems are fixed by one of four techniques: rescaled
  constants (`x^(30/target)`), a synthetic 30 Hz frame counter, 30 Hz
  Bresenham gating, or corrected time caches. Details are in
  `docs/implementation.md`.
- All patch sites are found by signature scanning (`src/game_layout.c`), then
  checked again before writing (`src/game_patches.c`), with transactional
  rollback.

## Docs layout
- `docs/goals.md`: short status overview (keep current, don't pile up).
- `docs/goals_detailed.md`: hypotheses, plans, acceptance criteria.
- `docs/implementation.md`: the original author's detailed implementation
  notes.
