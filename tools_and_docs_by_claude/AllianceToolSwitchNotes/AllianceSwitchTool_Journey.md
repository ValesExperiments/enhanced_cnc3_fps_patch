# AllianceSwitchTool — how it was built

Select an enemy unit or structure in **Command & Conquer 3: Tiberium Wars 1.9 or 1.10**, press
**F11**, and it becomes yours. A full, permanent ownership transfer performed by the
engine's own code — not a temporary hijack or mind-control effect.

This document is the engineering story: what was tried, what failed, why it failed, and how
the working answer was eventually found. The dead ends are included deliberately. Several
of them looked completely convincing at the time, and the reasons they were wrong are more
useful than the final addresses.

Everything below was verified against a running game. Nothing here is assumed.

---

## 1. What the tool does

```
ThePlayerList = *(void**)0x00BEE110
TheGameLogic  = *(void**)0x00BEE574

localPlayer   = *(Player**)(ThePlayerList + 0x1C)
myTeam        = *(Team**)(localPlayer + 0xE8)

for each object in TheGameLogic's object list:
    drawable   = *(void**)(object + 0x104)
    isSelected = *(uint32*)(drawable + 0x144) & (1 << 15)

squadId = object[+0x324] ? object[+0x324] : object[+0xE8]
targets = every object where [+0xE8] == squadId or [+0x324] == squadId

for each target:  changeOwner(target, myTeam, 1, 0, 0, 1, 1)   // 0x00652C42, __thiscall
                  // garrisons and transports: setTeam(target, myTeam, 0) @ 0x0063C219
```

That is the shape the tool ended with (1.10 addresses; 1.9's are in `versions.json`,
see "Supporting 1.9"). It started with `setTeam` for everything; section 9 explains how
the engine's own change-owner routine was found, and section 13 why garrisons and
transports still use `setTeam`.

### The address table

| Address / offset | Meaning | How it was established |
|---|---|---|
| `0x00BEE110` | `ThePlayerList` | subsystem registration table |
| `0x00BEE574` | `TheGameLogic` | subsystem registration table |
| `0x0063C219` | `Object::setTeam(Team*, char)` | only 8 writers of `Object+0x33C` |
| `0x00659E77` | `Object::Object` | writes Object's primary vftable |
| `0x004AFAD1` | `operator new(0x460)` + ctor | caller of the constructor |
| `0x004EE108` | `GameLogic` per-frame update | measured: 423 calls vs 0 |
| `PlayerList + 0x1C` | local player | 96 of 662 call sites read it |
| `Player + 0xE8` | default `Team` | verified live on 3 of 4 players |
| `Team + 0x04` → `+0x08` | prototype → `Player` | from `FUN_00497ab0` |
| `Object` size | `0x460` | allocator argument, twice confirmed |
| `Object + 0x04` | `ThingTemplate*` | vftable `0x00AA81A8` |
| `Object + 0x78` / `+0xEC` | global object list next / prev | observed |
| `Object + 0xB8` (on GameLogic) | object list head | observed |
| `Object + 0xE8` | **`ObjectID`** | 1146 distinct values across 1146 objects |
| `Object + 0xF0` | producer/builder id — **not squad** | spans 2 teams on a building |
| `Object + 0x104` | `Drawable*` | 100% of objects; vftable confirmed |
| `Object + 0x324` | **containment parent's `ObjectID`** | present only for infantry; later shown to be a tree |
| `0x00652C42` | **full change-owner** - transfers the object AND its docked/contained units | hooked a live engineer capture, then Ghidra |
| `Object + 0x29c` | **module list** (NULL-terminated array) | the loop at the end of `FUN_00652C42` |
| `Object + 0x300` | `SlavedUpdate` module (when present) | vftable from the RTTI map |
| `SlavedUpdate + 0x04` | **parent object's `ObjectID`** | turret and hub agreed on the root; tank had none |
| `Object + 0x33C` | **`Team*`** | change-and-revert on one object |
| `Object + 0x354` / `+0x358` | owning team's member list next / prev | observed during transfer |
| `Drawable + 0x144` bit 15 | **selected** | calibrated over 7 samples |

---

## 2. The problems, in order

The tool needed three things. Each took a different technique.

1. **Transfer ownership** — solved by static analysis in a few hours.
2. **Resolve "me"** — solved by static analysis plus one live capture.
3. **Read the selection** — static analysis failed entirely; solved empirically.

Then the problems that surfaced in play, each taking longer than the last:

4. **Infantry squads** — eight failed approaches before the answer (section 6).
5. **Sub-objects** — turret hubs and repair drones, linked child-to-parent (section 8).
6. **Docked units** — parked aircraft and harvesters, solved by watching the engine
   capture a building rather than searching memory (section 9).

Section 7 (sound) is a shorter detour; section 13 covers shipping it as an exe and the
bugs found afterwards.

