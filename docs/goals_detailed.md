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
  `update_phase_interpolation` in [game_patches.c](../src_fps_patch/game_patches.c)).
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

### Bug: bullets kept moving while paused (fixed 2026-10-12, needs a test)
`W3DTracerManager_Update` (TW 0x834270) has no pause check. It moves tracers
only when the (now 30 Hz synthetic) client frame changes, and that frame
stops while paused. On the frames in between, `tracer_manager_update_every_frame`
called the movement pass (0x832B39) unconditionally, so tracers kept flying
during a pause. The in-between pass now runs only when `particles_simulate()`
(client simulating and logic not paused), the same gate the CPU particles
use.

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

**Status:** done, confirmed by the user 2026-10-12.

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

## B12. Tiberium crystals grow and shrink in steps

**Status:** closed 2026-10-12, kept vanilla. The smoothing below worked
("it animates correctly now"), but made the crystal glow flicker (see
"Why it was reverted"); the user chose vanilla and the code was removed.

### How it works in the game (TW 1.10 VAs)
- Crystals (`Xml/Tiberium/TiberiumCrystal.xml`) use a `ScriptedModelDraw`
  with `AnimationMode="MANUAL"` (TibCell01) and a `TiberiumCrystal` behavior
  (`GrowthStages="3"`, `GrowthTimePerStage="3s"`).
- `TiberiumCrystalBehavior::update` (0x5E607B, update-module vtable
  0xA811EC slot 0) is a logic module. From the logic frame
  (`[0xBEE574]+0x38`) it computes growth progress (stage / stages +
  elapsed / duration / stages) or harvest progress, and hands the pair
  (x, 1 − x) to two Drawable setters:
  - 0x47281C: for each draw module (Drawable +0x184 list), interface
    (+0xC8) → +0x6C(float). Three calls (0x5E6167, 0x5E61A8 = 0 at the end,
    0x5E62CD).
  - 0x472D74: for each draw module, +0x4C(float). On W3DScriptedModelDraw
    this stores the value (+0x2F4) with the current client frame (+0x2F8).
    Two calls (0x5E61B5 = 1.0 at the end, 0x5E62E7).
  - A fourth caller of 0x47281C (0x5FA32F) is another behavior and is left
    alone.
- So the crystal animation moved once per logic update.

### Fix (tried, removed)
- The 5 setter calls go to `set_tiberium_anim/second`, which only record the
  new value per Drawable (hash table, 2048 slots, rebuilt without idle
  entries when 3/4 full). Gaps over 400 ms, jumps over 0.25 and new crystals
  snap (the original setter is called at once).
- `Drawable::updateDrawable` calls a per-frame helper with
  `MOV ECX,ESI; CALL` (0x483645 → 0x4809DB). That call now goes to
  `update_drawable_with_tiberium`: the original helper, then, for a Drawable
  with an active tween, the eased value (from the last shown value to the
  new one over the measured time between the last two updates, 10–200 ms)
  through the original setters. Values are only written from the Drawable's
  own update, so a crystal destroyed by harvesting is never touched.
- The table is cleared at each session start.
- Resolver `resolve_tiberium_sites`: both setters by signature, the update
  function by its prologue, exactly 3 / 2 CALLs inside its first 0x300
  bytes, and the updateDrawable site by the tint-envelope/Object-check
  pattern. Verified on TW 1.10, TW 1.9, KW 1.03 and KW 1.02.

### Why it was reverted
- The "second" setter is not visual for crystals: on the W3D object it
  snapshots the current transform (+0x2C4) with a blend weight (+0x2F4) and
  stamps the client frame (+0x2F8); the draw (TW 0x850FFE) then lerps
  snapshot → current transform (0x760DDA, position lerp + slerp) while
  adding 1/30 per **client frame** up to 1. A static crystal never moves, so
  this is invisible (but it is a client-frame-paced ramp, 5× fast at 156).
- The brightening is the crystal shader's random glow
  (`Shaders/Tiberium.fx`, "Random glow for tiberium crystals"): phase and
  speed come from `GetRandomFloatValue(..., VertexColor.w * 255 +
  (objectCenterWorld.x + objectCenterWorld.y) * 100)`, where
  `objectCenterWorld` is the first bone's world position. Any bone movement
  (tiny steps cross random-table bins) re-rolls the glow. In vanilla the
  growth animation moves 15×/s, so the glow re-rolls 15×/s ("brighter in
  small intervals"); smoothed, it re-rolled every frame and flickered.
  Smooth growth and the vanilla sparkle rate are mutually exclusive.
- Options offered: 30 updates/s, vanilla, or fully smooth. User chose
  **vanilla** (2026-10-12).

---

## B13. Repair / power-off icons "breathe" in 15 Hz steps

**Status:** closed 2026-10-12, kept vanilla. The crossfade below worked but
the user didn't like the look ("they don't look that good"; 15 images/s is
fine), so the code was removed.

### How it works in the game (TW 1.10 VAs)
- `Xml/GlobalData/UnitOverlayIconSettings.xml`:
  `PowerIconImageSequence="PowerIconAnim"`,
  `RepairIconImageSequence="RepairIconAnim"`. `ImageSequences.xml` lists 10
  pre-rendered images each (`…0002` to `…0020`).
- The settings live at `[0xBEAC48]` (+0x18 power, +0x1C repair sequence).
  The building icon draw (0x4816BC) picks each sequence's current image with
  0x46EA8B (EDI = sequence; returns the Image):
  `i = (rate × GameClient frame / clientFPS) mod 2n`, mirrored on the way
  back. `rate` is a global (0xB7EDAC) = **15 images per second**. The speed is
  right at any FPS (it uses the client FPS global), but it only changes 15
  times a second.
- Each icon is drawn with `Display::drawImage` (W3DDisplay vtable 0xAC7E94
  +0xE4 = 0x80F882: image, x0, y0, x1, y1, ARGB colour, mode) at one
  `CALL [EDX+0xE4]` (0x481860), colour 0xFFFFFFFF, mode 2.

### Fix (tried, removed)
- Both picker calls go to a naked stub (`pick_overlay_icon_entry`: push EDI,
  call C) that computes the position on the continuous 30 Hz client clock
  (`g_retail_whole + g_retail_remainder / g_rate`), picks the same image as
  the game and remembers the next one plus the blend fraction.
- The draw call goes to `draw_overlay_icon` (fastcall = thiscall, pops 28
  bytes): it draws the picked image (fading out over the second half of the
  step) and then the next image on top (fading in over the first half), via
  the colour's alpha, so one of them is always fully opaque.
- With the patch inactive (target ≤ 30) the stock integer formula is used.
- Resolver `resolve_overlay_icon_sites`: the picker by signature (its DIV
  operand must be the client-FPS global), exactly 2 callers, and exactly one
  `FF 92 E4 00 00 00` within 0x200 bytes after the second. Verified on all
  four builds (rate 15 everywhere).

---

## B14. Summoned objects fade in too fast (Mothership)

**Status:** implemented 2026-10-12, needs an in-game test.

### Cause (TW 1.10 VAs)
- `OCL_SummonMothership` (`Xml/GlobalData/ObjectCreationLists.xml`) creates
  the Mothership with `Options="... FADE_IN"` and `FadeTime="15s"`.
- The CreateObject routine (0x63D6B9) converts FadeTime to **logic frames**
  (`FUN_0046D567`, 15 s → 225) and calls `Drawable::fadeIn(frames)`
  (0x46E933; fade-out 0x46E8E1) at 0x63DF48 / 0x63E061 (in) and 0x63DFBA (out),
  each `PUSH [EBP+0x58]; MOV ECX,[obj+Drawable]; CALL`.
- The fade (`FUN_004730DB`, per frame from updateDrawable) adds elapsed
  **client frames** (+0x21C) and sets opacity = elapsed / frames (+0x220).
  Vanilla at 30 FPS: 225 frames ≈ 7.5 s; at 156 FPS ≈ 1.4 s.
- Other fade callers: several convert seconds with the live client FPS
  (correct), and a few pass small constants (10, 30, 138 frames: 0x4D3CBB,
  0x6001A4, 0x570D45, 0x6383F2) that are also ~5x fast but short. Those are
  not changed (not reported).

### Fix
- The 3 OCL fade calls go to `summon_fade_in/out`: frames × g_rate / 30, so
  the fade keeps vanilla's duration and still advances every frame.
- Resolver `resolve_effect_timing_sites`: fadeIn/fadeOut by signature; the
  OCL calls are the only fade calls preceded by `FF 75 58 8B <ECX,[reg+0x1xx]>`
  (Drawable offset 0x104 in TW, 0x108 in KW): exactly 2 in + 1 out within
  0x200 bytes. Verified on all four builds.

---

## B15. Tiberium Vibration Scan highlight lasts 60+ s and pulses at 15 Hz

**Status:** done (crash fixed; confirmed in-game 2026-10-12).

### Cause (TW 1.10 VAs)
- `RadarScanMapSpecialAbilityUpdate` (Scrin Control Center): `FUN_00632F4F`
  converts the scan duration from logic frames to client frames with the
  live client FPS (`× [clientFPS] / [logicFPS]`) and calls `FUN_005C5905`
  per matching object, which sets the Drawable pulse (flag 0x20 at +0x158;
  parameters at +0x198..+0x1B4 via `FUN_0046F38B`): yellow (1, 0.8, 0),
  attack = decay = `client_fps_float` frames (1 s at the live rate),
  sustain = duration, pulse amplitude 1.0, frequency 0.5.
  `FUN_005CBF9D` is a second producer of such pulses (also live-rate frames).
- `FUN_00478BA9` (per frame from updateDrawable) starts it with
  `TintEnvelope::play(colour, attack, decay, sustain)` (0x474C80, call at
  0x478D38) on the +0x194 envelope. B9 steps envelopes as if their counts were
  30 Hz frames (sustain counts down per 30 Hz frame, attack/decay rates ×
  30/rate), so a 13 s highlight lasted 13 × 156/30 ≈ 67 s.
- The heartbeat: the envelope colour getter (0x46F478) returns
  colour × (0.5 + sin(logicFrame × f) × a / 2), using the GameLogic frame
  (`[0xBEE574]+0x38`), so it changed only per logic tick.

