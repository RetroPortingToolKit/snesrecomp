"""Compile and run the portable stopped-clock oracle in both build modes."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="superfx-idle-") as temporary:
    for native in (0, 1):
        output = Path(temporary) / (f"idle-{native}" + (".exe" if os.name == "nt" else ""))
        subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra",
            f"-DSNESRECOMP_SUPERFX_IDLE_NATIVE={native}",
            "-I" + str(root / "runner/src"),
            str(root / "tests/superfx/idle_span_test.c"), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)
