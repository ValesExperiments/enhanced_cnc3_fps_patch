# AllianceSwitchTool — reverse engineering findings

Target: `cnc3game.dat`, Tiberium Wars **1.10**, 32-bit x86 PE, 16,314,880 bytes
SHA-256: `af568a89b6d0d4f69ec690cf1d443539cb15afe197b96ac3fc5bf0ec0a56d141`
(Copy of `RetailExe\1.10\cnc3game.dat`, verified byte-identical. Git-ignored.)

## Big win: the binary retains full MSVC RTTI

`python scan_strings.py --rtti` yields **3767 type descriptors** with real C++ class names.
No PDB is available (Ghidra confirms none), but RTTI gives us the class graph for free.

Confirmed core classes (all present as plain `.?AVName@@`):

`Object`, `Player`, `PlayerList`, `Team`, `Squad`, `Drawable`,
`GameLogic`, `GameClient`, `ScriptEngine`, `ScriptActions`

## The ownership model (and why it matters for this tool)

SAGE does not store "owner" on the object as a player id. The chain is:

```
Object  ->  Team  ->  Player
```

So switching ownership means retargeting the object's **Team**, not writing a player field.
The engine-level operation we need is `Object::setTeam(Team*)`-shaped. Confirmed by the
presence of both `Team` and `PlayerList` as first-class classes.

## Anchors for locating the transfer routine

The script-action name strings are in the binary, so the dispatch table that references them
leads directly to the implementation:

| String | File offset |
|---|---|
| `NAMED_TRANSFER_OWNERSHIP_PLAYER` | `0x0065a388` |
| `PLAYER_TRANSFER_OWNERSHIP_PLAYER` | `0x0065a414` |
| `SET_PLAYER_OWNERSHIP_OF_TYPE_COUNTER` | `0x00651a70` |
| `SET_PLAYER_OWNERSHIP_OF_TYPE_COUNTER_INCLUDE_DEAD` | `0x006519dc` |
| `LocalPlayer` | `0x0064c994` |

Plan: xref these in Ghidra -> `ScriptActions` method -> the shared engine routine it calls.
That shared routine is what the hotkey path should call too.

Secondary anchors: the game ships capture/commandeer mechanics (`SCCCommandeer.ani`,
`SCCCapture.ani`; Commandeer entries in `LogicCommand.xml`, `NODAvatar.xml`), and those code
paths must reach the same routine.

## Two edge cases that need answering from the disassembly, not guessed

1. **Garrisoned infantry** (the question the user raised). Containment is module-based:
   `GarrisonContain`, `GarrisonStealthContain`, `OpenContain`, `TransportContain`,
   `TunnelContain`, `HealContain`, `EngineerContain`, `HordeGarrisonContain`, and
   `EvacuateGarrisonSpecialPower`. The question is whether the transfer routine walks the
   contain module's passenger list or leaves occupants on the old team. Must be read, not assumed.

2. **Squads.** C&C3 infantry are `Squad` objects (e.g. `GDIRifleSoldierSquad`) whose members
   are individual `Object`s. Selecting infantry likely yields the squad. Transferring a squad
   without its members — or vice versa — would produce a broken half-owned unit.

## Selection

Selection lives under `InGameUI::Tactical`: `DragSelectionHandler`, `SelectionArea`,
`SelectionLasso`, `IsDragSelectablePredicate`, `SelectionDetails`, `SelectionRefinement`,
plus an `Observable<DrawableSelectionObserver>`. Note selection is tracked on **Drawable**
(client side); the owning `Object` (logic side) is what needs its team changed.

## Decisions taken

- **Hotkey: F11.** Verified genuinely unbound: diffing `MappableKeys.xml` against
  `DefaultHotKeys.xml` leaves F11, F12, M, P and assorted punctuation/numpad keys free.
  F1 was rejected (it is `SideBarButtonSlot1`, used constantly). Delete was rejected as a
  precaution (it is `TelestratorErase`, a BattleCast overlay that is off unless Home is pressed).
  The key will still be INI-configurable, following the `fps_patch.ini` precedent.
- **Scope: skirmish/campaign only.** Multiplayer is explicitly out of scope.
- **"Me" = the local human player**, so the target team resolves from `LocalPlayer`.

---

# Session 2 — Ghidra analysis results

Database built and saved: `ghidra_workspace/CNC3TW.rep`, 191 MB, image base `0x00400000`.
Analysis succeeded (no PDB, as expected). Re-run scripts against it with:

```
analyzeHeadless <workspace> CNC3TW -process cnc3game_1.10.dat -noanalysis \
    -scriptPath <ghidra_scripts> -postScript <Script>.java
```

Note: Ghidra 12 rejects `.py` scripts unless started via PyGhidra, so the helper
scripts here are **Java**. It also rejects project paths containing a `.` element.

## Symbol recovery is far better than expected

- **169,248 symbols**, **10,335 vftable symbols**
- RTTI gave real class namespaces, including: `Object`, `Team`, `Player`, `PlayerList`,
  `PlayerTeamList`, `PlayerRelationMap`, `TeamRelationMap`, `TeamFactory`, `TeamPrototype`,
  `Squad`, `ScriptActions`, `GameLogic`
- **`ObjectDefectionHelper`** — "defection" is SAGE's term for an object changing sides.
  Strong lead for the ownership-switch path; worth following before anything else.

## Source paths are in the binary

EA shipped `__FILE__` strings from asserts: 143 paths rooted at
`C:\Projects\Projects\CnC3\Production\code\gameengine\0.0-dev\source\GameEngine\`.
Relevant ones: `gamelogic\object\Object.cpp`, `gamelogic\object\Contain\GarrisonContain.cpp`,
`gamelogic\object\Contain\OpenContain.cpp`, `gamelogic\scriptengine\ScriptEngine.cpp`,
`gamelogic\scriptengine\LuaScriptEngine.cpp`.

**C&C3 has a Lua script engine** (`LuaScriptEngine.cpp`). Not yet explored; may offer a
cleaner route than a native hook, or may be campaign-only. Worth a look.

Functions carrying asserts from those files (release builds assert sparingly, so this is
a partial view of each class, not a full method list):

| Source file | Functions |
|---|---|
| `Object.cpp` | `FUN_00628d27` (352 b), `FUN_00647799` (1115 b), `FUN_0065aeef` (975 b) |
| `GarrisonContain.cpp` | `FUN_005e75b2` (178 b), `FUN_00660e05` (733 b) |
| `OpenContain.cpp` | `FUN_0061995f` (358 b), `FUN_00661365` (431 b) |

## Script-action table: metadata only, no function pointers

All three ownership action names are referenced from one registration function,
`FUN_004f7a66`, which only assigns strings into a large struct via `CALL 0x0040c56e`
(an `AsciiString::operator=`):

| String | VA | Assigned to |
|---|---|---|
| `NAMED_TRANSFER_OWNERSHIP_PLAYER` | `0x00a5a388` | `ESI+0x2c44` |
| `PLAYER_TRANSFER_OWNERSHIP_PLAYER` | `0x00a5a414` | `ESI+0x2bc4` |
| `SET_PLAYER_OWNERSHIP_OF_TYPE_COUNTER` | `0x00a51a70` | `ESI+0x2c3c` region |

Entry stride looks like **0x80 bytes** (paired fields at +0x00/+0x08 recur at 0x2bbc/0x2bc4,
0x2c3c/0x2c44, 0x9e3c/0x9e44). So dispatch is a switch on an action enum elsewhere, and the
enum index could be derived from the table base — but that is an indirect route.
**Preferred route: `ObjectDefectionHelper` and the `Object`/`Team` vftables.**

## Loader design decision

`cnc3game.dat` imports 17 modules + 1 delay-load (`steam_api.dll`):

```
GDI32 USER32 KERNEL32 SHELL32 MSVCR80 ADVAPI32 DINPUT8 d3dx9_29
DSOUND WINMM IMM32 WININET WS2_32 NETAPI32 SHLWAPI ole32 OLEAUT32
```

`DINPUT8.dll` is taken by cnc3_fps_patch. Free, non-KnownDLL candidates that will actually
load from the app folder: **`d3dx9_29.dll`**, **`dsound.dll`**, **`winmm.dll`**.
Picking one of these lets AllianceSwitchTool coexist with the FPS patch.
(`version.dll`, used by the FFVII mod, is *not* imported here, so it is not an option.)

## Still open

1. Locate the ownership-transfer routine (`Object::setTeam`-shaped).
2. Read its behaviour for **garrisoned occupants** — unanswered, must not be guessed.
3. Read its behaviour for **Squad members** — same.
4. Find how selection is read on the logic side (`Drawable` -> `Object`).

---

# Session 3 — class landscape mapped, transfer routine not yet pinned

## Ghidra workspace location (no symlink needed)

The database lives at `.tools_and_docs_by_claude/CNC3_ModdingResearch/reverse_engineering/ghidra_workspace`, inside the
project, and **no path element starts with a dot**, so Ghidra accepts it as-is.
The earlier failure was *not* a dot-folder problem: it was passing a **relative** path
(`./ghidra_workspace`), whose leading `./` Ghidra rejects. Absolute path fixed it.
A junction/symlink would add indirection for no benefit here. Only reach for one if the
workspace ever has to live under a dot-directory such as `.tools_and_docs_by_claude`.

## Ownership-adjacent classes found (all RTTI-confirmed)

`CaptureBehavior`, `HijackerUpdate`, `ObjectDefectionHelper`, `TemporarilyDefectUpdate`,
`DefectorSpecialPower`, `CaptureSpecialAbilityUpdate`, `CaptureAndGiveCommandSetSpecialPower`,
`AISpecialPowerCaptureBuilding`. Related strings: `DEFECTOR`, `TEMPORARILY_DEFECTED`,
`SKIP_DEFECT_IF_UNCOMBINED`, `AModuleTag_DefectionHelper`.

Identification confirmed: `FUN_005c1c7a` returns the literal `"CaptureBehavior"`, so the
vftable at `0x00a7f91c` is definitely that class.

## Functions examined and ruled out (so the next session does not repeat this)

| Function | Class / vtable slot | What it actually is |
|---|---|---|
| `FUN_00653815` | CaptureBehavior [9], 181 b | Walks a global object list; not the team write |
| `FUN_0062219d` | HijackerUpdate [0], 301 b | Hijacker exit/cleanup path, not the takeover |
| `FUN_005d5d26` | ObjectDefectionHelper [0], 448 b | Defection **visuals** (colour flash timing) |
| `FUN_004f7a66` | — | Script-action name registration, metadata only |

## Object layout facts recovered so far

`Object` uses multiple inheritance — several vftables at `0x00aa3d18`-`0x00aa3db4`.
Field offsets seen in decompiled code:

| Offset | Evidence |
|---|---|
| `+0x7c` | bitfield, tested with `>> 0xd & 1` |
| `+0xe8` | passed to drawable/FX calls (likely Drawable or ID) |
| `+0x104` | pointer, non-null checked before FX |
| `+0x2e8` | pointer, checked in defection guard |
| `+0x33c` | passed to `FUN_006769f0` |
| `+0x3cd` | flag byte, bit 1 = defection active |
| `+0x3ce` | flag byte, bit 1 cleared on hijack exit |

Globals: `DAT_00bee574` is a core singleton (frame/time at `+0x38`, object list head at
`+0xb8`, objects linked via `+0x78`). `DAT_00bf4c44` is a client/FX subsystem.

## Best remaining lead

**`FUN_0063c7f2` (3576 bytes), `Object` vftable `0x00aa3d7c` slot [3].**
A function that large on `Object` is almost certainly `Object::xfer` (save-game
serialisation). `xfer` walks every member in declaration order, which would hand us the
**full Object struct layout including the team/owner pointer offset** in one read.
That is the highest-yield next step, and it also makes the runtime Frida work trivial.

Secondary: find `Team`'s object list add/remove pair, then whoever calls both is `setTeam`.

---

# Session 4 — executeAction found; runtime probe built

## CONFIRMED: `ScriptActions::executeAction` is `FUN_005365b7`

`ScriptActions` vftable `0x00a4d6f4`, **slot [14]**, **19,203 bytes**, decompiling to
~3900 lines with **529 switch cases**. Saved at `notes/execute_action.c`.
This is the dispatch point for every script action in the game.

Case bodies follow a uniform shape — fetch parameter *n*, then call the handler:

```c
case 0x59:
    iVar6 = FUN_004f3670();      /* fetch a parameter   */
    iVar6 = FUN_004f3670(2);     /* parameter index 2   */
    uVar12 = *(undefined4 *)(iVar6 + 8);
    iVar6 = FUN_004f3670(0);     /* parameter index 0   */
    FUN_0051845c(iVar6 + 0x10, uVar12);   /* <- the handler */