---

## 3. Problem 1 — transferring ownership

### What worked

The binary keeps **full MSVC RTTI**: 3767 type descriptors with real C++ class names, and
EA left the `__FILE__` strings from their asserts in, giving 143 source paths like
`gamelogic\object\Object.cpp`. There is no PDB, but this is nearly as good.

SAGE does not store an owner id on an object. The chain is:

```
Object -> Team -> TeamPrototype -> Player
```

So "change ownership" means retargeting the object's `Team`. Finding the setter:

1. Find the team field: **`Object + 0x33C`** (section 5 explains how).
2. Search the entire 16 MB binary for instructions writing `[reg+0x33c]` — only **8**
   exist, one of which is a `MOVSS` float store from an unrelated class.
3. `FUN_0063C219` sits immediately before `Object::xfer` (so it is in the `Object.cpp`
   region) and has exactly the right shape: early-out if unchanged, compare old and new
   controlling players, deregister, write, re-register, refresh UI.

Narrowing 16 MB to 8 candidates by searching for *writes to one field* is the single most
effective static technique used in this project.

### Why not just write the pointer?

Because `setTeam` does real work either side of the write. During a transfer you can watch
`Object+0x354`/`+0x358` — the owning team's member list — get unlinked from the old team
and spliced into the new one. Poking the pointer directly would corrupt that bookkeeping.

### Thread safety

The transfer is **not** performed on the injector's thread. Writing engine state from an
arbitrary thread races the logic tick. Instead the request is queued and executed from a
hook on the per-frame update, at the top of a tick.

Which function was the per-frame update was **measured, not guessed**: both plausible
`GameLogic` vtable slots were hooked and their calls counted. One took 423 calls, the other
**zero**.

---

## 4. Problem 2 — resolving "me"

Harder than it sounds, because at F11 time the selected object belongs to the *enemy*, so
"me" cannot be derived from the selection.

### The dead end

Player **colour** identified the local player immediately (green `0xff00a744` at
`Player+0x8C`). It was tempting to use it.

The user pointed out that colour is chosen before a match and the enemy can be any of three
factions. **Colour and faction are therefore unusable at runtime.** Had that not been
mentioned, the mod would have worked in testing and broken the first time a different
colour was picked — the worst kind of bug.

Colour was kept as a one-off cross-check during RE, never as a mechanism.

### What worked

The engine registers every subsystem with its name. Finding that code gives a table of
**72 named globals** — `TheGameLogic`, `ThePlayerList`, `TheTeamFactory`, `TheGameClient`,
`TheScriptEngine`, `TheThingFactory`, and more — each with its exact address. That table is
an asset far beyond this mod.

Then: 662 sites load `ThePlayerList`, and of the field reads immediately following,
**`+0x1C` accounts for 96** — an order of magnitude more than anything else. Statistically
decisive, but *not* directly observed, so the tool **validates before acting**: the
resolved player's vftable must be a known `Player` vftable, and its team must resolve back
to that same player. If either check fails it refuses and says why.

---

## 5. Problem 3 — reading the selection

This is where guessing started to cost real time, and where the working method was found.

### The method that worked: calibrated observation

Rather than reverse engineering the UI, ask the game to prove which field means
"selected", by making a **falsifiable prediction** and testing it:

> A real selection flag is set on exactly N objects when N units are selected, and on
> **zero** when nothing is selected.

Select a known number of units, scan every field and every bit, keep only the bits matching
that count, and **intersect across several different counts**.

| samples | surviving candidates |
|---|---|
| 1 own unit selected | 111 |
| 3 own units | 3 |
| 5 own units | **1** |

That single survivor is the answer. A coincidence does not stay correct at 1, then 3,
then 5.

### The mistake this replaced

The first attempt took **one** sample — nothing selected, one unit selected, deselected —
and found `Object+0xB4 bit 0` matching 0/1/0. It looked perfect.

An exhaustive rescan of the same data later showed **two** bits with that identical
signature, out of ~8000 bit positions. It was a 50/50 guess presented as a finding, and it
was wrong: in play it tracked a **harvester**.

> **A signature that only has to match once is not evidence.**

### Two useful discoveries along the way

- **Selection is not on `Object` at all.** The calibration collapsed to zero survivors for
  both own and enemy units. That clean negative killed the whole approach in one run and
  pointed at the client-side `Drawable`.
- **Game constraints shape the experiment.** You can select many of your own units but
  only **one** enemy unit, and selecting a building deselects units. So for the enemy case
  counting proves nothing; the discriminator there is **object identity** — select a
  different enemy each time and the real flag must move to a different object.

Final answer: **`Drawable + 0x144` bit 15**, reached via `Object + 0x104`, with own- and
enemy-selection converging independently on the same bit.

---

