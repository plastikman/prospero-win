#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A thread creates its first window while another holds the loader lock and
then creates one too: neither waits for the other (wine/patches/0981).

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and x86_64-w64-mingw32-gcc; without either
the check is skipped with a message. It runs headless (no DISPLAY): with a
display, the first window waits for explorer's desktop and the race is gone. A
win32u without the patch hangs (verdict=hang after 15 s)."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_builtin_classes.c"


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    if not os.access(wine, os.X_OK) or not compiler:
        print("wine builtin classes skipped: needs the host Wine and x86_64-w64-mingw32-gcc")
        return 0

    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "wine_builtin_classes.exe"
        subprocess.run([compiler, "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(exe), str(SOURCE)], check=True)
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
        for name in ("DISPLAY", "WAYLAND_DISPLAY"):
            env.pop(name, None)
        done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env, timeout=120)
        assert "builtin-classes verdict=pass" in done.stdout, f"{done.stdout}{done.stderr}"
    print("wine builtin classes passed: a first window under the loader lock does not wait for another thread")
    return 0


if __name__ == "__main__":
    sys.exit(main())
