# Multi-core for CnC3: detailed notes

Summary: [multicore.md](multicore.md). Addresses are TW 1.10 VAs unless
stated otherwise.

---

## 1. Measurements we already have (from the FPS patch, F2)

From `fps_patch.log` adaptive-pacing status lines, TW 1.9, 7 AIs, late game:

| Phase of the match | Logic (ms per game second) | Effects (ms/s) | Drawing (ms/frame) |
|---|---|---|---|
| Early game | 90–250 | 15–40 | 1.5–3 |
| Big fight | 400–650 | 40–70 | 5–15 |
| Late 7-AI battle | 600–900+ | 60–150 | 25–55 |

- "Logic" is the time inside the logic-phase dispatches (QPC around each
  phase). "Effects" is the 30 Hz particle and audio updates. "Drawing" is
  everything else in a frame (client update, W3D scene, D3D9 calls), so it
  is per-frame work.
- In the worst battle, one core needs logic (~0.8 s) + effects (~0.1 s) +
  frames × drawing. Only ~5 FPS fits in a second, and even then the game
  ran at 87–94% speed.
- If logic ran on another core, the main thread would only need effects +
  drawing: ~20–25 FPS in that same battle, at 100% speed.

## 2. Threads the game creates (static analysis)

| Creator | Thread |
|---|---|
| `0x407509` | Crash/exception handler (OpenThread, two helper threads) |
| `0x68B645` → `0x68B5DF` | One-shot DNS lookup (`gethostbyname`) |
| `0x96AC00` → `0x96AA30` ("recv-event-crit"), `0x96B950`, `0x9A0A50` | Networking (GameSpy-era code) |
| `0x9DBB30` | Steam initialisation |
| `0x7B885D` → `0x7C4583` | Generic `CreateThread` wrapper (4 KB stack) |
| `0x7B70C3` / `0x7B71CD` (generic thread class) | Started from W3DDisplay mode changes (`0x80D05E`, `0x822760`, only in "mode 3") and `0x56E06F`: loading-screen work |
| `0x8C541E` / `0x8C56F9` (EAThread, `_beginthreadex`) | Started by `0x878055`, `0x882DCE`, `0xA11069`; helpers `0x8C5C5D` / `0x8C6ABE` set the ideal processor |

- Nothing points to rendering or logic work on another thread during play.
  This still needs a runtime check (per-thread CPU times in a battle).
- `0x4772C6` sets the process affinity to core 0 only on machines with
  exactly 2 cores and only during a modal loop (restored afterwards). It
  is irrelevant on modern CPUs.

## 3. Large Address Aware (PE header)

| Build | LAA |
|---|---|
| TW 1.10 | yes |
| TW 1.9 | **no** (2 GB address space) |
| KW 1.03 | yes |
| KW 1.02 | yes |

The community "C&C 4GB Auto-Patcher" (NTCore-based) sets the flag on TW 1.9;
it fixes "out of memory" crashes, not lag. We can't modify game files; the
user can apply it to a copy if needed.

## 4. Web research (2026-10-12)

**CnC3 / KW / RA3:**
- No official multi-core support or patch exists. Patch notes 1.01–1.09 and
  KW 1.01/1.02 contain no threading work.
  - https://cncnz.com/features/technical-support-help-guides/command-conquer-3-tiberium-wars-v101-v109-patch-history/
  - https://cncnz.com/features/technical-support-help-guides/command-conquer-3-kanes-wrath-v101-v102-patch-history/
- Macworld (2008) measured no gain from more cores.
  https://www.macworld.com/article/189701/commandconquer3tw.html
- RA3's community patch 1.12.6 gained performance only by removing heavy
  particle effects. https://forums.cncnz.com/topic/21062-community-patch-spotlight-red-alert-3-v1126/

**TheSuperHackers GeneralsGameCode** (EA's 2025 Generals/ZH source release;
SAGE ancestor, so hot spots are likely similar but unconfirmed for CnC3):
- No logic multithreading has been merged. A roadmap issue proposes a job
  system and "parallel compute, serial apply", but it has no measurements
  and no follow-up. https://github.com/TheSuperHackers/GeneralsGameCode/issues/2048
