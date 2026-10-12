"""Decompile functions from the second Ghidra project (CnC3_extra: TW 1.9, KW 1.02).

The MCP server holds CnC3.gpr, so builds imported by import_extra.py are read
here instead. Prints, per address: function name, size, callers, strings it
references, RTTI vtables that point at it, and the decompiled C (truncated).

Usage (from the repo root):
    tools_and_docs_by_claude/venv/Scripts/python.exe \
        tools_and_docs_by_claude/ghidra_tools/decompile_extra.py TW_1.9_cnc3game.dat 0x7573fc 0x7592cb ...
Options: --chars N (decompiled text per function, default 2500)
"""

from __future__ import annotations

import sys

from ghidra_env import PROJECT_DIR, start_ghidra

PROJECT = "CnC3_extra"


def main(argv: list[str]) -> int:
    chars = 2500
    if "--chars" in argv:
        i = argv.index("--chars")
        chars = int(argv[i + 1])
        del argv[i:i + 2]
    program_name, addresses = argv[0], [int(a, 16) for a in argv[1:]]
    start_ghidra()

    from ghidra.app.decompiler import DecompInterface
    from ghidra.base.project import GhidraProject
    from ghidra.util.task import ConsoleTaskMonitor

    project = GhidraProject.openProject(str(PROJECT_DIR), PROJECT, True)
    try:
        program = project.openProgram("/", program_name, True)
        space = program.getAddressFactory().getDefaultAddressSpace()
        functions = program.getFunctionManager()
        references = program.getReferenceManager()
        symbols = program.getSymbolTable()
        listing = program.getListing()
        decompiler = DecompInterface()
        decompiler.openProgram(program)
        monitor = ConsoleTaskMonitor()
        for value in addresses:
            address = space.getAddress(value)
            function = functions.getFunctionContaining(address)
            if function is None:
                print(f"=== 0x{value:X}: no function")
                continue
            entry = function.getEntryPoint()
            callers = sorted({
                functions.getFunctionContaining(r.getFromAddress()).getName()
                for r in references.getReferencesTo(entry)
                if r.getReferenceType().isCall() and
                functions.getFunctionContaining(r.getFromAddress()) is not None})
            vtables = []
            for r in references.getReferencesTo(entry):
                if not r.getReferenceType().isData():
                    continue
                for back in range(0, 0x400, 4):
                    symbol = symbols.getPrimarySymbol(r.getFromAddress().subtract(back))
                    if symbol is not None and "vftable" in symbol.getName():
                        vtables.append(f"{symbol.getParentNamespace().getName(True)}[{back // 4}]")
                        break
            strings = []
            for instruction in listing.getInstructions(function.getBody(), True):
                for ref in instruction.getReferencesFrom():
                    data = listing.getDataAt(ref.getToAddress())
                    if data is not None and data.hasStringValue():
                        strings.append(str(data.getValue())[:50])
                if len(strings) >= 6:
                    break
            print(f"=== 0x{value:X} in {function.getName()} @ {entry} "
                  f"size={function.getBody().getNumAddresses()}")
            print(f"callers: {callers[:8]}")
            print(f"vtables: {vtables[:4]}  strings: {strings}")
            result = decompiler.decompileFunction(function, 60, monitor)
            if result.decompileCompleted():
                print(result.getDecompiledFunction().getC()[:chars])
        project.close(program)
    finally:
        project.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
