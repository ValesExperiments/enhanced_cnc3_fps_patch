"""Minimal Ghidra MCP server (stdio) for the CnC3 FPS-patch project.

Runs headless Ghidra through PyGhidra, holding the project-local Ghidra
project (tools_and_docs_by_claude/ghidra_data/projects/CnC3.gpr) open, and
exposes reverse-engineering tools over MCP. Registered in the repo's
.mcp.json as "ghidra".

Notes
- Only one process can hold a Ghidra project. Close the Ghidra GUI (or the
  import script) on this project before the server starts using it.
- Addresses are accepted as hex VAs ("0x5EF10F", "5ef10f") or as symbol
  names. All programs use image base 0x00400000.
- Write tools (rename/comment/label) change the in-memory program. Call
  save_program to persist them into the project.
"""

from __future__ import annotations

import io
import os
import sys

# MCP speaks JSON-RPC over stdout; the JVM and Ghidra also print to stdout.
# Keep the real stdout for MCP and point file descriptor 1 at stderr.
_mcp_stdout_fd = os.dup(1)
os.dup2(2, 1)
sys.stdout = io.TextIOWrapper(
    os.fdopen(_mcp_stdout_fd, "wb", buffering=0), encoding="utf-8",
    newline="\n", write_through=True,
)

import contextlib  # noqa: E402
import re  # noqa: E402
import threading  # noqa: E402
import traceback  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ghidra_env import PROGRAMS, PROJECT_DIR, PROJECT_NAME, start_ghidra  # noqa: E402

from mcp.server.fastmcp import FastMCP  # noqa: E402

DEFAULT_PROGRAM = "TW_1.10_cnc3game.dat"

mcp = FastMCP(
    "ghidra",
    instructions=(
        "Headless Ghidra over the CnC3 project. Programs: "
        + ", ".join(PROGRAMS)
        + f". Default program: {DEFAULT_PROGRAM}. Addresses are hex VAs "
        "(image base 0x00400000) or symbol names. Call save_program after "
        "renames/comments to persist them."
    ),
)


class _Ghidra:
    """Owns the JVM, the project and opened programs. All access is serialized."""

    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.ready = threading.Event()
        self.error: str | None = None
        self.project = None
        self.programs: dict = {}
        self.decompilers: dict = {}
        threading.Thread(target=self._boot, name="ghidra-boot", daemon=True).start()

    def _boot(self) -> None:
        def log(text: str) -> None:
            print(f"[ghidra-mcp] {text}", file=sys.stderr, flush=True)
        try:
            log("starting Ghidra")
            start_ghidra()
            log("JVM started; opening project")
            from java.lang import System  # type: ignore
            System.setOut(System.err)
            from ghidra.base.project import GhidraProject
            self.project = GhidraProject.openProject(str(PROJECT_DIR), PROJECT_NAME, True)
            log("project open")
        except BaseException:  # noqa: BLE001
            self.error = traceback.format_exc()
            log("boot failed:\n" + self.error)
        finally:
            self.ready.set()

    def wait(self) -> None:
        if not self.ready.wait(timeout=300):
            raise RuntimeError("Ghidra is still starting; retry shortly")
        if self.error:
            raise RuntimeError("Ghidra failed to start:\n" + self.error)

    def program(self, name: str | None):
        self.wait()
        name = name or DEFAULT_PROGRAM
        if name not in self.programs:
            if self.project.getRootFolder().getFile(name) is None:
                raise ValueError(f"program {name!r} is not in the project; "
                                 f"run import_analyze.py first")
            self.programs[name] = self.project.openProgram("/", name, False)
        return self.programs[name]

    def decompiler(self, name: str | None):
        name = name or DEFAULT_PROGRAM
        if name not in self.decompilers:
            from ghidra.app.decompiler import DecompInterface, DecompileOptions
            iface = DecompInterface()
            iface.setOptions(DecompileOptions())
            iface.toggleCCode(True)
            iface.toggleSyntaxTree(False)
            iface.openProgram(self.program(name))
            self.decompilers[name] = iface
        return self.decompilers[name]


G = _Ghidra()


@contextlib.contextmanager
def _session(program: str | None):
    with G.lock:
        yield G.program(program)


def _addr(prog, text: str):
    """Parse a hex VA or a symbol name into a Ghidra Address."""
    text = str(text).strip()
    space = prog.getAddressFactory().getDefaultAddressSpace()
    hex_text = text[2:] if text.lower().startswith("0x") else text
    if re.fullmatch(r"[0-9a-fA-F]{1,8}", hex_text):
        return space.getAddress(int(hex_text, 16))
    symbols = list(prog.getSymbolTable().getSymbols(text))
    if not symbols:
        from ghidra.app.util import NamespaceUtils
        symbols = list(NamespaceUtils.getSymbols(text, prog))
    if not symbols:
        raise ValueError(f"no address or symbol named {text!r}")
    return symbols[0].getAddress()


