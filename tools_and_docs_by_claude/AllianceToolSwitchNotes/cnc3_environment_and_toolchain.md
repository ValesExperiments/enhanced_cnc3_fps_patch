# C&C3 Tiberium Wars — environment, toolchain and RE starting points
(Branch: CNC3. Survey done 2026-09-17. Verified by direct inspection, not assumed.)

## Project layout (reorganised 2026-09-19)

The repo is being turned into a toolset for C&C3, with individual tools as submods.

| Path | What lives there |
|---|---|
| `Submods/<Tool>/` | one folder per tool. `Submods/AllianceSwitchTool/` (F11 transfers the selected enemy object to the player) is split into `src/` (the Python source) and `bin/` (the packaged exe). Launchers and scripts find the venv and pyBIG by searching UPWARD, so tools no longer depend on sitting at a fixed depth. |
| `Submods/<Tool>/bin/` | a PyInstaller **onedir** build (exe + `_internal/`), ~150 MB, 127 MB of it Frida's `_frida.pyd`. Git-ignored: over GitHub's 100 MB per-file limit. Rebuild with `src/build.py` (seconds). Read-only resources are packed into `_internal/`; user files (logs, sounds, a recalibrated `selection_flag.json`) live next to the exe - `src/paths.py` owns that split. |
| `Submods/GameEngineResearch/` | the read-only runtime probe (`watch.py` + `probe_agent.js`, hotkey-driven snapshots and diffs). Split out of AllianceSwitchTool; start here to investigate any new engine behaviour. |
| `.tools_and_docs_by_claude/CNC3_ModdingResearch/` | general C&C3 reverse-engineering, not tied to one tool: the Ghidra project of the 1.10 exe, the Java Ghidra scripts, BIG/RefPack extraction, audio-name extraction, and the session-by-session RE notes in `reverse_engineering/notes/01_re_findings.md` |
| `.tools_and_docs_by_claude/.venv` | the one shared Python venv. Both the tool and the research folder carry a `requirements.txt` for it. |
| `.claude_memory/AllianceSwitchTool_Journey.md` | how AllianceSwitchTool was built, every wrong turn included - read before extending it |

`.gitignore` rules for submods are wildcards (`Submods/**/logs`, `Submods/*/bin/`,
`Submods/**/sounds/*.wav`), so new submods need no gitignore edits.

**Packaging a tool as an exe** (proven on AllianceSwitchTool): PyInstaller 6.22 in the venv works with
Python 3.14 and Frida 17. Gotchas: a frozen exe's `sys.executable` is the tool itself (so never spawn
"python" as a helper process); Frida unpacks its injector into `%TEMP%` so TEMP must be writable;
double-click vs shell launch is detected with `GetConsoleProcessList` (count 1 = own console).
Staying alive across game restarts: register hotkeys before attaching, drive a 1 s `SetTimer` through the
`GetMessageW` loop, and use Frida's session `detached` signal - but ignore reason
`application-requested`, which is the tool detaching itself.

## Testing the tool safely (lessons from 2026-09-19)

- **Check whether the game is running before launching the tool or exe for a test.**
  It attaches to a running `cnc3game.dat` immediately (hooks a frame function,
  holds F11), so a "harmless" banner check once connected to the user's live game
  four times. `tasklist | grep -i cnc3game` first.
- **Never delete files by position** (`ls | tail -2 | xargs rm`) or by broad glob.
  Record the exact files a test creates (diff the folder before/after) and delete
  only those. A positional delete destroyed the user's own test log, which was not
  in git (`bin/` is ignored) and not recoverable from the Recycle Bin (`rm` skips it).
- Run cleanup only if the step before it succeeded (`&&`, not `;` or a new line).
- **rpc.exports is an object literal: a duplicate export name silently replaces the earlier one.**
  A new `identify()` collided with the Ctrl+Alt+N diagnostic and was unreachable. The tool's
  selftest now fails on duplicate export names.
- Synthesized keys (keybd_event) are not delivered while the screen is locked or an elevated
  window has focus - a key-driven test can then fail without the tool being at fault.
- A frozen exe block-buffers stdout when redirected; read its output from a
  `logging` log file (flushed per line), or the capture may be empty.

## Game install
- Root: `D:\SteamLibrary\steamapps\common\Command and Conquer 3 Tiberium Wars`
  (granted via `additionalDirectories` in `.claude/settings.json`; Edit/Write there are DENIED — copy files into the project instead.)
