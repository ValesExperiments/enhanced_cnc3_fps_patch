"""Command-line client for the Ghidra MCP server.

Starts ghidra_mcp_server.py over stdio (a real MCP session), runs a batch of
tool calls in order, and prints each result. Useful for testing the server and
for scripted analysis when the MCP tools aren't loaded in the current session.

Usage:
    python mcp_call.py --list
    python mcp_call.py TOOL '{"arg": "value"}' [TOOL '{...}' ...]
    python mcp_call.py --batch calls.json      # [["tool", {args}], ...]
"""

from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

SERVER = Path(__file__).with_name("ghidra_mcp_server.py")


def _parse(argv: list[str]) -> tuple[bool, list[tuple[str, dict]]]:
    if argv[:1] == ["--list"]:
        return True, []
    if argv[:1] == ["--batch"]:
        return False, [(name, args) for name, args in json.loads(Path(argv[1]).read_text())]
    calls = []
    i = 0
    while i < len(argv):
        name = argv[i]
        args = {}
        if i + 1 < len(argv) and argv[i + 1].lstrip().startswith("{"):
            args = json.loads(argv[i + 1])
            i += 1
        calls.append((name, args))
        i += 1
    return False, calls


async def main(argv: list[str]) -> int:
    list_only, calls = _parse(argv)
    params = StdioServerParameters(command=sys.executable, args=[str(SERVER)])
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            if list_only:
                for tool in (await session.list_tools()).tools:
                    print(f"{tool.name}: {(tool.description or '').splitlines()[0]}")
                return 0
            for name, args in calls:
                result = await session.call_tool(name, args)
                print(f"===== {name} {json.dumps(args)}" + (" [ERROR]" if result.isError else ""))
                for item in result.content:
                    print(getattr(item, "text", item))
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main(sys.argv[1:])))
