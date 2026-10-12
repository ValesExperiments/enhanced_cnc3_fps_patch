# Multi-core for CnC3: summary

Goal: keep the frame rate high in intense battles and improve the 1% / 0.1%
lows by using more CPU cores. Details and sources are in
[multicore_detailed.md](multicore_detailed.md).

## Where we stand (2026-10-12)

**Research done, no code yet.** Code will live in `src_multicore/`.

## What we know

- The game does almost all its work on **one thread**. Its other threads
  handle networking, Steam, crash handling, audio and loading screens, and
  they are idle in battle.
- Our own measurements in a heavy 7-AI battle:
  - the game logic needs **600–900 ms of CPU per game second**;
  - drawing a frame takes up to **~40 ms**.
  
  Both run on the same core, so the frame rate collapses. 5 FPS was needed
  to keep the game at full speed.
- The game logic must give **exactly the same results** on every PC
  (multiplayer and replays depend on it). Splitting the logic itself over
  several cores is therefore not realistic.
- DXVK already moves the Direct3D work to its own thread. More DXVK settings
  won't help.
- TW 1.9 (the version the user plays) is **not Large Address Aware**: it can
  only use 2 GB of memory. TW 1.10 and Kane's Wrath are. That causes memory
  crashes, not lag.

## Options, best first

1. **Measure first.** Build a profiler that shows where the main thread's
   time goes in a heavy battle (pathfinding? AI? particles? memory
   allocation? drawing?). Low risk, and it decides everything below.
2. **Run the logic and the drawing on two cores at the same time
   ("pipelining").** The biggest possible gain: in the worst battle the
   frame rate would depend only on drawing (~25 FPS instead of 5). Hard: the
   drawing code reads the logic's objects, so the two must never touch the
   same data at the same moment. Our scheduler already splits the logic
   into steps, which helps.
3. **Speed up specific hot spots** found by the profiler, without changing
   any results (e.g. a faster memory allocator, caching). The Generals
   community got large gains this way (pathfinding was up to 80% of CPU
   time in late games).
4. **Move client-only work to other cores** (particles, animation). Safe for
   multiplayer, but each piece must be made thread-safe, and the gain is
   limited to the drawing share.

Not worth it: core affinity / core parking tweaks, DXVK settings, splitting
the logic itself.

## Recommended next step

Option 1: a sampling profiler built into the patch, run during one heavy
battle, to find out which of options 2–4 pays off.