### Fix
- The play call goes to `play_tint_pulse_in_retail_frames`: attack, decay
  and sustain × 30 / `client_fps_float` (≥ 1; huge "forever" values kept).
  It goes through the naked stub `play_tint_pulse_entry`, which keeps EDX.
  The first build crashed on the first scan (access violation at TW
  0x46CCFB): the caller does `MOV ECX,EDX` after the call, relying on the
  stock play leaving EDX (the colour pointer) intact.
- Both getter calls (0x472F97, 0x472FD7) go to `tint_pulse_colour`, which
  uses a logic frame that advances smoothly between updates (one update
  behind; the tick length is measured; it stops when the logic pauses).
- Signatures: the play site (`PUSH [ESI+…]` ×3, `LEA EDX`, `CALL`) and the
  getter (its GameLogic operand must equal the particle gate's). Verified on
  all four builds.

---

## B16. Rocket smoke trails start behind the rocket

**Status:** implemented 2026-10-12, needs an in-game test (screenshots:
GDI missile soldiers in mission 1).

### Cause (TW 1.10 VAs)
- The missile (`GDIMissileSoldierMissileProjectile`) has a `ParticleSysBone`
  with `FollowBone="true"` and the system `OrcaMissileTrail`: `Type="TRAIL"`,
  one particle, `ParticlesAttachToBone="true"`, `TrailLife="90"`.
- `ParticleSystem::update` (`0x5B8D6A`, run only by the gated 30 Hz update):
  - reads the attached drawable's interpolated transform;
  - stores the system position (+0x14, previous at +0x20);
  - recomputes the world matrix (3×4 at +0xCC) from the local one (+0x9C).
- The attached particle's physics step (`0x5B3C05`, flag +0xD) sets the
  particle to that matrix's translation (`0x5AD464`: +0xD8/+0xE8/+0xF8). The
  trail storage update (`0x5C09EA`) then pushes the particle position as the
  newest point (`0x5BB5E8`).
- So the newest point is the rocket's position at the last 30 Hz step,
  while the rocket is drawn where it is now. Our trail wrapper (B3) grew the
  head from the previous point *toward* the newest one, so the drawn head
  trailed the rocket by about one step of travel. At 30 FPS every frame is a
  step, so it connects.

### Fix
- For a live, non-stopping trail whose newest point is bitwise equal to the
  system's world translation (only bone-attached trails), the visible curve
  gets one extra point: the newest point plus the last segment. The head
  ends at `points − 1 + α` on it, i.e. projected forward by the elapsed
  fraction (like CPU particles).
- The next step's point lands where the head already is, so nothing jumps.
- Free-flying trails (repair sparks, which decelerate) keep the old
  grow-toward behaviour, so they don't overshoot.
- Signature: the attach function (`8D 81 CC 00 00 00 …`), unique in all four
  builds.

---

## B17. Ion Cannon crash (mission 1, TW 1.9 mod)

**Status:** implemented 2026-10-12, needs an in-game test.

### Cause
- Crash dump (`%LOCALAPPDATA%\CrashDumps\cnc3game.dat.8784.dmp`, copied to
  `temp/crash_ion/`): an access violation reading `0xB1` in our DLL
  (`+0xCF05`).
  - Symbolised with the release PDB: `draw_trails_interpolated`, at
    `MOV EBX,[EDI+8]`, the start of `trail_point_count(record)`, with
    record = `0xA9`.
  - The deployed DLL's code was byte-identical to the build used to
    symbolise it.
  - Tools: `tools_and_docs_by_claude/crash_dump.py` and a PDB
    `S_GPROC32`/`S_LPROC32` scan.
- The stock `TrailDrawModule_Draw` (`0x5BB4D3`) never reads
  `system+0x84` directly:
  - it calls the storage's **slot 0x2C**, "as trail storage", which
    returns `this` for `TrailSystemStorageModule` and NULL for every other
    storage class;
  - it only draws when the flag `0xBF4DCD` is set, which the trail
    begin-draw (`0x5B12D2`) sets only when that cast succeeds.
- So a trail draw module on a non-trail storage, which the mod's Ion Cannon
  effect evidently has, draws nothing in vanilla. Our wrapper read the
  wrong object's fields as trail records and crashed.
- The swarm wrapper had the same latent bug: the stock swarm builder
  (`0x5B4D8C`) casts through **slot 0x28**.

### Fix
- `particle_storage_as(system, slot)` makes the game's own virtual cast.
- The trail and swarm wrappers call the original untouched when it returns
  NULL.
- Signatures:
  - the trail-draw pattern now includes `FF 50 2C 8B F0`;
  - the swarm vertex builder pattern pins `FF 50 28`.

  Both are verified unique on all four builds.

---

## B18. Aircraft micro-stutter at 155 FPS

**Status:** implemented 2026-10-12, needs an in-game test.

### Cause
- At 155 FPS a logic tick lasts R = 155/15 = 10.33 frames. Bresenham gives
  ticks F = 10, 10, 11, … frames.
- The interpolation fraction was j / F, so a frame advanced 1/10 of a tick
  in some ticks and 1/11 in others. Everything moved ~10% slower for one
  tick in three: a rhythmic speed wobble, most visible on fast, evenly
  moving aircraft.
- Stock multiples of 15 (R whole) were unaffected.

### Fix
- `write_tick_interpolation` now writes `(15·j − carry) / rate`. `carry` is
  the Bresenham remainder before the tick (`g_tick_carry`, fifteenths of a
  frame), i.e. how far the tick's ideal start lies before its first frame.
- Every frame advances exactly 1/R, including across tick boundaries. The
  value stays in (0, 1] (j ≥ 1, carry < 15, 15·F ≤ carry + rate) and is
  clamped anyway.
- Check (`temp/interp_sim.py`), per-frame step:

| Rate | Old step | New step |
|---|---|---|
| 155 | 0.0909–0.1000 | 0.0968 |
| 50 | 0.25–0.33 | 0.30 |

  Every rate from 16 to 240 now matches the ideal 15/rate to within 1e-12.
  `temp/scheduler_sim.py` still reports 0 errors (phase order and ticks are
  unchanged).
- Particles already used the continuous form (accumulator / rate).

---

## B28. Occasional crash while the game starts (2026-10-14)

**Status:** watching (not caused by the patch as far as the evidence goes).

- Two crashes with the identical stack (10/13 13:47 with an older DLL,
  10/14 15:50 with the current one), both while the shell/menu loads,
  ~1 s after the patch installed (before the first adaptive-pacing line).
  Dumps in `temp/crash_1014/`. Sessions in between started fine.
- Fault `cnc3game.dat+0x10A4E5` (TW 1.9 `0x50A4DA`): removing an observer
  from a global list (`0x44954D` → list at `0xBEDD78`) walks nodes via +4 and
  reads a garbage node: `EAX = 0xFFFFFFFF` (10/14), ASCII `"_lic"`/`"-tf"`
  (10/13). Caller `0x44A1B3` = destructor of
  `GlobalResourceManagerStreamObserver` (vtable `0xA33520`), constructed
  on the stack (`lea ecx,[ebp-0x64]` at `0x5B2F4C`) in the asset-streaming
  code (`0x5B2E6A` ← `0x5B9299`).
- Reading: an earlier observer stayed in the list after its stack frame was
  gone (e.g. a load that bailed out without its destructor), so the list
  points at reused stack memory (strings). No patch code is on the stack or
  touches the resource manager; the system dinput8's frame on the stack is
  not ours.
- If it gets frequent: launch several times without the DLL to see whether
  vanilla+Firestorm does it too.

## B27. Huge stretched geometry: the Reckoner, not the Beam Cannon (2026-10-13/14)

**Status:** diagnosing.

- **Report:** stretched geometry again, the user thinks with Firestorm's Beam
  Cannon (`LaserDraw` ×4 with `Laser.fx`, beams from
  `BeamSpecialAbilityUpdate`, `JoinWithOtherBeams="true"`).
- **Laser renderer (TW 1.10):** `W3DLaserManager` (vtable `0xACD7D4`) keeps
  a pool of 256 lasers (+0x5C24 count, +0x5C28 array); a laser
  (`0x86BD92`) stores its texture/scroll data and its start frame (+0x50).
  Lasers are grouped per shader (0x40C-byte records: count + 256 pointers);
  the render (`0x86C815`) gathers a list into a 256-entry stack array and
  `0x86BF51` writes 4 vertices (64 bytes each) per laser into a VB of
  1024 vertices (`0x86C5DD`: `FUN_0073A559(0, 0x400, 1, 0x40)`), indices for
  256 quads. Bounded by the pool, so **no overrun**.
- **Found instead:** `0x86BF51` uses `(frame − start) × 1/30` as seconds
  for the texture scroll and texture-frame animation, so beams scroll ~5×
  too fast at 155 FPS (visual only). A fix would redirect the two frame
  reads (`0x86BD92`, `0x86BF51`) to the synthetic 30 Hz frame.
- **Screenshot (round 1):** a huge soft-edged orange wedge with one corner at
  the Beam Cannon's barrel and the normal beam gone. That is a laser quad
  whose end point is garbage (behind the camera or enormous): the glow
  texture stretched over the screen. (The minimap flash was the "unit under
  attack" indicator; the target was **alive**, so "target died" is ruled
  out.)
- **Where the end point comes from:** `W3DLaserDraw` update (`0x8467C3`,
  once per new client frame) takes start/end from the object's beam
  interface (`0x83E231`, behaviour virtual +0x34 → getStart/getEnd) or its
  `LaserStateModule` (`0x83E1CA`, start +0x2C, end +0x38; active +100):
  logic data, not touched by the patch. Open suspicion: above 90 FPS frames
  are drawn between logic phases (vanilla draws only after phase 3 or 6),
  so the draw could read a beam's points mid-update. Not proven; the
  diagnostic below records the phase of each odd frame.
- **Guard `laser_guard` (implemented 2026-10-13, needs a test):** the stock
  update-and-copy (`0x846877`, 8-byte entry `JMP`) is reimplemented in C
  (`laser_draw_update`): same update/create calls, then a point with a
  non-finite coordinate or one beyond ±100,000 collapses the laser to its
  valid end for that frame (logged, max 20 lines per session). Resolver:
  the whole function (identical in all four builds; TW 1.10 `0x446877`,
  TW 1.9 `0xB2826`, KW 1.03 `0x3FB72A`, KW 1.02 `0xB2A30` RVAs).
- **Round 2 (2026-10-14, video `temp/videos/…3-42-27 PM.mp4`, frames in
  `temp/videos/frames/`):** the user found it's the **Reckoner** (Firestorm
  Nod Reckoner / Reckoner Deployed): a wide, dark, translucent band from the
  deployed Reckoner to the top of the screen, visible for many seconds
  (≈7–40 s and again at the end) and **lit up by the selection/attack
  highlight**, so it is the unit's own mesh (stretched skinning). The
  orange screen-filling plane (17–22 s) is likely the same thing seen from
  another angle. The laser theory is dropped (the guard stays as a harmless
  safety check).
  - Firestorm `GameObject:NODReckonerDeployed` (strings from the compiled
    asset, `temp/GameObject_NODReckonerDeployed.bin`): unpack animation
    `ONCE` while `UNPACKING`, then `MANUAL` + `START_FRAME_LAST` (held on the
    last frame); rooftop TV/speakers (`FX_ROOF_TV` at `BONE_ROOF`, `LOOP`,
    `MAINTAIN_FRAME_ACROSS_STATES`); no turret; recoil settings but no
    `WeaponRecoilBone` (`BONE_EXTB*` are damage-FX particle bones). So B21,
    B22 and B23 don't apply.
  - Suspicion only: a held/looped animation sampled past its last keyframe
    (garbage bone transforms). Next: is it vanilla (DLL off, 30 FPS,
    deploy/undeploy several Reckoners)? If not, bisect with
    `debug_disable`; since the band persists, a live process with the glitch
    on screen can be inspected (`hang_sample.py`-style reads).
