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
| B1 | Objective markers should not glitch for a few frames when they appear on screen in missions | Fix implemented, **needs in-game test** |
| B2 | Procedural unit motion (vehicle turning, suspension and body wobble) should look the same as at 30 FPS, not exaggerated | Not started |
| B3 | Particles should move smoothly at the render rate rather than in 30 Hz steps (e.g. by interpolation) | Not started |
| F1 | (Nice to have) Support beyond 90 FPS: 120, 144, 160 and higher | Not started |

## Groundwork needed first

| Item | Status |
|------|--------|
| Local build toolchain (Zig 0.16.0 + GNU make 4.4.1, inside the project) | Done: `tools_and_docs_by_claude/build.sh` builds `zig-out/bin/dinput8.dll` |
| Offline resolver check (does the patch find all its hook sites in a given game exe?) | Done: TW 1.10, TW 1.9, KW 1.03 and KW 1.02 (Steam) all resolve |
| Ghidra project for TW 1.10 and KW 1.03, plus a Ghidra MCP server | Done |
| Repeatable in-game test cases for each bug | Not started (B1 happens in every mission) |

## Current understanding in one line each

- **B1 (root cause found):** Markers are Flash/APT UI clips. A new clip gets
  its screen position on the next APT frame, and APT frames advance on a real
  ~30 Hz clock, so at high FPS the clip is drawn unpositioned for 1–2 frames.
  The fix asks APT for an immediate tick whenever a marker is created.
- **B2:** Most likely client-side per-frame physics on Drawables (pitch, roll,
  wobble and suspension recurrences) that run once per rendered frame without
  any time step, so at 90 FPS they spring three times as often.
- **B3:** The patch deliberately runs CPU particle simulation at 30 Hz. The
  plan is to keep it there and interpolate (or extrapolate) particle positions
  when drawing.
- **F1:** The main blocker is the 6-phase logic scheduler, which caps the
  client-to-logic ratio at 6 (90 FPS). FPS values that aren't multiples of 15,
  such as 144 and 160, also need fractional scheduling and more precise frame
  pacing.
