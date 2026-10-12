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

## 1b. First profile (2026-10-12, TW 1.10, ~11 min skirmish, lighter than the 1.9 marathon)

`temp/profile1/` holds the logs. Analysis: `analyze.py --windows 3-6` (the
busiest part, minutes 3–7, 45–110 FPS, logic peaking ~400 ms/s).

- **Threads:** the main thread used **97.7% of a core**. Everything else was
  nearly idle: DXVK's 4 worker threads ~12% in total, DSOUND/wdmaud audio
  <1%, a MSVCR80 worker 3.6%, the Steam overlay 1.3%, our sampler 1.6%. So
  the game really is one thread.
- **Zones** of the main thread: client 64%, logic 30%, effects 6%. With
  adaptive pacing the client share is "whatever is left", since the rate
  rises until the core is full.
- **Outside the game image:**
  - ntdll 13.4%, of which ~8% is idle waiting in the main loop's limiter
    (call site `0x55D9C3`); the rest is heap and lock functions;
  - D3D9 (DXVK) 4.6%;
  - d3dx9_29 4.4% (D3DX matrix math, e.g. `0x731C57` inverts a matrix per
    call);
  - our DLL (`DINPUT8`) 2.6%;
  - win32u 0.9%, MSVCR80 0.9%.
- **Inclusive (upper bound):**
  - GameEngine::update 86%; GameClient_update 52%;
  - W3DDisplay::draw (`0x83BE2D`, vtable slot 12) 47% → `0x839FB3` 36% →
    RTS3DScene render (`0x832120`) 32%;
  - logic (`GameLogic_UpdatePhase`) 27%;
  - W3DParticleSystemManager render (`0x80FDDE`) 14%;
  - TrailDrawModule_Draw 7.7%, of which the trail vertex builder `0x5BA171`
    is **7.0% exclusive**, the single hottest function;
  - the game's small-block allocator `0x91B136` (size classes, guarded by a
    critical section, `0x91771D`) 16.8%, `_Free` 9.4%, operator delete
    (`0x402954`) 7.9%.
- **Logic** is spread thin: no function above 0.8% exclusive. There is no
  single dominant hot spot like Generals' pathfinding.
- **Our trail fix (B3) is not the cost:** it moves existing points and adds
  at most one. The trail vertex builder runs every frame in vanilla too.

**What it means:**
- Drawing is the larger cost per second in this match. Moving logic to
  another core would have freed ~30% of the main thread here, and more in
  heavier battles (60–90% of a second of logic in the 1.9 7-AI marathon).
- Client-side candidates: the trail vertex builder (7%), the allocator (lock
  per call; several %), D3DX matrix inversions (~4%), particle rendering
  (14% inclusive).
- A profile of a heavy TW 1.9 battle is still needed. Function maps for
  TW 1.9 and KW 1.02 were produced by `ghidra_tools/import_extra.py`
  (separate project `CnC3_extra`; ~6 min of auto-analysis each).

## 1c. Heavy profile (2026-10-12, TW 1.9 modded, 7 AIs, ~7 min)

`temp/profile2/`. Windows 4–6 (minutes 4–7): 3.7–5.8 FPS. Logic went up to
900–1100 ms per game second, so the game ran at 56–70% speed even at 6 FPS.

- **Zones:** logic **64–75%** of the main thread, client 16–25%, effects
  8–11%. Outside the game image: ntdll 4.9%, MSVCR80 1.5%, D3D9/d3dx9 0.8%
  each, our DLL 0.4%.
- **Hot spots (exclusive, share of all samples)**, TW 1.9 VAs:
  - `0x7592CB` **10.6%** (client): the TrailDrawModule vertex builder
    (called from the TrailDrawModule vtable slot 7, `0x75A5E1`). The same
    function as 1.10's `0x5BA171`.
  - `0x7573FC` **≈8.5%** (7.3% logic): inserting a new particle system into
    the particle manager's list. It walks the whole linked list for the first
    system with the same key (+0x15C) and inserts before it (`0x756969`).
    Called from particle-system creation `0x75D5CA` (8+ callers in logic/FX
    code). With thousands of live systems this is O(n) per creation.
  - `0x98E360` (recursive spatial tree walk) + `0x98E129` / `0x98E1CE`
    (2D / 3D circle-overlap distances) **≈9.6%** (logic): object range
    queries (weapons, AI scans).
  - `0x75E74F` 3.4% (effects).
  - `_Allocate` is on the stack in up to 32% of samples (upper bound).
- **What it means:**
  - Moving logic to its own core would free ~70% of the main thread here
    (drawing would reach roughly 3× the frame rate). The logic core would
    also get a whole core instead of ~70% of one, raising game speed from
    ~56% towards ~80% in the worst minutes.
  - Pure speed-ups with identical results:
    - an O(1) index for the particle list insert (~8%);
    - a cheaper trail vertex builder or moving it off the main thread
      (~10%);
    - faster range queries (~10%, but it is logic, so results must stay
      bit-identical).

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

### A. Profiler (prerequisite) — built 2026-10-12

**Implementation** (`src_multicore/profiler.c`, compiled into dinput8.dll):
- `profiler=1` / `profiler_report_seconds=60` in fps_patch.ini. It starts
  once, from the game-hook path on the main thread, after the patch
  installs.
- A time-critical sampler thread wakes every ~1 ms (high-resolution
  waitable timer; `Sleep(1)` fallback). For each sample it does
  `SuspendThread` → `GetThreadContext(CONTEXT_CONTROL)` → copies 4 KB of
  stack above ESP → `ResumeThread`. Nothing allocates or locks while the
  thread is suspended.
- Zones: the patch sets `g_profile_zone` to LOGIC around every logic
  dispatch (`dispatch_counted`) and to EFFECTS around the 30 Hz particle
  and audio steps. Everything else is CLIENT (drawing, UI, limiter waits).
- Per window it records:
  - exclusive instruction RVAs per zone;
  - inclusive RVAs: the instruction plus every stack value that is a game
    return address (preceded by a CALL; stale values make this an upper
    bound);
  - the first game return address when the instruction is in another
    module ("ext": who called into DXVK/ntdll);
  - samples per module and zone;
  - per-thread CPU time (`GetThreadTimes`, start address via
    `NtQueryInformationThread` class 9);
  - frames and rate.
- Self-test (`temp/profiler_test`): ~650 samples/s, sampler cost ~3% of a
  core, zones and call sites attributed correctly, no deadlocks.

**Analysis:** `tools_and_docs_by_claude/profiler_report/analyze.py
fps_profile.log --map maps/functions_TW_1.10.tsv [--windows a-b]`. Function
maps are exported from Ghidra for TW 1.10 and KW 1.03 (TW 1.9 isn't imported
into Ghidra yet).

**Original plan:**
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