## 6. Problem 4 — infantry squads

A C&C3 infantry squad is **one squad object plus N soldier objects**. Transferring only the
selected object left the soldiers with the enemy, producing a distinctive symptom: the
player could command the unit, but it kept the enemy's colour, the enemy ignored it, and
the player's own units attacked it. The user described it as "hijacking", which was exactly
right.

### Eight approaches that failed

| # | Approach | Outcome |
|---|---|---|
| 1 | `Squad*` field on `Object` | no such field |
| 2 | The `Squad` class itself | **zero live instances in 512 MB** — vestigial RTTI from an older SAGE title |
| 3 | `HordeContain` rosters (SAGE calls squads "hordes") | module found on the object, but **empty**; 36–44 live contain modules, all empty |
| 4 | Contiguous allocation (members sit `0x460` apart) | real but an allocator artefact; not reproducible between samples |
| 5 | Any pointer in `Object`'s full extent, both directions | only the two list-link pairs |
| 6 | A linked list (`AIDependencyNode` looked promising) | breadth-first reach-out found only noise |
| 7 | A shared value at `Object+0x100` | **zero on all 1147 objects** |
| 8 | An id at `Object+0xF0` | **shipped, and broke the tool** — see below |

### The regression, and what caused it

Approach 8 looked excellent: six objects stored the selection's id at `+0xF0`, on one team,
sharing one template, identical across two runs.

It shipped. It flipped **neighbouring vehicles and whole groups of buildings**.

Two mistakes, the second worse than the first:

1. **The diagnostic had a bug.** It reported only the *first* matching offset per object —
   `break` after the first hit. `+0xF0` masked everything behind it.
2. **It was validated on one sample.** The theory was tested on infantry and shipped
   without ever checking what it did to a vehicle or a building.

### How it was actually solved

Fix the `break`, then run the same scan on **three** selections and compare:

| selection | `+0x00F0` | `+0x0324` |
|---|---|---|
| infantry | 6 matches, 1 team, 1 template | **6 matches, 1 team, 1 template** |
| vehicle | — | **nothing** |
| building | 4 matches, **2 teams, 2 templates** | **nothing** |

- `+0xF0` is a **producer/builder** reference — on a building it spans two teams. That is
  what swept up the player's base.
- **`+0x324` appears only for infantry.** That is squad membership.

`+0x324` had been invisible the entire time because it sits directly behind `+0xF0` in the
same object, and the scan stopped at the first hit.

**The negative cases produced the answer.** One sample could never have distinguished the
two fields.

### The technique that unlocked it

Memory scanning had failed eight times, so the relationship was observed **being created**
instead: hook the object allocator and watch a squad get built.

```
burst: 10 objects in 597 ms
    0x112be6f0  tpl=0x1086bee8   <- 1 object,  unique template  (the squad)
    0x112beb50  tpl=0x10c0be08   <- 9 objects, shared template  (the members)
```

That confirmed the 1+N structure as observed fact rather than inference, and pointed the
search at the right question.

### Guards in the shipped logic

After the regression, the expansion is deliberately conservative:

- Vehicles and buildings have nothing at `+0x324`, so they resolve to themselves and gain
  no members — **structurally**, not by a special case.
- Candidates must be on the **same team** as the selection, or they are reported skipped.
- **`maxSquadSize = 24`**: if more related objects appear than a squad could hold, nothing
  extra moves and the reason is printed. A wrong assumption should fail loudly and
  harmlessly.

### Postscript: it is a tree, not a flat squad

Transferring a **garrisoned building** moved exactly two objects: the building and one
"squad member". That exposed the real meaning of `+0x324`. It is not squad membership
specifically — it is **"the object I belong to / am inside"**, and it nests:

```
building (id B)
  squad object        +0x324 = B
    soldiers          +0x324 = squad id
```

The expansion descended a single level, which is enough for plain infantry (squad ->
soldiers) but one level short for a garrisoned building.

Fixed by making the descent **transitive**: collect the whole subtree beneath the root,
with a depth limit and the sanity ceiling raised to 48 (a building holding several squads
legitimately reaches the high twenties).

Verified offline before testing in game:

| case | objects transferred |
|---|---|
| vehicle | 1 |
| empty building | 1 |
| infantry squad | 10 |
| garrisoned building | 11 |
| building with two squads | 21 |

The lesson repeats the one from section 6: the field was named after the first case it was
observed in, and the name quietly became an assumption. `+0x324` was "squad membership"
until a garrisoned building proved it was containment in general.

---

## 7. Sound — a failure of curiosity

The tool runs against a fullscreen game, so audio is the main feedback channel. The
request was specific: the engineer-capture sound on success, the GDI "mission failed"
sting on error.

### The wrong conclusion

C&C3 keeps its audio in `EnglishAudio.big`. Unpacking it gives entries like:

    166b084d.46410f77.00002053.4233f9b5.cdata