- Real executables are `.dat` files under `RetailExe\<version>\`. Versions 1.0-1.10 all present.
- `RetailExe\<version>\` also holds `config.txt`, `steam_api.dll`, `data\`. No `dinput8.dll` present yet.
- Build targets per CLAUDE.md: **1.10 and 1.9**. 1.10 = `RetailExe\1.10\cnc3game.dat`, 16,314,880 bytes,
  32-bit x86 PE. 1.9 is the version most mods use (e.g. Firestorm, a data-only mod on the stock 1.9 exe).
  AllianceSwitchTool supports both (1.9 added 2026-09-19; both verified in play the same day).
- 1.9 (linked 2012-07-31) and 1.10 (linked 2025-02-19) share ~all code: 1.10's .text is only 0.3 %
  bigger; its extra 8 MB is resources. Functions sit at very different addresses (the 2025 rebuild
  reordered the binary) but bodies are byte-identical apart from relocations, and struct offsets
  match (change-owner/setTeam compared whole; the selection bit tested in 34 identical places).
- Recognise a build by PE TimeDateStamp + SizeOfImage: 1.7 and 1.8 share a timestamp. The 4 GB
  patch flips a header flag only. Per-build addresses live in `Submods/AllianceSwitchTool/src/versions.json`,
  per-build class maps in `vftables_<version>.json` beside it.
- Research tooling still 1.10-only: the Ghidra project and `Submods/GameEngineResearch` (no version check).
- `Core\` has per-version data; `Support\`, `Movies\`, `WorldBuilder\` alongside.

## Toolchain actually installed (checked)
| Tool | Status |
|---|---|
| Python | 3.14.6 on PATH |
| Ghidra | 12.1.3 at `C:\Users\<user>\UsefulSSD\ToolsSSD\ghidra_12.1.3_PUBLIC` |
| Java (Ghidra dep) | 21.0.12.1 LTS — satisfies Ghidra 12 |
| Visual Studio / MSVC | 18.10.1 Community at `C:\Program Files\Microsoft Visual Studio\18\Community`, x86+x64 VC tools. Locate via `vswhere.exe` in `C:\Program Files (x86)\Microsoft Visual Studio\Installer\` |
| zig / make | **NOT installed** (the cnc3_fps_patch Makefile wants `zig cc` + `make`) |
| Frida | 17.18.0 in the project venv at `.tools_and_docs_by_claude/.venv` (with frida-tools, unicorn - installed but not imported by any code). Frida 17 removed `Module.findBaseAddress` and `frida.enumerate_processes` - see selftest.py's removed-API check |
| PyInstaller | 6.22.3 in the project venv (installed 2026-09-19 with the user's permission) - builds `Submods/AllianceSwitchTool/bin/` |

## Workflow
The previous branch (FinalFantasyVII, `FMV_fix_mod`) went Ghidra -> Frida prototype -> C++ proxy DLL.
This branch kept the first two steps but **shipped the Frida prototype as a PyInstaller exe instead of
a DLL** (the user's choice: "exactly like it works now, without Python"). A DLL is still an option;
see the Journey's Status section.

## cnc3_fps_patch-main — the single most valuable reference in the repo
A 32-bit `dinput8.dll` proxy (game imports only `DirectInput8Create`, so it is a cheap loader).
It does **masked signature scanning**, not hardcoded addresses, and explicitly supports TW 1.09/1.10 + KW.
`src/game_layout.h` already contains recovered struct offsets that are reusable:
- `GameEngine` fields (0x18 max update fps, 0x40 logic phase, 0x5C frame history, ...)
- `g_theWriteableGlobalData` singleton pointer + GlobalData offsets
- **Keyboard**: event queue begin/end 0x1C/0x20, key-state base 0x2C, stride 8, inputFrame 0x0E28,
  plus hooks on the hardware-poll and check-repeat `__thiscall`s. Directly reusable for hotkey detection.
- **W3DView / CNC3_View** singleton pointer and vtable slots (0x14 scrollBy, 0x43/0x44 get/set zoom).
It does NOT cover the game-logic object model (Object/Player/Team) — that still needs fresh RE.
Important caveat it documents: do CRT-safe work only in DllMain; defer real patching to a later hook.

## Ownership in SAGE (from EA's CnC_Modding_Support, `Tiberium Wars/`)
- 4107 files: `Xml/` (source XML), `Schemas/` (XSD), `Libraries/`, `Shaders/`.
- The engine has first-class ownership-transfer **script actions**, found in shipped campaign map.xml:
  - `NAMED_TRANSFER_OWNERSHIP_PLAYER` (710 uses) — one named object -> a player
  - `PLAYER_TRANSFER_OWNERSHIP_PLAYER` (22 uses) — all of one player's objects -> another
  - `SET_PLAYER_OWNERSHIP_OF_TYPE_COUNTER` (12 uses)
- Catch: these operate on **map-editor-named** objects. Units selected in skirmish have no script name,
  so a selection-driven tool cannot use the script path; it must call the underlying engine routine
  on the selected object directly. (Resolved: that routine is `0x00652C42` - see Key addresses.)
- Game already ships capture/commandeer mechanics (`SCCCommandeer.ani`, `SCCCapture.ani` cursors;
  Commandeer refs in `LogicCommand.xml`, `SpecialPowerTemplates.xml`, `NODAvatar.xml`) — good RE anchors
  for finding the ownership-transfer function, since those code paths must call it.

## Running Ghidra headless on the saved project

The workspace now lives under `.tools_and_docs_by_claude`, and Ghidra 12 refuses any project
path element starting with `.`. Workaround (2026-09-19): create a **junction** to
`ghidra_workspace` in the session scratchpad, run `analyzeHeadless <junction> CNC3TW -process
cnc3game_1.10.dat -noanalysis -readOnly -scriptPath <ghidra_scripts> -postScript X.java`, then
remove the link with `cmd /c rmdir <junction>` (removes only the link; never `Remove-Item -Recurse`
on a junction). `-scriptPath` may contain dots. `DecompileThis.java` takes addresses in env `ASTA_FUNCS`.

## Game modes (Capture the Flag etc.)

`TheGameLogic + 0x120` holds the game type: VERSUS 0, KOTH 1, CAH 2, CTF 3, SIEGE 4 (values from
EA's `MultiplayerCommonUIData.xsd`; the CTF check at 1.10 `0x4C7A5C` compares it with 3). Only
Versus is `AvailableWithAI`, so Skirmish never offers the other four. CTF spawns a `BASE_FLAG_<n>`
object at each player's start; every official MP map places one neutral `CaptureFlag`.

## Searching the game's data (lessons from the content hunts, 2026-09-19)

- **RefPack has four header forms**: flags byte `0x10`/`0x11`/`0x90`/`0x91` + `0xFB` (`0x80` = 4-byte
  sizes, `0x01` = extra compressed-size field). `big_extract.py` handles all four since 2026-09-19
  (`refpack_header()`; before that `static_common.bin`, `0x90FB`, came out still compressed) and
  rejects a "decompression" whose size differs from the header's. pyBIG only strips the `0x10FB` header.
- **`.csf` string tables are bit-inverted UTF-16** - raw text search never finds them.
  `.tmp/csf_dump.py` decodes `cnc3.csf` (English.big / patchN.big) to `LABEL<TAB>text`.
- **Already searched, nothing found:** "Incursion" and "CABAL" (ASCII + UTF-16, every archive fully
  decompressed, all exes, WorldBuilder, EA modding data). The only CABAL mention is the shipped Intel
  DB article `001_GDI_News_Alert_132_Ghost_in_the_machine`; Kane's Wrath data mentions CABAL (canon).
- Real leftovers found: dummy Intel DB entries (`IDB:00x_*_Entry_*`), `SCRIPT:TestString`,
  `SPECIALPOWER:Satellite` "[placeholder] Satellite Launch", and Generals' factions `GLA`/`China`/`America`
  beside the online ladder code. Internal exe name: `RTSF.exe` (1.9), `RTS-final.pdb` (1.10).

## Porting to another build

In `.tools_and_docs_by_claude/CNC3_ModdingResearch/reverse_engineering/`:
- `rtti_vftables.py` - every class vftable from MSVC RTTI in ~1 s, no Ghidra. Validated: reproduces
  5119 of the 5133 Ghidra-built 1.10 entries (the rest are UI-local classes).
- `port_addresses.py` - ports addresses between builds; each needs TWO agreeing methods (masked
  signature + cross-reference vote, or vftable slot for virtuals). Control: 1.10 -> 1.10 must be
  identity. Lessons baked in: cap match counts (an uncapped generic pattern hit 8.7 GB RAM), and
  mask with context on both sides (an address straddling the window edge broke a signature).

## Key C&C3 1.10 addresses found by this project

(1.9 equivalents are in versions.json.)

| Address / offset | Meaning |
|---|---|
| `0x00652C42` | **full change-owner**: transfers an object AND its docked/contained units. Call as `(this, Team*, 1, 0, 0, 1, 1)` — args recorded from a live engineer capture. This is what an engineer capture runs. |
| `0x0063C219` | `Object::setTeam(Team*, char)` — the low-level team write, reached via a 14-byte thunk at `0x00647220` that has exactly 3 callers |
| `Object + 0x29c` | **module list**, a NULL-terminated array. Modules are NOT direct fields of Object — this is the single most useful structural fact about this engine, and six probes failed for want of it. |
| `Object + 0x300` | `SlavedUpdate` when present (a direct-field special case); its `+0x04` is the parent's ObjectID |
| `Object + 0x29c` loop | `FUN_00652C42` ends by asking each module, via interface vtable slot `0x3c`, to hand over what it holds |
| `Team + 0x04 -> + 0x08` | Team -> TeamPrototype -> **Player**. Compare ownership by Player, never by Team pointer: one player owns many teams, the AI reshuffles units between them, and garrisoned infantry sit on a different Team than their building. **Caveat:** comparing by Player is looser, so any scan that matches ObjectIDs by value (small integers - counters match them by chance) must be tightly scoped, or it drags in same-player buildings. The occupant ID-scan runs only on garrisoned buildings for this reason. |

The full address table (player list, selection flag, ObjectID, containment, ...) is in
`AllianceSwitchTool_Journey.md`, section 1.

**Method note that kept paying off:** when a structural search keeps returning
"not found", stop improving the scan. Find a situation where the game already
performs the operation correctly, hook it, and follow the call chain. Two
measurements beat six probes here.
