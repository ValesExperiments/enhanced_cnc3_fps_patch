# Multi-core for CnC3: summary

Goal: keep the frame rate high in intense battles and improve the 1% / 0.1%
lows by using more CPU cores. Details and sources are in
[multicore_detailed.md](multicore_detailed.md).

## Where we stand (2026-10-12)

**Research done; profiler built; first profile taken (TW 1.10, a lighter
match).** Code lives in `src_multicore/`. Turn it on with `profiler=1` in
`fps_patch.ini`; it writes `fps_profile.log`. Everything must work on TW
1.10, TW 1.9 and Kane's Wrath. Function maps exist for all four builds
(TW 1.10/1.9, KW 1.03/1.02), so any of their profiles can be read.

First profile, busiest minutes:
- The main thread used a full core; all other threads were nearly idle.
- Drawing took ~64% of it, game logic ~30% and particle/audio updates ~6%.
- The biggest single item is the trail renderer (~7%). The game's memory
  allocator, D3DX matrix math and particle drawing follow; game logic is
  spread thin.
- Heavy 7-AI battle on modded TW 1.9 (3–6 FPS): **game logic was 64–75%**
  of the main thread and needed up to ~1.1 s of CPU per game second, so the
  game ran at 56–70% speed. Biggest single items:
  - drawing trails ~11%;
  - adding new particle effects to the manager's list ~8% (it searches the
    whole list each time);
  - "find objects near me" range searches ~10%.
- Both directions pay off: logic on its own core (biggest win in heavy
  battles) and specific speed-ups that keep results identical.

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
