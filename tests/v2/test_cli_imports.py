"""Every shipped Python entry point must at least parse.

snesrecomp_cli.py sat on main with a SyntaxError -- an orphaned `if` fragment
left by a conflict resolution -- which makes the module unimportable and takes
`generate` down with it. Every port's tools/regen.sh then dies with
IndentationError before it even reads the ROM.

Nothing caught it because nothing imports these files: the v2 suite tests the
recompiler package, not the command-line front ends. A syntax check is the
cheapest possible gate and it would have caught this one
(recomp-ai-rules/PRINCIPLES.md, "Enforce the Rule in the Artifact").
"""
import ast
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]

# Entry points a port or CI actually invokes.
ENTRY_POINTS = [
    "snesrecomp_cli.py",
    "tools/rom_identity.py",
    "tools/new_project/probe_rom.py",
    "tools/new_project/setup_project.sh",   # shell: existence only
]


def test_cli_starts_without_optional_toml_parser():
    # Reproduce Python < 3.11 without tomli on every CI interpreter. The CLI
    # must remain usable by cfg-only projects, including its help/verify paths.
    result = subprocess.run([
        sys.executable, "-c",
        "import runpy, sys; "
        "sys.modules['tomllib'] = None; sys.modules['tomli'] = None; "
        "sys.argv = ['snesrecomp_cli.py', 'generate', '--help']; "
        "runpy.run_path('snesrecomp_cli.py', run_name='__main__')",
    ], cwd=REPO, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "--cfg-dir" in result.stdout


def test_entry_points_parse():
    missing, broken = [], []
    for rel in ENTRY_POINTS:
        path = REPO / rel
        if not path.is_file():
            missing.append(rel)
            continue
        if path.suffix != ".py":
            continue
        try:
            ast.parse(path.read_text(encoding="utf-8", errors="replace"),
                      filename=str(path))
        except SyntaxError as exc:
            broken.append(f"{rel}: line {exc.lineno}: {exc.msg}")
    if missing:
        raise AssertionError("missing entry points: " + ", ".join(missing))
    if broken:
        raise AssertionError("entry points do not parse:\n  " +
                             "\n  ".join(broken))