Hash-named, no readable names anywhere in the archive. The conclusion drawn was that
mapping "engineer captures a building" to one of those hashes meant reverse-engineering
the SAGE manifest format and then a proprietary audio container — a project in itself,
disproportionate to a cosmetic feature. `sound.py` was written to play user-supplied WAVs
instead, and its docstring explained at length why the game's own sounds were out of reach.

That conclusion was wrong, and it was wrong for an avoidable reason: **the `.manifest` was
never opened.** It was assumed to be an opaque proprietary format on the strength of its
name.

### What it actually took

The user supplied `cc3tools` and `pyBIG`. `audiodump.exe`'s string table gave the shape of
the thing immediately:

    Streams (*.manifest)
    %s\cdata\%08x.%08x.%08x.%08x.cdata
    libmpg123

So the manifest maps names to those four-word hashes — exactly the missing piece. Opening
the file showed why it had looked like noise: the first two bytes are `0x10FB`, the
**RefPack** signature. It is compressed, not proprietary. pyBIG ships a RefPack decoder.

    340 KB compressed  ->  1 MB  ->  10,421 asset names

including `AudioEvent:BuildingCaptured` and `AudioFile:Geva_MissionFailed`. Total work
after the realisation: one afternoon, most of it spent unpacking the archive.

The sounds shipped are `Geva_EstablishingB` (attach), `Geva_SelectUnit` (nothing
selected) and `Geva_MissionFailed` (error). A fourth, `Geva_FlagCaptured` for success, was
retired once the tool began using the engine's change-owner routine: the game announces
captures itself. They are
played as plain WAV files — the engine is still never called for something cosmetic, which
was the one part of the original reasoning that held up.

### The lesson

Every other dead end in this project was closed by a measurement: a count that came back
zero, a hook that fired 423 times instead of 0. This one was closed by an assumption, and
it survived long enough to get written into a docstring as an explanation.

The check that would have caught it costs nothing — read the first two bytes and look for
a compression magic before declaring a format opaque. "This looks like a big job" is a
hypothesis like any other, and it deserves the same falsifiable test as the rest.

---

## 8. Problem 5 - sub-objects: turret hubs and repair drones

Two late reports: transferring one turret of a Nod turret hub moved only that
turret, and aircraft resting on a structure stayed with the enemy.

### The link, and which way it points

Both looked like the garrison problem, so the search started from the RTTI map.
`SlavedUpdate`, `SlaveOwnerInterface`, `GatherSlavesUpdate` and
`ParkingPlaceBehavior` are all present in 1.10.

The reason neither worked was one line. The occupant scan is generic - it looks
inside whitelisted modules for ObjectIDs - but the whitelist was
`"Contain" in n or "Horde" in n`, which excludes every one of those classes.
The machinery was already there and pointed at the wrong modules.

Measured on a turret:

    SlavedUpdate at Object +0x300,  module +0x04 = the parent's ObjectID

The direction is the important part. The **slave** carries the module and names
its parent; the parent carries nothing. So unlike containment, the set cannot
be walked outward from the parent - every object has to be asked who its
parent is. A reverse walk.

It was confirmed before any code was written: selecting a turret and selecting
its hub both resolved to the same root and the same three slaves, on a second
cluster too, while a tank produced no module and no slaves.

### Getting the meaning wrong anyway

A building probed as a negative control reported three slaves, and that was
written up as "you actually picked another turret hub". It was not. With
template names fixed (below) it identified as **Factory**, exactly as the user
had said.

`SlavedUpdate` does not mean "turret". It means "sub-object of X". The
Factory's three slaves are its **repair drones** - every vehicle and aircraft
factory in this game fields drones that repair nearby units - which also shows
a slave can be a mobile unit, not a static one.

The correction matters beyond the label: the feature touches far more than Nod
turrets, and transferring a factory without its drones would leave enemy drones
servicing the player's building. The mistake was the project's recurring one,
assuming a link is narrower than it is, and this time it was made while
*writing up* a clean measurement rather than while taking it.

### Template names are UTF-16

Every name in those probes printed `(no name found)`. `readCString` stops at
the first NUL, so a wide string returns one character and reads as a miss.
This was already recorded in the gotchas below and still shipped in two code
paths. Reading UTF-16 first made `Factory` resolve immediately - and that is
what settled the argument above.

---

## 9. Problem 6 - docked units, and the function we should have found first

Two reports: a Nod turret hub transferred only the clicked turret, and aircraft
parked on an air tower stayed with the enemy. The turret half was solved by the
slave link in the previous section. The aircraft half took six failed probes and
was solved by not looking at data at all.

### Six ways of finding nothing

