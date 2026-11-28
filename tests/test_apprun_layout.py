"""The actual shared AppRun preserves portable user state across relaunches."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_shared_apprun_portable_state_contract(tmp_path):
    bash = (Path('C:/Program Files/Git/bin/bash.exe') if os.name == 'nt'
            else shutil.which('bash'))
    if not bash:
        pytest.skip('Bash unavailable')
    appdir = tmp_path / 'AppDir'
    binary = appdir / 'usr/bin/FixtureSNESRecomp'
    binary.parent.mkdir(parents=True)
    binary.write_text('#!/bin/sh\n[ -f config.ini ] || printf "[Fixture]\\n" > config.ini\nexit 1\n')
    binary.chmod(0o755)
    package = appdir / 'usr/bin/mods/preloaded/packages/fixture/1/manifest.toml'
    package.parent.mkdir(parents=True); package.write_text('release-owned\n')
    script = tmp_path / 'layout.sh'
    script.write_text('''set -eu
REPO="$2"; APPDIR="$3"; EXE=FixtureSNESRecomp
ROM_EXTS="sfc smc"; EXTRA_ARGS=""
. "$1/tools/release/appimage_layout.sh"
write_apprun
sh "$1/tools/release/test_appimage_layout.sh" "$APPDIR" "$EXE"
''')
    subprocess.run([str(bash), str(script), ROOT.as_posix(), tmp_path.as_posix(), appdir.as_posix()],
                   env=os.environ, check=True)
