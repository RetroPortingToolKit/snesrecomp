"""The regeneration driver's real no-console child must retain its output."""
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("exit_code", [0, 7], ids=["success", "failure"])
def test_framework_test_output_survives_no_console_launch(tmp_path, exit_code):
    engine = tmp_path / "fixture-engine"
    tests = engine / "tests"
    tests.mkdir(parents=True)
    (tests / "run_tests.py").write_text(
        "import sys\n"
        "print('framework child stdout', flush=True)\n"
        "print('framework child stderr', file=sys.stderr, flush=True)\n"
        f"raise SystemExit({exit_code})\n",
        encoding="utf-8",
    )
    project = tmp_path / "game"
    project.mkdir()
    driver = tmp_path / "driver.py"
    # Generation/ROM discovery are unrelated to child-process forwarding.
    # Keep the real main() and subprocess launch; only replace those stages.
    driver.write_text(
        "import sys\nfrom pathlib import Path\n"
        f"sys.path.insert(0, {str(ROOT)!r})\n"
        "from tools import regen_project as regen\n"
        f"regen.ROOT = Path({str(engine)!r})\n"
        "regen.ensure_tool_paths = lambda root: None\n"
        "regen.load_recipe = lambda cfg: None\n"
        "regen.parse = lambda path: {'rom_file': 'fixture.sfc'}\n"
        "regen.generate = lambda **options: None\n"
        f"raise SystemExit(regen.main(['--project-root', {str(project)!r}]))\n",
        encoding="utf-8",
    )
    result = subprocess.run(
        [sys.executable, str(driver)], capture_output=True, text=True,
        encoding="utf-8", errors="replace",
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        timeout=30,
    )
    assert (result.returncode == 0) == (exit_code == 0)
    assert "framework child stdout" in result.stdout
    assert "framework child stderr" in result.stderr