- Real gains came from single-thread hot-spot fixes:
  - pathfinding up to 80% of CPU late-game (#3198);
  - script lookups 85% cheaper (#3096);
  - per-frame allocations (#2761, #2646);
  - particle batching 15–30% (#3155);
  - sorting renderer +4–5% (#2854);
  - a per-write `fflush` in the replay recorder caused stutters (#1076).
  - https://github.com/TheSuperHackers/GeneralsGameCode/pull/3198
  - https://github.com/TheSuperHackers/GeneralsGameCode/pull/1076
- Logic changes are validated against 50+ replays to keep CRCs identical
  (#3258). Even the existing audio thread has had races and deadlocks
  (#2718, #3110).

**Other games / techniques:**
- DXVK already records D3D9 calls into chunks for its own CS worker thread
  (`DxvkCsThread`); the game thread keeps its scene traversal and per-call
  overhead. No option is needed or available.
  https://raw.githubusercontent.com/doitsujin/dxvk/master/src/d3d9/d3d9_device.h
- Supreme Commander splits a deterministic 10 Hz sim from a variable-rate
  renderer on separate threads. That is the model for CnC3's 15 Hz logic and
  client. https://www.forrestthewoods.com/blog/synchronous_rts_engines_2_sync_harder/
- The successful injected mods for old games were allocator and timing
  fixes (Oblivion Stutter Remover, NV Heap Replacer, SSE Engine Fixes using
  tbbmalloc/mimalloc). They are effective but sometimes unstable. No public
  example moved a game's simulation to another thread via injection.
- `timeBeginPeriod` is per-process since Windows 10 2004; occluded windows
  may lose it on Windows 11.

## 5. Options in detail

### A. Profiler (prerequisite)
- A background thread in our DLL samples the main thread's EIP ~1000×/s
  (`SuspendThread` / `GetThreadContext` / `ResumeThread`) and builds a
  histogram by function. It tags each sample with "inside a logic phase" /
  "inside effects" / "drawing" (we already know which, from the F2 timers).
- It also logs per-thread CPU times (`GetThreadTimes`) to confirm that other
  threads are idle.
- Output: the top functions by share, written to the log at the end of a
  match, and mapped to names in Ghidra.
- Risk: very low (diagnostic only, off by default).

### B. Logic/drawing pipelining (biggest win)
- The idea: start logic tick N+1 on a worker thread while the main thread
  draws interpolated frames between N−1 and N, then join before the next
  tick's results are needed.
- Determinism: logic stays serial and ordered, so results are identical as
  long as its inputs (commands, RNG, frame number) are unchanged.
- The hard part is shared state:
  - the client reads Object positions, health, model conditions and
    module data every frame;
  - logic creates and destroys Objects and Drawables, and sends
    Object→Drawable updates (the KW "drain pending updates" step);
  - shared allocators and pools;
  - the audio and particle systems are called from both sides.
- Possible designs:
  1. A hard sync at each phase boundary, overlapping only the parts of a
     frame that don't read logic state.
  2. A snapshot of what drawing needs.
  3. Locks around shared systems.

  Each design needs extensive reverse engineering and a long tail of race
  testing.
- Our F1 scheduler already places logic phases on chosen frames. That is
  the natural place to hand them to a worker.

### C. Hot-spot fixes (decided by the profiler)
- Must produce bit-identical logic results: caching, avoiding allocations,
  or a faster container with the same iteration order. No float reordering.
- Allocator replacement (mimalloc / tbbmalloc for `operator new` /
  `malloc` / the game's memory pools): low determinism risk unless
  something orders by pointer value. Moderate crash risk.

### D. Client-only parallelism
- Particles (30 Hz CPU particle update), skinning, culling. No
  determinism risk. Each subsystem's shared caches need to be thread-safe.
  The gain is capped by the "effects" and "drawing" shares.

### Not worth it
- Affinity / core-parking tools, DXVK options, LAA for speed, and
  multithreading the logic itself.
