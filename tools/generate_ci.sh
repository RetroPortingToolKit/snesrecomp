#!/usr/bin/env sh
# Write the release workflow into an existing SNES project (Linux / macOS /
# WSL / Git Bash). The logic is tools/generate_ci.py; this only finds Python.
#
#   sh snesrecomp/tools/generate_ci.sh                 # cwd is the project
#   sh snesrecomp/tools/generate_ci.sh ~/src/MyGame    # or name the root
#   sh snesrecomp/tools/generate_ci.sh --check         # stale? (exit 1)
#   sh snesrecomp/tools/generate_ci.sh --force         # overwrite
#
# Every flag passes through: --display-name, --project-name, --zip-prefix,
# --dry-run. `--help` prints the full list.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHON=${PYTHON:-}
if [ -z "$PYTHON" ]; then
  # Windows App Execution Aliases can exist on PATH without running Python.
  for _snes_python in python3 python; do
    if "$_snes_python" -c 'import sys; sys.exit(sys.version_info[0] != 3)' >/dev/null 2>&1; then
      PYTHON=$_snes_python
      break
    fi
  done
fi
[ -n "$PYTHON" ] || { echo "generate_ci: python3 not found on PATH" >&2; exit 1; }
exec "$PYTHON" "$SCRIPT_DIR/generate_ci.py" "$@"