- **Round 3 (2026-10-14):** the user saw it again (also near Firestorm's
  Scorpion Tank) and paused the game with the band on screen (PID 9092,
  TW 1.9). They had confirmed it does not happen at 30 FPS without the DLL
  (one run). The video frames show the band starting exactly when a green
  Tiberium spray (a `W3DStreamDraw` stream) starts. The band is a
  **constant-width, dark, see-through strip lying on the terrain** from the
  unit to off-screen. Everything below was checked read-only on the paused
  process (scripts in `temp/b27_live/`) or in Ghidra (TW 1.10), and is
  **ruled out**:
  - **Streams / my B26 `stream_rate` fix.** Each stream owns its vertex
    buffer (`0x87411F` → `DX8VertexBufferClass` `0x73A559` → `0x7370D9`:
    `CreateVertexBuffer(DYNAMIC|WRITEONLY, DEFAULT pool)`), locked with
    `DISCARD` (`0x5AE7E7` → `0x731687`, `IDirect3DVertexBuffer9::Lock`) by
    the fill `0x87351C`. D3D9 keeps that data between draws, so drawing it
    at 155 FPS while it's rebuilt at 30 Hz is fine. `W3DStreamManager`
    (vtable `0xACD8BC`) draws every stream each frame in one of two passes
    (`0x86CE3B` / `0x86CE6B`, the byte at stream+0x54 picks the pass;
    draw `0x87338A`) with (points − 1) × S quads. The geometry builder
    `0x8748E8` writes exactly one 0x18-byte entry per point (position from
    the age curve `0x873086`), and the fill builds rings from the frame
    (dir × up, dir, (dir × up) × dir), so every vertex stays within the
    radius (+0x48) of its point. Expiry `0x874489` only drops points whose
    age (+0x4C) left 0..1, and ages only move in the 30 Hz rebuild
    (`0x873452`). Live: 6 streams, 36/36 points/geometry entries (one 6/6),
    largest step between neighbouring points 30 units, no NaN.
    (`stream_geom.py`)
  - **Skinning.** The 0x48-byte HTree pivot record is: +0 ?, +4 parent
    pointer, +8 local quaternion, +0x18 local translation, +0x24 posed
    quaternion, +0x34 posed translation (model space for skinned units, world
    for simple 2-pivot props), +0x40 index. All 1,711 skeletons have unit
    quaternions and no bone more than 400 units from the median, except one
    8-bone model with a bone 1,372 units straight up (an attachment, the
    same on two copies). (`bone_scan_tight.py`, `near_models.py`)
  - **Render-object transforms** (3×4 at +0x18): every placed HLod
    (`0xA3043C`, 1,330), mesh (`0xA310E4`, 3,602) and `0xA2F164` (898) is a
    pure rotation; no scale or shear. (`xform_scan.py`)
  - **Animation sampling past the end.** `HCompressedAnimClass` (vtable
    `0xAB453C`: frames +0x20, rate +0x28, channels +0x2C, 0x14 per pivot);
    the adaptive-delta decoder `0x733DF6` stops at the channel's frame count
    and returns the last value; time-coded `0x7357A0` uses the last key at
    the end. Both safe.
  - **`multicore_trail_fix`.** The patched builder is `TrailDrawModule`'s
    `0x5BA171` (missile/smoke trails, not streams). Re-checked the stub: at
    the read-back EDX = [EBP-0x10] = module data (same `[EDX+0xC]` scale as
    the first vertex), so it writes the identical V. Correct.
  - **Track marks:** 11 live tracks, all healthy, none near the Reckoner
    (a deployed Reckoner doesn't move). **Timed decal fades**
    (`0x855674`): fade by frame, can't stretch.
  - GPU-side buffers can't be read: DXVK (32-bit) unmaps them; the 9
    write-combined regions hold no world-space vertices while paused.
  - So the CPU-side data of every candidate is clean. The band is either
    GPU-side data corrupted by something else (an overrun like B26 into a
    neighbouring buffer), or Firestorm's own "random stretching geometry"
    (seen at 30 FPS, B24) made more frequent by the higher rate.
- **Next: bisect run** (`debug_disable` with every non-core feature except
  `stream_rate`, which is the B26 crash fix): if the band still appears, it
  is the game/mod running above 30 FPS (like B19), not one of our fixes.
- **Diagnostic `debug_laser_draws=N`:** logs laser frames longer than 3,000
  units or whose end point jumps more than 500 units, with logic phase,
  interpolation fraction and the shooter's position. If the glitch comes
  back without a "laser guard" line, the bad point was finite: the
  diagnostic shows its value and the phase.

## B26. Stream vertex buffer overrun → stretched polygons, crash, freeze (2026-10-13)

**Status:** done, confirmed 2026-10-13 (no more crashes).

- **Report:** the game froze (sound kept playing) ~10 s into play, again on
  Firestorm "Alexandra" with Tiberium Troopers; `multicore_verify=1` showed
  0 mismatches. Earlier: a crash (B25) and "random stretching geometry".
- **Freeze diagnosis (live process, `tools_and_docs_by_claude/hang_sample.py`,
  scripts in `temp/hang_1013/`):** no thread busy; a deadlock:
  - main thread owns the game allocator's critical section
    (`0x06B10988`, recursion 2) and waits in `EnterCriticalSection` for a
    subsystem lock (`[obj+0x4C]`, via `0x8031D6`/`0x84B66A`);
  - a worker thread (job `0x80E8C2` → … → `0x7FDB87` → allocator callback
    `[0xC179A0]` → `0x402EA6` → `0x8A0C14` → `0x89C7EF`) owns that lock
    and waits for the allocator.
  - The main thread held the allocator lock with no allocator frame on its
    stack: an exception record left in its stack (`0x1AE720`, context at
    `0x1AE770`) shows an **access violation in the allocator**
    (`0x8A2B0B`, segment walk reading a garbage size, ECX = segment
    `0x51930000`) that the game's top-level handler swallowed, leaking the
    lock.
- **What corrupted the heap:** the segment header at `0x51930000` (and 26 KB
  after it) was overwritten by **64-byte vertices** (position on the
  ground, normal, UVs with V increasing along a strip). They run from
  `0x518D4000` across the end of a 16 MB write-combined region
  (`0x50930000`–`0x51930000`, a DXVK-mapped vertex buffer) straight into the
  next allocation, the allocator's segment. Inside the mapped region the
  same overrun lands on other geometry: the stretched polygons.
- **Writer:** `W3DStreamDraw`'s stream (`StreamDraw::Stream`, TW 1.9):
  - constructor `0x4DFD80`: segments `S = clamp(data+0x14, 4, 32)`; the
    buffers (`0x4DF6C0`) hold `(S+1)·512` vertices of 64 bytes and 256
    strips of indices, sized once;
  - update `0x4E0002` (per client frame): frame = `GameClient` vtable +0x78;
    if it changed, `dt = Δframes / client-FPS float` (`0x4DEA05`: age +=
    dt / lifetime), **append one point** (`0x4DFC38` → `push_back`),
    recompute and refill. Points expire at age 1 (`0x4DFAC6`).
  - So a stream holds lifetime × client-FPS points: ~5× retail at 155 FPS,
    and long sprays (Corrupter Tioxin, used by Firestorm's Tiberium
    Trooper) overran the buffer.
- **Fix `stream_rate`:** both frame reads (update and constructor,
  `MOV EAX,[ECX]; CALL [EAX+78h]`) go through `get_retail_visual_frame` (the
  synthetic 30 Hz frame, as the cloud/Anim2D reads) and the update's
  `DIVSS [client-FPS float]` operand points at a constant 30.0. A stream
  then grows, ages and is rebuilt exactly as at 30 FPS (its shape updates
  at 30 Hz, as in vanilla). Resolver: update pattern (GameClient pointer and
  client-FPS float operand checked against the resolved layout) and
  constructor pattern; unique on all four builds (TW 1.10 `0x4749D9`
  `0x47488F` `0x4749EF`; TW 1.9 `0xE0016` `0xDFECC` `0xE002C`; KW 1.03
  `0x429FF1` `0x429EA7` `0x42A007`; KW 1.02 `0xE09FF` `0xE08B5` `0xE0A15`).
- Bisect name: `stream_rate`.
- **Sweep for similar code (2026-10-13):** every function that reads the
  GameClient frame number (vtable +0x78 after loading `TheGameClient`):
  95 in TW 1.10 (list in `temp/frame_readers_110.txt`, decompiled candidates
  in `temp/frame_candidates_110.txt`; TW 1.9 scan `temp/frame_readers.py`).
  - Only the stream appended per new frame into a buffer sized once. No
    other overrun risk found.
  - Most others are stamps, per-frame throttles, or already handled
    (particles, tracers, physics springs, Anim2D, drawable interpolation).
  - Frame-counted timers that would run faster above 30 FPS (visual speed
    only; not reported by the user, not changed): `FloatingTextDisplay`
    (`0x9F3DB1`, floating text rises/fades per frame), scripted camera
    moves (`0x82EBB7` state 3 → `0x809F08`, one step per frame),
    `W3DScriptedModelDraw` frame accumulator (`0x867572`, +0x2AC),
    blink helpers (`0x9EEDF7`, `0x9F0594`, `0x79A12D`), the +1/30 blend in
    `0x850FFE`, and `W3DProjectedShadowManager` fades (`0x855674`, frame
    stamps). If one of these shows up, retime it like the others.

## B25. Crash in the GPU particle storage (2026-10-13)

**Status:** explained by B26 (the stream vertex overrun corrupts the heap).

- **Event:** TW 1.9 + Firestorm 1.12, 155 FPS target with adaptive pacing,
  ~2,568 s into a mission (rates dropping 148 → 86 in a heavy battle).
  Windows log: `0xC000001D` (illegal instruction) at `0x1A576437` (no
  module), then an access violation in the game's own handler at
  `cnc3game.dat+0x4A0ED7`. Dumps copied to `temp/crash_1013/` (stacks only,
  no heap).
- **Chain (TW 1.9 VAs):** our `update_particles_at_retail_rate`
  (`dinput8+0xF13B`, named with `tools_and_docs_by_claude/pdb_syms.py`) →
  the manager's system loop (`0x7585FA`, list at manager +0x24) →
  `ParticleSystem` update `0x75803E` → `0x74CFF1` → storage
  `[system+0x84]` vtable +0x20 = `0x75C24E` (shared by
  `GPUParticleSystemStorageModule` and `TerrainFireSystemStorageModule`): a
  time-ordered queue whose expired front elements are deleted by
  `0x75B3F4` (`call [vtable]`, deleting destructor). The front element's
  "vtable" was `0x42DE9A4F` (the float 111.3): the element's memory had been
  freed and reused.
- **Ruled out:**
  - The GPU particle clock (`sync_ms × client_fps / 1000` at `0x75C257`;
    creation `sync_ms × 0.03` at `0x753A9F`): our patch already points the
    expiry operand at a constant 30, so adaptive rate changes can't make it
    jump (checked: nothing disabled, 82 patch writes installed).
  - Trail/swarm code (B16/B17): this storage's trail/swarm casts return
    null.
  - The M1 particle index: the loop walks the indexed list, but TW 1.9 has
    exactly 3 erase calls and 1 insert call to the list helpers, all
    hooked; a stale entry also has to pass "still linked" and "same key"
    checks.
  - Our wrapper calls the stock update 30 times per second, as vanilla.
- **Round 2 (user: Firestorm mission "Alexandra", while a Tiberium Trooper
  sprayed liquid Tiberium):**
  - Firestorm 1.12's spray effects: `XPCG_LiquidTibDistort` and
    `XPCG_LiquidTibSteam01` (GPU), `XPCG_LiquidTibSpark` (TRAIL),
    `NODTibTrooperContaminationZoneDecal` (CPU). No template in the mod uses
    `GPU_TERRAINFIRE`, so the storage was the GPU one.
  - GPU storage (TW 1.9 vtable `0xAB0B78`): +0x10 `0x75E855` adds a batch (a
    0x60-byte element, ctor `0x753A9F`, slot in a shared GPU page from
    `[0xC15A70]`, pushed onto the queue +0x18); +0x20 expires batches; first
    use `0x75E74F` shares a pool object (+0x14) with an initialized sibling
    of the same template (the search M1b narrows to the template's group).
  - Checked our writers on these paths: the CPU draw wrapper uses the
    storage's count/get, and the GPU storage's get always returns NULL (no
    writes); the trail wrapper only blends points the stock trim
    (`0x756115`: live = particle +0x40 ≠ 0, keeps TrailLife) leaves alone, and
    restores before anything is freed; M3a writes the same value to the
    same vertex address. Nothing of ours frees or writes a GPU batch.
- **Round 3 (user: "stretching geometry" seemed gone with
  `multicore_trail_fix=0`; the verify run's log was overwritten by a later
  launch, so no index data):**
  - M3a re-checked on TW 1.9 (builder `0x7592CB`, patch `0x75955D`): first
    vertex V = `cvtsi2ss [EBP-4] / [EBP-0x24] * [EDX+0xC]` with
    EDX = `[EBP-0x10]`; at the patch EDX is reloaded from `[EBP-0x10]`,
    `[EBP-4]`/`[EBP-0x24]` are unchanged, XMM0 is dead, no branch lands
    inside the 7 bytes. The shipped stub's bytes are exactly those
    instructions. So M3a writes the identical V and cannot change geometry.
  - Trail draw `0x75A5E1`: trim first, then sizes the dynamic VB from the
    current counts (2 vertices per point, 6 indices per segment, trails
    with ≤ 1 point skipped); the builder writes exactly that. Our TrailLife
    +1 happens before the sizing, so no overflow.
  - Conclusion: the stretching is most likely random (it was also seen at
    30 FPS); one session without M3a isn't conclusive.
- **If it happens again:** note the mission and what was on screen; try a
  session with `multicore_particle_index=0` (or `multicore_verify=1`, which
  checks every index answer against the original scan and counts
  mismatches in the log).

## B24. Rain-reflective ground flickers (GDI "White House")

**Status:** closed, not our bug (2026-10-12). The user confirmed it is a
Firestorm mod issue: the mod already shows glitches such as randomly
stretching geometry at 30 FPS, and reloading the game clears this one.
Nothing to fix in the patch. Notes below are kept for reference.

- **Report:** the wet, reflective ground (clouds reflected, walkable, no
  water on the map) sometimes shows triangles turning brighter/whiter,
  changing with camera movement. Stable at 30 FPS without the DLL.
- **Shader (`Shaders/Terrain.fx`):** with rainy weather (`WeatherMode` 2/3)
  the terrain lerps toward a reflection by the macro texture: either the
  mirror reflection render target `WaterDraw.ReflectionTexture` (screen
  projected per vertex) or the cube map `Terrain.EnvironmentTexture`. The
  cloud layer scrolls with the shader `Time`.
- **Engine (TW 1.10):** `W3DDisplay::draw` (`0x839FB3`) calls the reflection
  renderer (`[0xC16BF0]`) method `0x837903` every drawn frame (unless
  `[0xBEE574]+0x65`): clear "valid" +0x118, then if `0x8377FD(camera)`
  (rainy and water quality > 2, or a reflective area visible; never with
  `-noReflections` = `[0xC16BD1]`) render it (`0x8188A5`) and set +0x118.
  The terrain's `ReflectionTexture` getter (`0x8205F7`) binds +0xC0 when
  valid, else a default texture (`0x756324`).
- No frame-rate dependency found statically yet; next: bisect
  (`debug_disable` with all non-core features).

**Round 1 (2026-10-12):**
- Bisect with all 27 non-core features disabled: **no change**, so it's the
  core (the game running above 30 FPS), not one of our fixes.
- The user plays **Firestorm 1.12**, which ships its own White House map
  (`data\maps\official\map_sp_gdi_1_4_white_house\map.xml`, readable XML,
  copy in `temp/b24/`): `weather=0` (sunny, so the rain reflection above is
  **not** involved), `EnvironmentData Cloud="CM2_cloud" Macro="TSCloudMed"`,
  post effects `BleachByPass_vol` and `LUBlueZone_Vol`; it also replaces
  many terrain textures and `_nrm` maps (specular in the normal map), but
  no terrain shader (its compiled shaders: particles, objects, rain,
  water).
- Screenshots: lighter, straight-edged patches (triangles) that differ
  between two nearby camera positions; they stay while the camera is
  still. The cloud layer (`CurrentOffsetUV`, per frame, map-wide) isn't
  camera-dependent; camera-dependent terrain inputs are the shadow map
  (fitted to the view), the per-vertex specular half vector and fog.
- Outcome: the user found it's a mod issue (see Status); the tests above
  were not needed.

## B23. Barrel recoil plays too fast (Juggernaut, Mammoth, ...)

**Status:** fix implemented 2026-10-12, needs an in-game test.

- **Report:** the barrel kick-back of the Juggernaut and the Mammoth (and
  every other unit with a `WeaponRecoilBone`) plays too fast.
- **Data:** the draw module's `InitialRecoilSpeed`, `MaxRecoilDistance`,
  `RecoilDamping`, `RecoilSettleSpeed` (`ScriptedModelDraw.xsd`; Juggernaut
  and Mammoth: 16 / 10 / 2.0 / 3). C&C3 has no INI strings for them (binary
  assets), so the code was found from the 0.01 "tiny rate" constant.
- **Code:** W3D draw's client recoil handler (TW 1.10 `0x863B46`) runs per
  drawn frame and steps each active recoil record (+0x14 state: 1/2 kick,
  3 settle, 0 idle; +0x18 shift; +0x1C rate) with 30 Hz constants from the
  module data (+0x5C max, +0x60 damping, +0x64 settle speed):
  - kick: `shift += rate; rate *= damping`; settle when `shift >= max`
    (clamped) or `|rate| < 0.01`;
  - settle: `shift -= settle`; idle at 0.

  So at 155 FPS the whole kick and settle took about 1/5 of the time.
- **Fix `recoil_rate`:** the 7 bytes that start the state switch
  (`CMP EAX,2; JLE; CMP EAX,3`) become `CALL recoil_step_entry; JMP` to the
  switch end (`XORPS XMM0,XMM0`). The entry keeps EAX/ECX/EDX/XMM0-7 (the
  code after the switch needs ECX = 0) and calls `recoil_step(record, data)`,
  which advances by k = 30 / live FPS of a retail frame:
  - kick: `rate *= d^k`, `shift += rate * (d^k - 1)/(d - 1)` (k when
    d = 1), which sums to exactly the retail shift at every 1/30 s;
  - settle: `shift -= settle * k`;
  - the retail end tests; at or below 30 FPS, or for damping <= 0, the
    retail step.
- Simulation (`temp/recoil_sim.py`): end times at 155 FPS match 30 FPS
  within one retail frame (Juggernaut values 3.48 s vs 3.53 s).
- Resolver: the whole switch is identical in all four builds (TW 1.10
  `0x463C29` RVA, TW 1.9 `0xD0D05`, KW 1.03 `0x4191F5`, KW 1.02
  `0xD15BD`).
- Seen on the way (not reported yet): `0x850FFE` (a W3D draw update) adds
  1/30 to a blend at +0x2F4 once per client frame (`+0.033333335`), which
  is also 30 Hz-tuned. If a "fades in too fast" report comes up, look there.

## B22. Turret pitch (up/down) steps at 15 Hz

**Status:** done, confirmed by the user 2026-10-12.

- **Report (after B21):** turrets turn smoothly, but aiming up/down still
  steps. This is vanilla: the same function (TW 1.10 `0x8580A2`) keeps a
  pitch history like the rotation's (cache +4 previous, +0xC current,
  shifted when s == 1.0), but the pitch bone gets the **raw logic pitch**:
  `MOVSS XMM0,[-0.0]; SUBSS XMM0,[EBP-8]` → `0x46CF1E` (rotate X).
- **Fix `turret_pitch`:** those 13 bytes become `CALL turret_pitch_stub` +
  8 NOPs. The stub returns `-0.0 - pitch`, where pitch is:
  - the previous pitch on the shift frame (as the rotation draws it);
  - otherwise previous + (current − previous)·s (s at `[EBP-0x14]`, from
    the B21 view);
  - the raw pitch when the turret keeps no history (`[EDI+0x24] == 0`).

  It uses EAX and XMM0–2 only; the following code reloads EAX and ECX.
- Resolver: the whole pitch block is identical in all four builds (same
  stack offsets); the constant is checked to be −0.0. The compiled stub
  was checked.
- The patch transaction capacity was raised from 96 to 128 (room for all
  diagnostics).

## B21. Turrets shake back and forth while moving

**Status:** done (confirmed in-game 2026-10-12).

**Report:** every TurretAI vehicle and base defence (e.g. Predator, missile
batteries) shakes rapidly while moving, on TW 1.10 and 1.9 with the patch.
Not at 30 FPS without the DLL. The bisect with all switchable features off
still shook. The user's idea, B18, was right: B18 is part of the scheduler
core, which can't be switched off.

**Cause (TW 1.10 `0x8580A2`, W3D turret positioning, turn bone +0x14 and
pitch bone +0x18):**
- It reads `s = GameEngine+0x48`.
- `if (s == 1.0)` it shifts each turret's angle history (prev = cur,
  cur = logic angle) and draws prev; otherwise it draws
  `prev + wrap(cur − prev)·s`. So the history only advances on a frame
  where s is **exactly 1.0**.
