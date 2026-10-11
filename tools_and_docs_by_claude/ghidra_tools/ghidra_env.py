"""Start headless Ghidra (PyGhidra) with all state kept inside this project.

Ghidra normally writes user settings to %APPDATA%\\ghidra, caches to
%LOCALAPPDATA%, and PyGhidra writes a plugin lock to the system temp folder.
CLAUDE.md forbids writing outside the project, so every one of those is
redirected under tools_and_docs_by_claude/ghidra_data/ and temp/.

Import this module before anything touches `ghidra.*`:

    from ghidra_env import start_ghidra, PROJECT_DIR, PROJECT_NAME
    start_ghidra()
"""

from __future__ import annotations

import os
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parents[1]
DATA_DIR = REPO_ROOT / "tools_and_docs_by_claude" / "ghidra_data"
BINARIES_DIR = DATA_DIR / "binaries"
PROJECT_DIR = DATA_DIR / "projects"
PROJECT_NAME = "CnC3"
SETTINGS_DIR = DATA_DIR / "settings"
CACHE_DIR = DATA_DIR / "cache"
TEMP_DIR = REPO_ROOT / "temp" / "ghidra_tmp"

GHIDRA_INSTALL_DIR = Path(
    os.environ.get(
        "GHIDRA_INSTALL_DIR",
        r"C:\Users\Kellan\UsefulSSD\ToolsSSD\ghidra_12.1.3_PUBLIC",
    )
)

# Program names inside the Ghidra project -> source file in BINARIES_DIR.
PROGRAMS = {
    "TW_1.10_cnc3game.dat": BINARIES_DIR / "TW_1.10_cnc3game.dat",
    "KW_1.03_cnc3ep1.dat": BINARIES_DIR / "KW_1.03_cnc3ep1.dat",
}


def _redirect_temp() -> None:
    TEMP_DIR.mkdir(parents=True, exist_ok=True)
    # PyGhidra's plugin lock uses tempfile.gettempdir(), which reads these.
    for name in ("TEMP", "TMP", "TMPDIR"):
        os.environ[name] = str(TEMP_DIR)
    import tempfile

    tempfile.tempdir = str(TEMP_DIR)


def _detect_java_home() -> str:
    """Ask the JDK on PATH for java.home.

    Setting the launcher's java_home ourselves stops PyGhidra from running
    Ghidra's `LaunchSupport -jdk_home -save`, which writes java_home.save into
    %APPDATA%\\ghidra.
    """
    import subprocess

    explicit = os.environ.get("GHIDRA_JAVA_HOME")
    if explicit:
        return explicit
    output = subprocess.run(
        ["java", "-XshowSettings:properties", "-version"],
        capture_output=True, text=True, check=True,
    ).stderr
    for line in output.splitlines():
        key, sep, value = line.strip().partition(" = ")
        if sep and key == "java.home":
            return value.strip()
    raise RuntimeError("could not determine java.home; set GHIDRA_JAVA_HOME")


def start_ghidra(verbose: bool = False):
    """Start the JVM once; later calls are no-ops."""
    _redirect_temp()
    import pyghidra
    from pyghidra.launcher import HeadlessPyGhidraLauncher

    if pyghidra.started():
        return None
    for path in (SETTINGS_DIR, CACHE_DIR, PROJECT_DIR):
        path.mkdir(parents=True, exist_ok=True)
    launcher = HeadlessPyGhidraLauncher(verbose=verbose, install_dir=GHIDRA_INSTALL_DIR)
    launcher.java_home = Path(_detect_java_home())
    launcher.add_vmargs(
        f"-Dapplication.settingsdir={SETTINGS_DIR}",
        f"-Dapplication.cachedir={CACHE_DIR}",
        f"-Djava.io.tmpdir={TEMP_DIR}",
        # Ghidra's analysis of a 16-24 MB image benefits from a larger heap.
        "-Xmx8G",
    )
    launcher.start()
    return launcher