| Attempt | Result |
|---|---|
| `ParkingPlaceBehavior` on the tower | absent |
| `JetAIUpdate` on the aircraft | no reference to the tower |
| Tower's ObjectIDs, parked vs empty | identical but for noise |
| Tower's **pointers** (probe only read ids until then) | only known list links |
| Two-level scan, module -> array | followed the object's own vftable into code and produced 48 arrays of pure noise |
| Aircraft docked vs airborne | one field either way: `+0xF0`, the producer |

Each round reported "absent", and each time the instrument was the limit: a
class-name filter that excluded every `Dock*` class, a resolver that matched
ids but not pointers, a block test that required a vftable. "Not found" was
repeatedly presented as "not there".

### The question that could be answered

The user pointed out that capturing a refinery with an engineer gives you the
harvester inside it, and capturing an air tower gives you the aircraft parked
on it - while units *not* docked stay with the enemy.

That reframes everything. The engine can already distinguish parked from
flying, so the answer exists as **code**, whatever the data looks like. Hooking
`Object::setTeam` during a real engineer capture recorded the building and the
harvester moving together, and following the call chain gave:

    0x652d59 -> FUN_00651c35 -> thunk FUN_00647220 -> setTeam @ 0x63C219

The thunk has exactly three callers in the whole binary, so the search space
went from "somewhere in 16 MB" to three functions. The relevant one is
`FUN_00652c42`, and its tail is the entire answer:

```c
for (m = *(int**)(this + 0x29c); *m != 0; m++) {
    iface = (**(code**)(*(int*)(*m + 0x10) + 0x3c))();
    if (iface) { (**(code**)(*iface + 0x3c))(team, arg3); break; }
}
```

### Object + 0x29c is the module list

This is why six probes found nothing. Modules are **not direct fields** of
`Object`; they live in a NULL-terminated array at `+0x29c`. Every scan of the
object's own fields was looking in the wrong place, and the earlier finding
that a turret's `SlavedUpdate` sits at `+0x300` was a direct-field special
case that made the wrong model look right.

The slot dump had actually printed it - `+0x029c -> 0xa4fd7c8`, labelled
"plain data, not a class" - and it was walked past, because the search was for
something with a vftable.

### Calling it instead of reimplementing it

`FUN_00652c42` takes seven parameters. Rather than guess them, the same hook
trick recorded what the game itself passes during a capture:

    refinery   (team, 1, 0, 0, 1, 1)   from 0x6560e1
    harvester  (team, 1, 0, 0, 1, 1)   from 0x65738a, carrying the refinery's
                                       Object* as a trailing argument

Identical flags, two different callers - the module hand-over path firing
exactly as the decompiled loop predicts. F11 now calls that function, so the
engine hands over contained and docked units using its own code.

Verified in play: infantry, vehicles, buildings, turret hubs, refinery with
harvester, air tower with aircraft. The game even announces "Building
captured" itself, so the tool's success sound was retired.

### The lesson

Six probes asked "where is the parking link stored?" and the honest answer was
that there isn't one - the association is computed, not stored. The question
that could actually be answered was "what does the engine run when it does
this correctly?", and that took two measurements.

When a structural search keeps returning nothing, the next move is not a
better scan. It is to find a situation where the game already does the thing,
and watch it.

---

## 10. What actually worked, as method

**Make falsifiable predictions.** Every real result came from "if this is true, then
exactly N objects will match" — and several predictions failed, which is the point.

**Test the negative cases.** The squad field was found by checking vehicles and buildings.
Skipping that step is what caused the one regression.

**Vary the sample.** Repeating a measurement with a different count, a different unit, a
different faction and colour caught more errors than any amount of re-reading code.

**Let clean negatives kill theories.** "Zero `Squad` instances in 512 MB" and "selection is
not on `Object`" each closed off a whole direction in a single run.

**Observe behaviour when structure fails.** Eight memory-scanning approaches failed; hooking
the allocator answered the question in one run.

**Measure instead of reasoning.** Which vtable slot is the per-frame update: 423 calls vs 0.

---

## 11. Gotchas specific to this game and toolchain

- **Ghidra rejects project paths containing a `.` element**, and a relative `./path`
  counts. It reports success (exit 0) while having done nothing.
- **Ghidra 12 refuses `.py` scripts** unless launched through PyGhidra; the helper scripts
  here are Java.
- **`Thread.backtrace` is unusable in this binary.** No frame pointers, so it returns stale
  stack slots — it reported the constructor as its own allocator's caller, an address
  inside no function, and a destructor. A validated stack scan (keep only values preceded
  by a real `CALL`) works; hooking a known wrapper and reading its return address is better
  still.
- **Frida RPC replies are JSON.** An `ArrayBuffer` inside a returned object is silently
  dropped and arrives as `{}`. Encode binary explicitly.
- **`ThingTemplate` display names are UTF-16.** `readCString` stops at the first NUL byte
  and returns a single character, which defeats naive name readers.