```

So **`FUN_004f3670` is the script parameter accessor**, and the last call in each case is
that action's implementation. This is a reusable key for reading any script action.

## NOT CONFIRMED: which case is the ownership transfer

A heuristic (adjacent enum values sharing a rarely-used callee) pointed at cases
`0x27d`/`0x27e` sharing `FUN_005157d5`. **That lead was checked and rejected**: the
template-table arithmetic makes it impossible.

Template entries are `0x80` apart, verified against three known offsets:

| Action | Struct offset | Delta / 0x80 |
|---|---|---|
| `PLAYER_TRANSFER_OWNERSHIP_PLAYER` | `0x2bbc` | 0 |
| `NAMED_TRANSFER_OWNERSHIP_PLAYER` | `0x2c3c` | 1 (exactly adjacent) |
| (third entry seen) | `0x9e3c` | 229 |

For case 637 to be `PLAYER_TRANSFER`, the array base would have to be negative.
Since `0x2bbc mod 0x80 = 0x3c`, the base is `0x3c`-aligned, putting the pair at
**index ~85-89**, i.e. cases `0x55`-`0x59`. Those cases exist and match the expected
shape, but the exact base is still unknown, so **the identification is open**.

To close it: recover the array base by reading the start of `FUN_004f7a66`, then
index = `(0x2bbc - base) / 0x80`. That names the case with certainty.

**Do not treat `FUN_005157d5` as the transfer routine.** It was ruled out.

## Runtime probe built (`Submods/GameEngineResearch/`)

Read-only Frida tooling, since the empirical route to the garrison answer is faster and
more reliable than finishing the static hunt:

| File | Purpose |
|---|---|
| `probe_agent.js` | Walks the engine object list; snapshots Object fields |
| `probe.py` | Host: `validate`, `snapshot`, `owners`, `diff` |
| `selftest.py` | Proves the tooling works **without the game running** |
| `README.md` | Step-by-step procedure |

`selftest.py` **passes** (Frida 17.18.0, memory reads and range lookups verified), so any
failure against the game is an offset problem rather than a tooling problem.

The `owners` command finds the owner field without guessing: the owning Team pointer is
the field whose values split all objects into a *small number of large groups*, one per
player. The `diff` command then answers the garrison question directly — snapshot before
and after a transfer, and see whether occupant objects' owner fields moved too.

Caveat: the venv Python is x64 and the game is x86. Frida handles cross-arch injection on
Windows, but suspect it first if attaching misbehaves.

---

# Session 5 — observed garrison behaviour (from the user, 2026-09-18)

Reported from actually playing, not inferred:

> Train a rifleman squad, put them into a civilian building. The squad is removed/moved
> off the map and the building goes from **neutral to mine**. Empty the building and it
> returns to **neutral**. The soldiers never change allegiance.

## What this implies

1. **A civilian building's ownership is derived from its occupants**, not stored and held
   independently. Garrisoning transfers the building; evacuating reverts it.
2. **Occupants keep their own allegiance** through the process. Ownership is not pushed
   down from container to contained.
3. **Contained units leave the visible map**, so they may also leave the engine's object
   list — the probe should check whether garrisoned squad objects are still walkable.

## Consequence for AllianceSwitchTool (important)

Switching a garrisoned civilian building to the player while **enemy infantry are still
inside** may be immediately undone by the engine, because the occupants determine
ownership. The mod will need one of:

- transfer the occupants along with the building, or
- force the occupants out first, or
- decline on garrisoned civilian buildings and say why.

This must be settled with data before the ownership logic is written.

## Correction to the earlier test plan

The previous procedure told the user to "cause an ownership change", which was impossible —
no such tool exists yet; that *is* the mod. **Garrisoning is itself the ownership
transfer**, needs no mod, no engineer and no AI cooperation, and is fully repeatable.
The corrected 3-snapshot procedure is in `Submods/GameEngineResearch/README.md`.

---

# Session 6 — probe reworked for fullscreen play

Tiberium Wars is exclusive fullscreen and alt-tabbing risks a crash, so the probe became
a background, hotkey-driven, **audio-feedback** tool. The user never has to leave the game.

| File | Purpose |
|---|---|
| `Run-Probe.bat` | launcher (finds the venv, starts the watcher) |
| `watch.py` | global hotkeys + beeps, holds one Frida session open |
| `analysis.py` | shared analysis so CLI and hotkey tool cannot drift |
| `probe.py` | CLI equivalent |
| `selftest.py` | pre-flight: Frida + hotkeys + audio, no game needed |

Hotkeys (Ctrl+Alt combos; none clash with C&C3 defaults):
`V` validate, `1`/`2`/`3` snapshots, `D` report, `Q` quit.

## Verified

- `selftest.py` **passes**: Frida 17.18.0 attach + memory read, all six hotkeys
  register, all five beep patterns audible.
- Analysis pipeline verified against **synthetic** data modelled on the garrison
  scenario: it flags the owner-like field, ignores constant fields, isolates the one
  object whose owner changed, and reports objects that vanished from the list.
- Both front-ends fail gracefully when the game is not running.

## Bug found and fixed during testing

`frida.enumerate_processes()` **does not exist in Frida 17** — it is
`frida.get_local_device().enumerate_processes()`. This was in the error path of both
`watch.py` and `probe.py`, so it would only have surfaced at the worst moment (when the
game was not found). Fixed in both.

## Guard added

A snapshot that returns zero objects is **not saved** and beeps an error, so an empty
capture cannot silently poison a later diff.

---

# Session 7 — SOLVED: the owner field, and the garrison answer

Measured live, 2026-09-18, TW 1.10 skirmish. 1972 objects walked; three snapshots
around a full garrison cycle. Raw data in `Submods/GameEngineResearch/logs/`.

## CONFIRMED: `Object + 0x33C` is the Team pointer

The building `0x116aa2e0` moved and moved back:

```
+0x33C   before 0x09decd10  ->  garrisoned 0x09ded210  ->  emptied 0x09decd10
```

Exactly **one** object in the whole world showed the change-and-revert signature, which
is what ownership must look like. Team membership across the cycle:

| Team pointer | snap1 | snap2 | snap3 | identity |
|---|---|---|---|---|
| `0x09decd10` | 1702 | 1699 | 1700 | neutral / world (props, tiberium, civilian) |
| `0x09dece50` | 256 | 256 | 256 | (untouched third party) |
| `0x09ded210` | 14 | **15** | 14 | **the local human player** |

The only object to join and leave the player's team was the building. This also
corroborates the static work: `Object+0x33c` appeared in the `HijackerUpdate` decompile.

## ANSWER to the garrison question

**Ownership does NOT propagate to occupants.** Garrisoning transfers *only the container*.
All 14 of the player's units kept their own team pointer throughout. This matches exactly
what the user observed on screen ("the soldiers never change allegiance") — the visible
behaviour and the underlying data agree.

## CORRECTION: garrisoned units do NOT leave the object list

An earlier hypothesis (written into the probe README) said contained units might vanish
from the object list because they are "removed from the map". **That was wrong.**
All 14 player units remained present and walkable while garrisoned.

Two objects *did* vanish, but they were on the **neutral** team, not the player's — almost
certainly civilians fleeing the building, which is a real C&C3 mechanic. Do not confuse
them with the garrison.

Practical consequence: the mod can reach garrisoned units through the ordinary object
list; no contain-module traversal is required just to find them.

## Other fields that moved on the building (leads, not yet identified)

| Offset | 1 -> 2 | reverts? |
|---|---|---|
| `+0x007c` | `0` -> `2` | yes — flags; matches the bitfield seen statically |
| `+0x0088` | `0` -> `0x100` | yes |
| `+0x0094` | `0` -> `0x400` | yes |
| `+0x00a8` | `0` -> `0x1000000` | no |
| `+0x0340` | `0x117ed9d8` -> `0x0a54dac0` | no — sits right beside the team ptr |
| `+0x0354` | `0x116aa740` -> `0` | yes — plausibly the occupant/contain list |
| `+0x0358` | `0x116a9e80` -> `0x11c890e0` | no — new value is a *player unit's* address |

`+0x0358` pointing at one of the player's own units while garrisoned is a strong hint that
it references the occupant (or the object that caused the capture).

## Noise to filter in future runs

`+0x01a4` is a frame/tick counter — it changed on all 1970 objects (`0x522` -> `0x5cf`)
and swamped the "fewest fields changed" ranking. Exclude it when ranking.

## Next step, now tractable

Find the routine that **writes** `Object+0x33C`. Naively poking the pointer would corrupt
the engine's per-team object lists, so the mod must call the real setter. Searching the
disassembly for writes to `[reg+0x33c]` is now a targeted search rather than a hunt.

## FOUND: `Object::setTeam` is `FUN_0063c219` (VA `0x0063C219`, TW 1.10)

Located by searching the whole binary for writes to `[reg+0x33c]` — only **8** exist, and
one is a `MOVSS` float store belonging to an unrelated class.

```
Write sites for [reg+0x33c]:
  004aafff  FUN_004aaf96   (145 b)
  005568cd  FUN_00556842   (232 b)
  0063c323  FUN_0063c219   (602 b)   <-- Object::setTeam
  0065a10d  FUN_00659e77   (2549 b)
  006923ac  FUN_00692328   (186 b)
  0069299c  FUN_00692870   (732 b)
  00692bf7  FUN_00692b4c   (304 b)
  00865916  FUN_00865821   (275 b)   MOVSS - float, different class, ignore
```

`FUN_0063c219` sits immediately before `Object::xfer` (`FUN_0063c7f2`), i.e. in the
`Object.cpp` code region, and has exactly the right shape:

```c
void __thiscall Object::setTeam(Object *this, Team *newTeam, char suppressFx)
{
    if (this->team /* +0x33C */ == newTeam) return;   /* early-out */

    /* If both teams exist, compare their controlling players via FUN_00497ab0;
       local_5 records whether the *player* actually changes, not just the team. */

    /* deregister from the old team, update selection//UI/shroud state */

    this->team /* +0x33C */ = newTeam;                /* the write */

    /* register with the new team, refresh UI, notify observers */
}
```

Signature: `__thiscall`, `this` = `Object*`, `param_2` = `Team*`,
`param_3` = a flag that suppresses some cross-player handling when set.

`FUN_00497ab0` is called on a Team and its result compared — almost certainly
`Team::getControllingPlayer()`.

### Why this matters

The mod must **call this**, not write `+0x33C` directly. The function does real work
either side of the write — deregistering from the old team, registering with the new one,
refreshing selection and UI. Poking the pointer would leave the engine's per-team
bookkeeping inconsistent.

### Still to determine

- What `param_3` actually suppresses (pass `0` to mirror the normal path until known).
- How to resolve the local human player's `Team*` at runtime. From the live data the
  player's team pointer was `0x09ded210`, but that is session-specific; the mod needs to
  look it up properly (`PlayerList` -> local player -> default team).
- How to read the current selection on the logic side.

---

# Session 8 — starting the mod; two facts still needed

## NEW: `Object + 0x354` is the "contained by" pointer

Found by re-running the garrison data through the new automated revert analysis:

```
object 0x11c890e0 (a player unit)   +0x354:  0 -> 0x116aa2e0 -> 0
object 0x116aa2e0 (the building)    +0x354:  0x116aa740 -> 0 -> 0x116aa740
```

`0x116aa2e0` is the garrisoned building. So a contained object's `+0x354` points at its
container. This is how the mod can enumerate occupants: walk the object list and match
`+0x354` against the container, with no contain-module traversal needed.

(The building's own `+0x354` also changed, so the field is overloaded per object type —
treat it as "current containment relationship", and verify before relying on it.)

## Confirmed relationships for the mod

```
Object + 0x33C  ->  Team*                       (ownership; setTeam writes this)
Object + 0x354  ->  container Object*           (when garrisoned/loaded)
Team   + 0x04   ->  TeamPrototype*
  prototype + 0x08  ->  Player*                 (from FUN_00497ab0)
```

`Object::setTeam` = `FUN_0063c219` @ `0x0063C219`,
`__thiscall (Object *this, Team *newTeam, char suppressFx)`.

## Vftables recovered (for runtime singleton discovery)

| Class | vftable VAs |
|---|---|
| `PlayerList` | `0x00A4426C`, `0x00A44258` |
| `Player` | `0x00A45AA4`, `0x00A45A94`, `0x00A45A78` |
| `Team` | `0x00A44F40` |

## The two facts still missing

1. **How to resolve the local human player's `Team*`.** At F11 time the selected object
   belongs to the *enemy*, so "me" cannot be derived from the selection. Plan: find
   `ThePlayerList` by scanning the game image's writable globals for a pointer to an
   object whose vftable is `PlayerList`'s. Implemented as the agent's `discoverPlayers`
   export (hotkey `Ctrl+Alt+P`).

2. **How to tell which object is selected.** Plan: the same change-and-revert experiment
   that found the team pointer — snapshot with nothing selected, with one enemy unit
   selected, then deselected. The field that flips on exactly one object and flips back
   is the selection flag. Implemented as the `selection` experiment profile.

Once both are known the mod is straightforward: on F11, find the selected object, resolve
the local player's team, call `setTeam(object, myTeam, 0)`.

## Tooling changes this session

- `analysis.revert_candidates()` / `format_reverts()` — the decisive analysis is now
  automatic, and filters the `+0x01a4` frame-counter noise.
- `watch.py` gained **experiment profiles** (`garrison`, `selection`) so snapshot slots
  are labelled per experiment, plus `Ctrl+Alt+P` for player/team discovery.
- Verified by re-running the stored garrison data through the new code path: it
  reproduces `+0x33C` on the building exactly.

---

# Session 9 — SOLVED: the selection flag

Match: user = GDI (orange), one enemy = NOD (red, Brutal), different map from session 7.
Far noisier than the garrison run because a Brutal AI constantly changes unit state.

## CONFIRMED: `Object + 0x00B4`, bit 0, is the "is selected" flag

The revert analysis produced several 1-object candidates, so the tie was broken with a
sharper test: **a real selection flag must be set on exactly one object while one unit is
selected, and on zero objects when nothing is selected.**

| Offset | value/bit | nothing | SELECTED | deselected | verdict |
|---|---|---|---|---|---|
| **`+0x00B4`** | **bit 0** | **0** | **1** | **0** | **exact match** |
| `+0x00E0` | `0x80000000` | 7 | 6 | 7 | background noise |
| `+0x01C0` | bit `0x10000` | 60 | 92 | 116 | background noise |
| `+0x0198` | `0x80000000` | 319 | 337 | 332 | background noise |

The selected object also belonged to the player with 10 teams (the NOD AI), as expected
since an enemy unit was selected.

So the mod finds the selection by walking the object list and testing
`*(uint32*)(obj + 0xB4) & 1`. No InGameUI reverse engineering required.

## Player/Team structure in a real match

One Player owns MANY Teams — the Brutal AI had **10**. Any logic that assumes
"one team per player" is wrong.

| Player | objects | teams | likely identity |
|---|---|---|---|
| `0x89e8d60` | 920 | 1 | neutral / world |
| `0x89e96e0` | 264 | **10** | NOD AI (many squad teams) |
| `0x89e8fc0` | 47 | 1 | civilian, or the user |
| `0x89e9480` | 24 | 1 | the user, or civilian |

The `Team -> prototype(+0x04) -> Player(+0x08)` chain resolved for **every** team
(`playerVftableKnown = true` throughout), so that relationship is solid.

## STILL OPEN: which Player is the local human

`playerListCandidates` came back **empty** — the scan failed. Two bugs identified:

1. It compared `NativePointer`s with `<` and `>`, which does not compare addresses in
   Frida. Now uses module-scoped `enumerateRanges` (with a `.compare()` fallback).
2. It only looked for a global *pointer* to a heap PlayerList. If the singleton is
   embedded directly in `.data`, the vftable sits at the global itself. Both shapes are
   now checked.

Also added: a dump of each distinct `Player` struct (first `0x400` bytes). The user is
**GDI orange** and the enemy **NOD red**, so the player colour should identify the local
human directly, and comparing the four structs should expose any local/human flag.

Guessing from object counts is not acceptable here — 47 vs 24 is ambiguous, and getting
it wrong would hand units to the wrong player.

---

# Session 10 — discovery hang diagnosed and fixed; design corrections

## The hang was our bug, not the game's

`Ctrl+Alt+P` appeared to freeze. It was not stuck, just pathologically slow:
`discoverPlayers` walked the game's globals four bytes at a time and called
`Process.findRangeByAddress()` for **every** candidate — roughly 190,000 expensive
lookups, then a second pass doing it again.

Rewritten to read whole ranges with `readByteArray` and scan them as `Uint32Array`.
Measured (`selftest.py`, new step 2/4): **20,000 individual reads take 4 ms**, so raw
reads were never the cost — `findRangeByAddress` was. The remaining per-pointer pass is
hard-capped at 20,000 reads so it can never hang again.

## Two design corrections from the user (2026-09-18)

1. **Player colour is chosen before the match**, so it varies between matches.
2. **The enemy can be GDI, NOD or Scrin**, not always NOD.

Consequence: **neither colour nor faction may be used to identify the local player at
runtime.** Colour is still useful as a one-off cross-check while reverse engineering
(the user was orange in both sessions so far), but the mod needs a general mechanism.

The intended general mechanism is a **global that holds the local player's pointer**.
Globals are at fixed VAs in the binary, so once identified it works for any match,
faction, colour or map. `discoverPlayers` now reports, for each Player, every global in
the game image that holds its address — a global referencing exactly one player is the
local-player candidate.

Also beware: **object counts must not be used to guess** which player is which. In the
last match "you or civilian" was 47 vs 24 objects, which is not decisive, and a wrong
guess would hand units to the wrong player.

## Note on the selftest hotkey check

If `Run-Probe.bat` is already running, `selftest.py` reports every hotkey as
`FAILED (error 1409; already in use)`. That is correct behaviour — the running probe
holds them. Close it before running the self-test.

---

# Session 11 — COMPLETE: every address the mod needs

Match: user = **NOD, green**; enemy = **Scrin, yellow** (deliberately different faction and
colour from session 9, so nothing below depends on either).

## Named globals, confirmed from the subsystem registration code

`FUN_00567800` constructs each subsystem and registers it with its name. The call site
pushes (globalSlot, nameString, instance), and reading the name strings gives certainty:

| Global VA | Subsystem |
|---|---|
| `0x00BEE110` | **`ThePlayerList`** |
| `0x00BEE574` | **`TheGameLogic`** |

`0x00BEE574` is the singleton the probe has walked objects through since session 7 — it is
`TheGameLogic`, now properly named rather than assumed.

## The complete chain

```c
/* --- singletons ------------------------------------------------------- */
ThePlayerList  = *(void **)0x00BEE110;
TheGameLogic   = *(void **)0x00BEE574;