- `j / F` gave that on every tick's last frame. B18's continuous
  `(15j − carry)/rate` almost never does, so the history went stale and
  turrets swept between old angles and snapped back every tick.
- It's the only user of `+0x48` that compares it for equality. The other
  six readers (positions, transforms, transition, UI progress, two draw
  helpers) use it as a factor.

**Fix `turret_fraction`:**
- The function's `MOV EAX,[engine]` (5 bytes, followed by
  `MOVSS XMM0,[EAX+0x48]`; EAX is overwritten right after) becomes
  `MOV EAX, imm32` pointing at our `g_turret_engine_view`.
- Every write of the fraction (`write_interpolation`, from both the
  continuous scheduler path and the stock-schedule path) also writes the
  view: the same value, except **exactly 1.0 on a tick's last frame**. So
  turrets get the old once-per-tick shift, and everything else keeps the
  even steps.
- Resolver: a unique signature, and the operand must equal the resolved
  engine pointer. Verified on all four builds.
- Depends on `phase_scheduler` (which writes the view).

## B19 / B20. One-frame glitches: aircraft in the air, build icons gone

**Status:** diagnosing (2026-10-12).

**Reports (TW 1.9 + Firestorm, 155 FPS):**
- **B19:** a freshly produced aircraft at the Firestorm airfield shows in
  the air for one frame. Not at 30 FPS, and not on TW 1.10 (vanilla data)
  with the patch.