- **`ThingTemplate + 0x04` is a hash, not a pointer.** Dereferencing it yields nonsense.
- **SAGE `.manifest`, `.bin` and `.relo` entries are RefPack compressed** (magic `0x10FB`).
  Read raw they look like binary noise, which is exactly how a solvable problem got
  written off as a hard one. It also breaks extraction silently: a dump that copies
  them as stored looks complete, but `audiodump.exe` then parses a compressed blob and
  crashes. `big_extract.py` decompresses on extraction unless given `--raw`.
- **EVA asset names encode perspective, not just subject.** `Geva_BuildingCaptGen` is
  *you captured a building*; `Geva_BuildingCapt` is *your building was captured*. Picking by
  keyword match gets this backwards, and nothing in the manifest flags the difference.
  Both were wrong here anyway: F11 transfers tanks and infantry too, so the success sound
  has to be subject-neutral (`Geva_FlagCaptured`). Matching a name to the *mechanic* is not
  the same as matching it to what the player will actually be looking at. (The success
  sound was later retired altogether: the game announces captures itself.)
- **Third-party apps squat on Ctrl+Alt combos.** `Ctrl+Alt+G` was silently taken by Google
  Drive (`RegisterHotKey` error 1409).
- **`Object` is `0x460` bytes.** Scanning further reads the *next* object and produces
  convincing-looking phantom fields.
- **AI teams churn.** A brutal AI holds ~10 teams and reassigns units between them
  constantly. Team pointers changing between two captures is normal.

---

## 12. Reproducing the analysis

Everything is scripted and re-runnable.

| Path | Purpose |
|---|---|
| `.tools_and_docs_by_claude/CNC3_ModdingResearch/reverse_engineering/ghidra_scripts/` | Java scripts: xrefs, field writes, vftable map, decompilation |
| `.tools_and_docs_by_claude/CNC3_ModdingResearch/reverse_engineering/notes/01_re_findings.md` | the full session-by-session log, including every wrong turn |
| `Submods/AllianceSwitchTool/src/switch.py` + `switch_agent.js` | the working tool |
| `Submods/AllianceSwitchTool/bin/AllianceSwitchTool.exe` | the same tool packaged with PyInstaller - no Python needed. Built by `src/build.py`; not in git (~150 MB) |
| `Submods/AllianceSwitchTool/src/learn_selection.py` | selection-flag calibration |
| `Submods/AllianceSwitchTool/src/selftest.py` | checks the agent, Frida, hotkeys and audio without the game |
| `Submods/GameEngineResearch/` | the read-only probes (`watch.py`, `probe.py`, `probe_agent.js`) used to find everything above, with their own selftest |
| `Submods/AllianceSwitchTool/src/vftables_1.10.json`, `vftables_1.9.json` | vftable → class-name mappings per build (5133 for 1.10) |
| `.tools_and_docs_by_claude/CNC3_ModdingResearch/reverse_engineering/rtti_vftables.py`, `port_addresses.py` | regenerate the class map and port addresses for another build |

The class map (originally one `vftables.json`, now one file per build) deserves a mention: it turns any runtime pointer into a named class by
reading its first dword. It is what identified `HordeContain`, `Drawable`, `ThingTemplate`
and `ObjectDefectionHelper` in live memory, and it is reusable for any future work on this
engine.

---

## 13. Status

Working and verified in play:

- **Vehicles** — transfer exactly one object.
- **Structures** — transfer exactly one object.
- **Infantry squads** — transfer the squad and all its members; colour changes, the enemy
  turns hostile, and the player's own units stop attacking it.
- **Sub-objects** — a parent and everything slaved to it move together: a Nod turret
  hub with its 3 turrets (from either end), and a factory with its repair drones. Found
  by a reverse scan, since only the slave stores the link.
- **Campaign** — verified on the engine path.
- **Docked units** — a refinery brings its harvester, an air tower brings the aircraft
  parked on it. Units that are *not* docked correctly stay with the enemy, because the
  engine's own routine makes that distinction rather than the tool guessing at it.
- **Garrisoned buildings** — transfer the building, the squads inside and their soldiers
  (22 objects in one test). Occupants reachable only through `HordeGarrisonContain` are
  found by searching that module **by ObjectID**, since a pointer-only scan finds nothing.
  Evacuated infantry belong to the player.
- **Transports** — Ox, Carryall and the MRT (APC) keep their passengers on board, and the
  passengers are the player's when unloaded.
- **Game versions** — 1.9 (tested with the Firestorm mod) and 1.10, both verified in play
  on 2026-09-19.

Known campaign quirk: in a mission whose garrisons share one scripted team, taking one
building can make the mission empty the others. The log proves the tool touched only the
selected building and its occupants; see `cnc3_game_mechanics.md`.

### Running it