/* --- resolving "me" --------------------------------------------------- */
localPlayer    = *(Player **)((char *)ThePlayerList + 0x1C);
myTeam         = *(Team   **)((char *)localPlayer   + 0xE8);

/* --- finding the selection ------------------------------------------- */
obj            = *(Object **)((char *)TheGameLogic + 0xB8);   /* list head */
next           = *(Object **)((char *)obj + 0x78);            /* link      */
isSelected     = *(uint32_t *)((char *)obj + 0xB4) & 1;

/* --- ownership -------------------------------------------------------- */
objTeam        = *(Team **)((char *)obj + 0x33C);
setTeam        = (void (__thiscall *)(void *, void *, char))0x0063C219;
```

### How each was established

- **`PlayerList + 0x1C` = local player.** 662 sites load `ThePlayerList`; of the field
  reads immediately following, `+0x1C` accounts for **96** — an order of magnitude more
  than the next candidate (`+0x24`, 13). Statistically decisive, but *not* directly
  verified against live memory, so the mod should sanity-check it (see below).
- **`Player + 0xE8` = default Team.** Verified live: for 3 of the 4 players, `+0xE8` held
  exactly that player's own Team pointer. The fourth is the civilian player, whose default
  team owned none of the walked objects, so it was absent from the team list — expected,
  not a contradiction.
- **`Object + 0xB4` bit 0 = selected**, **`Object + 0x33C` = Team** — both verified live in
  earlier sessions.

## Colour, for cross-checking only

Player colour lives at `Player + 0x8C` (and `+0x94`) as ARGB. This session:
green `0xff00a744` (the user), yellow `0xfffce953` (Scrin enemy), grey `0xff808080`
(civilian), light grey `0xffc0c0c0` (neutral). **The mod must not use colour** — it is
chosen per match. This is a reverse-engineering cross-check only.

## Required runtime sanity checks

Because `PlayerList + 0x1C` is inferred statistically, the mod must validate before acting:

1. `ThePlayerList` is non-null and readable.
2. The resolved local player's vftable is one of the known `Player` vftables
   (`0x00A45AA4`, `0x00A45A94`, `0x00A45A78`).
3. `localPlayer + 0xE8` is a plausible pointer, and the Team it points at resolves back to
   the same player via `Team+0x4 -> prototype+0x8`.

If any check fails, do nothing and log — never guess, and never write a team pointer
directly.

## Prototype built (session 11)

`Submods/AllianceSwitchTool/src/switch_agent.js` + `switch.py` + `Run-Switch.bat` implement the tool:
select an enemy object, press **F11**, ownership transfers via `Object::setTeam`.

Design points worth keeping when porting to the DLL:

- **The transfer must run on the game's thread.** An `Interceptor` on a per-frame engine
  function performs queued requests at the top of a tick. Calling `setTeam` from an
  arbitrary thread would race the logic tick and risk corrupting per-team object lists.
- **Never write `Object+0x33C` directly** — always call `setTeam`.
- **Validate before acting**, because `PlayerList+0x1C` is statistically inferred:
  check the local player's vftable against the known `Player` vftables, and check the
  team resolves back to the same player. Refuse rather than guess.
- Two `GameLogic` vtable slots are hooked as per-frame candidates (`0x4E7132`,
  `0x4EE108`); call counts at runtime will identify the real update and the other can be
  dropped.

---

# Session 12 — CORRECTION: the selection flag was wrong

## What the in-game test showed

First live test of the prototype (user, 2026-09-18):

- `Ctrl+Alt+S` worked.
- Selecting an enemy unit or structure -> **"nothing selected"**.
- Selecting a garrisoned structure -> reported a successful transfer, but the structure
  did not change hands. Instead an **unselected enemy harvester** became the user's.

The user's read was correct: **`setTeam` works; selection detection does not.**

## `Object + 0xB4 bit 0` is NOT the selection flag

It was accepted on a single sample: exactly one object had it set while one unit was
selected, and none otherwise. An exhaustive rescan of the same data shows **two** bits
with that identical 0/1/0 signature out of roughly 8000 bit positions:

```
  +0x0094 bit 21   0/1/0
  +0x00b4 bit  0   0/1/0     <- the one that was picked, and it is wrong
```

A single select/deselect sample cannot distinguish a real flag from a transient state bit
(0xB4 bit 0 evidently tracks something about harvesters). This is the same failure mode as
the earlier `FUN_005157d5` false lead, except it survived into running code.

**Lesson: a signature that only has to match once is not evidence.** Require the candidate
to predict several different counts.

## Fix: calibration instead of a single sample

`learn_selection.py` (+ `Run-Learn.bat`) asks the user to select a **known number** of
units and say how many. It reports every bit set on exactly that many objects, then
intersects across samples with different counts. A coincidence does not stay correct at
1, then 3, then 5.

The result is saved to `selection_flag.json` and consumed by `switch.py`.

`switch_agent.js` now has **no default selection offset** (`objectSelected: null`) and
refuses to transfer anything until calibrated, rather than acting on a guess. That refusal
is deliberate: transferring the wrong object is worse than doing nothing.

## Bonus: 72 named engine globals recovered

Parsing the subsystem registration in `FUN_00567800` yields the complete table of named
subsystem globals. Selected entries:

| Global | Subsystem |
|---|---|
| `0x00BEE574` | `TheGameLogic` |
| `0x00BEE110` | `ThePlayerList` |
| `0x00BEE120` | `TheTeamFactory` |
| `0x00BEAC00` | `TheGameClient` |
| `0x00BF0430` | `TheScriptEngine` |
| `0x00BF041C` | `TheLuaScriptEngine` |
| `0x00BF34A0` | `TheWritableGlobalData` |
| `0x00BF7618` | `TheThingFactory` |
| `0x00BF4C44` | `TheAudio` |
| `0x00BF36C8` | `TheGameState` |

Full list in `notes/init_asm.txt` (regenerable). Note there is **no `TheInGameUI`** — the
C&C3 UI lives in a separate lib-package and is not registered here. If selection turns out
not to live on `Object` at all, `TheGameClient` and the `Drawable` side are the next place
to look.

## Calibration redesigned around a game constraint (session 12b)

The user pointed out a rule that invalidates the original plan: **you can select several of
your OWN units, but only ONE enemy unit or structure at a time.** So "select 3 enemies"
is impossible, and for the enemy case the count is always 1 — which on its own proves
nothing, since ~128 bits sit on exactly one object at any moment.

Two discriminators are used instead:

1. **Own units at counts 1/3/5.** Coincidences do not track an arbitrary count.
2. **Object identity for the enemy case.** Take several samples with a *different* enemy
   selected each time; the real flag must move to a different object, whereas a stray
   state bit tends to stay on the same one.

They are calibrated **separately and compared**, because selecting an enemy may be a
different mechanism from selecting your own units (an inspect-selection rather than a
command-selection). If the two disagree, **the mod needs the enemy one.**

### The strict zero rule matters

A "nothing selected" sample must disqualify every bit that is set on **any** object, not
just those set on exactly one. Replaying the stored snapshots both ways:

| rule | survivors |
|---|---|
| weak ("not set on exactly one") | 22 |
| **strict ("not set on anything")** | **2** — `+0x0094 bit 21`, `+0x00b4 bit 0` |

The strict version reproduces the exhaustive result exactly, and is what the tool now
uses. The agent returns full per-bit counts and the host applies the rules, so this logic
lives in one place and was verified offline against real captured data before asking for
another play session.

---

# Session 13 — PROVEN: selection is not stored on Object

Calibration run (user, 2026-09-18) gave a clean negative in both modes:

| samples | surviving candidates |
|---|---|
| own units, 1 selected | 33 |
| own units, 3 selected | 2 |
| own units, 5 selected | **0** |
| 1 enemy, sample #1 | 225 |
| 1 enemy, sample #2 | 21 |
| 1 enemy, sample #3 | 14 |
| 1 enemy, sample #4 | **0** |

Nothing on `Object` tracks selection, for either own or enemy units. That is a *useful*
negative: it rules out the entire approach rather than leaving two coincidences to choose
between, which is exactly how `+0xB4 bit 0` slipped through before.

## Consequence: selection lives on `Drawable` (client side)

`Object` is game logic; `Drawable` is its client-side counterpart. This matches the
earlier observation that there is no `TheInGameUI` subsystem — C&C3's UI lives in a
separate lib-package and works on drawables.

Recovered for this:

| Class | vftable VAs |
|---|---|
| `Drawable` | `0x00A4255C`, `0x00A42548` |
| `GameClient` | `0x00A42ABC`, `0x00A42AA4` |
| `TheGameClient` global | `0x00BEAC00` |

The tooling now finds the `Object -> Drawable` link **empirically** rather than assuming
`Object+0xE8`: every field of every object is tested for a pointer whose target carries a
known `Drawable` vftable, and the offsets are tallied. The offset that works for most
objects wins. Calibration then scans drawable memory, but still reports the **Object**
address, since that is what `setTeam` takes.

## More game constraints from the user

- Only **one building** can be selected at a time, owned or not.
- Selecting a building **deselects any units**.

So building samples are always a count of one, and cannot be mixed with unit samples.

## On automating tests (Unicorn)

Unicorn is worth adopting for the **DLL stage**: load the real `cnc3game.dat` bytes at
their true addresses and assert what a function does, as the FFVII branch did. Good for
pinning down `setTeam`'s third parameter offline, and for CI.

It does **not** help find selection state: that is live data the running client builds
from mouse input, not a function that can be called in isolation. The calibration harness
is the right automation for that, and it turned a coin-flip into a decisive answer in four
keypresses.

---

# Session 14 — SOLVED: selection found on Drawable

Calibration converged cleanly (user, 2026-09-18).

## `Object + 0x0104` -> `Drawable`

Found empirically, not assumed: **1030 of 1030 objects (100%)** have a pointer at
`+0x104` whose target carries a known `Drawable` vftable. Worth noting that the earlier
guess would have been `Object+0xE8`; session 4 had in fact already flagged `+0x104` as
"pointer, non-null checked before FX", which corroborates.

## `Drawable + 0x0144` bit 15 = SELECTED

| samples | surviving candidates |
|---|---|
| own, 1 selected | 111 |
| own, 3 selected | 3 |
| own, 5 selected | **1** |
| enemy #1 | 452 |
| enemy #2 | 248 |
| enemy #3 | 3 |
| enemy #4 | **1** |

Both paths converged **independently on the same bit**, and the enemy samples each
reported a **different object** (`0xb94d060`, `0xb946300`, `0xb954f40`, `0x112f4270`),
which is the identity discriminator confirming it tracks the actual selection rather than
sitting on one unlucky unit.

**Own and enemy selection use the same flag**, so no special casing is needed.

Saved to `Submods/AllianceSwitchTool/src/selection_flag.json` and consumed automatically.

## The complete address set (TW 1.10)

```c
ThePlayerList = *(void **)0x00BEE110;
TheGameLogic  = *(void **)0x00BEE574;

localPlayer   = *(Player **)((char *)ThePlayerList + 0x1C);
myTeam        = *(Team   **)((char *)localPlayer   + 0xE8);

obj           = *(Object **)((char *)TheGameLogic + 0xB8);   /* list head */
next          = *(Object **)((char *)obj + 0x78);
drawable      = *(void   **)((char *)obj + 0x104);           /* 100% coverage */
isSelected    = *(uint32_t *)((char *)drawable + 0x144) & (1u << 15);
objTeam       = *(Team **)((char *)obj + 0x33C);

