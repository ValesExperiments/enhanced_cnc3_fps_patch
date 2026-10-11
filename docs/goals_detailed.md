# Goals: detailed analysis and plan

Companion to [goals.md](goals.md), which has the short status overview. This
document holds the reasoning: what the existing patch already does, hypotheses
for each bug, how to investigate them, candidate fixes, and acceptance
criteria. Sections are updated as the work progresses. Superseded hypotheses
are struck out or removed rather than piled up.

Last updated: 2026-10-11 (B1, B2, B4 fixed; B5 diagnosed; B6 fix implemented).

---

## 0. Background that all goals depend on

### 0.1 Engine timing model (from the existing patch)

- **Logic**: authoritative simulation at **15 Hz** (`g_logicFPS`, never
  changed). Each logic tick is split into **6 ordered phases**.
- **Client/render**: stock 30 Hz, so 2 client frames per logic tick. The patch
  sets `g_clientUpdateFPS = targetFPS` and a client/logic ratio
  `R = targetFPS / 15` (3..6).
- Between logic ticks, Drawables are placed using a **phase interpolation
  float** written by `GameEngine_UpdatePhaseInterpolation` (hooked:
  `update_phase_interpolation` in [game_patches.c](../src/game_patches.c)).
- Many subsystems were authored per client frame without a time step. The
  patch handles them one by one, in four ways:
  - **Rescaling constants** with `30/target` or `x^(30/target)`: camera scroll,
    zoom, shake, rotation, height adjust.
  - **Feeding a synthetic 30 Hz frame counter**
    (`get_retail_visual_frame` = `ceil(frame*30/target)`): tracers, cloud
    effects, Anim2D.
  - **Gating to 30 Hz with a Bresenham accumulator**: CPU particle simulation,
    audio manager update, keyboard repeat.
  - **Fixing time caches**: W3D ms clock (990 ms per nominal second), audio
    ms/frame, visual seconds/frame, and radius-cursor frames/ms.

Takeaway: **every remaining bug is probably one more per-frame system that
none of these four treatments covers yet**, or a place where two clock domains
meet.

### 0.2 Environment facts

- Game: `D:\SteamLibrary\steamapps\common\Command and Conquer 3 Tiberium Wars`
  (read-only for us). The active SkuDef launches `RetailExe\1.10\cnc3game.dat`.
- The live 1.10 `cnc3game.dat` differs from `cnc3game.dat.bak` by exactly one
  byte, the PE Large-Address-Aware flag (`0x03` → `0x23` at file offset 0x12E).
  This is harmless to signature scanning.
- `RetailExe\1.9\cnc3game.dat` is the 2012 Steam build (PE timestamp
  0x501755E8). Its larger `.bak` is the 2007 retail original. The same pattern
  holds for KW `1.2\cnc3ep1.dat` (2012 Steam) vs its `.bak` (2008 retail).