- **B20:** after clicking a build button, the other available build icons
  vanish for one frame (screenshots: the top row is gone and the clicked
  button is white with "1").

**Hypothesis (shared): half-finished logic ticks being drawn.**
- At 30 FPS the stock schedule runs the six phases as 1–3 | 4–6, so a frame
  is only ever drawn after phase 3 or 6. Our spread schedule (one phase per
  frame above 90 FPS) also draws after phases 1, 2, 4 and 5.
- A new object's drawable has no interpolation history yet, so it uses the
  Object's live transform (B4 notes). If production creates the aircraft in
  one phase and parks it in a later one, the in-between position is shown
  for a frame. That would be invisible at 30 FPS when both happen in the
  same half.
- A build command is processed in phase 1. If the build panel's
  availability is refreshed in a later phase, a frame drawn after phase 1
  shows the half-updated list.
- Neither is mod-specific: the stock 60/90 FPS schedules also draw between
  phases.

**Test option `vanilla_phase_batches=1`** (default 0):
- Phases 1–3 run in a tick's first frame and 4–6 in its last; frames in
  between run no logic.
- `own_schedule` also covers 45–90 in this mode.
- `IsClientFrameBoundary` additionally requires the tick's first frame,
  because the counter rests at 3.
- `temp/scheduler_sim_vanilla.py`: 0 errors over 16–240 FPS (phase order,
  ticks, flushes, one boundary per tick), and frames are drawn only after
  phase 3 or 6. The default schedule is unchanged (`scheduler_sim.py`: 0
  errors).
- Costs: logic is concentrated in two (adjacent) frames per tick, which
  worsens frame-time spikes in big battles.

**Test result (2026-10-12): neither glitch went away** with
`vanilla_phase_batches=1`. The deployed DLL and ini were verified; the
option was active. Half-finished ticks are ruled out and the option was
removed.

**New hypotheses:**
- **B20, the UI clock.** Like B1/B6: the Flash UI advances movie frames at
  ~30 Hz (`AptPlayer_Update`), while the C++ build-queue code changes the
  panel per client frame. New or rebuilt button clips initialise on the next
  Apt frame, so for a few client frames the slots look empty. A fix would
  force an Apt tick (`AptPlayer+0x1E9`) when the build panel rebuilds.
  C++ → Flash traffic goes through variables (`0x773D27`, 52 callers) and
  five queued-call variants (`0x773A18`..`0x773BD7`, 63 callers of
  `0x773A92`). Diagnostic `debug_apt_calls=1`: entry stubs on the five
  variants count calls per call site and log the busiest once per second.
  This shows which build-queue calls fire on a click, and whether forcing a
  tick on them is cheap.
- **B19, interpolation of a new object.** A new Object's previous position
  is its spawn point (`+0x1A8`, valid flag `+0x1C1`: cleared by the
  constructor `0x659E77`, set by `0x4AFAFD`). If the airfield spawns it in
  the air and moves it to the pad, the drawn curve may start in the air for
  the first frames. Diagnostic `debug_spawn_frames=N`:
  - follows new objects (+0x1C1 still 0, from 10 s into a session) for 45
    frames each;
  - logs phase, `s`, tick frame/F, Object current and previous position,
    the history pair, the drawn position (all with z) and the physics z.

  TW offsets, verified identical in TW 1.9 (the interpolation code matches
  byte for byte).

**Log 1 (`temp/diag_b19b20/fps_patch.log`, TW 1.9 + Firestorm, 90 FPS):**
- **UI calls:** ~1,000 at session start, then rare bursts (2–16/s). At the
  build clicks, the busy call sites are `OnInitialized` callbacks of new
  clips:
  - 1.9 `0x9D7FFF` → `SetAffordable` (a build button);
  - `0x9A5FD0` and `0x9BE018` → `SetAdjusted`;
  - the help box's `OnOpen`/`OnClosed`.

  So **clicking makes the build menu create new button clips**. They stay
  empty until `OnInitialized` runs on the next ~30 Hz APT frame. This is the
  B1 mechanism.
- **Spawns:** the physics-hook logger caught six stationary objects (one
  every 10 s, z 120, no physics), **not the aircraft**. Their logic and
  drawn positions were identical from the first frame. A new Object's
  "previous position" holds garbage until its first tick.

**B20 fix (implemented):**
- The game registers every new clip's `"OnInitialized"` listener through
  queued-call variant 0 (1.10 `0x773A18`): `PUSH EDI; PUSH "OnInitialized";
  ECX = gadget; CALL`.
- That string (TW 1.10 `0xAB8E94`, the tail of `"<OnInitialized"`) is
  pushed before variant 0 at 60 sites (TW) or 83 (KW). Variant 1 with the
  same name is the removal inside the callbacks.
- Fix: a permanent stub on variant 0's entry (prologue `55 8B EC 83 EC 20`
  replayed). When the second stack argument is that string, it sets
  `AptPlayer+0x1E9` (`request_apt_tick`, as B1), so the new clip initialises
  before the frame is drawn.
- Resolver: exactly five call variants, and the string pushed most often
  (≥ 10 times) within 17 bytes before a CALL of variant 0, which must read
  `OnInitialized`. Verified on all four builds. If unresolved, the fix is
  skipped and logged.

**B19 logger 2:** `debug_spawn_frames=N` now wraps
`Drawable::getInterpolatedTransform` (the B11 resolver's function, prologue
`56 8B F1 83 BE 40 01 00 00 00`, same in all four builds). It follows
drawables not seen during the previous second (from 10 s in) for 30 frames
and logs phase, `s`, the Object position and the drawn translation (with
height).

**Test round 3 (2026-10-12):**
- The `OnInitialized` tick fix did **not** stop the flicker. New clue from
  the screenshots: after cancelling a build, only buttons whose state
  changed vanish (the unlocked ones). The locked Tech Center and the
  clicked Crane stay, and the grid background is gone for that frame.
- Without the DLL at 30 FPS: **no flicker**. So it's caused by the patch
  or by the higher rate.
- The aircraft still flashes in the air. Spawn logger 3 (getInterpolatedTransform,
  brand-new Objects only, 155 FPS): every logged object is drawn exactly at
  its logic position. Two new objects appear in phase 5 at (0, 0, 0) and
  are drawn for one frame only.
- So the flash isn't in the Drawable interpolation path.