setTeam       = (void (__thiscall *)(void *, void *, char))0x0063C219;
```

Every value above is either verified live or validated at runtime before use.

## Unicorn installed

`unicorn` 2.1.4 added to the venv (pinned in `.tools_and_docs_by_claude/CNC3_ModdingResearch/requirements.txt`) with
permission. Intended use is the **DLL stage**: load the real `cnc3game.dat` bytes at their
true addresses and assert behaviour offline — most immediately to determine what
`setTeam`'s third parameter does without needing a play session. It is not useful for
live UI state.

---

# Session 15 — the tool works; infantry is the last gap

First successful live run (user, 2026-09-18). F11 transferred **vehicles, structures, and
garrisoned buildings**, all confirmed by re-reading the team pointer after the call.

Notably the **garrisoned-building worry did not materialise**: an earlier concern was that
a civilian building's ownership derives from its occupants and might revert. It did not.

## RESOLVED: the per-frame hook

Both `GameLogic` vtable candidates were hooked and their calls counted live:

| candidate | calls | thread |
|---|---|---|
| `0x004E7132` | **0** | - |
| **`0x004EE108`** | **423** | tid 15516 |

So `GameLogic` vftable slot **[13] = `FUN_004EE108`** is the per-frame update, and
`0x4E7132` can be dropped from the DLL. Measured, not guessed.

## Known-good behaviour from the log

```
-> 0xb906cb0:  team 0x9d8d820 -> 0x9d8d1e0  [OK]
-> 0x112a7d80: team 0x9d8de60 -> 0x9d8d1e0  [OK]
-> 0x112a7d80: skipped (already yours)
```

Objects from several different enemy teams (`0x9d8d820`, `0x9d8de60`, `0x9d8d960`,
`0x9d8cce0`) all moved to the user's team `0x9d8d1e0`. The "already yours" guard works.

## OPEN: infantry

C&C3 infantry are **Squads**: one `Squad` with several member `Object`s. Transferring only
the object under the cursor leaves the rest of the squad with the enemy.

`Squad` vftable: `0x00A44768`.

Fix implemented: `findSquadLink()` discovers the `Object -> Squad` field the same way the
Drawable link was found — scan every field for a pointer whose target carries the `Squad`
vftable. **The acceptance rule differs**: only infantry have squads, so the winning offset
covers a *minority* of objects, unlike the Drawable link's 100%. Take the highest count
rather than requiring near-total coverage.

`performTransfer` now expands the selection: for each selected object, if it has a squad,
every object sharing that squad pointer is transferred too. Results are tagged
`[selected]` or `[squad member]` so the expansion is visible in the log.

Unverified in game yet — the squad link offset is discovered at runtime and printed at
startup, so the next run will show whether it was found.

---

# Session 16 — infantry half-transfer; logging and structural inspection added

## Symptom (user, 2026-09-18)

After F11 on enemy infantry:

- The squad **obeys the user's orders**, including force-fire.
- But it keeps the **enemy's colour**, the user's own units **attack it**, and the enemy
  mostly **does not** attack it.
- One soldier of the squad **attacked the user's building** — so part of the squad stayed
  with the enemy.

Contrast with vehicles and structures, where the enemy turns hostile almost immediately,
i.e. the transfer is recognised throughout the engine.

## Reading of the symptom

This is a split between the **logical squad object** and its **member objects**:

- Command authority follows the object that was transferred -> orders work.
- Rendering (colour) and both sides' targeting follow the **member** objects, which were
  not transferred -> enemy colour, wrong allegiance on both sides.

So the squad expansion did not reach the members. Either `findSquadLink()` found nothing,
or it found an offset that does not actually enumerate squad membership.

A promising hypothesis: members may point at their parent through **`Object + 0x354`**,
the "contained by" field identified back in session 8 (a garrisoned unit's `+0x354`
pointed at the building containing it). Squad membership may reuse that relationship.

## Added

- **File logging** (`tee_log.py`): both `switch.py` and `learn_selection.py` now mirror the
  whole console transcript to `logs/<tool>_<timestamp>.log`, flushed eagerly so a game
  crash does not lose it.
- **`Ctrl+Alt+I` — inspect selection.** Dumps, for the selected object: its team, drawable,
  squad pointer, every field pointing at another object in the object list, and — the
  important part — **every object that points back at the selection, and from which
  offset**, each with its team. That reveals the real squad structure instead of guessing
  at it, and will show whether the members link via `+0x354` or something else.

Note: the earlier acceptance rule for `findSquadLink()` deliberately differs from the
Drawable link's, because only infantry have squads, so the correct offset covers a
minority of objects rather than ~100%.

---

# Session 17 — CORRECTION: `+0x354` is a team list link, not "contained by"

The `Ctrl+Alt+I` inspect output settles what the Object-to-Object links actually are.

## The links, read off live data

Selected enemy infantry `0xb935b10`, before F11:

```
0x112d0ab0 --(+0x354)--> SELECTED --(+0x354)--> 0x112d1c30     (all enemy team)
SELECTED   --(+0x358)--> 0x112d0ab0
```

After F11 (`SELECTED` now on the user's team):

```
SELECTED   --(+0x358)--> 0x112dcf90            (user's team)
0x112dcf90 --(+0x354)--> SELECTED
```

| Offset | Meaning |
|---|---|
| `+0x78` / `+0xEC` | global object list: next / prev |
| **`+0x354` / `+0x358`** | **the owning Team's member list: next / prev** |

So on transfer the object is unlinked from the enemy team's list and spliced into the
user's. That is `setTeam` behaving correctly, and it is visible in the data.

**This corrects session 8**, which recorded `+0x354` as "contained by" on the strength of
the garrison snapshots. In that experiment the garrisoned unit and its building happened
to be adjacent in the team list, so the field looked like containment. It is not.

## `Squad` is a dead end as approached

`findSquadLink()` reported **not found**: no `Object` field in the first `0x400` bytes
points at anything carrying the `Squad` vftable. So either squad membership is stored
past `0x400`, or it is not a direct `Object*` field at all (for example a vector of
member pointers, where the field points at an array rather than at an Object).

`0x400` was an arbitrary early choice; nothing ever established `Object`'s real size.

## Added: `Ctrl+Alt+K` deep scan

Scans to `0x1000` instead of `0x400`, and follows **one level of indirection**: for any
field pointing at a block that is not itself an Object, it reads the first 32 dwords of
that block and reports any that *are* Objects. That catches a member array or vector,
which is the most likely shape for squad membership.

## Why the half-transfer looks the way it does

Command authority follows the transferred object, so orders work. Colour and both sides'
targeting follow the member objects, which were never moved — hence enemy colour, the
user's units attacking it, and one soldier still fighting for the enemy.

---

# Session 18 — Object size found; squad is 5 contiguous objects

## `Object` is `0x460` bytes

The deep scan on selected enemy infantry `0x11562a20` returned "related" objects at:

```
0x11562a20  (selected)
0x11562e80  = +0x460
0x115632e0  = +0x8c0
0x11563740  = +0xd20
0x11563ba0  = +0x1180
```

Exactly `0x460` apart. The apparently self-referential fields confirm the size: reading
`+0x54c` is really the *neighbour's* `+0xec` (`0x54c - 0x460 = 0xec`, the global-list prev
pointer), which points back at the selection. So scanning past `0x460` just reads the next
object, and every "field" beyond that in the earlier dump was an artefact.

`SCAN_DEPTH` is now `OBJECT_SIZE = 0x460`.

## The half-transfer, measured

Before and after one F11 on `0x11562a20`:

| object | before | after |
|---|---|---|
| `0x11562a20` (selected) | enemy | **user's team `0xa03d1e0`** |
| `0x11562e80` | enemy | still enemy `0xa03de60` |
| `0x115632e0` | enemy | still enemy |
| `0x11563740` | enemy | still enemy |
| `0x11563ba0` | enemy | still enemy |

**A 5-soldier squad: one transferred, four left behind.** That is exactly the reported
behaviour — the user commands the transferred soldier, while the rest keep the enemy's
colour and allegiance, and one of them attacked the user's building.

(Note the enemy team id changed between the scan and the transfer, `0xa03d960` ->
`0xa03de60`. The AI reassigns its many teams constantly; this is normal and not a bug.)

## Squad membership is still not located

Within the real `0x460` extent, the only Object-to-Object pointers are the two list link
pairs (`+0x78`/`+0xEC` global, `+0x354`/`+0x358` team). There is **no** squad field.

The "pointer-to-block" hits at `+0x290`-`+0x314` are behaviour/update **modules**: each
block holds a back-pointer to its owning Object (which is why the selection appeared at
block indices 3 and 9). Not a member roster.

Contiguous allocation is a real pattern but an allocation accident, so it must not be used
as the membership rule.

## Added: `Ctrl+Alt+G` — heap-wide Squad instance search

Since no `Object` points at a `Squad`, the relationship may run the other way. This scans
every writable range for the `Squad` vftable (`0x00A44768`), and for each instance reports
which live Objects it points at.

It also answers a more basic question: whether the `Squad` class is used in C&C3 at all.
Plenty of RTTI entries are inherited from earlier SAGE titles and never instantiated. If
zero instances exist, infantry grouping works some other way and the `Squad` avenue can be
abandoned rather than pursued further.

---

# Session 19 — `Squad` is unused; infantry are HORDES

## `Squad` is vestigial RTTI

Heap-wide scan for the `Squad` vftable (`0x00A44768`) across **512 MB**: **zero live
instances**. The class exists in RTTI but is never instantiated in C&C3 — inherited from
an earlier SAGE title. Every attempt to reach squad membership through `Squad` was
therefore doomed, and that avenue is now closed rather than merely unpromising.

## Infantry squads are **Hordes**

The vftable map contains the real vocabulary:

```
HordeContain          HordeContainInterface     HordeAIUpdate
HordeMemberCollide    HordeGarrisonContain      HordeTransportContain
AIHordeMemberStats    HordeMeleeBehavior        ContainModuleInterface
```

SAGE calls a multi-soldier infantry unit a **horde** (the term comes from Battle for
Middle-earth). So the member roster lives in a `HordeContain`-style **module** attached to
the Object, which is consistent with everything observed: no roster in the Object body,
but several module blocks hanging off it.

## New capability: vftable -> class name at runtime

`DumpVftableMap.java` exports **5133** vftable addresses with their class names to
`Submods/AllianceSwitchTool/src/vftables.json`. At runtime any pointer can now be identified by
reading its first dword and looking it up. Verified: `0xa4255c -> Drawable`,
`0xa44768 -> Squad`, `0xa45aa4 -> Player`.

This turns the anonymous blocks hanging off an Object into named modules, and is reusable
for any future work on this game.

## Added: `Ctrl+Alt+M` module scan

For the selected Object, walks its real `0x460` extent, and for every pointer that is not
itself an Object reports the target's vftable (named via the map) plus any live Objects
that block references. A `HordeContain` block listing the other soldiers is exactly what
we expect to see.

## Hotkey note

`Ctrl+Alt+G` clashes with **Google Drive** on this machine and was silently denied
(`error 1409`). The module scan therefore uses `Ctrl+Alt+M`. Worth remembering that
third-party apps squat on Ctrl+Alt combos.

---

# Session 20 — `HordeContain` located on the infantry Object

The module scan, with blocks named via `vftables.json`, found the real structure on a
selected enemy infantry Object (`0x11286410`, `objectSize=0x460`):

| Offset | Block | Class |
|---|---|---|
| `+0x0104` | `0x11659980` | **`Drawable`** (confirms the calibrated link) |
| `+0x0290` | `0x11a4b7d0` | `ObjectSMCHelper` |
| `+0x0294` | `0xa1fb078` | `ObjectDefectionHelper` |
| `+0x02ac` | `0x11421a68` | `EmotionTrackerUpdate` |
| **`+0x02b0`** | `0x11a2f920` | **`HordeContain`** |
| `+0x02c0` | `0x11345b8c` | `ImmortalBody` |
| `+0x02e4` | `0x11421938` | `AttributeModifierPoolUpdate` |
| `+0x02e8` | `0x11aefb38` | **`HordeAIUpdate`** |
| `+0x0004` | `0x107be308` | `ThingTemplate` |

This confirms infantry are hordes, and that `Object + 0x104 -> Drawable` is correct
(independently re-derived here from the vftable, having originally come from calibration).

## The roster is behind a pointer, not inline

`HordeContain` reported **`refs=0`**: no Object pointers inside its first `0x200` bytes.
But the fields immediately after it look like a `std::vector`:

```
+0x02b0 -> 0x11a2f920   HordeContain
+0x02b4 -> 0x113b8fd8   (first dword = 0x11a2f920)
+0x02b8 -> 0x113b8fdc
+0x02bc -> 0x113b8fdc
```

`begin` / `end` / `capacity_end`, with `end == capacity_end` - a full vector. So the member
list is reached through a pointer and is one level deeper than a flat module scan reaches.

## Added: `Ctrl+Alt+H` horde dig, and `dumpBlock`

`dumpBlock(addr, size)` walks a block field by field and, for every pointer, checks whether
it points at a **range of Object pointers** (a vector's `begin`), stopping at the first
hole. That is the shape a roster has.

`Ctrl+Alt+H` runs the module scan, picks out every block whose class name contains "Horde"
or "Contain", and digs into each, printing any array of Objects as a candidate roster with
each member's team.

## Note

`ObjectDefectionHelper` appears as a real module here - the class chased back in session 3
as a possible ownership route. It exists per-Object, but the ownership path we settled on
(`Object::setTeam`) is the correct one and is proven in play.

---

# Session 21 — the selected infantry object's `HordeContain` is EMPTY

`Ctrl+Alt+H` on selected enemy infantry `0x113f7e30`:

```
HordeContain  at +0x02b0 -> 0x11d14de0
    +0x0024  0xb5dfd28  (AIDependencyNode)          <- no roster

HordeAIUpdate at +0x02e8 -> 0x11c09008
    +0x000c  OBJECT 0x113f7e30   (back-pointer to itself)
    +0x0024  OBJECT 0x113f7e30   (back-pointer to itself)
    +0x0040  0x11c58f78  (AIStateMachine)
    +0x01f4  0x11c095b0  (Locomotor)
```

So the object owns a `HordeContain` **with no members**. Two readings:

1. The selected object is a horde **member**, and the parent is a different object.
2. Every soldier carries a (mostly unused) `HordeContain`, and grouping is expressed some
   other way entirely.

## An assumption never actually measured

Across every log so far there is exactly **one** `status` line, and it reads
`selected: 0`. So it has never been established how many objects the game flags as
selected when an infantry squad is clicked.

That number decides the shape of the problem:

- **1** -> the squad has a single selectable representative, and the other soldiers must be
  reached through some ownership relation.
- **5** -> there is no parent, and `selectedObjects()` should already have returned all
  five. If so, F11 transferring only one means the *selection scan* is stopping early, not
  that the roster is missing.

The second possibility has not been ruled out, and it would be a different bug entirely.

## Added: `Ctrl+Alt+J` horde survey

Walks **every** object in the match, finds any module whose vftable belongs to one of the
57 Horde/Contain classes (taken straight from `vftables.json`, not hardcoded), digs behind
it for a run of Object pointers, and reports every object that owns a **non-empty** roster.

It also reports:

- `selectedCount` - settling the question above;
- whether the current selection appears as **someone else's member**, which directly
  distinguishes reading 1 from reading 2.

---

# Session 22 — horde containment RULED OUT; an unverified assumption exposed

`Ctrl+Alt+J` horde survey, run twice on selected enemy infantry:

```
objects        : 1153            objects        : 1140
SELECTED COUNT : 1               SELECTED COUNT : 1
contain modules: 36              contain modules: 44
owners with a roster: 0          owners with a roster: 1
the selection is NOT listed as any horde's member.
```

The single "roster" in run 2 was a `HordeAIUpdate` field pointing at an object on a
**different team** - an attack target, not a squadmate.

## Conclusions

1. **`SELECTED COUNT` is 1.** Clicking an infantry squad flags exactly one object, so the
   selection scan is not stopping early. The earlier alternative explanation (a bug in our
   own loop) is eliminated.
2. **No horde rosters exist anywhere.** 36-44 contain modules are live and all are empty.
   Horde containment is not how C&C3 groups infantry at runtime.
3. **The selection is not a member of anything.**

## The assumption that was never tested

Since session 18 the working premise has been that the four objects allocated at
`+0x460` intervals around the selected infantry object are its squadmates. **That was
never verified** - it rested purely on contiguous addresses and a shared team.

They may equally be separate units the AI built at the same time and the allocator placed
together. If so, the entire "squad roster" hunt has been chasing something that does not
exist, and the real reason infantry misbehaves is different.

## Added: `Ctrl+Alt+N` identify

Reads each object's `ThingTemplate` (`Object + 0x04`) and extracts the first readable
ASCII string from it, for the selection and its neighbours in the object list.

This distinguishes the two cases directly:

- five entries named like `NODMilitantSquad` -> five **separate** squads, contiguity is an
  allocation artefact, and there is no roster to find;
- one `...Squad` plus four `...Militant` style entries -> a real parent/member split, and
  the link must be found some other way.

Reading the template name also gives the tooling a general "what is this object?"
capability, which is useful well beyond this question.

---

# Session 23 — identify produced garbage; name extraction was too loose

`Ctrl+Alt+N` returned strings like:

```
fxfxcqanblehelembk]ieeehcaf]f[eb\dYdYeYf[fZe\h[d\eacci]h^j`mboap
```

That is binary data (floats, most likely) misread as text, not a template name. The
validator accepted **any printable run of 3+ characters**, which almost anything passes.
It also varied between runs on the same object, which is the giveaway.

Fixed by requiring a strict identifier shape (`^[A-Za-z][A-Za-z0-9_]{3,47}$`) *and* at
least one uppercase letter, since template names look like `NODMilitantSquad`.

## A better signal than strings: the ThingTemplate POINTER

Objects of the same unit type share one `ThingTemplate` (`Object + 0x04`). Comparing
pointers needs no string parsing and cannot produce noise:

- selection and neighbours **share** a template -> same unit type, so contiguity is an
  allocation artefact and there is no parent/member split to find;
- selection has a **different** template from its neighbours -> a real parent/member
  split.

`identify` now also counts how many objects in the whole match share the selection's
template, broken down by team. Useful sanity data regardless: it says how many of that
unit type each side currently has.

## Observation from the raw output

Even without names, the three runs already show the neighbours sharing the selection's
**team** and sitting in one contiguous block of the object list, while the objects two
places before it sit on *different* teams (`0xa867210`, `0xa867d50`). So the block is real
- the open question is only whether it is one squad or several same-type units.

---

# Session 24 — the selected infantry object has a UNIQUE ThingTemplate

`Ctrl+Alt+N` with template-pointer comparison, on selected enemy infantry:

```
selected 0x1129a150   tpl=0x107ce308
objects sharing that template: 1          <- unique in the whole match
    team 0x9dedd10: 1
```

Neighbours by template:

| index | object | template | note |
|---|---|---|---|
| 1118 | `0x11709ae0` | `0x1079ccb8` | |
| 1119-1120 | | `0x10aaec40` | a pair |
| 1121 | `0x11296410` | `0x107bf540` | |
| **1122** | **`0x1129a150`** | **`0x107ce308`** | **selected, unique** |
| 1123-1128 | | `0x10b03f98` | **a run of six** |

So the selection is **not** part of a same-template run; it is one of a kind, and a block
of **six identical objects** sits immediately after it on the same team.

That shape fits a squad controller plus its soldiers, and would explain the symptom
exactly: transferring the controller grants command, while the six soldiers stay with the
enemy and so keep the enemy's colour and get attacked by the user's units.

**It is not proven.** Three previous plausible-looking patterns (`+0xB4 bit 0`, `+0x354`
"contained by", the `Squad` class) all turned out to be coincidences, so this is treated as
a hypothesis until the objects are actually named.

## Name extraction has failed twice

- **Too loose**: accepted any printable run of 3+ chars, returned float data as text, and
  gave different answers on repeated runs of the same object - the giveaway.
- **Too strict**: after requiring an identifier shape plus an uppercase letter, nothing
  matched at all, so the assumed layout (pointer -> AsciiStringData -> text at +8) is
  simply wrong.

## Added: `Ctrl+Alt+T` template hexdump

Rather than guess a third time at the string layout, dump the raw bytes of the
selection's `ThingTemplate` and two neighbours', as hex + ASCII. If names are stored
inline they will be plainly visible; if they are behind a pointer, the pointer layout will
be visible instead.

The host-side renderer is unit-tested against a synthetic buffer containing
`NODMilitantSquad`, so a real name will be unmistakable.

## Session 24b — Frida RPC drops ArrayBuffers

The template hexdump returned `(no data)` for every entry. Cause: **Frida's RPC replies
are JSON**, so an `ArrayBuffer` placed inside a returned object is silently dropped and
arrives as `{}`. Binary must be encoded explicitly (or returned as the second element of a
tuple).

Fixed with a `toHex()` helper in the agent; the host renders it with `bytes.fromhex()`.
Round-trip verified offline against a synthetic buffer containing `NODMilitantSquad`.

Worth remembering for the DLL port and for any future Frida work here: **anything binary
crossing the RPC boundary needs encoding.**

Incidentally the run confirmed the object layout is stable across repeated runs: the
selection kept `tpl=0x107ce308` (unique), and its neighbours `tpl=0x10b03f98` (the run of
six), across three consecutive dumps.

---

# Session 25 — ThingTemplate layout read from raw bytes

With the RPC encoding fixed, the template hexdump finally produced data.

Selected infantry object `0x9da1380`, template `0x107fe308`:

```
+0x00  a8 81 aa 00   ThingTemplate vftable (0x00AA81A8)   <- confirms identity
+0x04  6e 96 96 90   differs per template; looks like a name hash
+0x10  18 30 7f 10   pointer, per-template
+0x14  a8 11 80 10   pointer, per-template
+0x18  e0 a6 7f 10   pointer, per-template
+0x1c  01 00 00 00
+0x20  c0 e1 ad 08   IDENTICAL across templates  -> shared/default data
+0x24  a0 e2 ad 08   identical
+0x28  18 e2 ad 08   identical
```

Neighbour (template `0x10b33f98`) has the same shape with different values at
`+0x04/+0x10/+0x14/+0x18`, and the *same* values at `+0x20`-`+0x28`.

So the per-template identity lives at `+0x04` (hash) and the three pointers at
`+0x10/+0x14/+0x18`.

## Why the name reader kept failing

Both earlier attempts read those pointers as ASCII and found nothing usable. A likely
explanation is **UTF-16**: `readCString` stops at the first NUL byte, so a wide string
yields a single character, which then fails any sane name validator.

Rather than guess an encoding, the dump now also prints 48 bytes from **every pointer in
the template's first `0x40`**, rendered both as ASCII and as UTF-16LE. Whichever column is
readable settles the encoding and the offset at once.

## Note on `+0x04`

If that dword is a name hash it is per-template and stable, so even without readable names
it can serve as a **type identity**: two objects with the same hash are the same unit type.
That is enough to answer the controller-versus-soldier question if the strings prove
awkward.

## Session 25b — template names are UTF-16; a real gap found in our scans

### Names decoded

`ThingTemplate` display strings are **UTF-16LE**, which is why two attempts to read them
as ASCII failed (`readCString` stops at the first NUL byte, yielding one character):

| Offset | Content |
|---|---|
| `+0x04` | **hash, not a pointer** (dereferencing it produced `z.gameboys.com` garbage) |
| `+0x20` | display name, e.g. **`Rifleman Squad`** |
| `+0x24` | abilities text |
| `+0x28` | description, e.g. `Basic MG Infantry. Strong...` |
| `+0x34` | display name again |

### The selected object and its neighbours are BOTH "Rifleman Squad"

Different templates (`0x107be308` vs `0x10af3f98`), different hashes, but the **same
display-name pointer** (`0x8a9e1c0`). So one unit type is presented by two templates - the
squad and its members - which is consistent with the controller/soldiers hypothesis.

Display name therefore **cannot** distinguish them; the template pointer or hash can.

### A real gap: every object scan stopped at `0x400`

`Object` is `0x460`. Six separate scans - including the referrer scan that looks for
objects pointing *at* the selection - were still looping `0 .. 0x400`, so **the last
`0x60` bytes of every object were never examined by any of them**. A squad back-pointer
living there would have been invisible.

All six now use `OBJECT_SIZE`. The `identify` scan over `ThingTemplate` legitimately keeps
its own limit, since that is a different structure.

`inspectSelection` additionally reports each referrer's and referee's **template**, so
squad members are recognisable at a glance: six objects sharing one template, all pointing
at the selection, would settle it.

---

# Session 26 — no stored squad link exists in `Object`, at any offset

`Ctrl+Alt+I` re-run with every object scan widened to the true `OBJECT_SIZE` (`0x460`),
so the previously unexamined `0x400`-`0x460` region was finally covered:

```
SELECTED 0x9e61c20  team=0xa958110
  points to 4 object(s):
      +0x0078 -> 0x1136a150   tpl=0x10bd3f98      global list next
      +0x00ec -> 0x11366410   tpl=0x1088f540      global list prev
      +0x0354 -> 0x1136a150   tpl=0x10bd3f98      team list next
      +0x0358 -> 0x117da3c0   tpl=0x10b7ec40      team list prev
  referenced by 3 object(s):   (the same list neighbours)
```

Only the two link pairs, in both directions. **There is no squad pointer in `Object` at
any offset.** The wider scan closed the last gap in that theory rather than finding
something in it.

Note the three referrers have three *different* templates, so in this sample the
neighbours are not a run of identical soldiers - unlike the earlier capture. The
contiguous-block pattern is therefore not reliable either, which further weakens the
"neighbours are squadmates" reading.

## What has now been eliminated

1. A `Squad*` field on `Object` - no such field.
2. The `Squad` class itself - zero live instances in 512 MB.
3. Horde containment rosters - 36-44 contain modules, all empty.
4. A pointer anywhere in `Object`'s full `0x460` extent, in either direction.
5. Contiguous allocation as a membership rule - not consistent between samples.

## Remaining hypothesis: a linked list

Every scan so far has looked for **arrays or direct pointers**. If membership is a
**linked list**, each hop is a separate heap allocation and no flat scan can see it.

`HordeContain` held a pointer to an **`AIDependencyNode`** - a *node*, which is exactly
what a list is built from. That lead was noted in session 21 and never followed.

## Added: `Ctrl+Alt+R` reach-out

Breadth-first walk outward from the selected Object through intermediate blocks, up to
three levels deep, reporting every live Object reached **and the path taken**. Objects are
scanned to `OBJECT_SIZE`, other blocks more shallowly, with the four known list-link
offsets excluded as noise.

Results are grouped by `ThingTemplate`, since a squad's members share one. Several objects
of a single template reached through a non-list path would be the roster, and the printed
path would show exactly how to walk it.

---

# Session 27 — reach-out negative too; pivoting to observing creation

`Ctrl+Alt+R`, run twice, reached 16 and 14 objects respectively at depth 3. They spread
across ~11 different templates and **three different teams**, via inconsistent paths, and
the two runs disagree. That is incidental pointer noise (AI targets and similar), not a
roster.

## Everything ruled out so far

| Theory | Verdict |
|---|---|
| `Squad*` field on `Object` | no such field |
| `Squad` class | zero live instances in 512 MB |
| Horde containment roster | 36-44 modules, all empty |
| Pointer anywhere in `Object`'s `0x460` | none, either direction |
| Contiguous allocation | inconsistent between samples |
| Linked list reachable from the Object | reach-out found only noise |

**There is no discoverable stored link from a squad object to its soldiers.**

## New approach: watch the relationship being created

`Object::Object` = **`FUN_00659E77` @ `0x00659E77`**, found by locating the two writers of
Object's primary vftable `0x00AA3D90` (the other, `FUN_0065A906`, is the destructor). It
also writes `+0x33C`, consistent with a constructor initialising the team.

Hooking it records every object creation in order, with its `ThingTemplate` and the
**return address of whatever created it**. If a squad's soldiers are constructed from
inside the squad's own setup, they appear as a burst sharing a caller - the relationship
expressed as behaviour rather than as a stored pointer.

Added `Ctrl+Alt+W` (start watching) and `Ctrl+Alt+O` (dump log). The dump groups events
into bursts separated by 250 ms and summarises the callers per burst.

Best procedure: watch, then **build one infantry squad yourself**, so the timing is known
and the burst is unambiguous - rather than waiting on the AI.

## Housekeeping

`logs/` cleaned: 39 MB -> 20 KB. Deleted the analysed garrison and selection snapshots,
`players.json`, `report.txt` and the old switch logs; kept the two most recent logs.
`selection_flag.json` and `vftables.json` live outside `logs/` and were untouched.

---

# Session 28 — BREAKTHROUGH: a squad is 1 + 9 objects, built together

Watching `Object::Object` while the user built **one NOD Militant Squad** (Hand of Nod):

```
burst 1: 10 object(s) in 597 ms
    0x112be6f0  tpl=0x1086bee8   <- ONE object, unique template   (the squad)
    0x112beb50  tpl=0x10c0be08   <- NINE objects, shared template (the members)
    0x112befb0  tpl=0x10c0be08
    ... seven more ...
    callers: 0x4afaf9 x10
```

This is the parent/member split **observed being created**, not inferred from a pattern.
It also explains the contiguity seen since session 18: the ten objects are consecutive at
exactly `0x460` intervals because they are allocated back to back.

A Militant Squad is therefore **1 squad object + 9 member objects**. Transferring only the
squad object leaves nine soldiers with the enemy - exactly the reported symptom.

## The caller was only an allocator

`0x4afaf9` sits in `FUN_004afad1`:

```c
iVar1 = FUN_005ef3e4(0x460);        /* operator new, Object size */
if (iVar1 != 0) {
    return FUN_00659e77(...);       /* Object::Object */
}
```

Just `operator new` + constructor. Incidentally this **independently confirms
`OBJECT_SIZE = 0x460`**, which had only been inferred from address spacing.

So the squad-building code is further up the stack; the immediate return address is not
enough.

## Fix: capture a real backtrace

The creation watch now records up to 8 stack frames per creation (`Backtracer.ACCURATE`,
falling back to `FUZZY`), filtered to frames inside the game image and reported as RVAs.

The dump prints the stack for the **first** and **last** object of each burst. The squad
object and its members will share frames above the allocator, and the deepest shared frame
is the function that builds a squad - which is where the membership relationship is
established, and therefore where it can be read or replicated.

## Session 28b — `Thread.backtrace` is unreliable here

The captured stacks were identical for every object in the burst:

```
0x4afaf9 <- 0x659e77 <- 0x64f99d <- 0x4d0456 <- 0x616a38 <- 0x4ee6d7 <- ...
```

but they are **not a real call chain**:

- `0x659e77` is `Object::Object` itself, shown as the caller of its own allocator;
- `0x64f99d` is not inside any function;
- `0x4d0456` resolves to `FUN_004d0439`, a scalar-deleting **destructor**.

These are stale stack slots. `Backtracer.ACCURATE` needs unwind data and `FUZZY` guesses,
and this 32-bit binary without frame pointers defeats both. **Do not trust
`Thread.backtrace` in this game.**

## Fix: hook the allocator wrapper instead

`FUN_004afad1` (`0x004AFAD1`) is `operator new(0x460)` + `Object::Object`. Hooking *it*
gives a trustworthy **immediate** return address - the code that actually asked for an
object - with no unwinding involved. The new object comes back in `EAX`, so one hook
yields both the object and its true creator.

The dump now lists the **distinct callers** per burst. If the squad object and its members
are requested from different call sites, that names both functions directly; if all ten
share one caller, that single function both creates and links them.

---

# Session 29 — peeling wrappers, and a validated stack scan

Hooking the allocator gave one distinct caller for all ten objects: `0x4b24ea`, inside:

```c
void __thiscall FUN_004b24d5(void *this, ...) {
    FUN_004afad1(this, param_2, param_3, param_4, param_5);   /* alloc + ctor */
}
```

Another thin forwarding wrapper. The creation path is therefore several wrappers deep:

```
??? -> FUN_004b24d5 -> FUN_004afad1 -> operator new(0x460) + Object::Object
```

Chasing it one hook per play session is too slow, and `Thread.backtrace` cannot be used
here (session 28b).

## Validated stack scan

`stackReturnAddresses()` reads the raw stack and keeps only values that are **provably**
return addresses: the instruction immediately before each candidate must be a `CALL` -
either `E8 rel32` (checked 5 bytes back) or `FF /2` (checked 2-7 bytes back, modrm reg
field 2). Stale stack slots essentially never satisfy that, which is exactly what defeated
the built-in backtracer.

The creation watch now records up to 12 validated frames per object, and the dump prints:

- the first object's frames, and
- **the frames shared by every object in the burst** - the squad-building code, since all
  ten objects pass through it.

This should name the real function in one run rather than one wrapper at a time.

## Note on a lower-priority correction

`CREATION_LOG_CAP` is 400 and each entry now carries up to 12 frames; the log is taken and
cleared on dump, so growth is bounded. The scan costs roughly 1k memory reads per object
creation, which at a few creations per second is negligible - but the hook should still be
started only for the measurement, not left running.

---

# Session 30 — the link is a SHARED VALUE, not a pointer: `Object + 0x100`

The validated stack scan worked, and the run captured **two** bursts, which gave a free
control group:

```
squad (10 objects): 0x4b24ea <- 0x676a77 <- 0x623b09 <- 0x64f99d <- 0x917748 <- 0x91bb5c <- 0x4ee6d7
other (4 objects) : 0x4b24ea <- 0x676a77 <-                                     0x4ee6d7 <- 0x7f58c6 <- ...
```

Frames common to both are generic (`0x4b24ea` allocator wrapper, `0x676a77`, `0x4ee6d7`).
`0x917748`/`0x91bb5c` proved to be CRT critical-section internals - real return addresses,
but heap plumbing. That leaves **`0x623b09`** as the squad-specific frame.

## `FUN_00623ac7` - the member-creation helper

```c
iVar3 = FUN_006769f0(param_2, *(this+0xc)->[0x33c], param_3, param_4);
                   /* create an object on the PARENT's team (+0x33C) */
...
FUN_004f1777(*(this+0xc), iVar3, 1);        /* event/audio plumbing, not the link */
*(iVar3 + 0x100) = *(*(this+0xc) + 0x100);  /* <<< copies +0x100 parent -> child */
```

`this+0xc` is the parent Object; `FUN_006769f0(template, team, ...)` is the object factory
(the same call appears in `HijackerUpdate`).

`FUN_004f1777` was checked and rejected: it calls `FUN_005f660f`, the same EVA/sound event
helper used inside `setTeam`, so it announces rather than links.

## Why every previous search failed

Members and their parent share a **value** at `+0x100`, not a pointer to one another.
Every scan so far looked for *references* - direct pointers, arrays, vectors, linked
lists, back-pointers. A shared integer is invisible to all of them.

That single line explains 8 sessions of negative results.

## Added: `Ctrl+Alt+U` group probe

Reads `+0x100` on the selection and finds every object with the same value, then reports
how discriminating the field is overall (distinct values, largest group, count of zeroes)
so a mere flag cannot masquerade as a group key. `+0xFC` and `+0x108` are probed too, in
case the copy in the decompiler output is off by a field.

**Falsifiable prediction:** for a selected infantry squad, `+0x100` should match about
**10 objects, all on one team** - one with the squad template and nine with the member
template. If it matches everything, or only itself, the reading is wrong.

---

# Session 31 — `+0x100` FALSIFIED; trying ID-based references

The group probe was run three times and gave a stable, unambiguous refutation:

```
+0x100  key=0x0  matches=1147/1147  distinct=1   (zero on EVERY object)
+0x0fc  key=0x0  matches=1147/1147  distinct=1
+0x108  key=0x0  matches=1144/1147  distinct=4
```

`Object + 0x100` is **zero on every object in the match**. It is not a group key, so the
shared-value theory at that offset is dead. Either that copy only runs in circumstances
that do not apply here, or `0x623b09` was a stale stack slot that passed the CALL check by
chance - the validator reduces false positives but cannot eliminate them when scanning
0x400 bytes of stack.

Prediction made, prediction tested, prediction wrong. Recorded rather than quietly
abandoned.

## The underlying idea still stands, aimed better

SAGE identifies objects by **`ObjectID`**, an integer. If a member references its squad by
**ID** rather than by pointer:

- every pointer scan run so far (direct, array, vector, linked list, back-pointer, deep
  reach-out) is structurally incapable of finding it, and
- probing one guessed offset, as `+0x100` did, would miss it unless the guess was exact.

## Added: `Ctrl+Alt+Y` id scan

1. **Finds the ID field empirically** rather than guessing: for every offset, count
   distinct values across all objects and keep offsets that are near-unique per object
   with small non-zero values.
2. **Searches by value in both directions**: every field of every object for the
   selection's ID, and every field of the selection for any other object's ID.

Unlike the `+0x100` probe this does not depend on guessing an offset - it tests all of
them. If a squad relationship is stored as an ID anywhere in an object, this finds it.

Expected shape if the theory holds: about **nine objects storing the selection's ID** at
one common offset, all on the same team and sharing the member template.

---

# Session 32 — SOLVED: squad membership is stored BY OBJECT ID

The id scan, run twice with identical results:

```
ObjectID field candidates (near-unique small integers):
    +0x00e8  distinct=1146/1146          <- unique for every object

using +0x00e8 as the id (mine=0x486)
  objects storing MY id: 6
      0x9e820c0 at +0x00f0  team=0x9eddd50  tpl=0x10bf4000
      ... five more, identical offset, team and template ...
```

## The answer

| Offset | Meaning |
|---|---|
| **`Object + 0x0E8`** | **`ObjectID`** - unique per object (1146 distinct of 1146) |
| **`Object + 0x0F0`** | **the parent squad's `ObjectID`** (0 for non-members) |

**Members reference their squad by ID, never by pointer.** That is why eight sessions of
pointer scanning found nothing: there was no pointer to find. Direct fields, arrays,
vectors, linked lists, back-pointers and a three-level reach-out were all structurally
incapable of seeing an integer.

The six referrers were identical across both runs, at one offset, on one team, sharing one
template - none of the flapping that marked every false lead.

## Implemented

`performTransfer` now expands the selection by ID:

```
squadId = (selection.parentId != 0) ? selection.parentId : selection.id
transfer every object where  id == squadId  or  parentId == squadId
```

This works whether the squad object or a member is selected, since it resolves to the
squad id either way.

**Guard:** object ids are small integers, so a coincidental match is possible (the "ids
stored BY me" direction was visibly noisy and varied run to run). Only objects that
currently share the selection's team are moved; anything else is reported as skipped
rather than transferred. Wrongly flipping an unrelated unit would be worse than
transferring nothing.

Results are tagged `selected`, `squad parent` or `squad member`.

## Note on the earlier `+0x100` claim

`Object + 0x100` was predicted to be the group key and **falsified** - zero on all 1147
objects. The prediction came from a decompiled line in `FUN_00623ac7`, reached via a stack
frame that may itself have been stale. The correct answer was found by testing *every*
offset rather than one guess, which is the same method that found the selection flag.

---

# Session 33 — REGRESSION: `+0xF0` is not squad membership

The ID-based expansion shipped in session 32 made the tool **worse**, and was reverted.

## What the user saw

- Infantry still not fully transferred (still enemy colour, no ownership sound, enemy
  ignores it) - "feels like hijacking".
- **F11 on a vehicle also flipped nearby vehicles.**
- **F11 on a building flipped a group of buildings.**

## What the log shows

```
squad id 0x4a7 ... 4 transferred + 8 skipped (different team)
squad id 0x468 ... 2 transferred + 10 skipped (different team)
```

A dozen objects, spread across several teams, share each supposed "squad id". A real squad
is one parent plus its members, on one team. So **`Object + 0x0F0` is not a parent-squad
reference.** Something much more common fits - a producer/builder id would explain it:
everything a factory built shares one value.

The same-team guard prevented cross-team damage but not same-team damage, which is exactly
the grouped vehicles and buildings that were observed. The guard limited the blast radius;
it did not make the premise true.

## Why the earlier evidence looked convincing

The id scan reported **only the first matching offset per object** (`break` after the first
hit). Six infantry objects matched at `+0xF0`, on one team with one template - genuinely
clean - but that was one instance of a broad field, and any more specific offset later in
the object was masked by the early `break`.

Generalising from a single sample of one unit type is what went wrong; the vehicle and
building cases were never checked before the behaviour shipped.

## Actions

1. **Squad expansion disabled** (`OFF.squadExpansion = false`). F11 transfers only the
   selected object again, restoring the behaviour that worked for vehicles, structures and
   garrisoned buildings. It can be re-enabled through `setSquadExpansion` for testing.
2. **`idScan` fixed** to report *every* matching offset, with a per-offset summary: match
   count, number of distinct **teams**, and number of distinct **templates**. An offset
   matching a handful of objects on **one team with one template** is a real candidate; one
   spanning several teams is not.

## Correct next step

Run the fixed id scan on **infantry, a vehicle, and a building**, and compare. The squad
offset must show a tight single-team single-template group for infantry and **nothing**
for the other two. Confirming the negative cases matters as much as the positive one - had
that been done before session 32, this regression would not have shipped.

---

# Session 34 — SOLVED PROPERLY: `Object + 0x324` is squad membership

Running the fixed id scan on **three** selections settled it. Comparing the negative cases
is what produced the answer; one sample could never have.

| selection | `+0x00F0` | `+0x0324` |
|---|---|---|
| **infantry** | 6 matches, 1 team, 1 template | **6 matches, 1 team, 1 template** |
| **vehicle** | - | **nothing** (zero referrers at all) |
| **building** | 4 matches, **2 teams, 2 templates** | **nothing** |

## Conclusions

- **`Object + 0x0324` = the squad's `ObjectID`**, stored by each member. Present only for
  infantry, tight on one team and one template, absent for vehicles and buildings.
- **`Object + 0x00F0` = a broad producer/builder reference.** For a building it spans two
  teams and two templates. Expanding on it is what flipped neighbouring vehicles and
  groups of buildings in session 32.
- **`Object + 0x00E8` = `ObjectID`**, unique per object (unchanged).

`+0x324` was invisible earlier purely because the scan stopped at the first matching
offset: it sits directly behind `+0xF0` in the same object.

## Implemented

```
squadId = (selection[+0x324] != 0) ? selection[+0x324] : selection[+0xE8]
transfer every object where  [+0xE8] == squadId  or  [+0x324] == squadId
```

Works whether the squad object or one of its soldiers is selected. Vehicles and buildings
have nothing at `+0x324`, so they resolve to themselves and gain no extra members.

Guards retained and added:

- **same team** as the selection, or the object is reported skipped rather than moved;
- **`maxSquadSize = 24`** - if more related objects turn up than a squad could plausibly
  contain, nothing extra is moved and the reason is reported. A wrong assumption should
  fail loudly and harmlessly rather than mass-transfer.

Verified offline against the three observed cases: infantry expands to 7 objects, vehicle
and building stay at 1.

---

# Session 35 — `+0x324` is CONTAINMENT, and it nests

Transferring garrisoned buildings moved exactly **two** objects each time:

```
squad id 0x530 - expanding to its members
  -> [selected]     0xa8ee9a0:  team ... -> ...  [OK]
  -> [squad member] 0x12190bb0: team ... -> ...  [OK]
  (2 objects transferred in total)
```

The building plus one other object - not the soldiers inside.

## What this means

`Object + 0x324` is **not squad membership specifically**. It is "the object I belong to /
am inside" - a containment parent id - and it **nests**:

```
building (id B)
  squad object        +0x324 = B
    soldiers          +0x324 = squad id
```

So a garrisoned building is two levels deep, while plain infantry is one. The expansion
descended a single level, which is why the building and its squad object moved but the nine
soldiers did not.

The field was named after the first case it was seen in ("squad membership"), and that name
silently became an assumption - the same failure mode as `+0xF0` in session 32, though this
time it degraded gracefully instead of causing damage.

## Fix

The expansion now walks the **whole subtree** beneath the root: repeated passes, each
adding newly claimed objects' ids as roots for the next, with a depth limit of 6.

`maxSquadSize` raised from 24 to **48**: a building holding several squads legitimately
reaches the high twenties. Exceeding it still refuses and reports, rather than
mass-transferring.

Verified offline against five shapes before asking for a play test: vehicle 1, empty
building 1, infantry squad 10, garrisoned building 11, building with two squads 21.

Labels changed from `squad parent`/`squad member` to `contained root`/`contained lvlN`,
since the relationship is containment rather than squads.

---

# Session 36 — garrisoned buildings REVERT: the occupants are not being moved

Reported: F11 on an enemy garrisoned building with 2 infantry inside transfers the
building, but evacuating produces **enemy** infantry.

## The log shows more than the symptom

The same building (`0x9de19a0`, id `0x1f8`) appears twice:

```
15:26:26  -> [selected] 0x9de19a0: team 0x9ded810 -> 0x9ded1d0  [OK]   (+3 contained)
15:27:05  -> [selected] 0x9de19a0: team 0x9ded810 -> 0x9ded1d0  [OK]
          -> 6 objects skipped: "matched but on a different team"
```

It transferred, then **reverted to the enemy**, and on the second press the objects moved
the first time are on a *different* team (still the user's) while the building is enemy
again.

This is exactly the session 7 finding coming back: **a garrisoned building's ownership is
derived from its occupants.** The real soldiers are still enemy, so the building reverts,
and evacuation yields enemy infantry. The concern raised in session 8 - that a garrisoned
building might revert - was right after all; it simply did not show up while testing empty
buildings.

## Working hypothesis

The 3-4 objects currently moved are not the occupants. Garrisoned units are removed from
the map, and may not be in the **global object list** at all - in which case the `+0x324`
walk cannot see them, because it only iterates that list.

Session 7 recorded objects vanishing from the list on garrison; they were attributed to
fleeing civilians because they were on the neutral team. That attribution may have been
incomplete.

## Added: `Ctrl+Alt+C` contain report

Rather than infer, compare two views of the same building:

1. the containment tree reachable via `+0x324` **within the object list**, and
2. every object referenced by the building's **contain modules** (`GarrisonContain` and
   friends), following one level of indirection for vector-style storage.

Then print the difference. Anything in view 2 but not view 1 is an occupant currently
being missed, and its module and offset are reported so the roster can be read directly.

If view 2 is also empty, the occupants are not reachable from the object list at all and
must be enumerated from the contain module's own storage - which is what the report is
designed to expose.

## Session 36b — the occupants ARE reachable; the pointer-only module scan was blind

`Ctrl+Alt+C` on an enemy garrisoned building (`0x11e66e00`, id `0x530`):

```
via +0x324 tree: 3
    0x121e5560  id=0x7c8 parent=0x530   tpl=0x108accb8   <- squad object
    0x121e59c0  id=0x7c9 parent=0x7c8   tpl=0x10bbec40   <- soldier
    0x121e5e20  id=0x7ca parent=0x7c8   tpl=0x10bbec40   <- soldier
via contain modules: 0
```

So the earlier hypothesis - that garrisoned units leave the global object list and are
therefore invisible - is **wrong**. The tree reaches two levels and finds the soldiers, and
they are on the building's team. That matches the "4 objects transferred" already seen.

Two things stand out instead:

1. **Only 2 soldiers**, where a militant squad has nine. Either the squad was down to two,
   or most members do not exist as objects while garrisoned.
2. **`via contain modules: 0`** - but the module scan only looks for object **pointers**,
   and this engine stores relationships **by ObjectID**. A `GarrisonContain` roster held as
   ids would be completely invisible to it: the same blind spot that hid `+0x324` for eight
   sessions.

## Fix to the diagnostic

The contain report now:

- builds an `ObjectID -> object` map and searches each contain module for **id values**, not
  just pointers, tagging each hit `by id` or `by pointer`;
- lists every contain-style module found **even when it references nothing**, with its class
  name - knowing a `GarrisonContain` exists but looks empty is itself a result.

The same mistake in two different diagnostics is worth naming: **in this engine, always
search by id as well as by pointer.**

---

# Session 37 — `HordeGarrisonContain` holds occupants BY ID

`Ctrl+Alt+C` before and after F11 on an enemy garrisoned building (`0xb8d1530`, id `0x530`):

```
contain-style modules present: 1
    +0x02b0 -> 0xa231c98  HordeGarrisonContain

via contain modules: 7   (searched BY ID - a pointer-only scan found 0)
```

Searching the module for **id values** rather than pointers finally exposed the roster.
The two soldiers already in the `+0x324` tree appear at consecutive offsets
(`+0x13c`/`+0x140` before, `+0x154`/`+0x158` after - the list shifts), which confirms the
entries are real and stored as a contiguous run of ids.

## What is actually wrong

The transfer works better than the symptom implied. After F11 the building **and** all
three tree objects are on the user's team and stay there - no revert in this capture.

But one module-referenced object does not move:

```
before:  0x11b45030  team=0xafd9690 (enemy)   at HordeGarrisonContain +0x1e4
after:   0x11b45030  team=0xafd9690 (STILL enemy)
```

That is an occupant reachable only through the contain module, not through `+0x324`.
Since a garrisoned building's ownership derives from its occupants, a single missed
occupant is enough to make the building revert and to make evacuation produce enemy
infantry.

The other "missing" entries are on **different teams** - false positives from matching
small integers across 0x200 bytes of module data. This is why the same-team guard matters.

## Implemented

`performTransfer` now also collects objects referenced **by id** from contain-style modules
on the selection, subject to the existing guards: same team as the selection, and the
`maxSquadSize` ceiling. They are labelled `garrison occupant`.

The contain-module vftables are supplied by the host from `vftables.json` (191 classes), so
nothing is hardcoded.

## Latent bug fixed while here

`vftables.json` stores **static** VAs, but runtime vftable values include the ASLR slide.
Three comparisons were using static values directly. This game loads at its preferred base
(`slide=0x0` in every log so far), so it worked by luck. All three now convert through
`rt()` before comparing.

---

# Session 38 — garrison transfer WORKS; cosmetic pop-out remains

A garrisoned building with several squads transferred cleanly: **22 objects**, every one
confirmed, including `garrison occupant` entries found through `HordeGarrisonContain` by
id. Evacuated infantry are now the user's.

Remaining issue is purely visual: occupants sometimes **pop out of the building model** at
transfer time while staying garrisoned.

## `Object::setTeam` read in full

The relevant tail:

```c
*(int *)((int)this + 0x33c) = param_2;        /* the team write */
...
if (*(int *)((int)this + 0x104) != 0) {       /* has a Drawable */
    FUN_00479398();                            /* forced drawable refresh */
}
```

Every transfer forces a **drawable refresh**. That is what recolours a unit - desirable
normally - but on a *contained* unit it re-evaluates visibility, which is the pop-out.

Two other details worth recording:

- **`param_3` is not the answer.** It gates exactly one block: a player-relationship
  notification (`FUN_005cd769`) when both teams exist and the controlling player actually
  changes. Nothing visual. Passing 1 would not affect the pop-out, only suppress that
  notification.
- `setTeam` **deselects** the object if it was selected: it tests
  `Drawable + 0x145 & 0x80`, which is bit 15 of the dword at `Drawable + 0x144` - the very
  selection flag calibrated in session 14. Two independent routes to the same bit.

## Added: `Ctrl+Alt+X` drawable deltas

The transfer now snapshots each object's `Drawable` (first `0x200` bytes) **either side of
the `setTeam` call** and records which fields changed.

Reported per object, plus a tally of how often each offset changed across all transferred
objects. The discriminator:

- a field changing on **every** object is the recolour, expected;
- a field changing only on **contained** objects is the visibility state to restore.

This is the same measure-rather-than-guess approach that found the selection flag and the
containment id. If the visibility field is identified, the fix is to re-apply it after
`setTeam` - a small, targeted, cosmetic-only write.

## Session 38b — the drawable snapshot measured nothing: window too small

Across four F11 tests on garrisoned buildings, every transferred object reported
**0 fields changed** in its `Drawable`:

```
DRAWABLE DELTAS: 5 object(s)
  0x11ab4770 [selected]          drawable=0x12f0e6e0  0 field(s) changed
  0xb8f86d0  [contained lvl1]    drawable=0xa5cc8c0   0 field(s) changed
  0x11b45030 [garrison occupant] drawable=0x9e68e80   0 field(s) changed
```

That is not evidence that `setTeam` leaves the drawable alone. A transfer **visibly
recolours** the unit, so something must change. The measurement is therefore wrong, in one
or both of these ways:

1. **The window was too small.** `0x200` was picked arbitrarily, exactly the mistake made
   with `Object` in session 18 (where `0x400` was assumed and the real size is `0x460`).
   `Drawable`'s size has never been established.
2. **Part of the refresh is deferred.** `FUN_00479398` may only flag the drawable dirty,
   with the real work happening on the next client frame - after the snapshot was taken.
   This fits the user's report that the pop-out happens "at random times" rather than every
   transfer.

## Changes

- `DRAWABLE_SNAP_BYTES` raised `0x200` -> **`0x800`**.
- Added a **deferred re-check**: each transferred object's drawable is compared again from
  the per-frame hook on the following tick, and reported as `deferred (next tick)`
  alongside the `immediate` result.

The two together distinguish the cases: if immediate stays empty while deferred shows
changes, the refresh is asynchronous and there is nothing to "restore" at transfer time.

## Session 38c — the pop-out is DEFERRED animation state; stopping here

With the window widened to `0x800` and a next-tick re-check added, the picture is clear.

**Immediate (inside `setTeam`)** - exactly one field, on every object:

```
+0x0488   0xfffd7602 -> 0xffd50000        the team colour
```

That is the desired recolour and nothing else. The earlier "0 fields changed" was purely
the `0x200` window being too small.

**Deferred (next tick)** - the colour again, plus:

```
+0x0130   0xbb0 -> 0xffffffff
+0x05f0   0xbb0 -> 0xffffffff
(sometimes +0x0134, +0x05f4, +0x0584..+0x05a8)
```

The old values (`0xbb0`, `0xce0`) are **frame numbers**, and they are being reset to
`0xffffffff` - an invalid/never sentinel. These are animation/model-condition timestamps,
and they are cleared **after** `setTeam` returns, on the client's own tick.

## The user's observation is the explanation

> "The infantry popped out when it was firing their weapon."

In C&C3 garrisoned infantry appear at the building's windows to fire - that is standard
engine behaviour. The transfer resets the animation timestamps, so the model-condition
state is re-evaluated from scratch, and the firing/at-window pose is what shows.

## Decision: stop here, as agreed

The relevant changes are **deferred-only**. There is no clobbered field to restore at
transfer time; the state is recomputed by the client after the fact. "Fixing" it would mean
writing to client animation state on an ongoing basis - real crash risk and ongoing
maintenance, for a purely cosmetic quirk on a tool that otherwise works.

Recorded as a **known cosmetic limitation**.

## One cheap check worth doing (no code required)

Watch an **untouched enemy** garrisoned building while its infantry fire at you. If those
soldiers also appear at the windows, the pop-out is normal C&C3 behaviour rather than
anything the mod causes - it simply became noticeable once the occupants started fighting
for the player.

## Session 38d — CORRECTION: the pop-out IS mod-caused

The user checked an untouched enemy garrisoned building: normally **only the weapon shows
through slits in the building, never the whole unit model**, whether firing or not.

So the earlier suggestion that this might be normal C&C3 behaviour is **wrong**, and the
"stop here" reasoning that partly rested on it no longer holds. The full model appearing is
caused by the transfer.

## Revised reading of the deferred data

The fields going from a frame number to `0xFFFFFFFF` (`Drawable + 0x130`, `+0x5F0`, and
neighbours) are animation/model-condition timestamps being invalidated. The engine then
recomputes which model to draw and chooses the full body instead of the garrisoned
weapon-through-slit representation.

Note what did **not** appear in the snapshot: the model-condition flags themselves. Only
timestamps changed within `0x800`. So the selector is either beyond that window or lives in
the Drawable's draw sub-modules (SAGE keeps model state in `W3DModelDraw`-style modules).
That is consistent with a *recompute* rather than a *clobber* - which is why restoring a
single field still looks unpromising.

## Low-risk fix attempted first: transfer order

Previously the container was transferred **first**, then its occupants - so the container's
state was rebuilt while the occupants were still mid-transfer, and each occupant's later
refresh had no settled containment to re-establish.

Now the deepest objects go first and the container **last**:

```
contained lvl2  ->  contained lvl1 / garrison occupant  ->  contained root  ->  selected
```

Pure reordering: no extra writes, nothing touching animation state, no new failure modes.
If the container's own refresh then re-establishes its occupants' pose, the glitch resolves
with no risk taken.

## Zero-code diagnostic worth running

Evacuate a transferred squad and re-garrison it. If the weapon-through-slit appearance
returns, the state **is** recoverable through the engine's own garrison path - which both
gives the user a workaround and suggests what the mod could invoke. If it stays wrong, the
state is not recoverable that way and the remaining options all involve writing client
animation state.

## Session 38e — the state IS recoverable; hunting the entry handler

User findings:

1. **Transfer order made no difference.** The reordering (children before container) is
   harmless and was kept, but it is not the fix.
2. **Exiting and re-entering the building fixes the visuals.** So the engine rebuilds
   garrison state on the *entry* path, and an ownership change never triggers that rebuild.
3. **The building's slits are faction-specific**, modelled after the *owner's* faction
   regardless of the occupants' faction. After a takeover the previous owner's slits remain
   until the squad exits and re-enters.
4. Rarely, a slit model appears **flipped** on one side after takeover.

Point 2 is the important one: this is not a lost cause. The correct state is reproducible
through the engine's own code path, so the mod only needs to invoke whatever that path runs.

Points 3 and 4 fit the same root cause: garrison visuals (the building's slits *and* the
occupants' representation) are established when units enter, not recomputed on ownership
change.

## Added: `Ctrl+Alt+Z` / `Ctrl+Alt+V` - measure the entry handler

`HordeGarrisonContain`'s vftable is at `0x00AA34F4`, with ~30 live slots. Rather than guess
which one handles "a unit entered" - guessing has produced three wrong answers in this
project - hook **every** slot and count calls while the user re-garrisons.

Calls are filtered to the selected building's own module instance, so other buildings in the
match do not pollute the counts. `Ctrl+Alt+V` reports the counts and removes the hooks.

The slot that fires on entry (and not otherwise) is the handler to re-run after a transfer.
This is the same measure-don't-guess method that settled the per-frame update (423 calls
versus 0).

## Session 38f — transfer reordering REVERTED (it caused a regression)

The "children first, container last" reordering from session 38e was **reverted**.

It did not fix the visuals, and it **broke the transfer**: the building became the
player's while the infantry did not - the original garrison bug returning.

Likely mechanism: moving an occupant while its container still belongs to the enemy leaves
the two on different teams, and the engine reconciles that by reverting the occupant.
Transferring the container **first** avoids the mismatch, which is why the original order
worked.

Order restored to: **selection (the container) first, then contained objects.**

Recorded as a comment in `performTransfer` so the idea is not retried. It was proposed as
"pure reordering, no new risk" - which was wrong. Ordering *is* semantic here, because the
engine enforces consistency between a container and its occupants.

## Session 38g — contain-module call counts; need a control run

Watching `HordeGarrisonContain` (25 of 44 slots hookable) while the user evacuated and
re-garrisoned a squad:

```
slot 24  0x5e7836  calls=782     <- per-frame update, noise
slot 34  0x5c454d  calls=189     <- per-frame update, noise
slot 16  0x5c4651  calls=43
slot 17  0x5d520c  calls=32
slot 20  0x6310f8  calls=10
slot 25  0x6560fa  calls=9
slot 29  0x6610e2  calls=3
slot 28  0x619718  calls=3
slot 27  0x5c4440  calls=3
slot 38  0x618707  calls=1
slot 35  0x630632  calls=1
```

The high-frequency slots are clearly per-frame work. The candidates are the rare ones:
three slots firing exactly **3** times (27, 28, 29) and two firing **once** (35, 38).

Counting alone cannot separate them - the same limitation that made the single-sample
selection-flag guess wrong in session 12.

## Two discriminators added

1. **Arguments.** An "object entered" handler receives the entering `Object` as its first
   argument. The watch now records the first stack argument for each call and resolves it
   against the live object list **on read** (too expensive inside the hook). A slot whose
   argument is a real Object, on the right team, with the occupant template, is the target.
2. **A negative control.** Run the watch while doing *nothing*, so per-frame noise is
   measured on its own. Slots that fire only during garrison activity are what matter.

The control is the same technique that finally separated `+0x324` from `+0xF0` in session
34: comparing against cases where the thing should NOT happen.

## Session 38h — control run isolates the candidates; 10 calls == squad size

Two watches on the user's OWN garrisoned Militant Squad (9 visible soldiers). F11 is not
needed for this measurement - the entry path is ordinary engine behaviour, and using an
owned building removes ownership as a variable.

**Control (watch, do nothing, dump):**

```
slot 34  calls=436
slot 17  calls=436
```

Only two slots, both high-frequency: per-frame updates, now positively identified as noise.

**With evacuate + re-garrison:**

```
slot 25  calls=30  args: soldier objects (tpl 0x10d3bda0)
slot 20  calls=24  args: the squad object (tpl 0x1099be80)
slot 29  calls=10  args: soldiers, all DISTINCT
slot 28  calls=10  args: squad object + soldiers
slot 27  calls=10  (no object args captured)
slot 38  calls=1
slot 35  calls=1
```

**Three slots fired exactly 10 times: 9 soldiers + 1 squad object.** That is the signature
of a per-unit containment handler, and it was only recognisable because the squad size was
known - a good argument for always capturing the expected count before measuring.

Slots 38 and 35 firing once each look like "container became empty" / "became occupied".

## Remaining ambiguity

The run combined **evacuate and re-garrison**, so a "unit left" handler and a "unit entered"
handler would each fire 10 times and are indistinguishable by count.

Next: split the actions.

- Watch -> **evacuate only** -> dump.
- Watch -> **re-garrison only** -> dump.

The slot that fires in the second but not the first is the entry handler, and that is what
the mod should invoke for each occupant after a transfer.

## Session 38i — entry/exit handlers IDENTIFIED; decision: do not call them

Splitting the actions gave a completely clean separation.

**Exit only:**

```
slot 29  0x6610E2  calls=10      <- unit LEFT
slot 35  0x630632  calls=1
slot 25  0x6560FA  calls=20
```

**Entry only:**

```
slot 28  0x619718  calls=10      <- unit ENTERED
slot 27  0x5C4440  calls=10
slot 16  0x5C4651  calls=28
slot 20  0x6310F8  calls=22
slot 25  0x6560FA  calls=10
```

`10 = 9 soldiers + 1 squad object`, matching the squad exactly.

### `HordeGarrisonContain` (vftable `0x00AA34F4`)

| Slot | RVA | Meaning |
|---|---|---|
| 28 | `0x00619718` | object **entered** the container |
| 27 | `0x005C4440` | also fires on entry, once per object |
| 29 | `0x006610E2` | object **left** the container |
| 16 | `0x005C4651` | entry-related, 28 calls |
| 35 | `0x00630632` | fires once on exit (container emptied?) |
| 34, 17 | `0x005C454D`, `0x005D520C` | per-frame updates (confirmed by control run) |

### Decision: identified, but NOT invoked

Calling these to repair the garrison visuals was considered and rejected:

1. **Entry is a sequence, not a call.** Four slots fire on entry with different counts
   (10, 10, 28, 22). Replaying that out of context means guessing ordering and arguments
   for several functions - the classic source of crashes.
2. **They are event handlers, not refreshers.** Slot 28 announces a transition that, after
   a transfer, has already happened. Re-invoking it asserts something false: likely
   re-assigning window positions, incrementing occupancy counts, or allocating per-occupant
   state twice.
3. **The window-slot state is already visibly fragile** - the user reports a rare
   wrong-facing slit after takeover. That is exactly the state an entry handler touches.
4. **Cosmetic, with a working user-side workaround** (evacuate and re-garrison).

Everything shipped so far is either read-only or goes through `Object::setTeam`, which is
designed to be called and does its own bookkeeping. This would be the first call to a
function never meant to be invoked externally, on a tool that otherwise works.

The addresses are recorded so a future attempt - ideally in the DLL, with a test harness -
can start from measured facts rather than repeating the search.

---

# Session 39 — campaign session analysed; no functional differences

The user ran the tool through a **campaign** mission. Totals:

```
F11 presses     : 47
objects moved   : 89
skipped (guard) : 32
errors          : 0
NOT CONFIRMED   : 0
```

Every transfer confirmed. Largest single transfer: 10 objects.

## What is the same

- **Local player resolution is stable.** Every transfer targeted one destination team
  (`0xa2239c0`) across the whole session, so `ThePlayerList + 0x1C -> Player + 0xE8`
  behaves identically in campaign.
- Selection, containment expansion and garrison occupants all worked; one garrisoned
  building moved 4 objects including a `garrison occupant`.
- `slide=0x0` again, as in every session.

## What is different

**Many more teams.** Nine distinct *source* teams appeared:

```
0xa223b00 x22   0xa225540 x13   0xa224dc0 x9    0xa225680 x4
0xa225400 x4    0xa224b40 x2    0xa2257c0 x1    0xa2252c0 x1   0xa223c40 x1
```

Skirmish showed 3-13 teams total; campaign uses many scripted teams, which is expected.

Consequence: the **skip ratio is high** (32 skipped against 89 moved). Those are
`squad id matched but on a different team` - id false positives. With more teams in play,
more unrelated objects share small integer ids, so the same-team guard rejects more
candidates. It is working exactly as intended; without it, campaign would be where the
session-32 regression would have done the most damage.

## Risks worth flagging (gameplay, not tool bugs)

Campaign missions are script-driven, so transferring ownership can interact with mission
logic in ways skirmish cannot:

1. **Objective triggers.** A mission that waits on "destroy all enemy units in area" may
   never fire if those units became the player's instead of dying.
2. **Scripted units.** Campaign objects are often referenced by script name and expected to
   belong to a specific player. Changing ownership may break a trigger that reads their
   owner.
3. **Mission-critical objects.** Taking an objective target (a structure that must be
   destroyed or defended) could make a mission unwinnable.
4. **Neutral/civilian objects** are transferable like anything else, and some are used as
   scripted props.

None of these are defects in the tool - it does exactly what it is asked. They are
consequences of using it in a scripted context, and worth knowing before relying on it
mid-mission. Saving before experimenting is the practical mitigation.

## Tidy-up

The startup line `squad link: not found` referred to the abandoned `Squad`-class approach
(zero live instances in this game) and read like a failure every session. Replaced with the
mechanism actually in use: `containment: Object +0x324`.

---

# Session 40 — quality-of-life: opt-in logging, selectable sound

## Capturing satisfies "destroy" objectives

User observation: a mission objective to destroy two specific structures **completed when
they were transferred instead**. So the engine appears to treat "no longer the enemy's" as
satisfying a destroy condition. Useful, and it means the campaign risk noted in session 39
is smaller than feared - at least for that objective type.

## Logging is now opt-in

Off by default in both `switch.py` and `learn_selection.py`; pass `logging` (or `--log`).
It had been on since it was added and quietly accumulated 29 files. A play session should
not write files unless asked.

## Sound: why the game's own audio is not used

C&C3 keeps its audio in `EnglishAudio.big` (191 MB, `BIG4` format) as **hash-named**
entries:

```
data\global_english\cdata\166b084d.46410f77.00002053.4233f9b5.cdata
```

4259 entries, no readable names. `Misc.big` by contrast has ordinary names
(`russellsquare.ttf`, `window\apt\checkbox.wnd`), so the hashing is specific to the
streamed asset archives.

Mapping "engineer captures building" to its hash would mean parsing the SAGE `.manifest`
format (`global_common.manifest`, 252 KB; `static_common.manifest`, 510 KB) and then
decoding the `.cdata` audio container. That is a reverse-engineering project comparable in
size to the rest of this work.

The alternative - calling the engine's audio subsystem (`TheAudio` at `0x00BF4C44`) to
play an event - means invoking engine functions for a cosmetic feature, which was
explicitly declined for the garrison visuals in session 38i. The same reasoning applies.

**Implemented instead:** `sound.py` plays ordinary WAV files from `sounds/`, falling back
per-event to the original beeps when a file is absent. Same result, no reverse
engineering, no engine calls, and the user can choose any sounds.

| Flag | Effect |
|---|---|
| *(none)* | WAV files from `sounds/`, beeps as fallback |
| `cmdsound` | the original console beeps |
| `nosound` | silence |
| `logging` | write a transcript to `logs/` |

Unrecognised arguments are still treated as the process name.

Added `big_extract.py`, which lists and extracts from `BIGF`/`BIG4` archives, in case the
manifest route is attempted later.

---

# Session 41 — audio asset names RECOVERED (the manifest is RefPack)

The user supplied `cc3tools` and `pyBIG-master`, which changed the picture completely.

## What the tools revealed

`audiodump.exe` leaked the format in its strings:

```
Streams (*.manifest)
%s\cdata\%08x.%08x.%08x.%08x.cdata
?StreamOpen@@YAPAUManifest@@PBD0PAU1@@Z
```

plus it ships `libmpg123-0.dll`, so C&C3 stream audio is **MP3** inside `.cdata`.

Both `audiodump.exe` and `bigextract.exe` are **GUI only** (`GetOpenFileNameA`), so they
cannot be driven from a script.

## The breakthrough: the manifest is RefPack compressed

`global_english.manifest` begins `10 fb 0f 9b` - **`0x10FB` is EA RefPack**. pyBIG ships a
RefPack implementation:

```
global_english.manifest   340,855 -> 1,022,807 bytes   10,421 asset names
global_english.bin         53,991 ->   317,600 bytes    4,255 names
```

Names are stored as a plain string table, `AudioEvent:BuildingCaptured\0`.

**The names were never missing - they were compressed.** The earlier conclusion that this
needed a large reverse-engineering project was wrong, and it was wrong because the file was
judged by its raw bytes without checking for a compression header.

## The sounds requested exist, by exact name

```
AudioEvent:BuildingCaptured        <- engineer capture
AudioFile:Geva_MissionFailed       <- GDI mission failed
AudioFile:Neva_MissionFailed       <- NOD
AudioFile:Aeva_MissionFailed       <- Scrin
```

## What is still unresolved: name -> hash

`.cdata` filenames are four dwords, constant prefix `166b084d.46410f77` (the type). Tried
FNV-1a/FNV-1/djb2/sdbm/CRC32 over several spellings of `AudioFile`/`AudioEvent`: **no
match**, so SAGE uses its own hash. Finding it means more RE - and `audiodump.exe` already
implements the whole path, so that is the sensible route.

## New tooling (all reusable)

| Script | Purpose |
|---|---|
| `big_extract.py` | list/extract/extract-all from `BIGF` and `BIG4` archives |
| `audio_names.py` | decompress manifests via pyBIG RefPack and list asset names |

`EnglishAudio.big` has been unpacked to `.tmp/EnglishAudio/` (193 MB, 4254 `.cdata` plus
the manifest) in the layout `audiodump.exe` expects, so only the GUI step remains.

---

## Later sessions (2026-09-18/19) — where the story continues

This log stops at the audio work. Everything after it is written up in
`.claude_memory/AllianceSwitchTool_Journey.md` (sections 8-9); the reverse-
engineering facts it produced are summarised here so this file does not end
mid-story.

**Audio: resolved.** The name -> hash question above never needed answering.
The user extracted the audio with the GUI tools in `cc3tools` into
`.tmp/ExtractedV1.0Audio/` (4254 named WAVs), and the tool plays three of them.

**New engine facts**

| Address / offset | Meaning | Found by |
|---|---|---|
| `Object + 0x300` | `SlavedUpdate` when present; its `+0x04` is the parent's ObjectID | turret and hub agreed on the root; a tank had none |
| `0x00647220` | 14-byte thunk into `Object::setTeam`; exactly 3 callers | hooking setTeam during a live engineer capture |
| `0x00652C42` | **full change-owner**: transfers an object and asks its modules to hand over what they hold. Called as `(this, Team*, 1, 0, 0, 1, 1)` | following that call chain, then recording the engine's own arguments |
| `Object + 0x29c` | the **module list**, a NULL-terminated array — modules are not direct fields | the loop at the end of `0x00652C42` |
| `Team + 0x04 -> + 0x08` | Team -> TeamPrototype -> Player | used to compare ownership by **Player**: garrisoned infantry sit on a different Team than their building |

**Method lesson:** six structural probes failed to find how an air tower knows
its parked aircraft, because the association is not stored at all. Hooking the
engine while it performed the operation (an engineer capture) found the answer
in two measurements.
