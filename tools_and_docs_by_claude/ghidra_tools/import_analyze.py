"""Import the game executables into the project-local Ghidra project and run
auto-analysis. Safe to rerun: already imported and analyzed programs are skipped.

Usage (from the repo root):
    tools_and_docs_by_claude/venv/Scripts/python.exe \
        tools_and_docs_by_claude/ghidra_tools/import_analyze.py [program names...]

With no arguments, every program in ghidra_env.PROGRAMS is processed. The
project is created in the standard (non-nested) layout, so it can also be
opened in the Ghidra GUI: tools_and_docs_by_claude/ghidra_data/projects/CnC3.gpr.
"""

from __future__ import annotations

import sys
import time

from ghidra_env import PROGRAMS, PROJECT_DIR, PROJECT_NAME, start_ghidra


def main(names: list[str]) -> int:
    start_ghidra()

    import pyghidra
    from ghidra.base.project import GhidraProject
    from ghidra.framework.model import ProjectLocator
    from ghidra.program.util import GhidraProgramUtilities
    from java.io import File  # type: ignore

    if ProjectLocator(str(PROJECT_DIR), PROJECT_NAME).exists():
        project = GhidraProject.openProject(str(PROJECT_DIR), PROJECT_NAME, True)
    else:
        project = GhidraProject.createProject(str(PROJECT_DIR), PROJECT_NAME, False)

    try:
        for name in names or list(PROGRAMS):
            source = PROGRAMS[name]
            if project.getRootFolder().getFile(name) is not None:
                program = project.openProgram("/", name, False)
                print(f"[{name}] already imported", flush=True)
            else:
                print(f"[{name}] importing {source}", flush=True)
                program = project.importProgram(File(str(source)))
                if program is None:
                    print(f"[{name}] import FAILED", flush=True)
                    return 1
                project.saveAs(program, "/", name, True)

            if GhidraProgramUtilities.shouldAskToAnalyze(program):
                print(f"[{name}] auto-analysis started", flush=True)
                started = time.time()
                log = pyghidra.analyze(program)
                project.save(program)
                print(f"[{name}] auto-analysis finished in "
                      f"{(time.time() - started) / 60:.1f} min", flush=True)
                if log.strip():
                    print(log[-4000:], flush=True)
            else:
                print(f"[{name}] already analyzed", flush=True)

            functions = program.getFunctionManager().getFunctionCount()
            print(f"[{name}] functions: {functions}", flush=True)
            project.close(program)
    finally:
        project.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