**Bisect switch `debug_disable=`** (comma-separated install features,
case-insensitive; skipped features are logged). Names:
`continuous_visuals`, `keyboard_timing`, `audio_timing`,
`objective_indicator_tick` (B1), `selection_panel_name` (B6),
`camera_timing`, `frame_authored_effects` (tracer/cloud/Anim2D frame reads),
`drawable_physics_timing` (B2), `tracer_motion`, `camera_ease_timing`,
`shroud_timing` (B7), `tint_envelope_timing` (B9), `minimap_glide` (B10),
`waypoint_lines` (B11), `effect_timing` (B14/B15), `particle_index` (M1),
`trail_vertex_fix` (M3a), `frame_boundary`, `range_stats`,
`apt_call_counter` (debug_apt_calls), `button_enable_tick` (B20),
`spawn_logger`,
`drawable_interpolation_window` (B4), `particle_and_cursor_timing` (B3),
`phase_scheduler`, `w3d_clock`, `frame_limiter`. The last three (and
`frame_boundary`) are core to running above 30 FPS.

**Bisect (2026-10-12, TW 1.9 + Firestorm, 90 FPS; log verified each time):**
- Run 1: eight UI/drawing features off. Both glitches remain.
- Run 2: all 22 non-core features off (only 10 core patch writes left:
  rate globals, scheduler hooks, W3D clock, limiter). **Both glitches
  remain.**
- So they come from the game itself running above 30 FPS, not from one of
  our fixes.

**B19 cause and fix (implemented, needs a test):**
- `GameClient_update` (1.10 `0x48EBF1`) runs, per drawable,
  `Drawable_updateDrawable(IsClientFrameBoundary())`. In
  `updateDrawable` (`0x48353F`) the flag gates exactly one block: applying
  a **pending model-state change** (+0x147 dirty → `0x4718D7` and the +0x23C
  model observers, which set the model condition/pose).
- Boundary frames are every other frame at 30 FPS, but once per tick at
  90+ FPS. So a fresh aircraft's model condition (parked/landed) can wait
  up to five frames, and meanwhile its default (airborne) pose is drawn.
  The logic and drawable positions were correct (spawn logger), so the
  wrong part is the pose.
- Fix `model_state_every_frame`: the loop's `PUSH [EBP-0x18]` becomes
  `PUSH 1; NOP`, so a pending change is applied on the next frame. The
  block only runs while +0x147 is set, and it clears it. The swap path
  `0x5ED320` keeps its own argument.
- Resolver: the unique call site (`FF 75 ?? 8B CE E8 … 8B 75 ?? 85 F6 0F
  85`) whose callee matches the `updateDrawable` prologue with the +0x147
  test. Verified on all four builds.
- Other boundary-gated client work (drawable shroud status refresh, ghost
  objects) is unchanged.

**B20, still diagnosing:**
- Without the DLL at 30 FPS: no flicker. With only the core patch at 90:
  flicker.
- The variant-0 `OnInitialized` tick stays in (harmless) until the trace
  explains it.
- Trace mode `debug_apt_calls=2`: one line per queued call (variants 0–4)
  and per variable set (new entry stub on `0x773D27`, prologue
  `55 8B EC 83 EC 24`, optional resolver), with client frame, time, call
  site and name (the second argument, when it points into the image). It
  starts 10 s into a game and stops after 4,000 lines.

**Test round 4 (2026-10-12): neither fix worked.** The aircraft still
flashes with `model_state_every_frame`, and the icons still flicker. Both
attempts (that patch, and the B20 `OnInitialized` tick) were **removed**.
The diagnostics stay.
- The trace hit its 4,000-line cap after 34 s: `UnitName` (B6 re-send) and
  `Resource` are pushed every frame.
- It does show how build buttons get their icons: `Image` (variant 3)
  plus `OnContentLoaded` callbacks, i.e. **the icon is loaded as content and
  reported on a later UI frame**, in addition to `OnInitialized`.
- The trace now skips `UnitName`/`Resource` and allows 20,000 lines.

**B19 root cause (height detector log `temp/diag_b19_4`, TW 1.9 +
Firestorm, 155 FPS, Orca Fighter at the GDI airfield):**
- The Orca is **created at z = 520** (high above its pad, frame 32851,
  phase 5) and **placed on the pad (z = 125.35) in one step on the next
  tick**.
- The drawable then interpolates that teleport like movement: p1 = 520,
  p2 = 125, drawn z 454 → 416 → … → 147 over the whole tick. On the
  following tick it dips to ~96 (below the pad) and back, because p0 is
  still 520.
- At 30 FPS the same happens over two frames (one frame halfway), which
  is easy to miss. At 155 FPS it's ~10 frames. After that the Orca is
  hidden inside the opening pad (screenshot 3).

**Fix `teleport_snap` (confirmed in-game 2026-10-12: the Orca flash is
gone, and teleporting units look fine):**
- An entry hook on `Drawable::refreshInterpolationHistory` (TW 1.10
  `0x474EAF`, thiscall(snap), RET 4, prologue `55 8B EC 83 EC 0C`; KW 1.03
  `0x4BAB25`, thiscall(snap, force), RET 8, prologue `55 8B EC 51 51`).
- After a normal refresh: if `|p2 − p1| > 250` world units (3,750/s), copy
  p2 into p0, p1, p3 and the output (+0x124), and the current rotation
  (+0x94) into the previous one (+0x64, 48 bytes). That's exactly what the
  engine's own snap mode does.
- Offsets are identical in TW and KW. Signatures (TW and KW shapes) are
  verified on all four builds. The trampolines are called through typed
  volatile pointers. The compiled wrappers were checked (ECX kept,
  `ret 4` / `ret 8`).
- The diagnostic `debug_spawn_frames` is now a "drawn height ≠ logic
  height" detector: more than 4 units → follow for 12 frames, with the
  Object's previous position, history pair, drawn position and flags.

**B20 trace 2 (`temp/diag_b20_trace2`):** during the Crane clicks
(frames 31309–31569) the only UI traffic is the tooltip being rebuilt about
every 5 frames (`Title`/`HotKey`/`Text`, `OnInitialized` → `OnOpen`). Nothing
reaches the button grid through queued calls or variable sets, so the grid
changes reach Flash another way (still open).

**How the build menu is built (2026-10-12, TW 1.10 RE; Flash read with
`temp/b20_ui/aptdis.py`, an Apt bytecode disassembler for the compiled
`data\aptui\*.bin` assets, RefPack-decompressed with `temp/b20_ui/refpack.py`):**
- **Menu (Flash):** `TacticalHUDBuildQueuePage` holds one page per type tab
  (`InsertPage` → `attachMovie("BuildQueuePage")`). Each page sets
  `disableCommandButtonLoadAnim = true` and loads
  `TacticalHUDCommandListBox.swf` (the grid: 3 columns, 64×64 slots, 4
  visible rows). C++ drives the grid with `SetButtonSlotCount` /
  `InsertButtonSlot` / `EraseButtonSlot`. Every slot is
  `attachMovie("LoadMovieStage")`, reported back with
  `FSCommand <grid>:OnButtonSlotStageCreated index=..&name=..`.
- **Menu (C++):** `InGameUI::Tactical::BuildQueue` (`Registry`,
  `TypePageSet`, `PageObserver`, `BuildQueueUIManager`), the grid gadget
  `InGameUI::Gadget::CommandList` (`0x7A11F7` = its `OnInitialized`:
  listener + `SetButtonSlotCount`; `0x7A0FBF` = `OnButtonSlotStageCreated`).
- **One tile (e.g. the Crane):** `InGameUI::Tactical::BuildButton` (ctor
  `0x7AC362`; `StructureButton::ActualButton` vtable `0xABD3CC`,
  `UnitButton::ActualButton` `0xABD46C`) owns a
  `Gadget::StandardCommandButton` (ctor `0x785372`), which loads
  `StandardCommandButton.swf` into the slot stage (`FUN_00770962`). The tile
  movie's frame-0 script calls `_parent.OnContentLoaded(this)` and
  `FSCommand :OnInitialized`; the C++ handler `0x785256` registers
  OnClicked/OnRollOver/OnRollOut and pushes the whole state (SetEnabled,
  SetVisibility, SetSelected, SetNumberVisibility, SetTimerVisibility/Value,
  SetImageTint). The icon is a `RenderImage` clip whose `_imageMap` is
  `<tile>:Image` (set in its `Load` clip event).
- **State logic:** `BuildButton::Refresh` (`0x7AC486`):
  - *locked* (`FUN_0049DBB3`: prerequisite missing): once, on the change,
    locked image, number/timer hidden, `SetEnabled(false)`, flag +0x35;
  - *unlocked*: normal image (if it was locked), queue count, progress
    timer, then virtual +0x34 decides enabled. For structures (`0x7ABE49`):
    enabled when it's the one being built/ready, else
    `BuildAssistant::canMakeUnit` (`[0xBF36D0]` vtable +0x5C); a refusal
    ("factory busy" while the Crane is in production) → `SetEnabled(false)`
    (`0x7AC1D6`), OK → `SetEnabled(true)` (`0x7AC1BE`).
- **Enabled vs disabled in Flash:** `SetEnabled(true)` = `gotoAndPlay("_up")`,
  `SetEnabled(false)` = `gotoAndPlay("_disabled")`. Frame `_up` **removes**
  `bg`, `disabledImage`, `frame` (depths 3/15/19) and **places new** `bg`,
  `image` (RenderImage) and `frame` clips; `_disabled` does the reverse. So
  every tile whose enabled state flips rebuilds its picture from new clips.
  Tiles that stay locked (Tech Center) never change frame.
- **Why our trace missed it:** the gadget setters go through
  `FUN_00784E58` → `FUN_00416693` → `FUN_00412E96` (`_levelN.path.name`) →
  `FUN_008CCF9A`, which pushes the arguments and **runs the ActionScript
  function immediately** (not one of the five queued call variants we
  traced). So the frame jump happens in whatever client frame the C++
  refresh runs, while the new clips are only filled in on Flash's ~30 Hz
  tick. Leading hypothesis: at 30 FPS a Flash tick follows in the same frame;
  at 155 FPS the rebuilt tiles are drawn empty until the next tick.

