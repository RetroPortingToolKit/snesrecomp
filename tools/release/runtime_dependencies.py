"""Python entry point to the canonical Windows PE dependency checker."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess


def bundle_windows_runtime(stage: Path, search_dirs) -> None:
    """Stage the PE import closure and verify architecture and presence.

    Uses the same pure .NET reader as PowerShell release entry points. Search
    directories are explicit; unrelated PATH entries cannot satisfy imports.
    """
    powershell = shutil.which("pwsh") or shutil.which("powershell")
    if powershell is None:
        raise RuntimeError("Windows runtime staging requires PowerShell")
    subprocess.run([
        powershell, "-NoProfile", "-NonInteractive", "-File",
        str(Path(__file__).with_name("stage_runtime_dlls.ps1")),
        "-StageDir", str(Path(stage).resolve()),
        "-SearchDirsJson", json.dumps([str(Path(p).resolve()) for p in search_dirs]),
    ], check=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
