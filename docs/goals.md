# Enhanced CnC3 FPS patch: goals and status

This page is the short, current overview. It is kept up to date rather than
appended to. For analysis, hypotheses, and test plans, see
[goals_detailed.md](goals_detailed.md). For how the original patch works, see
[implementation.md](implementation.md).

## What this project is

This is a fork of the `dinput8.dll` FPS patch for Command & Conquer 3 (Tiberium
Wars and Kane's Wrath). Stock rendering is 30 FPS on a 15 Hz simulation. The
original patch adds 45, 60, 75 and 90 FPS. This fork fixes the remaining visual
bugs and raises the FPS ceiling.

Primary test target: **Tiberium Wars 1.10 (Steam)**. Kane's Wrath must keep
working too. It is only lightly modified from TW, so TW fixes should carry over.

## Goals

| # | Goal | Status |
|---|------|--------|
| B1 | Objective markers should not glitch for a few frames when they appear on screen in missions | **Done** (confirmed in-game 2026-10-11) |
| B2 | Procedural unit motion (vehicle turning, suspension and body wobble) should look the same as at 30 FPS, not exaggerated | **Done** (Surveyor bobbing fixed, confirmed 2026-10-11) |
| B4 | All moving vehicles and aircraft should move smoothly (a small stutter is visible at 90 FPS) | **Done** (confirmed 2026-10-11) |
| B5 | No random small hitches; camera scrolling as smooth as unit movement | **Solved** with our DXVK build (G-Sync works borderless; confirmed 2026-10-11) |
| B6 | Selected unit/building panel must never show the placeholder name "Unit name" | **Done** (fix 3, confirmed 2026-10-11) |
| B3 | Particles should move smoothly at the render rate rather than in 30 Hz steps (e.g. by interpolation) | CPU particles, bullets, trails, Shockwave, Ion Cannon ball and swarms (Scrin Buzzers) confirmed. **Done**: all particle types confirmed smooth 2026-10-11, including repair sparks and pausing. Smoke pop is vanilla behaviour (ignored) |
| B7 | Fog of war / shroud should clear smoothly when units reveal it | **Done** (confirmed 2026-10-11). The shadow blink when the shroud covers an object again is vanilla behaviour (ignored) |
| B8 | Secondary-object animations should not be sped up (e.g. Pitbull rocket pods' idle motion) | **Closed**: the same in vanilla (user, 2026-10-11) |
| B9 | The white "selected" flash should last as long as at 30 FPS | **Done** (confirmed 2026-10-11) |
| F1 | (Nice to have) Support beyond 90 FPS: 120, 144, 160 and higher | **Works** for any 91–240 FPS; user runs 157 FPS with no stutter (2026-10-11) |
| B10 | Dragging on the minimap should pan the camera smoothly | **Done**: game's steps lightly eased (18 ms); user: "the best version" (2026-10-11) |
| B11 | Queued-order (Alt) waypoint lines should follow the unit smoothly | **Done** (confirmed 2026-10-12) |
| F2 | Game stays at 100% speed at any frame rate (even ~10 FPS) | Working (`adaptive_fps=1`); user: "feels better" (round 7) |
| B12 | Tiberium crystals should grow and shrink (when harvested) smoothly | **Closed, kept vanilla** (2026-10-12): smoothing worked, but made the crystal glow flicker; user chose vanilla |
| B13 | The "breathing" repair / power-off icons on buildings should animate smoothly | **Closed, kept vanilla** (2026-10-12): crossfade tried and reverted; pre-rendered 15 images/s |
| B14 | Summoned objects (e.g. the Scrin Mothership) should fade in as slowly as in vanilla | **Done** (confirmed 2026-10-12) |
| B15 | Tiberium Vibration Scan highlight should last ~13 s (not 60+) and pulse smoothly | **Done** (crash fixed, confirmed 2026-10-12) |
| B16 | Rocket smoke trails should stay connected to the rocket (they started a bit behind it) | Implemented 2026-10-12, **needs in-game test** |
| B17 | Using the Ion Cannon in mission 1 (TW 1.9 mod) must not crash | Implemented 2026-10-12, **needs in-game test** |
| B18 | No micro-stutter on smoothly moving units (aircraft) at rates that aren't multiples of 15 (e.g. 155) | Implemented 2026-10-12, **needs in-game test** |
| B19 | Newly produced aircraft (Firestorm airfield) must not show in the air for one frame | Cause found (a teleport drawn as a slide); fix `teleport_snap` implemented 2026-10-12, **needs in-game test** |
| B20 | Build menu icons must not vanish for one frame after clicking a build button | **Diagnosing**: game behaviour at high FPS (bisect); the OnInitialized UI tick didn't help and was removed; per-frame UI trace (`debug_apt_calls=2`) |

## Groundwork needed first

| Item | Status |
|------|--------|
| Local build toolchain (Zig 0.16.0 + GNU make 4.4.1, inside the project) | Done: `tools_and_docs_by_claude/build.sh` builds `zig-out/bin/dinput8.dll` |
| Offline resolver check (does the patch find all its hook sites in a given game exe?) | Done: TW 1.10, TW 1.9, KW 1.03 and KW 1.02 (Steam) all resolve |
| Ghidra project for TW 1.10 and KW 1.03, plus a Ghidra MCP server | Done |
| Repeatable in-game test cases for each bug | Not started (B1 happens in every mission) |

## Current understanding in one line each

- **B1 (fixed):** Markers are Flash/APT UI clips. A new clip gets
  its screen position on the next APT frame, and APT frames advance on a real
  ~30 Hz clock, so at high FPS the clip is drawn unpositioned for 1–2 frames.
  The fix asks APT for an immediate tick whenever a marker is created.
- **B2 (fixed):** The locomotor tilt/suspension springs ran once
  per rendered frame. They now run at the retail 30 Hz, and the result is
  smoothly blended in between.
- **B4 (fixed):** The game only refreshes a unit's smoothed
  position for 6 drawn frames after it moves. That's 3 logic ticks at 30 FPS
  but only 1 at 90, so every 6th frame froze, and stopped units were left
  short and then snapped. The window now scales with FPS.
- **B5 (solved):** The judder came from G-Sync not engaging under DXVK. Our DXVK
  build lets the NVIDIA driver handle the borderless window like a native Vulkan
  game, and G-Sync now works.
- **B6 (fixed):** Only happens at high FPS, production building → deselect →
  another production building. At 30 FPS one Flash frame always ran between the
  panel's C++ steps; now each state change and button rebuild forces one.
- **B3:** GPU particles (~72%) were already smooth. CPU particles (position,
  size, colour, fade, rotation), trails and bullet tracers are now drawn blended
  or moved per frame instead of in 30 Hz steps. Swarms are now projected
  forward per frame too. Only 30 Hz emission batching remains.
- **B7:** The shroud fades lost time at high FPS (rounding down every frame);
  they now carry the remainder over, so the speed is right (confirmed). The
  grey fog still stepped because its texture has only 4 bits per channel (16
  shades), so a fade shows a few coarse steps. That texture is now 8 bits per
  channel, filled at full precision. The second, 32-bit fog texture's
  instant cell writes are also ramped over ~66 ms.
- **B9:** The flash is a colour "tint envelope" that steps once per drawn
  frame (attack 1, sustain 1, decay 4 steps = 200 ms at 30 FPS, 67 ms at
  90). Its colour steps are now scaled by 30/FPS and its hold time counts
  30 Hz frames, so it lasts 200 ms again but fades smoothly. Repeated
  flashes (a counted "flash N times") used a 15-frame interval; it's now
  FPS/2 frames, i.e. 500 ms again.
- **B3 sparks (done):** Repair-drone sparks are very short trails (3 points). The
  front grew smoothly, but the oldest point was dropped in 30 Hz steps, so the
  whole spark jumped. The tail now slides smoothly too. The vehicle repair
  sparks (`HealingSparks`) barely move and fade white→black within 2–10
  steps. A log showed the motion was blended, but every spark *death*
  jumped one step (constant with these short-lived sparks). Deaths are now
  continuous too.
- **B10:** Minimap drag moves only arrive with the ~30 Hz UI (and only when
  the point changes), and each one snapped the camera. The click still
  jumps instantly; during a drag the camera goes where the game sends it,
  but each step is eased over about 18 ms instead of snapping.
- **B11:** The Alt waypoint lines start at the unit's logic position, which
  only updates with the game logic. During the line build the patch now
  uses the unit's smoothly drawn position instead.
- **B12 / B13 (kept vanilla):** Tiberium growth is stepped by the game logic
  (15×/s). Smoothing it worked, but the crystal shader picks a random glow
  from the crystal's bone position, so a smooth animation made the glow
  flicker every frame; the user preferred vanilla. The repair/power icons
  are 10 pre-rendered images at 15/s; a crossfade didn't look good, so
  they're vanilla too.
- **B14:** The Mothership's summon fade is given in logic frames but counted
  in rendered frames, so at 156 FPS it ran ~5x faster. The patch stretches
  that frame count to the live rate; the fade stays smooth.
- **B15:** The Vibration Scan's yellow highlight counts its time in
  live-rate frames, but the B9 flash fix slows every flash to 30 Hz steps,
  so it lasted ~5x too long. Its frames are now converted to 30 Hz frames.
  Its heartbeat followed the logic frame (15x/s); it now follows a smoothly
  advancing logic frame.
- **F2:** The patch measured speed in frames, so a PC that couldn't hold the
  target ran the whole game slower. Now it measures each frame's work and,
  if needed, lowers the effective rate to what the PC can sustain, re-timing
  everything (logic schedule, animations, particles, audio, camera). Below
  15 FPS each frame runs several logic ticks. It rises again when there's
  headroom. It now tells the game's logic cost (same at any frame rate)
  from drawing cost and computes the right rate directly, and the limiter
  repays time lost to heavy logic frames. It times the game's logic
  directly, judges over 2 seconds, drops only when the game really runs
  slow (a driver frame cap no longer fools it, never more than halfway at
  once) and rises after a second of clearly low load. It only goes very
  low (down to 5 FPS) when that really buys the game speed back, and it
  notices an outside frame cap (e.g. an NVIDIA limit equal to the target)
  and settles just below it instead of bouncing. Best results: no driver
  frame cap for this game, since the mod limits itself. The game's own
  heavy-logic hitches remain, as in vanilla. It can use any frame rate
  from 15 up (not only 15, 30, 45 …), so big battles no longer get stuck at
  15 FPS when 20–25 would fit.
- **F1:** Above 90 FPS some frames must run no logic at all. A new
  scheduler gives each 15 Hz logic tick 10 or 11 frames (160 FPS: 11, 11,
  10), runs its six logic phases spread across them (phase 1 on the first
  frame, phase 6 on the last), and skips logic on the frames in between.
  Unit interpolation advances every frame and the frame limiter targets the
  exact rate. If the PC can't hold the rate, the game slows down (like retail
  below 30 FPS).