**Fix `button_enable_tick` (confirmed in-game 2026-10-12):**
- An entry hook on the bool direct-call helper (TW 1.10 `0x784E58`, cdecl
  `(path, name, &bool)`, first instruction `MOV EDX,[g_aptPlayer]`, 6 bytes
  replaced by `JMP` + NOP and replayed in the stub). When `name` is the
  `"SetEnabled"` literal (pointer compare), it requests the engine's forced
  Flash tick (`AptPlayer+0x1E9`, run before drawing, as in B1), so the
  rebuilt tile is filled in before the frame is shown.
- Covers every gadget that sends SetEnabled through it: build tiles
  (`StandardCommandButton`), selection-refinement buttons and player-power
  buttons. Senders only call it when the state changes, so the forced tick
  is rare (a click/cancel or a production change); per-frame setters such as
  SetTimerValue use other names and never force a tick.
- Resolver: the unique SetEnabled sender (`55 8B EC 51 8A 41 ?? C0 E8 02 24
  01 ... 68 <str> 51 E8`, chosen by its pushed string reading
  `SetEnabled`; the same shape also sends SetVisibility), its call target
  must match the helper pattern and load the already-resolved AptPlayer
  pointer. TW 1.10 helper RVA `0x384E58`, TW 1.9 `0x5A7CEE`, KW 1.03
  `0x521BD9`, KW 1.02 `0x56C5ED` (KW's sender reads the flag at +0x50).
- Bisect name: `button_enable_tick`.

**Earlier design options (for the ruled-out hypothesis):**
- Batches 1–3 | 4–6 only when logic is light (cheap there), spreading when
  heavy.
- 4–6 halfway through the tick, which needs the engine's wrap handled
  without the phase-6 counter trick: `GameEngine_update` bumps a tick
  counter and sets a "new tick" flag on every wrap.
- Or targeted fixes (hide a new drawable until the next 3/6 boundary).

---

## Check: can the patch change game logic? (2026-10-12)

Asked after an infantry unit got stuck on a bridge (mission 1, TW 1.9 +
Firestorm). Static check (TW 1.10):
- **Logic itself:** the scheduler only changes *when* in a frame the six
  phases run; every tick still runs all six, in order. The other hooks are
  client-side.
- **Client FPS global** (`0xB877C4`, 30 in vanilla, 156/adaptive with the
  patch): of ~70 readers, the logic-side ones are:
  - CastleBehavior, TeleportSpecialAbilityUpdate, SpawnBehavior,
    BezierProjectile/MissileUpdate, SlowDeathBehavior, RebuildHoleExposeDie,
    SubObjectsUpgrade, LivingWorldEyeTower;
  - RadarScan (B15).

  The ones checked use it to time **drawable fades** in client frames
  (`seconds × clientFPS` → `0x46E933` / `0x46E8E1` / `0x473097`), which is
  correct at any rate and visual only. **No locomotor, pathfinding, AI or
  movement code reads it.**
- **`IsClientFrameBoundary`** (patched): its callers are the engine, the
  GameClient, W3D, `Drawable_updateDrawable`, and a UI progress smoother
  (`0x5C98F0`, build/upgrade buttons and health bar). None is gameplay.
- Conclusion: the patch can't make a unit get stuck. Units sticking on
  bridges is a known SAGE/CnC3 pathfinding issue (bridge layers).
- Firestorm's Ion Cannon (35 mod-only particle systems; type read from the
  compiled `data\mod.bin`, see claude_memory) uses only standard types: 21
  GPU, 11 CPU, 2 render-object, 1 trail. All are covered by B3.

---

## F2. 100% game speed at any frame rate

**Status:** implemented 2026-10-12 (`adaptive_fps=1`, default on), needs an
in-game test. The user's request: if a busy scene drops 157 → 60 (or even
~10) FPS, the game must still run at 100% speed.

### Why it slowed down
Every conversion in the patch is per frame: frames per logic tick, particle
and audio steps per frame, W3D milliseconds per frame, camera coefficients
per frame. Frames that take longer than 1/target therefore slow everything.

### Design
- **Effective rate `g_rate`** replaces the configured target in every per-
  frame conversion: the scheduler (fractional above 90, stock 15–90),
  interpolation, slice flush, W3D clock, particle and audio gates (now loops,
  so below 30 FPS a frame runs several 30 Hz steps), tracer speed, tint
  rates, the limiter divisor, camera constants and camera ease.
- **Synthetic 30 Hz frame** is a running count (whole + remainder in 1/rate)
  advanced per new client frame, instead of `ceil(frame × 30 / target)`, so
  it stays continuous across rate changes. The B2 physics blend uses its
  fraction; the tint sustain uses its "advanced this frame" flag.
- **Retime** (`retime_client_rate`), applied only at a logic-tick start (the
  dispatch wrapper with phase 1): it rescales the fixed-point remainders and
  rewrites camera constants, camera ease, flash interval, the B4 window bytes
  (`3 × ceil(rate/15)`, written with VirtualProtect), and the game's globals
  (client FPS, its float, audio ms/frame, visual s/frame, W3D ms/frame,
  GlobalData FPS limit, engine max FPS, nominal ratio, pacing multiplier).
- **Pacing monitor** (in the limiter's interval hook, once per limited
  frame). Round 1 estimated frame work from QPC plus the planned wait and used
  a fast EMA. In-game (TW 1.9, cap 156) the rate wandered: 120 in the menu,
  15 → 60 → 45 → 75 in game. The QPC estimate ran up to 1 ms high (the
  stock wait works in whole `timeGetTime` ms; ~15% of a 6.4 ms budget), and
  single hitches and loading frames swung the EMA. Round 2: work =
  `timeGetTime() − stamp`, where the stamp is the stock end of the previous
  wait (`previous_frame_time_ms`), exactly what the limiter measures. Frames
  over 250 ms are skipped, and the trimmed mean (middle 60%) of the last
  ~0.25 s of frames (8–64) is used. Drop when it exceeds the budget by more
  than 10% (to the best rate ≤ 95% of capacity); rise to the best rate with
  10% headroom after 0.5 s (round 3: was 2 s, felt slow; back-off doubles up
  to 8 s when a rise fails within 3 s). The window is cleared after every switch. Every switch is logged with
  the measured work.
- **Round 3 (oscillation).** The user's log (TW 1.9, cap 156) showed a cycle
  about once a second: 156 (work 7.1 ms, GPU-bound: the CPU waits in Present)
  → 132 (work still 8.7 ms while the GPU queue drains) → ~105 (work 3.9 ms,
  CPU only) → raise to 156 → … Each switch is felt as a stutter. Fixes:
  ignore 300 ms of frames after any switch (queues drain); a drop within 3 s
  of a rise marks that rise as failed and sets a ceiling (the higher of the
  rate before the rise and the new measurement), returning there. Rises are
  capped at the ceiling. When the retry timer expires (10 s, doubling to
  60 s; reset after 60 s without failures) the ceiling rises gently, by 15%
  or at least one supported step, instead of being removed. Simulated
  (temp/gpu_limit_sim.py, CPU 3.5 ms, GPU 7.4/11 ms, 2-frame queue): it
  settles at 133 / 90 FPS with only brief, smaller probes at 12 s, 33 s, ….
- **Round 4 (model).** The user's tests showed the remaining stutter is the
  game's own heavy logic (1% lows of 15 FPS with 7 AIs; same at a fixed 105
  FPS without adaptive pacing), and the GPU was barely used: the game is
  CPU-bound on one thread, so round 3's "GPU queue" explanation was wrong.
  The cascades (156 → 134 → 75) came from logic: at lower rates the same
  logic is spread over fewer frames, so the average frame gets heavier.
  Now:
  - The dispatch wrapper counts the logic phases each frame ran
    (`dispatch_counted`: the stock batch via the engine phase before/after;
    catch-up ticks count 6; a stall counts 0).
  - The monitor fits `work = drawing + phases × phase_cost` by least squares
    over the last second. With no phase-count variance (fixed schedules) it
    reuses the last fit. Load = mean work × rate. It drops when load > 97%
    to the rate whose second fits 90%: `(900 − 90 × phase_cost) / drawing`.
    It rises when the 85% rate is higher and load < 90%, for 0.5 s. Settle
    and ceiling stay as safety nets.
  - **Limiter debt repayment** (adaptive only): a frame that overran its
    interval leaves a debt (cap 250 ms, loading ignored) that the next frames
    repay by waiting less. The stock limiter never recovers lost time, so
    heavy logic frames permanently slowed the game even when the second as a
    whole fitted (simulated: 87% speed at 156 FPS with 84% CPU load).
  - Simulated (temp/model_sim.py, one thread, per-phase costs with one heavy
    phase): light → 156 FPS; 7-AI-like (25 ms logic/tick, 3 ms drawing) → 156
    FPS; with 6 / 9 ms drawing → one drop straight to 97 / 60 FPS; logic ≈
    90% of the CPU → 5 FPS with catch-up. All at 100–101% speed.
- **Round 5 (robust fit).** The user's test felt much better, but with 1% lows
  of ~20 FPS the mod dropped straight to 5 FPS. The log showed "logic 178.00
  ms/tick, drawing 0.30 ms/frame, load 98% at 75 FPS; drop to 5". At 75 FPS
  (stock schedule, phases 1,1,1,1,2 per frame) the phase counts barely vary,
  so the least-squares split was ill-conditioned and gave almost all the work
  to logic. A stale fit reused from another rate is just as unreliable. There
  was also drop/raise churn on bursty load (drop at 97% → 38% → raise).
  Now:
  - The split is trusted only with phase-count variance ≥ 0.15 and 15–85%
    of frames running no logic (only the high-rate schedule has those).
    Logic is capped at 90% of the measured work. Otherwise all work is
    treated as scaling with the rate (phase cost 0); stale fits are no
    longer reused.
  - Window: two seconds of frames (`2 × g_rate`, 16–512); decides once one
    second is in.
  - Drop only when load > 100% over that window (short bursts are repaid by
    the limiter debt). Never below half the current rate in one step.
  - Rise only after one second with load < 75%, to the 85% rate, at most
    1.5× per step, still capped by the failed-rise ceiling.
  - Simulated (temp/model_sim5.py): light, 7-AI-like and short-spike loads
    stay at 156 with no switches. A game-like load with dips to ~20 FPS stays
    at 156. Starting inside the stock schedule (75 / 45) holds. Logic that
    doubles for 1 s every 6 s settles at 112 (lowest 75, 4 switches a
    minute). Logic at 93% of the CPU walks down to 11 FPS. All at 100–101%
    speed, except bursts whose logic alone exceeds a second (96.8%,
    physically unavoidable).
