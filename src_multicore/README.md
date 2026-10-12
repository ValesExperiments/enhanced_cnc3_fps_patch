# src_multicore

Experimental work on making Command & Conquer 3 (Tiberium Wars / Kane's Wrath)
use more than one CPU core.

Goals:
- Keep the frame rate high in intense battles.
- Improve the 1% and 0.1% lows.

Status: **closed (2026-10-12).** M1, M1b and M3a are in the DLL. M2-lite
(logic beside the render) and M4 (range searches) were researched and
dropped. Findings are in `docs/multicore.md` (summary) and
`docs/multicore_detailed.md` (details).

## Files

- `profiler.c` / `profiler.h`: sampling profiler for the game's main
  thread. It is built into the same `dinput8.dll` as the FPS patch (see the
  Makefile) and stays off unless `profiler=1` is set in `fps_patch.ini`.
  Output: `fps_profile.log`; analyse it with
  `tools_and_docs_by_claude/profiler_report/analyze.py`.
- `particle_index.c` / `particle_index.h`: M1, an instant insertion point for
  the particle manager's list (replaces a full-list search when a particle
  effect is created). `multicore_particle_index=1` (default);
  `multicore_verify=1` checks it against the original search. Also M1b
  (a system's first-draw sibling search only looks at its own group).
- `render_fixes.c` / `render_fixes.h`: M3a, the trail vertex builder
  recomputes a texture coordinate instead of reading it back from GPU memory
  (`multicore_trail_fix=1`, default).
- `range_stats.c` / `range_stats.h`: M4 diagnostic. With `profiler=1` it
  counts and times the partition range queries per caller ("Range queries"
  lines in fps_patch.log). It doesn't change any results.
