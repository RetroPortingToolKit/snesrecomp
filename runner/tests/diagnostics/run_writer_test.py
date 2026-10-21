"""Run the writer's blocking-sink regression in an isolated output directory."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="snes-diagnostic-writer-") as work:
    env = os.environ | {"SDL_AUDIODRIVER": "dummy", "SDL_VIDEODRIVER": "dummy"}
    subprocess.run([str(Path(sys.argv[1]).resolve())], cwd=work, env=env, check=True, timeout=30)
