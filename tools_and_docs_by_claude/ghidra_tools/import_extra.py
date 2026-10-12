"""Import extra game builds into a second Ghidra project and export function maps.

The MCP server keeps the main project (CnC3.gpr) open, so these builds go into
ghidra_data/projects/CnC3_extra.gpr. After auto-analysis each program's function
list is written to profiler_report/maps/functions_<program>.tsv (entry RVA, end
RVA, name) for tools_and_docs_by_claude/profiler_report/analyze.py.

Usage (from the repo root):
    tools_and_docs_by_claude/venv/Scripts/python.exe \
        tools_and_docs_by_claude/ghidra_tools/import_extra.py
"""

from __future__ import annotations

import shutil
import sys
import time
from pathlib import Path

from ghidra_env import BINARIES_DIR, PROJECT_DIR, REPO_ROOT, start_ghidra

PROJECT = "CnC3_extra"
GAME_TW = Path(r"D:\SteamLibrary\steamapps\common\Command and Conquer 3 Tiberium Wars\RetailExe")
GAME_KW = Path(r"D:\SteamLibrary\steamapps\common\Command and Conquer 3 - Kane's Wrath\RetailExe")
EXTRA = {
    "TW_1.9_cnc3game.dat": GAME_TW / "1.9" / "cnc3game.dat",
    "KW_1.02_cnc3ep1.dat": GAME_KW / "1.2" / "cnc3ep1.dat",
}
MAPS = REPO_ROOT / "tools_and_docs_by_claude" / "profiler_report" / "maps"


def export_map(program, name: str) -> None:
    base = program.getImageBase().getOffset()
    MAPS.mkdir(parents=True, exist_ok=True)
    short = name.replace("_cnc3game.dat", "").replace("_cnc3ep1.dat", "")
    path = MAPS / f"functions_{short}.tsv"
    with open(path, "w", encoding="utf-8") as out:
        for function in program.getFunctionManager().getFunctions(True):
            body = function.getBody()
            out.write("%X\t%X\t%s\n" % (function.getEntryPoint().getOffset() - base,
                                        body.getMaxAddress().getOffset() - base,
                                        function.getName()))
    print(f"[{name}] map written: {path}", flush=True)


def main() -> int:
    for name, source in EXTRA.items():
        target = BINARIES_DIR / name
        if not target.exists():
            shutil.copyfile(source, target)  # read-only copy of the game file
    start_ghidra()

    import pyghidra
    from ghidra.base.project import GhidraProject
    from ghidra.framework.model import ProjectLocator
    from ghidra.program.util import GhidraProgramUtilities
    from java.io import File  # type: ignore

    if ProjectLocator(str(PROJECT_DIR), PROJECT).exists():
        project = GhidraProject.openProject(str(PROJECT_DIR), PROJECT, True)
    else:
        project = GhidraProject.createProject(str(PROJECT_DIR), PROJECT, False)
    try:
        for name in EXTRA:
            if project.getRootFolder().getFile(name) is not None:
                program = project.openProgram("/", name, False)
            else:
                print(f"[{name}] importing", flush=True)
                program = project.importProgram(File(str(BINARIES_DIR / name)))
                project.saveAs(program, "/", name, True)
            if GhidraProgramUtilities.shouldAskToAnalyze(program):
                started = time.time()
                print(f"[{name}] auto-analysis started", flush=True)
                pyghidra.analyze(program)
                project.save(program)
                print(f"[{name}] analysed in {(time.time() - started) / 60:.1f} min", flush=True)
            export_map(program, name)
            project.close(program)
    finally:
        project.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