- **Round 6 (timed logic, speed-based drops).** The user's run (TW 1.9, cap
  156, driver cap 156) mostly held 156, but showed two problems:
  - In menus and light scenes it dropped 156 → 140 → 156 at "drawing 6.4
    ms, load 100%". 6.41 ms is the NVIDIA cap of 156, while the limiter aims
    ~1% fast (990/rate ms). The driver's wait counted as work, so load read
    100%+ although nothing was slow.
  - In a huge battle it walked 109 → 93 → 75 → 45 → 30 → 15 → … → 5, each
    step at "load 100%". At 15 FPS the fit said logic ≈ 62 ms/tick (≈ 930
    ms per game second): the logic alone filled the CPU, so lower rates
    couldn't help. The proportional fallback still assumed they would.
  Now:
  - `dispatch_counted` times every logic dispatch with QPC
    (`g_frame_logic_us`). The monitor no longer fits anything: over the last
    2 s of real time (≥ 1 s and ≥ 8 frames before judging) it computes
    logic ms per game second (logic time / ticks × 15), drawing ms per frame
    ((work − logic) / frames), load, frame speed (frames × 1000/rate ÷
    elapsed) and game-tick speed (ticks ÷ 15 ÷ elapsed).
  - Drop only when load > 100% **and** frame speed < 97% (a driver cap
    raises load but not slowness). Same 90% target and halving limit.
  - Logic-bound floor: logic > 800 ms per game second → never below 30 FPS
    (and back to 30 if below). Lower rates only help when drawing is the
    problem.
  - The time-based window makes slow-drawing PCs react in seconds (80 ms
    frames: 156 → 10 FPS in ~7 s instead of ~25 s).
  - Logging: every decision line has a `[seconds]` timestamp, frame speed
    and logic ms/s, and a status line is written every 10 s
    ("Adaptive pacing [Ns]: R FPS, frames %, game ticks %, logic ms/s,
    drawing ms/frame, load %"). Game ticks % also shows whether below-15
    catch-up keeps real time (an open question: a skirmish may stall extra
    ticks per frame).
  - Simulated (temp/model_sim6.py): light / 7-AI / stock starts stay put; menu
    with a driver cap at 156 holds 156; logic 930 ms/s → 30 FPS (97%);
    slow drawing → 10 FPS. Heavy load with a driver cap of exactly the
    target drops to ~107: the cap leaves no room to repay overruns.
- **Round 7 (effects timing, 5% floor, outside-cap ceiling).** The user's
  next run (TW 1.9, target 156, NVIDIA cap 156; ~10 min skirmish):
  - Near the max it alternated 156 ↔ 129–150 even in calm play. At 156 the
    "drawing" read 5.2–6.4 ms; right after dropping, the same scene drew in
    1.9–3.4 ms. The difference was waiting on the driver cap: with the cap
    equal to the target, the limiter can never catch up after a heavy logic
    frame, so the game ran at 94–98% of 156 → drop → plenty of room → raise
    → … At 150 it held 100% for 24 s.
  - The round-6 logic-bound floor was wrong: at 5 FPS the late-game battle
    ran at **100% game speed** (logic 600–735 ms/s, drawing ~40 ms/frame).
    A brief logic reading of 816 ms/s forced 30 FPS, and the game then ran
    at **30–50% speed for over 3 minutes**. Lower rates did help: the drawing
    itself was huge.
  - Confirmed: below-15 catch-up keeps real time ("game ticks" 99–103% at
    5–10 FPS).
  - "Drawing" per frame grew as the rate fell (8.7 ms at 45 → ~40 ms at 5)
    partly because each frame runs several 30 Hz particle/audio steps below
    30 FPS.
  Now:
  - The particle and audio updates are timed too (`g_frame_effects_us`,
    steps). Fixed cost per second = logic (per tick × 15) + effects (per
    step × 30); drawing = (work − logic − effects) per frame.
  - Drop target: the rate fitting 90%; if none ≥ 5, the rate fitting 98%;
    if even 5 FPS can't hold full speed, the highest rate losing at most 5%
    speed against the best reachable (speed capped at 100%). That last value
    is also the floor for drops. There is no more "jump up to the floor"
    (that caused the 30 FPS disaster); only the normal raise path goes up.
  - Outside-limit ceiling: on the first judgement after a drop, if the old
    rate's per-frame drawing was > 1.3 × the new one + 0.5 ms (real drops
    from a heavier scene measured 1.03–1.15×; the cap drops 1.8–2.7×), the
    old rate waited on something outside (driver cap or GPU). It sets a
    lasting ceiling at 96% of that rate (156 → 149), kept across sessions
    for the process, and logs it. Sessions then start at that ceiling.
  - Simulated (temp/model_sim7.py, now with 30 Hz effect cost and a correct
    driver-cap model where the driver holds Present until 1/cap after the
    previous one): light / 7-AI / stock starts / calm game and menu at cap
    156 stay put; busy game with cap 156 reproduces the user's churn, then
    detects the cap and settles below it; logic 930 ms/s → 15 FPS at 100%;
    the user's battle → 6 FPS; slow drawing → 10 FPS.
  - **Driver caps and heavy logic:** after a heavy logic frame (~14 ms
    phase), the next frames must run faster than the target to catch up.
    Any cap close to the target forbids that. Simulated "heavy 7 AIs" at
    target 156 holds 156 with no cap, but settles at ~107 with a cap of 156,
    160 or 164. With a 165 cap, target 138 or lower holds. Advice: no driver
    cap for this game (the mod limits itself), or a target ~15–20% under the
    cap.
  - **In-game result (2026-10-12, TW 1.9, cap 156, ~6.5 min).** The user
    said it "feels better". The cap was detected at 32 s (156 took 4.66 ms
    per frame against 1.96 at 105) → ceiling 149, then 149 FPS at 100% for
    90 s with no switches. Fights: 149 → 123 → 105 → back to 149 in 4 s.
    The escalating late-game battle stepped 149 → 120 → 60 → 30 → 15 → 7 →
    5 at 97–104% speed. At its peak (logic 800–900 ms/s, effects ~100 ms/s,
    drawing ~45 ms/frame, ~36 ms even after the game ended) 5 FPS ran at
    87–94%: nothing could hold 100% (a fixed 30 FPS ≈ 45%, 15 FPS ≈ 64%).
    Open option: an `adaptive_min_fps` setting if the user prefers
    smoothness over speed in such battles.
- **Round 8 (2026-10-12): bug, the rate stuck at 7 FPS after a match.**
  - In a heavy TW 1.9 battle the outside-limit check misfired at low rates:
    "15 FPS took 15.17 ms/frame against 10.42 here … at or below 14", then
    "… at or below 7". In a big battle the per-frame time swings by itself.
  - The ceiling was kept across sessions, and sessions started at it, so the
    menu after the match stayed at 7 FPS.
  - Fix: the check only judges drops from 60 FPS or more
    (`OUTSIDE_LIMIT_MIN_FPS`), and the ceiling is cleared at every session
    start. A real driver cap is then found again within seconds of each
    match start.
- **Round 9 (2026-10-12): any rate from 15 up.** The user saw the rate
  "stick" at 15 FPS in heavy battles. The next supported step was 30, which
  didn't fit, while 20–24 would have.
  - Our scheduler (previously used only above 90) now runs **every rate above
    15 except the stock multiples 30/45/60/75/90**.
  - A tick gets F frames by Bresenham. For F ≤ 6, frame j runs the batch of
    phases up to `floor(6j/F)`, the same batches the stock schedule uses for
    whole ratios (75 FPS: 1, 1, 1, 1, 2). Above 90 the existing spread is
    unchanged.
  - Pieces involved:
    - `dispatch_batch` runs the batch one stock dispatch per phase;
    - `get_phase_batch_limit` returns `phase + 1` in our mode, so the stock
      dispatch never batches by itself;
    - `is_client_slice_end` = last phase of this frame's batch;
    - interpolation = j/F;
    - `GameEngine::IsClientFrameBoundary` (TW `0x548131`, 8 callers: client
      update, main loop, render, objects) is replaced through a naked stub
      that keeps ECX/EDX: the phase counter equals the last phase of the
      tick's first frame. That is exactly the stock "6 / ratio" (or "1" from
      ratio 6) for every whole ratio.
  - Every other user of clientFPS / logicFPS was checked in Ghidra (physics
    uses a float ratio; the pacing multiplier matches the limiter's floor).
  - `supported_rate_at_most` now allows any whole rate.
  - Offline simulation (`temp/scheduler_sim.py`) of GameEngine::update with
    the wrapper, rates 15–240: phases 1–6 in order, exactly 15 ticks/s, one
    slice-end flush per frame that runs logic, one frame boundary per tick.
    0 errors.
- **Supported rates:** the target; any whole rate from 15 up (our
  scheduler, or stock at multiples of 15 up to 90); below 15 any whole number,
  down to 5.
- **Below 15 FPS:** the game's integer rate globals stay at 15 (stock code
  divides by `clientFPS/15`; 0 would crash), so its schedule runs one whole
  tick per frame. After it, `run_catch_up_ticks` dispatches more whole ticks
  (Bresenham `15 / rate` per frame, at most 4 extra). A network stall ends
  the burst (phase set to 6 so the next frame retries). The patch's own
  ratio uses are clamped to ≥ 1.

### Offline simulation (temp/adaptive_sim.py)
Per-frame work 4/9/20/45/120/70/4 ms → logic 101/98/99/98/94→101/101/101%
(segments include transitions; retail itself runs 101% because its tick is
66 ms). At 100–120 ms per frame it settles at 8 FPS and 100–101% speed. At
250 ms (4 FPS, below the 5 FPS floor) it runs at 76%, as expected.

### Known limits
- Rate changes alter `clientFPS`; stock timers created in frames before a
  change run at the new rate (brief timing blips during transitions).
- Below 30 FPS some 30 Hz systems step coarser (e.g. the tracer manager emits
  once per frame; vehicle tilt springs update once per frame).
- Multiplayer is untested (lockstep stalls are respected by the catch-up).

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
1. **Config validation** ([config.c](../src_fps_patch/config.c)): only 45–90 in steps of
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