def _func_name(prog, address) -> str:
    func = prog.getFunctionManager().getFunctionContaining(address)
    return f"{func.getName(True)}@{func.getEntryPoint()}" if func else "-"


def _monitor():
    from ghidra.util.task import TaskMonitor
    return TaskMonitor.DUMMY


@mcp.tool()
def list_programs() -> str:
    """List programs in the Ghidra project and whether they are analyzed."""
    G.wait()
    with G.lock:
        lines = []
        for name in PROGRAMS:
            present = G.project.getRootFolder().getFile(name) is not None
            lines.append(f"{name}: {'imported' if present else 'not imported'}")
        return "\n".join(lines)


@mcp.tool()
def program_info(program: str | None = None) -> str:
    """Image base, memory blocks, function count and language of a program."""
    with _session(program) as prog:
        lines = [
            f"name: {prog.getName()}",
            f"language: {prog.getLanguageID()} / {prog.getCompilerSpec().getCompilerSpecID()}",
            f"image base: {prog.getImageBase()}",
            f"functions: {prog.getFunctionManager().getFunctionCount()}",
            "blocks:",
        ]
        for block in prog.getMemory().getBlocks():
            perms = ("r" if block.isRead() else "-") + ("w" if block.isWrite() else "-") + \
                    ("x" if block.isExecute() else "-")
            lines.append(f"  {block.getName():10} {block.getStart()}-{block.getEnd()} {perms}")
        return "\n".join(lines)


@mcp.tool()
def function_info(address: str, program: str | None = None) -> str:
    """Name, entry, body range, signature, and calling convention of the function containing an address."""
    with _session(program) as prog:
        func = prog.getFunctionManager().getFunctionContaining(_addr(prog, address))
        if func is None:
            return f"no function contains {address}"
        body = func.getBody()
        return "\n".join([
            f"name: {func.getName(True)}",
            f"entry: {func.getEntryPoint()}",
            f"body: {body.getMinAddress()}-{body.getMaxAddress()} ({body.getNumAddresses()} bytes)",
            f"signature: {func.getPrototypeString(True, True)}",
            f"convention: {func.getCallingConventionName()}",
        ])


@mcp.tool()
def decompile(address: str, program: str | None = None, timeout_seconds: int = 90) -> str:
    """Decompile the function containing an address (or named by a symbol) to C."""
    with _session(program) as prog:
        func = prog.getFunctionManager().getFunctionContaining(_addr(prog, address))
        if func is None:
            return f"no function contains {address}"
        result = G.decompiler(program).decompileFunction(func, timeout_seconds, _monitor())
        if not result.decompileCompleted():
            return f"decompilation failed: {result.getErrorMessage()}"
        return f"// {func.getName(True)} @ {func.getEntryPoint()}\n" + \
            result.getDecompiledFunction().getC()


@mcp.tool()
def disassemble(address: str, count: int = 40, program: str | None = None) -> str:
    """Disassemble `count` instructions starting at an address, with bytes and references."""
    with _session(program) as prog:
        listing = prog.getListing()
        start = _addr(prog, address)
        instr = listing.getInstructionAt(start) or listing.getInstructionAfter(start)
        lines = []
        while instr is not None and len(lines) < count:
            raw = " ".join(f"{b & 0xFF:02X}" for b in instr.getBytes())
            refs = [str(r.getToAddress()) for r in instr.getReferencesFrom()
                    if not r.getReferenceType().isFlow() or r.getReferenceType().isCall()]
            ref_text = f"   ; -> {', '.join(refs[:4])}" if refs else ""
            label = prog.getSymbolTable().getPrimarySymbol(instr.getAddress())
            if label is not None:
                lines.append(f"{label.getName(True)}:")
            lines.append(f"  {instr.getAddress()}  {raw:<30} {instr}{ref_text}")
            instr = instr.getNext()
        return "\n".join(lines) if lines else f"no instructions at {address}"


@mcp.tool()
def xrefs_to(address: str, limit: int = 200, program: str | None = None) -> str:
    """References to an address: source address, reference type, and containing function."""
    with _session(program) as prog:
        target = _addr(prog, address)
        lines = []
        for ref in prog.getReferenceManager().getReferencesTo(target):
            src = ref.getFromAddress()
            lines.append(f"{src}  {ref.getReferenceType()}  in {_func_name(prog, src)}")
            if len(lines) >= limit:
                lines.append("... (limit reached)")
                break
        return "\n".join(lines) if lines else f"no references to {target}"