Double-click `Submods/AllianceSwitchTool/bin/AllianceSwitchTool.exe`, before or after
starting the game. The tool's README documents every mode and option; the short
version: `logging`, `nosound`, `cmdsound`, `setteam`, and the modes `calibrate`,
`selftest` and `help`.

Logging is **off by default** — a play session should not quietly fill the disk.

### How it ships

The DLL port once planned here was replaced by packaging the working Python tool
with PyInstaller: the same code, no Python needed, ~150 MB (127 MB of it Frida's
engine). Double-clicking gives it its own console, which waits for Enter on exit so
errors stay readable; from a shell it does not wait.

It also survives the game coming and going. Hotkeys are registered before it
looks for the game, a 1-second timer lets the message loop notice changes, and
Frida's `detached` signal reports the game closing. The signal's `reason` matters:
`application-requested` is the tool detaching itself (after a failed attach), and
treating that as "game closed" produced a false alarm during testing.

### Ownership is per PLAYER, not per Team

The garrison bug came back after the engine path landed, and the cause was not
the engine path at all. The log said it plainly once it printed enough:

    (2 objects transferred in total)
    garrison occupants found: 1
    9 x skipped (squad id matched but on a different team - not moved)

Detection was right, the path was right, all nine squad members were found -
and then thrown away by a sanity check that required the occupants to be on
the same `Team*` as the building.

A player owns **many** teams, and the AI reassigns units between them
constantly (already recorded in the gotchas below). Garrisoned infantry
routinely sit on a different Team from the building they are inside while
belonging to the same enemy. The check now resolves
`Team + 0x04 -> TeamPrototype + 0x08` on both sides and compares **players**.

This is why the bug looked intermittent: it depended on whether the AI happened
to have the infantry on the building's team at that moment. The same guard
protects the squad and sub-object paths, so it was quietly limiting those too.

### The price of that fix: a second factory

Loosening the guard to "same Player" fixed garrisons and opened a new bug: F11 on a
factory would sometimes also take a nearby factory. The guard sits behind an ObjectID
scan of the building's contain modules, and ObjectIDs are small integers — counters and
indexes inside a module match real objects by chance. The Team check had been quietly
filtering most of those chance matches; the Player check lets any same-enemy building
through.

The logs proved it without the game: one building that was *not* garrisoned claimed as
occupants the squads of three other buildings. The scan now runs only where nothing else
can find occupants — skipped on the engine path, whose change-owner hands over a
building's contents itself, and limited to the garrison roster modules on a garrisoned
building.

Verified 2026-09-19 over 122 presses: the scan skipped on all 116 engine-path presses,
and no press moved anything beyond the selection and its own sub-objects or squad members.

The same log settled whether the tool's own squad expansion is redundant on the engine
path. Mostly: in 15 infantry presses the members were already the player's by the time
the tool's tree walk reached them — the engine had moved them. But in one press the
engine missed a member and the tree walk transferred it. **Keep the expansion**; it is a
safety net the engine occasionally needs, not dead code.

The general rule: an ID-matching scan is a hypothesis generator, not evidence. Every
filter placed after it is load-bearing, and loosening one widens the scan's reach.

### Garrison is the one exception

Garrisoned buildings still go through `Object::setTeam` plus this tool's own
occupant expansion. On the engine path a garrisoned building forces its
occupants out but leaves them **parented to it**, so they cannot move away -
the engine is running eviction logic that assumes a capture in progress rather
than an instant ownership swap.

Detection is deliberately narrow: only `GarrisonContain`,
`GarrisonStealthContain` and `HordeGarrisonContain`, the modules that actually
hold occupants. Matching "Garrison" anywhere in the class name would sweep in
AI states, heuristics and upgrades and send ordinary buildings down the
fallback for no reason. Infantry squads also use contain modules and work
correctly on the engine path, so keying off "has a contain module" would
regress them too.

The choice is made once per keypress, not per object: occupants must move by
the same mechanism as their building, or the two end up inconsistent - the
same failure that reordering caused during the squad work.

### How a transfer is performed

F11 calls `FUN_00652C42`, the engine's own change-owner routine, with the arguments
recorded from a real engineer capture: `(newTeam, 1, 0, 0, 1, 1)`. The engine then walks
the module list at `Object + 0x29c` and hands over whatever each module holds. Passing
`setteam` on the command line restores the older path — `Object::setTeam` plus this
tool's hand-written squad/slave/garrison expansion — which is still present and still
works.

### Transports: the garrison bug, again

F11 on an Ox or Carryall carrying infantry evicted the passengers on the spot, and once
unloaded the soldiers were the player's but stuck, with their squad's outline adrift. The
log showed why: the engine's change-owner handed over the transport's *soldiers* but left
the *squad objects* on the enemy side, which the tool then moved separately - the same
container mismatch as garrisons. Transport containers (`TransportContain`,
`HordeTransportContain`) now take the setTeam path too. They need no ID scan: the
containment tree already reaches every passenger. Ground APCs (the MRT) use the same
`HordeTransportContain`, so the fix covered them without a change - confirmed in play.

