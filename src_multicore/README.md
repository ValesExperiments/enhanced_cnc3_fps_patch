# src_multicore

Experimental work on making Command & Conquer 3 (Tiberium Wars / Kane's Wrath)
use more than one CPU core.

Goals:
- Keep the frame rate high in intense battles.
- Improve the 1% and 0.1% lows.

Status: research phase. Findings are in `docs/multicore.md` (summary) and
`docs/multicore_detailed.md` (details).

## Files

- `profiler.c` / `profiler.h`: sampling profiler for the game's main
  thread. It is built into the same `dinput8.dll` as the FPS patch (see the
  Makefile) and stays off unless `profiler=1` is set in `fps_patch.ini`.
  Output: `fps_profile.log`; analyse it with
  `tools_and_docs_by_claude/profiler_report/analyze.py`.