- Kane's Wrath: `D:\SteamLibrary\steamapps\common\Command and Conquer 3 - Kane's Wrath`,
  with versions `RetailExe\1.2` and `1.3`.
- The user's own WIP DLSS `d3d9.dll` mod is present. It is currently off, but
  it's kept for its DXVK backend, which fixes out-of-memory bugs. It offers
  only upscaling modes (DLAA to Ultra Performance) and **no frame
  generation**, so it doesn't distort frame-rate observations.
- Build: `tools_and_docs_by_claude/build.sh [targets]` runs the project
  Makefile with a local Zig 0.16.0 (same version as upstream CI) and GNU make
  4.4.1 (ezwinports). Zig caches are redirected to `.zig-cache/` inside the
  project. `scripts/link_release.py` needed a Windows fix: POSIX `shlex`
  splitting ate the backslashes in Zig's printed linker paths.
- Offline resolver check: `tools_and_docs_by_claude/layout_check/run.sh <exe>...`
  maps a game executable at 0x00400000 in a 64-bit host and runs every stage of
  `resolve_game_layout`. Results on 2026-10-11:

  | Executable | Result |
  |---|---|
  | TW 1.10 (2025) | all stages resolve |
  | TW 1.9 Steam (2012) | all stages resolve |
  | KW 1.03 (2025) | all stages resolve |
  | KW 1.02 Steam (2012) | all stages resolve |
  | TW 1.9 / KW 1.2 retail `.bak` | bootstrap fails (old retail images, not used) |

  Rerun it after every change to signatures or resolver code, against TW 1.10
  and KW 1.03 at least.

### 0.3 Useful references

- [implementation.md](implementation.md): the original author's detailed
  notes, with RVAs for KW 1.02 Steam and some TW 1.09/1.10 addresses.
- EA's GPL release of the **C&C Generals / Zero Hour source** (SAGE, the
  predecessor engine to CnC3's). Class names such as `GameClient`, `Drawable`,
  `Anim2D`, `ParticleSystem`, `Keyboard`, `W3DView` and `RadarEvent` largely
  carry over, so it is the best guide to what the CnC3 binary is doing. To
  verify: clone it into `tools_and_docs_by_claude/gitignored/` as a read-only
  reference.
- Ghidra 12.1.3 (system-wide), driven headless through the project's `ghidra`
  MCP server (`tools_and_docs_by_claude/ghidra_tools/`), with the project
  `ghidra_data/projects/CnC3.gpr` (TW 1.10 and KW 1.03 analysed).
- `tools_and_docs_by_user/CnC_Modding_Support/`: EA's source XML, schemas
  and shaders for TW and KW. Use it for data-driven parameters (locomotor
  suspension for B2, FX/particle definitions for B3).
- `tools_and_docs_by_claude/AllianceToolSwitchNotes/` and
  `CNC3_ModdingResearch/`: notes from an earlier CnC3 reverse-engineering
  project.

### 0.4 Cross-cutting test method

Each bug gets a **repeatable repro** (mission/skirmish, unit, camera action).
Compare it at three settings:

1. Stock 30 FPS (patch disabled via `enabled=0`)
2. Patched 45 FPS
3. Patched 90 FPS

A bug that scales with FPS (worse at 90 than at 45) is a per-frame recurrence.
A bug that is the same at every patched rate is more likely a clock-domain
mismatch. Capture with the game-automation tooling (windowed mode +
screenshots/recording) where possible.

---

## B1. Objective markers glitch for a few frames when they appear

**Status:** fixed. Confirmed in-game by the user on 2026-10-11.

### Symptom
In campaign missions, an objective marker that comes on screen visibly
glitches for a few frames before settling. The user reports that this happens
in **every mission**, at both 90 and **45 FPS**.

### Root cause (confirmed in Ghidra, TW 1.10 VAs)
1. On-map objective markers are **APT (Flash) UI clips** in `TacticalHUD.swf`
   (`TacticalObjectiveIcon`, `TacticalHUDObjectiveIndicatorStage`), not
   models or Anim2D sprites.
2. `ObjectiveIndicator_ObjectTarget_UpdateOnOffScreen` (0x7A24D1) and its
   waypoint twin (0x7A201A) project the target to the screen every client
   frame. When the target **enters** the screen, they create a new on-screen
   indicator through `TacticalObjectiveIndicatorStageImpl::AddObjectIndicator`
   / `AddWaypointIndicator`. When it **leaves**, they create an edge arrow
   through `AddOffScreenIndicator`. These are vtable slots 0..2 at 0xABEB30.
3. Each new indicator gadget only pushes its position to ActionScript
   (`SetScreenRegion` / `SetScreenPos` / `SetObjectivePos`) after the clip's
   **`OnInitialized`** callback (e.g. 0x7B134D sets flag +0x10 and pushes the
   cached position). Until then, position updates are only cached.
4. `OnInitialized` runs on the next **APT movie frame**.
   `AptPlayer_Update` (0x44B7BC) feeds real `timeGetTime()` deltas (clamped to
   60 ms) into `AptRuntime_AdvanceFramesByAccumulator` (0x8CC57F), which
   advances one movie frame per ~33 ms. That is a fixed ~30 Hz, regardless of
   render FPS.
5. At stock 30 FPS, every rendered frame advances one APT frame, so the gap is
   invisible. At 45 FPS the new clip is drawn unpositioned for about 1
   rendered frame, and at 90 FPS for about 2. That is the glitch, and it
   explains why it happens at 45 too and only when a marker appears.

### Fix (implemented)
The engine already has a "force an APT tick" switch: `AptPlayer+0x1E9`.
`AptPlayer_RenderPrep_ForceTickIfFlagged` (0x448104) runs one APT update
before drawing when it is set, and the update raises `dt` to at least 34 ms
so one movie frame always advances. The game uses this itself, e.g. around
display-mode changes.

The patch redirects the three `Add*Indicator` vtable entries to wrappers
that call the stock method and then set `g_aptPlayer->[0x1E9] = 1`. The new
clip's `OnInitialized` → position push therefore runs before the frame is
drawn.

- Resolution: the `AptPlayer` constructor tail (`CMP/MOV [g_aptPlayer]`, both
  operands equal) and three `Add*Indicator` prologue signatures (alloc size
  0x54/0x60/0x4C, counter offset 0x24/0x30/0x3C, `RET 8` tail). The vtable is
  the unique read-only pointer to `AddObjectIndicator`, with slots 1–2
  verified.
- Verified offline on TW 1.10, TW 1.9, KW 1.03 and KW 1.02 (all resolve).
- Side effect: each forced tick advances APT time by about 34 ms instead of
  the true ~11–22 ms, i.e. UI animations jump ahead by ~20 ms once per
  marker creation. This is not noticeable.
- Not covered (yet): radar/minimap objective indicators
  (`TacticalRadarObjectiveIndicatorStage`) and other APT pop-ups. Extend the
  same treatment if they show the same glitch.

### Done when
The user confirms in-game, at 45 and 90 FPS, that markers appear cleanly
when scrolled into view and that edge arrows appear cleanly when scrolled
out. Then this section moves to `implementation.md`.

---

## B2. Procedural unit animation is too intense

**Status:** fixed. Confirmed in-game on 2026-10-11: the Surveyor no longer bobs
and there are no odd tilts. The Harvester improved; its remaining slight
stutter belongs to B4.

### Symptom
Some vehicles turn glitchily, and vehicle suspension/body wobble is
exaggerated. At 45 FPS it was absent or too small to notice, but clearly
visible at 90. The best repro cases (user, 2026-10-11) are the **GDI
Surveyor**, which bobs up and down while driving (the worst case), and the
**GDI Harvester**, which glitches when turning or stopping.

### Root cause (confirmed in Ghidra, TW 1.10 VAs)
- Locomotor templates (EA XML, `GlobalData/Locomotor.xml`) define SAGE's
  visual "physics transform": `Appearance`, `PitchStiffness`/`PitchDamping`,
  `RollStiffness`/`RollDamping`, `BounceKick`, `AccelPitchLimit`,
  `ForwardAccelerationPitchFactor`, `HasSuspension`, and
  `MaximumWheelExtension`/`Compression`. The Surveyor's
  `GDISurveyorLocomotor` uses `FOUR_WHEELS` with suspension.
- `Drawable_applyPhysicsXform` (0x48E8C7) zeroes a 4-float record (pitch,
  roll, yaw, z) and calls `Drawable_calcPhysicsXform_dispatch` (0x48E0F5). It
  applies the tilt only when the dispatcher returns true.
- The dispatcher reaches `Object(+0x140) → AI(+0x2E8, KW +0x2F4) →
  Locomotor(+0x1F4)`, has a **once-per-GameClient-frame guard**
  (`locomotor+0xC0 == frame`), and then switches on the appearance type into
  four spring/damper integrators (0x473349, 0x48C906, 0x47354F, 0x47D7FD).
  These are found via `DrawableLocoInfo`'s constructor.
- Those integrators have no time step, so at 90 FPS the springs, kicks and
  acceleration-tilt run 3× as often as retail.

### Fix (implemented)
1. The dispatcher's frame read now uses the synthetic 30 Hz frame
   (`get_retail_visual_frame`), so its own guard advances the springs at
   exactly the retail rate (same technique as tracers/Anim2D).
2. `applyPhysicsXform`'s call is redirected to
   `calc_physics_xform_interpolated`. It keeps the last two 30 Hz results per
   drawable (8192-entry open-addressing cache keyed by `Drawable*`, stale
   entries recycled after two retail frames, cleared each session) and
   returns a linear blend for the current client frame. Retail time
   `t = frame*30/target`; blend factor = frac(t) (1 at exact retail frames).
   The tilt lags by at most one retail frame (33 ms) but never steps or
   disappears.
- Signatures: the dispatcher body (Object→AI offset wildcarded, since it
  differs between TW and KW) plus the call site after the four `movss`
  zeroing stores. The call target must equal the dispatcher entry. Verified
  offline on TW 1.10, TW 1.9, KW 1.03 and KW 1.02.
- Client-side only (Drawable); no effect on logic/CRC.

### Done when
Side by side with stock 30 FPS, the Surveyor no longer bobs harder, and the
Harvester turns and stops without glitches at 90 FPS. Then check a hover unit
and an aircraft too.

---

## B4. All vehicles and aircraft stutter slightly while moving (90 FPS)

**Status:** fixed. Confirmed in-game on 2026-10-11. The user's clue: units **snap** into place when they
stop.

### Measurement (debug_motion_frames, harvester at 90 FPS; temp/logs/motion_run2.log)
On every logic tick, the phase-2 client frame repeated the previous frame's
drawn position, because `clientFrame - historyFrame` reached exactly 6 there
and the `< 6` test failed. That's one frozen frame in six, i.e. a 15 Hz
stutter. The blend fraction itself was aligned correctly (the new pair arrives
at phase 1 with s = 1/6 and reaches s = 1 at phase 6), so hypothesis 2 is ruled
out. When a unit stops, the final pair arrives 5 frames after the last move,
gets one frame at s = 1/6, and then freezes there until the "stale for 2 logic
frames" fallback snaps it to the Object transform.

### Fix (implemented)
Both `CMP EAX,6` windows are rewritten to `3 × targetFPS/15` client frames
(the stock three-tick window). They're resolved by signature in two shapes:
TW's `SUB EAX,[reg+0x138]` (disp32) and KW's `SUB EAX,[reg]`. Exactly two
matches are required, each preceded by `MOV ECX,[g_theGameClient]` with the
same global as the physics resolver. Verified offline on TW 1.10, TW 1.9,
KW 1.03 and KW 1.02.

### How drawn positions are interpolated (TW 1.10 VAs)
- Units are drawn one logic tick behind. `Drawable_getInterpolatedPosition`
  (0x479410) evaluates `D3DXVec3CatmullRom(p0, p1, p2, p3, s)`, with
  `p1 = Object previous position`, `p2 = current position` and
  `p3 = 2*p2 - p1`. `s` is **`GameEngine+0x48` = phase/6**, written by
  `GameEngine_UpdatePhaseInterpolation`.
- `Drawable_getInterpolatedTransform` (0x48187A) also lerps the rotation
  matrix (previous `+0x64`, current `+0x94`) with the same `s`. If the
  drawable's last logic update (`+0x47C`) is older than `logicFrame - 2`, it
  uses the Object transform directly: a **snap**.
- History refresh `FUN_00474eaf` and the spline evaluation both only run while
  `clientFrame - Drawable+0x138 < 6`. That's a **hard-coded 6 client frames**:
  3 logic ticks at stock 30 FPS, but only 1 tick at 90 FPS.
  `Drawable+0x138` is set to the client frame by `Drawable::onTransformChanged`
  (0x4748D6, virtual).
- Logic phases: behaviour update lists run in phases 3–6 (list 0 split over
  phases 3/4, lists 1–2 in phase 5, list 3 in phase 6). This looks like
  Generals' INITIAL/PHYSICS/NORMAL/FINAL update phases, so movement probably
  happens in phase 5.
- Other `+0x48`/`+0x44` consumers exist (0x46F10B transition lerp, W3D draw
  modules around 0x8476AD/0x850FFE/0x8580A2, …) and may need the same
  treatment.
- Side finding: `Drawable::updateDrawable` (0x48353F) has a raw
  `getFrame() % N` counter (+0x150/+0x154), probably scripted "flash unit"
  effects, which will flash faster at high FPS.

### Hypotheses (resolved by the measurement above)
1. **Confirmed.** The 6-frame window has no slack at 90 FPS, so the spline sometimes isn't
   evaluated and the drawn position freezes for a frame (stutter). When a
   unit stops, the drawn position freezes short of the end until the
   "stale → use Object transform" branch snaps it.
2. **Ruled out.** `s = phase/6` isn't aligned with the frame in which a new
   previous/current pair arrives, so `s` can wrap from 1 back to a small
   value on the same pair (a small backwards step).

### Diagnostic (added 2026-10-11)
`debug_motion_frames=N` in `fps_patch.ini` makes the patch follow the first
vehicle whose history shows movement and log one `M` line per client frame:
frame, µs timestamp, engine phase, `s`, `historyFrame`, drawable flags, Object
position and previous position, `p1`/`p2`, and the interpolated position.
These are TW 1.10 offsets; they may be wrong on KW.

### Candidate fix (after the log confirms the cause)
- Scale the two `< 6` windows to `3 * R`.
- Give the spline/matrix blends a per-drawable `s` that advances `1/R` per
  client frame from the frame its new history pair arrived, clamped at 1.
  This is timing-independent and lands exactly on the final position when a
  unit stops.

---

## B5. Random small hitches and camera judder (90 FPS)

**Status:** solved (2026-10-11). With our DXVK build (`dxvk.fseDriverDefault`,
driver-default FSE mode) G-Sync engages in borderless and the user reports the
game as "buttery smooth" (it acts semi-borderless, which they accept).

Frame log (`debug_frame_times=3600`, 40 s skirmish; temp/logs/frames_run1.log):
mean 11.11 ms, p1–p99 9.8–12.4 ms, no frame over 20 ms except the first two
after the session starts. The limiter always plans 11 ms; timeGetTime
resolution is ~1 ms. The game produces frames evenly.

Display path (the user's DXVK 3.1.1-based DLSS mod `d3d9.dll`; its log in
`RetailExe/1.10/logs/`): exclusive fullscreen, `VK_PRESENT_MODE_FIFO_KHR`
(V-Sync), 3 images, and the warning **"Built-in frame rate limiter
enabled"**. G-Sync doesn't engage (user), so 90 FPS on a fixed 160 Hz refresh
alternates frames between 1 and 2 refreshes (6.25/12.5 ms). That's the
visible judder/hitching, worst when panning the camera. Three pacing
mechanisms are stacked: game limiter, DXVK limiter, and V-Sync.

Recommendations (outside this patch):
1. Get VRR working with Vulkan/DXVK: NVIDIA Control Panel → G-SYNC for
   full-screen (and windowed); Manage 3D settings → "Vulkan/OpenGL present
   method" = "Prefer layered on DXGI swapchain"; check with the G-SYNC
   indicator overlay.
2. Find why DXVK's built-in limiter is active (display-mode `@0` handling)
   and disable it, leaving the game's limiter in charge.
3. Without VRR, use a frame rate that divides the refresh rate (80 or 160 FPS
   on 160 Hz), which needs F1's arbitrary/fractional rates.

G-Sync investigation (2026-10-11):
- DXVK's limiter warning is benign. In fullscreen with V-Sync, DXVK caps at
  the display refresh rate (`UpdateTargetFrameRate`: `-m_displayRefreshRate`),
  i.e. 160, which never affects 90 FPS.
- DXVK disallows Vulkan full-screen exclusive unless `dxvk.allowFse = True`.
  Its source notes that FSE is required for VRR/HDR on Windows. **The user
  prefers borderless**, so FSE isn't used.
- Borderless VRR with Vulkan on NVIDIA therefore depends on the driver's
  "Prefer layered on DXGI swapchain" path reaching independent flip (the
  window exactly covers the monitor, nothing overlaps it, no injected
  overlays). The user tried that setting without success (likely profile
  scope, overlays, or the DLSS mod's presentation path). To test with the
  plain cursor-lock DXVK build: setting in the Global profile, G-SYNC for
  windowed and full screen, G-SYNC indicator on, overlays off.

Possible patch improvement later: sub-millisecond (QPC) pacing for even
smoother VRR output (±1 ms jitter today). F1 needs this anyway.

## B6. Selection panel sometimes shows "Unit name"

**Status:** fixed. Fix 3 (forced APT tick per panel step) was confirmed in-game on
2026-10-11.

Evidence: two screenshots of the same Barracks, 4 s apart. One shows
"Barracks" and the other the placeholder "Unit Name", both with the correct
portrait. The user thinks they clicked empty ground in between.

Mechanism (TW 1.10 VAs):
- `SelectionDetails::PanelImpl`: deselect → `FUN_0079133c` destroys the mode
  handler and calls `gadget->SetState(0)`. Reselect → a new
  `SingleUnitModeHandler` (ctor 0x792EE1) calls `SetState(1)`, then
  `SetUnitName(name)` (+0x14), category, weapon, … Re-selecting the same
  single object is ignored entirely (slot 3, 0x793154).
- `TacticalSelectionDetailsPanelImpl` (vtable 0xABDDFC) forwards every
  property only when it changes. `SetUnitName` (0x79D708) keeps the old name
  at +0x40, so reselecting the same building after a deselect never re-sends
  `UnitName`, while the Flash panel's close/open cycle reset its text field.
  C++ → APT traffic is deferred to ~30 Hz APT ticks
  (`FUN_00773d27`: pending variable table, last value wins;
  `FUN_00773a92`: queued call). At 90 FPS, three frames of updates merge per
  tick, which makes this race common.

Fix: the `SetUnitName` vtable entry is redirected to
`set_unit_name_always_push`. It runs the stock setter, then (if the gadget is
initialized, +0x14) always calls the stock "push UnitName" helper
(0x79D05E). Resolution: the full 50-byte setter signature, plus "its third
call's helper pushes the string `UnitName`", unique; the vtable entry is the
unique read-only pointer. Verified offline on TW 1.10, TW 1.9, KW 1.03 and
KW 1.02. Static writes are now 34.

If other panel fields (category/weapon icons) ever show defaults after a
reselect, extend the same treatment to their setters.

**Retest 1 (user):** still happens, but only after deselecting and only for
**buildings with production** (Barracks, War Factory, Construction Yard).
These rebuild their exit/production buttons in a later frame
(`SingleUnitModeHandler` update 0x7924E9: when the `+0x40` flag is set it
calls `FUN_0082b110` + `FUN_00791e91`; the ctor also sets up the queue via
`FUN_00792569`), and that Flash layout pass resets the name field after the
push.

**Fix 2 (implemented):** the `SingleUnitModeHandler` update vtable entry is
redirected to `single_unit_update_with_name`. It runs the stock update, then
re-sends `UnitName` every client frame via handler+0x14 → PanelImpl+0x14 →
gadget (if initialized), just like the stock update already re-sends rank
and ammo every frame. The signature is the first ~70 bytes of the update,
identical in TW and KW. Static writes are now 35. If this still fails, Flash
ignores unchanged assignments and the panel's ActionScript (TacticalHUD.apt)
must be read.

**Retest 2 (user):** still happens with the per-frame re-send.

APT findings (`Core/1.0/Apt.big` → `dataptui	acticalhudselectiondetails.bin`,
RefPack-compressed; decompressed copy in temp/apt/):
- `unitNameText` has design-time text "Unit Name" and is bound to
  `$_parent:UnitName&dropshadow`, i.e. it displays its parent clip's
  `UnitName` variable.
- C++ (`FUN_00773d27`) writes `"_level<N>.<gadgetPath>.UnitName"` into a
  C++-side variable dictionary owned by `g_aptPlayer` (+0x68) whose entries
  notify listeners. Re-sending every frame doesn't reach the visible field, so
  in the failing case the field's `_parent` must be a different clip instance
  than the one C++ addresses (e.g. a rebuilt panel/production layout).
- Next: check whether this also happens at stock 30 FPS (`enabled=0`). If it
  does, it's a vanilla bug. Otherwise trace the gadget path and dictionary
  entry at runtime, or decode the panel's ActionScript.

**Retest 3 (user):** does **not** happen at stock 30 FPS. A reliable repro at
90 FPS: select production building A → deselect → select production building
B → "Unit name" almost every time.

**Fix 3 (implemented):** the panel's C++ steps (state/name/production setup,
then the button rebuild on the next client frame; teardown on deselect) used
to get exactly one APT frame each at 30 FPS. Now:
- `TacticalSelectionDetailsPanelImpl::SetState` (slot 1, derived as slot 5's
  entry − 16 and prologue-verified) is wrapped; a real state change requests
  the engine's forced APT tick (`g_aptPlayer+0x1E9`, as in B1).
- The single-unit update wrapper requests a forced tick when the handler's
  `+0x40` "rebuild buttons" flag is set.
The per-frame `UnitName` re-send (fix 2) is kept. Static writes are now 36.

---

## B3. Particles still run at 30 Hz

**Status:** CPU particles, trails, tracers, Ion Cannon ball and Shockwave
are confirmed smooth in-game (2026-10-11). Swarm smoothing is implemented and
needs a test. The remaining smoke pop also happens without the DLL (vanilla
behaviour), so it's out of scope.

### What kinds of particles exist (EA XML: 968 FXParticleSystemTemplates)
| Type | Systems | Moves via |
|---|---|---|
| `GPU_PARTICLE` (Gpu draw) | 694 | vertex shader, analytically from `Time` |
| default / CPU (Default draw) | 176 | CPU simulation, 30 Hz (patch gate) |
| `TRAIL` | 45 | CPU |
| `DRAWABLE` (RenderObject draw) | 39 | CPU |
| `STREAK` (Streak/Lightning) / `SWARM` | 14 | CPU |

- **GPU particles** (`Shaders/GpuParticle.fx`): `age = Time*30 - CreationFrame`,
  position/size/rotation from closed-form damping integrals. With the W3D
  clock advancing every client frame (patch), they already move smoothly.
  Only their *emission* happens in 30 Hz batches (the emitter runs in the
  gated simulation step).
- **CPU particles** (`Shaders/CPUParticle.fx`) get final positions from the
  CPU, so they visibly step at 30 Hz.

### Engine facts (TW 1.10 VAs)
- `ParticleSystemManager::UpdateForClientFrame` (0x5B929E) is the gated
  30 Hz simulation (`update_particles_at_retail_rate`), called from the W3D
  manager's per-frame update (0x835ECB).
- `DefaultPhysicsParticleModule` per-particle step (0x5B3C05): `vel += accel;
  vel *= damping; pos += drift + vel` (no time step).
- Particle layout: `+0x04` velocity, `+0x10` position, `+0x1C` spawn position,
  `+0x28` lifetime, `+0x40` remaining life (−1 per step;
  `CPUParticle::update` 0x5B5837).
- `DefaultDrawModule::Draw` (0x5AEA1A, `RET 0xC`, vtable slot 7 at
  0xA7C7F0): system = module+4; storage = system+0x84; count/get = storage
  vtable +0x14/+0x18; reads positions at particle+0x10 and batches them into
  W3D point groups. KW 1.03 is identical (0x6153C4).

### Fix (implemented; round 1 confirmed in-game 2026-10-11, round 2 needs a test)
**CPU particles (Default, Streak, Lightning, RenderObject draw modules).** Each
draw module's `Draw` (vtable slot 7, `RET 0xC`) is redirected to
`draw_particles_interpolated`. Per draw, ten values per particle are tracked
in a 16384-entry side table keyed by pointer: position (+0x10..+0x18), colour
(module +0x5C → +0x94..+0x9C), alpha (module +0x60 → +0x48), size (module
+0x64 → +0x10) and two rotations (module +0x64 → +0x08/+0x24). The life
counter detects pool reuse, and a 30 Hz step counter (incremented by the
particle gate) tells new steps apart. Values are temporarily set to
`lerp(previous, current, α)` with `α = particle accumulator / targetFPS`, then
restored after the stock draw (in reverse order, safe if modules are shared).
RenderObject's `RET` is at +0x4F3 (2025 builds) or +0x4F0 (2012 builds).

**Trails.** `TrailDrawModule::Draw` is redirected to `draw_trails_interpolated`.
Trail records (storage +0x1C..+0x20, 0x28 bytes) hold an STLport deque of
0x14-byte points (6 per block); the storage update pushes the head particle's
position once per step. During the draw, each trail's newest point is moved
to `lerp(second newest, newest, α)` and restored afterwards, so the front
segment grows continuously.

**Tracers (bullets).** `W3DTracerManager_Update` (0x834270, gated by "frame
== +0x90" on the synthetic 30 Hz frame) is entered through a
`MOV ECX,[ECX+1Ch]; JMP` thunk. That jump is redirected: on frames where the
30 Hz frame is unchanged, only the movement/expiry pass (0x832B39) runs. The
pass's call to the per-tracer update (0x8083D7: expiry +0x6C, moves both
endpoints by `dir × speed(+0x58)`) is redirected to `tracer_update_scaled`,
which scales the speed by 30/targetFPS for that call. Emission
(Min/MaxTracersPerFrame, SweepSpeed) and expiry stay on the 30 Hz frame.

All verified offline on TW 1.10, TW 1.9, KW 1.03 and KW 1.02. Static writes
are now 43.

### User feedback (round 1)
Many particles became smooth. Still at 30 Hz: infantry/watchtower bullets
(tracers, fixed in round 2), Ion Cannon blast (CPU flash/shockwave size,
RenderObject distortion; round 2), Shockwave artillery blast (CPU ring
**size** growth; round 2).

### User feedback (round 2) and round 3 (implemented, needs a test)
Bullets and trails confirmed smooth; the Shockwave ring is fixed. Still at
30 Hz: the Ion Cannon **ball** that grows before the explosion, and some
smoke disappears "glitchily". Also asked: fix the camera ease in advance.

- **Ion Cannon ball** = `FXIonCannonExplosionSphere` (DRAWABLE/RenderObject,
  InitialDelay 160, Lifetime 90). Its size is a 3D vector in
  `RenderObjectParticleUpdateModule` (step 0x5AC37D: size +0x30..+0x38 +=
  rate +0x10..+0x18, rotation +0x08), not the DefaultUpdate layout. Field
  tables are now per draw kind (RenderObject: position, colour, alpha, size
  xyz, rotation; others: position, colour, alpha, size, two rotations).
- **Smoke pop:** the blend lagged one 30 Hz step, so a particle was removed
  while its displayed state was still one step behind (e.g. not yet fully
  faded). The draw now **projects forward**: `current + (current - previous)
  × α`, with sizes/colour/alpha clamped at zero. There's no lag; a value
  that fades linearly reaches its final state as the particle is removed.
  (Trails keep their lerp.)
- **Camera ease** (`CameraTransition_PerFrameAccel_0_03`, 0x828D30): two
  phases, each "progress += speed; if (speed < limit) speed += accel" per
  client frame (limits 0.5/0.4, accels 0.05/0.03, each read only here). The
  four `MOVSS XMM2,[const]` operands are redirected to DLL floats: limit ×
  dt, accel × dt², with dt = 30/targetFPS. Static writes are now 47.

### User feedback (round 3) and swarm (implemented, needs a test)
Ion Cannon ball confirmed smooth. Smoke still disappears abruptly, but the
user saw the same without the DLL, so it's vanilla behaviour and ignored.
Nothing odd noticed after the camera-ease change. Swarms (Scrin Buzzers) were
still 30 Hz.

- **Swarm** (`SwarmDrawModule::Draw`, TW 0x5B68B0 / KW 0x61D374, `RET 0xC`
  at +0x8B). Records (storage +0x10..+0x14, 0x1C bytes) hold position and
  velocity (+0x0C); each 30 Hz step does `velocity += attraction; position
  += velocity`. During the draw each record's position is temporarily set to
  `position + velocity × α` and restored afterwards. Static writes are now 48.

### User feedback (round 4): sparks (implemented, needs a test)
Swarms confirmed. Still at 30 Hz: **repair-drone sparks**
(`GDIRepairDroneSparks`: `Type="TRAIL"`, `TrailLife="3"`, lifetime 1–10,
velocity 5–10/step).
- TW 1.10: `TrailSystemStorage_Update` (0x5C09EA) per step either pushes
  the head particle's point (live; `FUN_005BB06B` is a plain `push_back`) or
  pops one from the front (particle dead, or system stopping: `+0x170`). The
  trim to TrailLife happens **in the draw**: `TrailDrawModule_Draw`
  (0x5BB4D3) first calls `FUN_005B6B07(TrailLife)`, which pops live trails
  down to TrailLife points. Point = position, width (+0x0C), colour (+0x10).
  The vertex builder (0x5BA171) only uses each point's own fields (V is
  normalised by the count), so an extra point is harmless.
- So each step the oldest point vanished, and a 3-point spark jumped as a
  whole even though its front grew smoothly.
- **Fix:** during the wrapped draw, TrailLife (module data `+0x10 → +4`) is
  raised by one, keeping the point about to be dropped. That oldest point
  slides toward the next by α when it will go next step (live trail at full
  length, dead, or stopping). The head only grows while the particle is alive
  (previously a draining trail's head also moved back each step). Position
  and width are blended; restored after the draw. The resolver asserts the
  trim call at draw +0x34 and the storage update's shape. No new patch
  writes.
- **Round 5:** drone sparks confirmed smooth. The **vehicle repair sparks**
  are a separate system: the drone's `HealFX="FX_Repair"` plays
  `HealingFlash` (GPU) and `HealingSparks` (TRAIL, TrailLife 3,
  SystemLifetime 20, lifetime 2–10, no gravity, damping 0.3–0.85) on the
  repaired unit. They go through the same update loop and trail draw (the
  system's transform comes from the unit's interpolated transform but is
  used only for emission; particles and trails are in world space). The
  difference: they barely move after the first step and fade white→black in
  2–10 steps. A point's packed colour (+0x10, includes alpha) is fixed when
  it's pushed, so brightness stepped at 30 Hz. The blended ends now also
  blend colour per byte, with a separate integer restore list (colours aren't
  routed through floats).
- Selection flash (B9) confirmed fixed.
- **Round 6 (measured):** colour blending changed nothing. A
  `debug_trail_draws` log (2026-10-11, Mammoth repaired near a War
  Factory) showed the HealingSparks system (`at=(0,0,0)`, FX-list system
  with world-space particles) *is* blended: heads grow and tails slide per
  frame while alive. Every trail system is drawn twice per frame (harmless).
  The fault was at **death**. The step that kills a particle still pushes the
  final head point; afterwards the trail drains one point per step and is no
  longer trimmed. Our code then stopped growing the head (head jumped
  forward one step) and slid the tail from the extra point kept for live
  trails (tail jumped back one step). HealingSparks spawn 2–3 sparks per step
  and live 2–10 steps, so deaths are constant → 30 Hz look. Drone sparks
  burst only every 10–30 steps, so it was rare there.
- **Fix:** a 4096-entry history keyed by particle pointer records the death
  tick and whether the live trail was full (held the extra point). On the
  death step the head keeps growing to its final point. While draining, the
  extra point is folded onto the sliding tail; when only it and the head are
  left, they collapse (vanilla shows nothing then). A live sighting resets
  the entry (pool reuse); the table is cleared per session.

### Not covered yet
30 Hz emission batching of continuous streams (all types, including GPU
particles).

---

## B7. Fog of war clears in steps

**Status:** round 1 confirmed the fade speed is now right; round 2 (8-bit
shroud texture) implemented 2026-10-11, needs an in-game test.

### Symptom
When a unit walks into the shroud, the revealed area clears in visible
steps ("at 30 FPS") instead of smoothly.

### How the shroud is drawn (TW 1.10 VAs; KW identical)
The terrain client (`[0xC18940]`) owns two CPU-side cell textures, each
uploaded every rendered frame from `FUN_00839FB3` (render path
`FUN_0083B400`):

| Object | Cells | Upload | Fade | Rate | Cap |
|---|---|---|---|---|---|
| `+0x198` "shroud" | 16-bit ARGB4444 (`FUN_00813CDD`) | `FUN_00834BF4` | `FUN_00833AB6` | 0.255 levels/ms | 255 |
| `+0x19C` "fog" | 32-bit (`FUN_00829F0D`) | `FUN_00834D47` | `FUN_00833D05` | 0.064 levels/ms | 128 |

Object layout: `+0x00` width, `+0x04` height, `+0x18` cell buffer, `+0x1C`
texture, `+0x38` target levels, `+0x3C` shown levels.

- Each upload first runs its **fade**: shown levels move toward target levels
  by `step = ftol(elapsed_ms × rate)` (truncating), with `elapsed` from
  `timeGetTime()` and a static stamp that advances by the *whole* elapsed time.
  Changed cells are rewritten in the cell buffer, then all rows are
  `memcpy`'d into the locked texture.
- Logic reports vision changes through a virtual callback (`FUN_008316E9`):
  the shroud object gets a new target level (and fades). The fog object gets
  `FUN_00829F0D(x, y, level, 1)`, which writes the cell **directly**, skipping
  its fade. (The `PUSH 1` left on the stack by `FUN_009E55A2`, `RET 8`,
  becomes the 4th argument.)

### Root causes
1. **Fog cells pop.** Logic writes revealed fog cells instantly, so the
   reveal advances in logic-rate steps.
2. **Fades lose time at high FPS.** Truncating `elapsed × rate` every frame
   while the stamp advances fully throws the fraction away. At 90 FPS (11–12
   ms frames) the fog fade gives `0.70 → 0` almost every frame (it stalls), and
   the shroud fade gives 2 instead of 2.8 levels/frame (~25% slow). `FUN_009D97D0`
   uses `CVTTSD2SI` (truncation).

### Fix (implemented)
- **Fade carry** (both textures): the upload's call of its fade is redirected
  to a wrapper. After the stock fade runs, the wrapper recomputes the same
  step and moves the stamp back by the unused milliseconds, so the fraction
  carries over to the next frame. Fades now run at their authored rate at any
  FPS. The cap still applies; the first call and long gaps are left alone.
- **Fog smoothing**: the fog upload's per-row `memcpy` is redirected. Rows
  are copied from a smoothed copy of the cells instead, where every byte channel
  moves toward the game's cell by at most `255 × dt / 66 ms` (one logic step
  for a full-range change). Instant pops become short ramps. The game's own
  buffer is never modified. It snaps to the game's cells on a new map, a grid
  size change, or a gap over 250 ms.
- Resolved by signature: each upload's code from the fade call through the
  copy loop (the `×2`/`×4` row scale tells them apart). The fade timing block
  supplies the stamp (both operands must agree), rate and cap. Verified
  offline on TW 1.10, TW 1.9, KW 1.03 and KW 1.02. Static writes are now 51.

### User feedback (round 1) and round 2
The **grey transparent fog** now clears at the right speed, but still in
visible steps, at roughly 30 per second. That fog is the `+0x198` shroud
texture. Its cells are **ARGB4444**: RGB = `ftol(level × GlobalData
+0xCE4/+0xCE8/+0xCEC)` and alpha = `min(255, max(0, level−4) × 255/46)`,
truncated to 4 bits. A fog→clear fade therefore crosses only a few of 16
brightness steps, so it looks stepped at any FPS (at 30 FPS it matched the
frame rate, so it went unnoticed).

Round 2 gives that texture 8 bits per channel:
- **Format:** in its creator (`FUN_00807070`, also used on device restore),
  `PUSH 1Ah` (D3DFMT_A4R4G4B4) is patched to `15h` (A8R8G8B8).
- **Rows:** the shroud upload's row `memcpy` (width × 2 bytes) is replaced.
  Each texel is computed at 8-bit precision from the cell's shown level
  (`+0x3C`, clamped to GlobalData `+0xCF2`), using the game's own formula via a
  256-entry table rebuilt each frame. If repacking that texel to 4444 doesn't
  equal the game's cell (an unknown writer), the game's cell is widened
  instead (nibble × 0x11).
- **Fill:** the upload's whole-texture fill (`FUN_00813D90`, which also sets
  the one-texel border the rows never touch, when the texture is new or
  restored) is replaced by a 32-bit fill through the game's lock/unlock
  (`FUN_0074C91F` / `FUN_0074C9C3`).
- Only these two paths write the texture (all `FUN_0074C96D`/`FUN_0074C91F`
  lock calls in the terrain code were checked). GlobalData offsets are read
  from the converter's code. Static writes are now 54.

### Done when
Walking a unit into fog/shroud reveals the area as a smooth wave at 90 FPS,
with no visible steps; nothing else about the fog's look changes.

**Confirmed 2026-10-11** ("the shroud looks good now").

### Side finding: shadow blink when the shroud returns (vanilla, ignored)
When the shroud re-covers an area, objects are swapped for frozen ghosts
(`W3DGhostObject::snapShot`, TW 0x83B14F: copy each draw module's render
object, remove the live one from the scene, add the copy; `freeSnapShot`
0x8190CE and restore 0x80C503 undo it). Their shadow blinks for a frame or
two during the swap. The user confirmed it also happens at 30 FPS without the
DLL, so it's left alone.

---

## B9. Selection flash plays too fast

**Status:** done, confirmed 2026-10-11.

### Root cause (TW 1.10 VAs; KW identical)
The white "selected" flash is a `TintEnvelope` (0x50 bytes): `+0x04`
attack rate, `+0x10` decay rate (RGB per step), `+0x1C` peak, `+0x28`
current colour, `+0x34` sustain countdown, `+0x38` state (0 off, 1 attack,
2 decay, 3 sustain), `+0x3C/+0x40` pulse (uses the logic frame, fine).
- `play` (0x474C80): `rate = Δcolour / frames` (0x46F4F2 / 0x46F56C).
- `Drawable` flash (0x478A33), default: white, attack 1, sustain 1, decay 4,
  on `Drawable+0x194`. Other tints (0x478BA9: 30/30 frames) use `+0x194`;
  `+0x190` is a second tint.
- `update` (0x474D1F) does one step per call. It's reached once per **client
  frame** from `Drawable_updateDrawable`: `CALL` at 0x48363E (`+0x190`) and
  tail `JMP` at 0x478DF8 (end of 0x478BA9, `+0x194`). At 90 FPS every
  flash/tint ran 3× faster.

### Fix (implemented)
Both branches are redirected to `update_tint_envelope_scaled`:
- attack/decay: the six rate floats are scaled by 30/targetFPS for the
  call and restored. The end tests compare against the rate lengths, so
  phases still end within one (smaller) step.
- sustain: the countdown only steps on client frames where the synthetic
  30 Hz frame advances.
Resolved by the update's body signature plus a scan for exactly two
`E8`/`E9` branches to it; each site keeps its opcode.

**Repeated flashes.** `Drawable_updateDrawable` also flashes a drawable
again while its flash count (`+0x150`) is positive, whenever
`GameClient::getFrame() % [0xBEAE48] == 0` (white, attack 1, decay 4). That
interval is set once at startup to `client FPS / 2` = 15 frames (500 ms at
30 FPS, 167 ms at 90). The patch sets it to `targetFPS / 2`, so the interval
is 500 ms again. It's resolved from the `DIV` site and proved by its only
writer (`MOV EAX,[client FPS]; SHR EAX,1; MOV [interval],EAX`).

Verified on all four builds. Static writes are now 57.

### Done when
The selection flash looks the same length as at 30 FPS, but fades smoothly.

---

## B8. Secondary-object animations sped up

**Status:** closed 2026-10-11. The user found the Pitbull pods behave the
same in vanilla. Notes kept for reference.

User report: "secondary objects" animate too fast, e.g. the **Pitbull's
rocket pods** have an abnormally fast idle animation.
- `GDIPitbull.xml`: the `TruckDraw` has no idle animation (`AnimationState`
  is empty). The pods are the turret (`Turret TurretNameKey="Turret"`,
  pitch bone `B_ Gun`) with `TurretTurnRate 2000`, `TurretPitchRate 200`
  and idle scans every 1–5 s up to 90°. There is also a `SpotlightDraw`
  (`RefreshTime 2.0s`, `SweepTime 1.0s`) on bone `B_SPOTLIGHT`.
- The turret AI (`TurretAI`, idle/idle-scan/aim/recenter/hold states) is
  logic (15 Hz). Suspect: client-side easing of the drawn turret toward the
  logic angle by a fixed fraction per client frame, or a per-frame timer in
  a secondary draw module. Needs a precise description of the motion.

---

## B10. Minimap drag pans the camera at 30 Hz

**Status:** done; round 4 confirmed by the user as the best version
(2026-10-11).

- Clicking the minimap moves the camera there; holding and dragging pans it.
  `RadarImpl::InputHandler` (vtable TW 0xABF160) forwards mouse moves (slot
  2, 0x7A6248) to the active drag operation; `LookAtDragOperation` (vtable
  0xABF184) calls the radar look-at (0x7A6325) whenever the radar point
  changes. The look-at converts radar → world (0x7A617D, `thiscall(handler,
  &world, radar)`, `RET 8`) and calls `view->lookAt(&world)` (virtual +0x48
  on the view global, TW 0xBEAC50), which snaps the camera. The mouse moves
  arrive with the APT UI (~30 Hz), so panning stepped.
- Fix: both direct calls of the look-at (the drag-starting click in
  0x7A63DC and the drag move in 0x7A6475) go to a wrapper. If the previous
  move was more than 100 ms ago it snaps (click). Otherwise it starts a glide
  from the currently shown point to the new one, lasting the measured move
  interval (clamped to 10–60 ms). The glide is advanced once per rendered
  frame from the particle gate, which also runs while paused.
- **Round 2.** User: smoother, but choppy with small movements. Cause: the
  drag operation only calls the look-at when the radar point changes, so
  slow drags produce irregular events (gaps over 100 ms were treated as
  clicks and snapped), and each glide stopped after at most 60 ms. Now the
  two callers are told apart by code, not timing: the move call sits at
  +0x26 of the mouse-move handler (prologue `56 57 8B 7C 24 0C F3 0F 10 07
  8B F1 0F 2E 46 0C`); the other is the drag operation's constructor (the
  click). Click: snap. Move: set a target. Every rendered frame the shown
  point closes `1 − exp(−dt / 30 ms)` of the distance and settles within
  0.5 world units.
- **Round 3.** User: better, but floaty (any smoothing lags). The radar
  point is normalised 0..1 per axis and clamped by the converter
  (0x7A617D: `world = map(point × 128)`, pure). Every real update (click
  or move) is still looked at immediately and becomes the anchor (point,
  `GetCursorPos`); consecutive moves teach minimap units per pixel per
  axis (first sample, then 25% EMA; samples off by more than 2× are
  ignored; moves under 4 px skipped). Every rendered frame while a mouse
  button is held, the point is predicted as anchor + scale × (cursor −
  anchor cursor), clamped to 0..1, converted and looked at. Releasing the
  button ends prediction, so the cursor never pans the camera afterwards.
- **Round 3b/3c (dropped).** Cursor prediction stair-stepped: the UI samples
  the cursor before the move reaches the look-at, so snapping to each real
  point pulled the camera back every update. A variant that only trusted
  resting samples still didn't feel right; the user asked to revert.
- **Round 4 (current).** As round 2 (click snaps; moves set a target; the
  shown point follows `1 − exp(−dt / τ)` per frame, settling within 0.5
  units) with τ = 18 ms instead of 30 ms: each game step is eased, and
  is mostly finished before the next one (~33 ms) arrives.
- Kane's Wrath has two radar look-at copies (two radar implementations), so
  the resolver accepts one or two, each with exactly two direct CALLs and the
  same view global. Patch capacity raised to 96 (KW now installs 63 writes).

---

## B11. Queued-order (Alt) waypoint lines step behind the unit

**Status:** implemented 2026-10-11, needs an in-game test.

- With Alt, queued move/attack orders are shown as tan lines (texture
  `EXLaser`, node model `SCMoveHintSml`) from the unit through each order.
  TW 1.10: the per-frame draw (0x834AAD) scrolls the texture by
  `timeGetTime`, then for each selected unit (or all, by mode) calls the
  per-object callback 0x833902, which calls the line builder 0x833699
  (`thiscall(buffer, Object*, ctx)`, `RET 8`). The builder copies the start
  point from `Object+0x38/+0x3C/+0x40`, the **logic** position, which only
  changes when logic updates it, while the unit is drawn interpolated.
- Fix: that call is redirected to `build_waypoint_line_from_drawn_unit`. If
  `Object→Drawable` (TW `+0x104`, KW `+0x108`) round-trips via the
  Drawable's Object back-pointer (`+0x140`), the Object's position is
  temporarily set to `Drawable::getInterpolatedTransform` (TW 0x48187A;
  translation at matrix floats 3/7/11), the builder runs, and the position
  is restored.
- Resolver: the callback's bytes (call at +34); the builder's prologue,
  whose three `MOVSS XMM0,[ESI+n]` must be consecutive (n = 0x38); the
  interpolation function (back-pointer offset at +5); and the particle-attach
  code `MOV EDI,[ESI+off]; …; MOV ECX,EDI; CALL getInterpolatedTransform`
  for the Object→Drawable offset. Verified on all four builds.

---

## F1. Beyond 90 FPS (120 / 144 / 160+)

**Status (2026-10-11):** works; the user runs 157 FPS (matching their
driver cap below the 160 Hz refresh) with no stutters seen. Implemented for any rate from 91 to 240 FPS
(option (b) below, fractional scheduling); first target 160 FPS, needs an
in-game test. B3 round 7 (resampled trails, pause freeze) was confirmed
before this started.

### How it works (TW 1.10 VAs; KW identical apart from the prologue)
- `GameEngine_update` (0x54B0CE) runs once per client frame: `phase++`;
  if `phase < 7` it calls `this->DispatchLogicPhase(phase)` (vtable slot
  0x94), else it wraps to phase 1 and dispatches that (the new logic tick:
  network/lockstep checks; a stall clears `GameClient+0xD4` and the phase
  reverts). So stock always runs at least one phase per frame: max 6 frames
  per tick = 90 FPS.
- `DispatchLogicPhase` (TW 0x55D866, KW 1.03 0x5C3B55) is reached through
  the `GameEngine` and `Win32GameEngine` vtables. Both entries now point to
  `dispatch_logic_phase_scheduled`. Above 90 FPS each tick gets
  F = 10/11 frames by Bresenham (`acc += target; F = acc / 15`). The schedule
  is: frame 1 → phase 1 only (above ratio 6, `IsClientFrameBoundary` is
  `phase == 1`, so phase 1 must not repeat); frames 2..F−1 → phases 2..5
  spread evenly (`2 + 3(j−2)/(F−3)`); frame F → phase 6 (otherwise the
  engine would wrap a frame early). On a frame without a phase, the wrapper
  undoes the engine's `phase++`.
- Interpolation (`GameEngine+0x48`) = j / F, written by the wrapper and
  by the interpolation hook, so units move every frame.
- `is_client_slice_end` returns TRUE for ratio ≥ 6 (every phase is its own
  slice); the stock batch block is skipped by the engine itself at ratio ≥ 6.
- Limiter: the interval is computed in fifteenths of a frame,
  `(budget × 15 + rem) / target`, so 160 FPS averages 990/160 =
  6.1875 ms (integer ratios are unchanged). If multiplayer adaptive pacing
  lowers the multiplier, `multiplier × 15` is used instead.
- B4 interpolation window = 3 × ceil(target / 15) (33 at 160).
- Config accepts 45/60/75/90 or any value from 91 to 240.
- Offline simulation of the update loop + wrapper: 160 FPS → 45 ticks per 480
  frames (exactly 15 Hz), phases in order, phase 1 on exactly one frame per
  tick, interpolation evenly spaced; 120 and 144 also exactly 15 Hz.

### Crash on first 160 FPS run (fixed 2026-10-11)
Access violation in the DLL (Windows Application log, event 1000, fault
offset 0xAB4D), found by disassembling the shipped DLL with capstone. In its
wrap path `GameEngine_update` does `MOV ECX,ESI; CALL UpdatePhaseInterpolation;
MOV EAX,[ESI]; PUSH EBX; CALL [EAX+0x94]`. It relies on the stock leaf
keeping ECX (the leaf only touches XMM0/XMM1). The original author's C
replacement (reached by a JMP at the leaf's entry) clobbered ECX. At 90 FPS
the original dispatch never used `this`, so it went unnoticed; our dispatch
wrapper writes through it → crash. The leaf entry now jumps to a naked stub
that saves EAX/ECX/EDX and XMM2–XMM7 around the C function.

### Known limits
- Logic is still tied to frames: if the PC can't sustain the rate, the game
  runs slower (as retail does below 30 FPS).
- Pacing is still `timeGetTime` + `Sleep(0)` (1 ms steps: 6/7 ms frames
  at 160). If that judders, a QPC limiter is the next step.
- Multiplayer: `IsClientFrameBoundary` (`phase == 1`) drives the network
  adaptation; the schedule keeps phase 1 to one frame, but MP is untested.

### Blockers found in the current code
1. **Config validation** ([config.c](../src/config.c)): only 45–90 in steps of
   15 are accepted.
2. **6-phase scheduler.** `get_phase_batch_limit`, `update_phase_interpolation`
   and `is_client_slice_end` assume `R ≤ 6`, i.e. every client frame
   dispatches **at least one** of the 6 logic phases. At `R > 6` (anything
   above 90) some client frames must dispatch **zero** phases and only
   render. We need to verify in Ghidra that the stock dispatch loop allows
   that (the batch block "increments EDI first", which suggests it always
   runs at least one phase). If it doesn't, the hook must skip the dispatch
   entirely on render-only frames.
3. **Interpolation resolution.** At `R > 6`, Drawable interpolation must
   advance on every client frame (`client_slot / R`), not only when a phase
   runs. Otherwise frames between phases repeat the same pose and the extra
   FPS gains nothing visually.
4. **Integer ratio assumptions.** `ENGINE_NOMINAL_CLIENT_FRAMES_PER_LOGIC_TICK`
   and `ENGINE_PACING_UPDATE_MULTIPLIER` are integers, and `ratio = fps / 15`
   truncates. So 144 (R = 9.6) and 160 (R = 10.67) need either:
   - **(a)** multiples of 15 only: 105, 120, 135, 150, 165, 180. This is
     simplest; 165 or 150 with VRR/G-Sync covers a 160 Hz monitor reasonably
     well. Or
   - **(b)** fractional scheduling: alternate 10 and 11 client frames per
     logic tick (Bresenham) so the long-term rate is exactly 160. Or
   - **(c)** fully decoupled, time-based rendering: render as fast as
     allowed and compute the interpolation α from wall-clock time within the
     current logic tick. This is the "modern engine" approach and the most
     flexible, but it means replacing the pacing model rather than
     subdividing it.
   Recommendation: do (a) first to prove the scheduler works above 90, then
   evaluate (b) or (c) for 144/160.
5. **Frame pacing precision.** The stock limiter uses `timeGetTime` + `Sleep(0)`
   at 1 ms granularity. At 160 FPS a frame is about 6.25 ms, so ±1 ms of
   jitter is ±16%. The original author removed a QPC-based limiter on
   purpose (to match retail audio cadence). Above 90 FPS a QPC/waitable-timer
   limiter is probably needed again (or rely on VSync/VRR). The existing
   Bresenham audio/keyboard gating should keep audio at retail cadence
   either way.
6. **Every accumulator and rate constant must be re-checked** for larger
   `target_fps` and for non-multiples of 15: W3D clock remainder, limiter
   remainder, `powf` exponents, `get_retail_visual_frame` (overflow-safe
   already), and the radius-cursor frames/ms.
7. **CPU headroom.** This is a 32-bit 2007 engine. Large battles may not
   reach 160 FPS. The engine's adaptive `pacing_update_multiplier` must keep
   degrading gracefully (lowering the effective rate) without speeding up or
   slowing down the simulation.

### Interaction with the bug fixes
B2 and B3 fixes must be written against a general `target_fps` (exponents
`30/target`, accumulators), not tuned for 90, so they keep working above 90.

### Done when
120 FPS works in campaign and skirmish with the logic still at exactly 15 Hz
(a game clock or stopwatch over 5+ minutes matches stock), audio and keyboard
cadence match retail, and no B1–B3 regressions appear. Stretch: 144 and 160
(or arbitrary/uncapped) with stable frame pacing.

---

## Non-goals / out of scope (for now)
- Multiplayer stability. Upstream already lists it as unstable at high FPS.
  Our changes must not make it worse, but we won't be fixing desyncs.
- Red Alert 3 port.
- Changing the 15 Hz simulation rate.

## Answered questions (2026-10-11)
- B1 occurs in every mission. B1/B2 were only observed at 90 FPS; whether
  they scale with FPS is still unknown, so this is the first thing to test.
- The DLSS mod has no frame generation and is currently off.
- Kane's Wrath stays supported. It differs only slightly from TW, so TW
  changes should work there too (still verify with the offline resolver
  check).

## Open questions
- Which units show B2 most clearly (wheeled, tracked or hover)?