@mcp.tool()
def xrefs_from(address: str, program: str | None = None) -> str:
    """References made by the instruction or data at an address."""
    with _session(program) as prog:
        source = _addr(prog, address)
        refs = prog.getReferenceManager().getReferencesFrom(source)
        lines = [f"{r.getToAddress()}  {r.getReferenceType()}  ({_func_name(prog, r.getToAddress())})"
                 for r in refs]
        return "\n".join(lines) if lines else f"no references from {source}"


@mcp.tool()
def callers(address: str, program: str | None = None) -> str:
    """Functions that call the function containing an address."""
    with _session(program) as prog:
        func = prog.getFunctionManager().getFunctionContaining(_addr(prog, address))
        if func is None:
            return f"no function contains {address}"
        names = sorted(f"{f.getEntryPoint()} {f.getName(True)}"
                       for f in func.getCallingFunctions(_monitor()))
        return "\n".join(names) if names else "no callers found"


@mcp.tool()
def callees(address: str, program: str | None = None) -> str:
    """Functions called by the function containing an address."""
    with _session(program) as prog:
        func = prog.getFunctionManager().getFunctionContaining(_addr(prog, address))
        if func is None:
            return f"no function contains {address}"
        names = sorted(f"{f.getEntryPoint()} {f.getName(True)}"
                       for f in func.getCalledFunctions(_monitor()))
        return "\n".join(names) if names else "no callees found"


@mcp.tool()
def search_functions(name_regex: str, limit: int = 200, program: str | None = None) -> str:
    """Functions whose (namespace-qualified) name matches a regex (case-insensitive)."""
    with _session(program) as prog:
        pattern = re.compile(name_regex, re.IGNORECASE)
        lines = []
        for func in prog.getFunctionManager().getFunctions(True):
            name = func.getName(True)
            if pattern.search(name):
                lines.append(f"{func.getEntryPoint()} {name}")
                if len(lines) >= limit:
                    lines.append("... (limit reached)")
                    break
        return "\n".join(lines) if lines else "no matches"


@mcp.tool()
def search_symbols(name_regex: str, limit: int = 200, program: str | None = None) -> str:
    """All symbols (labels, RTTI/vftable names, globals, functions) matching a regex."""
    with _session(program) as prog:
        pattern = re.compile(name_regex, re.IGNORECASE)
        lines = []
        for sym in prog.getSymbolTable().getAllSymbols(True):
            name = sym.getName(True)
            if pattern.search(name):
                lines.append(f"{sym.getAddress()} {sym.getSymbolType()} {name}")
                if len(lines) >= limit:
                    lines.append("... (limit reached)")
                    break
        return "\n".join(lines) if lines else "no matches"


@mcp.tool()
def search_strings(regex: str, limit: int = 100, program: str | None = None) -> str:
    """Defined string data matching a regex (case-insensitive), with xref counts."""
    with _session(program) as prog:
        pattern = re.compile(regex, re.IGNORECASE)
        refman = prog.getReferenceManager()
        lines = []
        for data in prog.getListing().getDefinedData(True):
            if not data.hasStringValue():
                continue
            value = str(data.getValue())
            if pattern.search(value):
                count = refman.getReferenceCountTo(data.getAddress())
                lines.append(f"{data.getAddress()} [{count} xrefs] {value[:160]!r}")
                if len(lines) >= limit:
                    lines.append("... (limit reached)")
                    break
        return "\n".join(lines) if lines else "no matches"


@mcp.tool()
def search_bytes(pattern: str, limit: int = 50, program: str | None = None) -> str:
    """Find a byte pattern in initialized memory. Hex bytes separated by spaces; '??' is a wildcard."""
    with _session(program) as prog:
        tokens = pattern.split()
        values = bytes(0 if t == "??" else int(t, 16) for t in tokens)
        masks = bytes(0 if t == "??" else 0xFF for t in tokens)
        memory = prog.getMemory()
        lines = []
        for block in memory.getBlocks():
            if not block.isInitialized():
                continue
            start = block.getStart()
            while start is not None and len(lines) < limit:
                found = memory.findBytes(start, block.getEnd(), values, masks, True, _monitor())
                if found is None:
                    break
                lines.append(f"{found}  in {_func_name(prog, found)}")
                start = found.next()
        return "\n".join(lines) if lines else "no matches"


