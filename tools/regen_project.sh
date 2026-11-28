#!/usr/bin/env sh
# Shared interpreter discovery; all generation policy is in the project recipe.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHON=${PYTHON:-}
if [ -z "$PYTHON" ]; then
  for _snes_python in python3 python; do
    if "$_snes_python" -c 'import sys; sys.exit(sys.version_info < (3, 9))' >/dev/null 2>&1; then
      PYTHON=$_snes_python
      break
    fi
  done
fi
[ -n "$PYTHON" ] || { echo "regen_project: Python 3.9+ not found" >&2; exit 1; }
exec "$PYTHON" "$SCRIPT_DIR/regen_project.py" "$@"
