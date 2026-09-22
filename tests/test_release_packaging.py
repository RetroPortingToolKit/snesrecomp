"""Tests for tools/release/MakeRelease.ps1.

The release pipeline had no test at all, and the two bugs it has shipped were
both silent: a hardcoded runtime-DLL list that omitted a new transitive
dependency (players got 0xC000007B), and a mod catalog that never reached the
zip (the Mods page rendered empty and nothing complained). Both are cheap to
pin down without a real game build.

The stand-in executable is a copy of a real Windows system DLL, because the
packaging step genuinely parses PE headers to walk the import graph; a
zero-byte placeholder would fail for the wrong reason. Its imports all resolve
under System32, so the closure walk correctly stages nothing extra.

Windows-only: the module is PowerShell and the thing under test is Windows
packaging. Skips elsewhere.
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
MODULE = REPO_ROOT / 'tools' / 'release' / 'MakeRelease.ps1'

# Small, always-present, x64. Ordered by preference.
PE_CANDIDATES = (
    r'C:\Windows\System32\version.dll',
    r'C:\Windows\System32\winmm.dll',
    r'C:\Windows\System32\kernel32.dll',
)


def _skip_reason():
    if sys.platform != 'win32':
        return 'windows-only packaging module'
    if not MODULE.exists():
        return f'module not present: {MODULE}'
    if not any(os.path.exists(p) for p in PE_CANDIDATES):
        return 'no system PE available to stand in for the executable'
    return None


def _stand_in_pe():
    for candidate in PE_CANDIDATES:
        if os.path.exists(candidate):
            return candidate
    raise AssertionError('no stand-in PE')


def _make_fake_title(root: pathlib.Path, *, with_mods=True,
                     stage_mods=True, extra_build_file=None):
    """A minimal repository + build directory shaped like a real title."""
    pe = _stand_in_pe()
    build = root / 'build-recompui'
    (build / 'assets' / 'fonts').mkdir(parents=True)
    (build / 'assets' / 'fonts' / 'font.ttf').write_bytes(b'not-a-pe')

    shutil.copyfile(pe, build / 'TestGameSNESRecomp.exe')

    runtime_bin = root / 'fake-runtime-bin'
    runtime_bin.mkdir()
    shutil.copyfile(pe, runtime_bin / 'SDL3.dll')

    (root / 'README.md').write_text('# Test Game\n', encoding='utf-8')
    (root / 'config.ini').write_text(
        '[Graphics]\nWidescreen = 1\nEnhancedRenderer = 1\n', encoding='utf-8')

    if with_mods:
        source_pkg = root / 'mods' / 'preloaded' / 'packages' / 'test.mod'
        source_pkg.mkdir(parents=True)
        (source_pkg / 'manifest.toml').write_text('id = "test.mod"\n',
                                                  encoding='utf-8')
        if stage_mods:
            staged_pkg = build / 'mods' / 'preloaded' / 'packages' / 'test.mod'
            staged_pkg.mkdir(parents=True)
            (staged_pkg / 'manifest.toml').write_text('id = "test.mod"\n',
                                                      encoding='utf-8')

    if extra_build_file:
        target = build / extra_build_file
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b'rom-like-payload')

    return build, runtime_bin


def _run_release(root: pathlib.Path, build: pathlib.Path,
                 runtime_bin: pathlib.Path, *, extra_args=''):
    script = f'''
$ErrorActionPreference = 'Stop'
. "{MODULE}"
$zip = Invoke-SnesRecompRelease -Root "{root}" -Version '1.2.3' `
  -ExeName 'TestGameSNESRecomp.exe' -StageBase 'TestGameRecomp' `
  -BuildDir "{build}" -RuntimeBinDir "{runtime_bin}" `
  -AllowUnstampedExe -ConfigOverrides @{{ Widescreen = '0' }} {extra_args}
Write-Output "ZIP=$zip"
'''
    completed = subprocess.run(
        ['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
        capture_output=True, text=True, cwd=str(root))
    return completed


def test_packages_a_portable_zip_with_the_mod_catalog():
    reason = _skip_reason()
    if reason:
        print(f'  skipped: {reason}')
        return
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp) / 'TestGameRecomp'
        root.mkdir()
        build, runtime_bin = _make_fake_title(root)
        result = _run_release(root, build, runtime_bin)
        assert result.returncode == 0, (
            f'packaging failed:\n{result.stdout}\n{result.stderr}')

        zips = list((root / 'release-stage').glob('*.zip'))
        assert len(zips) == 1, f'expected one zip, got {zips}'
        with zipfile.ZipFile(zips[0]) as archive:
            names = archive.namelist()

        assert all('\\' not in n for n in names), (
            f'non-portable entry names: {names}')
        assert 'TestGameSNESRecomp.exe' in names
        assert 'SDL3.dll' in names
        assert 'README.md' in names
        assert 'assets/fonts/font.ttf' in names, names
        assert 'mods/preloaded/packages/test.mod/manifest.toml' in names, names

        with zipfile.ZipFile(zips[0]) as archive:
            config = archive.read('config.ini').decode('ascii')
        assert 'Widescreen = 0' in config, config
        # An override must not disturb neighbouring keys.
        assert 'EnhancedRenderer = 1' in config, config


def test_refuses_to_ship_a_title_whose_mod_catalog_never_reached_the_stage():
    """The regression guard: source has packages, the build staged none."""
    reason = _skip_reason()
    if reason:
        print(f'  skipped: {reason}')
        return
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp) / 'TestGameRecomp'
        root.mkdir()
        build, runtime_bin = _make_fake_title(root, stage_mods=False)
        result = _run_release(root, build, runtime_bin)
        assert result.returncode != 0, (
            'packaging succeeded with an empty mod catalog; that is the bug '
            'this test exists to prevent')
        combined = result.stdout + result.stderr
        assert 'Mods page would ship' in combined, combined
        assert not list((root / 'release-stage').glob('*.zip')), (
            'a zip was produced despite the failure')


def test_rejects_forbidden_content_in_the_stage():
    reason = _skip_reason()
    if reason:
        print(f'  skipped: {reason}')
        return
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp) / 'TestGameRecomp'
        root.mkdir()
        build, runtime_bin = _make_fake_title(
            root, extra_build_file='assets/owner.sfc')
        result = _run_release(
            root, build, runtime_bin,
            extra_args=r"-ForbidPathPattern @('\.sfc$')")
        assert result.returncode != 0, 'a ROM-like payload was packaged'
        combined = result.stdout + result.stderr
        assert 'Forbidden content' in combined, combined


def test_requires_the_version_stamp_by_default():
    reason = _skip_reason()
    if reason:
        print(f'  skipped: {reason}')
        return
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp) / 'TestGameRecomp'
        root.mkdir()
        build, runtime_bin = _make_fake_title(root)
        script = f'''
$ErrorActionPreference = 'Stop'
. "{MODULE}"
Invoke-SnesRecompRelease -Root "{root}" -Version '9.9.9' `
  -ExeName 'TestGameSNESRecomp.exe' -StageBase 'TestGameRecomp' `
  -BuildDir "{build}" -RuntimeBinDir "{runtime_bin}"
'''
        result = subprocess.run(
            ['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
            capture_output=True, text=True, cwd=str(root))
        assert result.returncode != 0, (
            'packaged an executable not stamped with the release version')
        combined = result.stdout + result.stderr
        assert 'not stamped with version' in combined, combined