@mcp.tool()
def search_instructions(regex: str, start: str | None = None, end: str | None = None,
                        limit: int = 200, program: str | None = None) -> str:
    """Instructions whose text (e.g. 'MUL.*0x78' or 'dword ptr \\[0x7877c4\\]') matches a regex.

    Without start/end this scans the whole program, which can take a minute.
    """
    with _session(program) as prog:
        pattern = re.compile(regex, re.IGNORECASE)
        listing = prog.getListing()
        if start:
            from ghidra.program.model.address import AddressSet
            lo = _addr(prog, start)
            hi = _addr(prog, end) if end else lo.add(0x1000)
            instructions = listing.getInstructions(AddressSet(lo, hi), True)
        else:
            instructions = listing.getInstructions(True)
        lines = []
        for instr in instructions:
            text = str(instr)
            if pattern.search(text):
                lines.append(f"{instr.getAddress()}  {text}   in {_func_name(prog, instr.getAddress())}")
                if len(lines) >= limit:
                    lines.append("... (limit reached)")
                    break
        return "\n".join(lines) if lines else "no matches"


@mcp.tool()
def read_memory(address: str, length: int = 64, program: str | None = None) -> str:
    """Hex dump of program memory (max 4096 bytes)."""
    with _session(program) as prog:
        import jpype
        start = _addr(prog, address)
        length = max(1, min(int(length), 4096))
        buffer = jpype.JArray(jpype.JByte)(length)
        got = prog.getMemory().getBytes(start, buffer)
        data = bytes(b & 0xFF for b in buffer[:got])
        lines = []
        for offset in range(0, len(data), 16):
            chunk = data[offset:offset + 16]
            text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
            lines.append(f"{start.add(offset)}  {chunk.hex(' ').upper():<47}  {text}")
        return "\n".join(lines)


def _write(prog, description: str, action):
    tx = prog.startTransaction(description)
    ok = False
    try:
        result = action()
        ok = True
        return result
    finally:
        prog.endTransaction(tx, ok)


@mcp.tool()
def rename_function(address: str, new_name: str, program: str | None = None) -> str:
    """Rename the function containing an address (unsaved until save_program)."""
    with _session(program) as prog:
        from ghidra.program.model.symbol import SourceType
        func = prog.getFunctionManager().getFunctionContaining(_addr(prog, address))
        if func is None:
            return f"no function contains {address}"
        old = func.getName(True)
        _write(prog, "rename function", lambda: func.setName(new_name, SourceType.USER_DEFINED))
        return f"renamed {old} -> {func.getName(True)} @ {func.getEntryPoint()}"


@mcp.tool()
def create_label(address: str, name: str, program: str | None = None) -> str:
    """Create a primary label (e.g. for a global variable) at an address (unsaved until save_program)."""
    with _session(program) as prog:
        from ghidra.program.model.symbol import SourceType
        target = _addr(prog, address)

        def action():
            sym = prog.getSymbolTable().createLabel(target, name, SourceType.USER_DEFINED)
            sym.setPrimary()
            return sym
        _write(prog, "create label", action)
        return f"label {name} @ {target}"


@mcp.tool()
def set_comment(address: str, comment: str, kind: str = "plate", program: str | None = None) -> str:
    """Set a comment (kind: plate, pre, post, eol, repeatable) at an address (unsaved until save_program)."""
    with _session(program) as prog:
        from ghidra.program.model.listing import CommentType
        kinds = {"plate": CommentType.PLATE, "pre": CommentType.PRE, "post": CommentType.POST,
                 "eol": CommentType.EOL, "repeatable": CommentType.REPEATABLE}
        target = _addr(prog, address)
        _write(prog, "set comment",
               lambda: prog.getListing().setComment(target, kinds[kind.lower()], comment))
        return f"{kind} comment set @ {target}"


@mcp.tool()
def save_program(program: str | None = None) -> str:
    """Persist pending renames/comments/labels of a program into the Ghidra project."""
    with _session(program) as prog:
        G.project.save(prog)
        return f"saved {prog.getName()}"


@mcp.tool()
def run_python(code: str, program: str | None = None) -> str:
    """Run Python inside the Ghidra JVM session. Escape hatch for anything the other tools lack.

    Available names: currentProgram, flat (FlatProgramAPI), addr(text), project,
    decompiler, monitor. Whatever is print()-ed is returned. Wrap writes in
    currentProgram.startTransaction/endTransaction.
    """
    with _session(program) as prog:
        from ghidra.program.flatapi import FlatProgramAPI
        output = io.StringIO()
        scope = {
            "currentProgram": prog,
            "flat": FlatProgramAPI(prog),
            "addr": lambda text: _addr(prog, text),
            "project": G.project,
            "decompiler": G.decompiler(program),
            "monitor": _monitor(),
        }
        with contextlib.redirect_stdout(output):
            try:
                exec(compile(code, "<run_python>", "exec"), scope)
            except Exception:  # noqa: BLE001
                print(traceback.format_exc())
        return output.getvalue() or "(no output)"


if __name__ == "__main__":
    mcp.run()