Worth knowing for next time: the engine path is right for things that merely *hold* units
(refinery, air tower, factory) and wrong for things that *carry* them (garrisons,
transports). Those units are squads of soldiers inside a container, and the engine's
capture logic assumes a capture in progress rather than an instant ownership swap.

### Supporting 1.9

Most mods run on 1.9, and the first attempt at it showed the danger of assuming a build:
1.9 runs under the same process name, so the tool attached and hooked 1.10's per-frame
address - unrelated code in 1.9. F11 queued and never ran, and a hook landing
mid-instruction could have crashed the game.

The two builds turned out to be nearly the same program. 1.10 (linked 2025) is 1.9
(linked 2012) rebuilt: its code is only 0.3 % larger, and its extra 8 MB is resources. The
rebuild moved every function, but their bodies are byte-identical apart from relocated
addresses - including every struct offset, which settled that the object layout (and the
calibrated selection bit) carries over unchanged.

The addresses were ported, not guessed. Class vftables come straight from MSVC RTTI with a
small Python reader, validated by reproducing Ghidra's 1.10 map. Functions and globals come
from `port_addresses.py`, which needs two independent methods to agree for each one - a
masked byte signature, plus a vote over every place the other build uses the address (or,
for a virtual function with no call sites, its vftable slot). The control, porting 1.10
onto itself, returns every address unchanged.

The tool now reads the running exe's header, matches it against `versions.json`, and
refuses any build it does not know - before anything is hooked.

Three lessons, all from bugs caught during the work:

- **Cap every search.** A tiny, heavily-masked pattern matches almost everywhere; an
  uncapped match list grew one check to 8.7 GB of RAM.
- **Mask with context.** An address straddling the edge of a byte window is only half
  inside it, goes unrecognised, and breaks the match - it made the per-frame function look
  changed between builds when it was not.
- **Export names are a flat namespace.** A new `identify()` silently replaced, or was
  replaced by, the Ctrl+Alt+N diagnostic of the same name. The selftest now rejects
  duplicate export names.

### Known cosmetic limitation

Transferred garrisoned infantry sometimes appear at the building's windows. Measured
cause: `setTeam` changes only the team colour (`Drawable + 0x488`) synchronously, but on
the following client tick the animation timestamps at `Drawable + 0x130` and `+0x5F0` are
reset from a frame number to `0xFFFFFFFF`, so the model-condition state is recomputed and
the firing/at-window pose shows.

Related symptoms with the same root cause: the building keeps the **previous owner's
faction-specific window slits** until the squad re-enters, and very occasionally a slit
model appears flipped on one side.

**The state is recoverable** — evacuating and re-garrisoning restores everything — because
the engine rebuilds garrison visuals on the *entry* path, and an ownership change never
triggers that rebuild. The handlers were then identified by hooking every
`HordeGarrisonContain` vtable slot and splitting the measurement into exit-only and
entry-only runs:

| Slot | RVA | Meaning |
|---|---|---|
| 28 | `0x00619718` | object **entered** (10 calls = 9 soldiers + 1 squad object) |
| 27 | `0x005C4440` | also fires on entry |
| 29 | `0x006610E2` | object **left** |
| 34, 17 | `0x005C454D`, `0x005D520C` | per-frame updates (proven noise by a control run) |

**Deliberately not fixed.** Entry is a *sequence* of four handlers with different call
counts, not a single refresh, so replaying it out of context means guessing ordering and
arguments for several functions. They are event handlers announcing a transition that has
already happened, so re-invoking them asserts something false — likely re-assigning window
positions or double-counting occupants. The window-slot state is already visibly fragile
(hence the flipped slit). And the payoff is cosmetic, with a working user-side workaround.

At the time, everything else in the tool was read-only or went through `Object::setTeam`,
which is built to be called, and this would have been the first call to a function never
meant to be invoked externally. The tool has since come to call the engine's change-owner
routine, which is also internal — but with arguments recorded from the engine's own calls
during an engineer capture, which is a different proposition from replaying a sequence of
event handlers out of context. The addresses are recorded so a future attempt can start
from measured facts.

Scope is single-player and skirmish. Multiplayer is explicitly out of scope: only the local
client would apply the change, and the game would desync.

The tool ships as an exe (see "How it ships"). A C++ DLL remains possible — smaller, and
loaded with the game — and every address it would need is established. If it is ever
built, the proxy target should be `d3dx9_29.dll`, `dsound.dll` or `winmm.dll` — **not**
`dinput8.dll`, which the community FPS patch already claims.
