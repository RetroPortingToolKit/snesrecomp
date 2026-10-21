"""Project-owned generation inputs shared by the CLI and setup launcher.

Recipes live at <cfg-dir>/generate.toml. Hooks are Python argv, never shell
commands, and edit a fresh unpublished tree. Game-specific transformations
remain in the game repository.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import string


def hook_python() -> str:
    """A frozen CLI executable cannot be used as a Python script runner."""
    if not getattr(sys, "frozen", False):
        return sys.executable
    for candidate in (os.environ.get("PYTHON"), shutil.which("python"),
                      shutil.which("python3")):
        if not candidate:
            continue
        try:
            result = subprocess.run(
                [candidate, "-c", "import sys; sys.exit(sys.version_info < (3, 9))"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            if result.returncode == 0:
                return candidate
        except (OSError, subprocess.TimeoutExpired):
            continue
    raise RuntimeError("project generation hooks require Python 3.9+; "
                       "install Python or set PYTHON to its executable")


@dataclass
class Recipe:
    path: Path
    root: Path
    data: dict

    def local(self, value: str) -> Path:
        path = (self.root / value).resolve()
        if not path.is_relative_to(self.root):
            raise ValueError(f"recipe path escapes project: {value}")
        return path

    def profiles(self, key: str) -> list[Path]:
        return [(self.path.parent / value).resolve()
                for value in self.data.get(key, [])]

    def identity(self) -> dict:
        from tools.rom_identity import parse
        value = self.data.get("identity_file")
        return parse(self.local(value)) if value else {}

    def digest(self) -> str:
        inputs = {"recipe": self.path.read_bytes().hex()}
        for phase in ("prepare", "postprocess"):
            for hook in self.data.get(phase, []):
                script = self.local(hook["script"])
                inputs[hook["script"]] = hashlib.sha256(script.read_bytes()).hexdigest()
        return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()

    def run(self, phase: str, *, rom: Path, cfg_dir: Path,
            out_dir: Path, reporter) -> None:
        values = dict(rom=str(rom), cfg_dir=str(cfg_dir),
                      out_dir=str(out_dir), project_root=str(self.root))
        for hook in self.data.get(phase, []):
            script = self.local(hook["script"])
            argv = [hook_python(), str(script),
                    *[arg.format_map(values) for arg in hook.get("args", [])]]
            reporter.phase(phase, pct=.85 if phase == "postprocess" else .1,
                           message=f"{phase}: {hook['script']}")
            result = subprocess.run(
                argv, cwd=self.root, text=True, encoding="utf-8", errors="replace",
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            for line in result.stdout.splitlines():
                reporter.log(line)
            if result.returncode:
                raise RuntimeError(f"{phase} hook {hook['script']} failed "
                                   f"(exit {result.returncode})")


def load_recipe(cfg_dir: Path) -> Recipe | None:
    path = cfg_dir / "generate.toml"
    if not path.is_file():
        return None
    try:
        import tomllib
    except ModuleNotFoundError:
        try:
            import tomli as tomllib
        except ModuleNotFoundError as exc:
            raise ValueError(f"{path}: Python < 3.11 requires tomli "
                             "(python -m pip install tomli)") from exc
    data = tomllib.loads(path.read_text(encoding="utf-8"))
    allowed = {"schema", "identity_file", "cfg_roots", "no_host_root_scan",
               "profiles", "historical_profiles", "bind_legacy_profiles",
               "prepare", "postprocess"}
    if type(data.get("schema")) is not int or data.get("schema") != 1 or data.keys() - allowed:
        raise ValueError(f"unsupported generation recipe: {path}")
    recipe = Recipe(path, cfg_dir.parent.resolve(), data)
    if "identity_file" in data and not isinstance(data["identity_file"], str):
        raise ValueError("recipe identity_file must be a path")
    for key in ("cfg_roots", "no_host_root_scan", "bind_legacy_profiles"):
        if key in data and not isinstance(data[key], bool):
            raise ValueError(f"recipe {key} must be a boolean")
    for key in ("profiles", "historical_profiles"):
        value = data.get(key, [])
        if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
            raise ValueError(f"recipe {key} must be a list of paths")
        for profile in recipe.profiles(key):
            if not profile.is_relative_to(recipe.root) or not profile.is_file():
                raise ValueError(f"missing or external recipe profile: {profile}")
    for phase in ("prepare", "postprocess"):
        hooks = data.get(phase, [])
        if not isinstance(hooks, list):
            raise ValueError(f"recipe {phase} must be a list of hook tables")
        for hook in hooks:
            if (not isinstance(hook, dict) or set(hook) - {"script", "args"}
                    or not isinstance(hook.get("script"), str)
                    or not isinstance(hook.get("args", []), list)
                    or not all(isinstance(a, str) for a in hook.get("args", []))):
                raise ValueError(f"invalid recipe {phase} hook")
            if not recipe.local(hook["script"]).is_file():
                raise ValueError(f"missing recipe hook: {hook['script']}")
            for argument in hook.get("args", []):
                for _literal, field, _spec, _conversion in string.Formatter().parse(argument):
                    if field is not None and field not in {"rom", "out_dir", "cfg_dir", "project_root"}:
                        raise ValueError(f"unknown recipe argument field: {field}")
    return recipe
