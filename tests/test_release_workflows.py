"""Release primitives verify dependency closure and actual portable ZIP bytes."""
import json
import os
from pathlib import Path
import struct
import subprocess
import zipfile

import pytest

ROOT = Path(__file__).resolve().parents[1]
pytestmark = pytest.mark.skipif(os.name != 'nt', reason='Windows PE staging entry point')
POWERSHELL = Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'


def pe(path, imports=(), machine=0x8664):
    data = bytearray(4096)
    data[:2] = b'MZ'; struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', data, 0x84, machine, 1)
    struct.pack_into('<H', data, 0x94, 240)
    struct.pack_into('<H', data, 0x98, 0x20b)
    section = 0x98 + 240
    struct.pack_into('<IIII', data, section + 8, 0x800, 0x1000, 0x800, 0x200)
    if imports:
        struct.pack_into('<II', data, 0x98 + 112 + 8, 0x1000, 20 * (len(imports) + 1))
        names = 0x400
        for index, name in enumerate(imports):
            struct.pack_into('<I', data, 0x200 + 20 * index + 12, 0x1000 + names - 0x200)
            raw = name.encode() + b'\0'; data[names:names+len(raw)] = raw; names += len(raw)
    path.write_bytes(data)


def bundle(stage, search):
    env = dict(os.environ, PATH=str(POWERSHELL.parent))
    return subprocess.run([str(POWERSHELL), '-NoProfile', '-NonInteractive', '-File',
                           str(ROOT / 'tools/release/stage_runtime_dlls.ps1'),
                           '-StageDir', str(stage), '-SearchDirsJson', json.dumps([str(search)])],
                          env=env, capture_output=True, text=True)


def test_transitive_closure_is_complete_without_toolchain_on_path(tmp_path):
    stage = tmp_path / 'runtime'; search = tmp_path / 'toolchain'
    stage.mkdir(); search.mkdir()
    pe(stage / 'game.exe', ['SDL3.dll'])
    pe(search / 'SDL3.dll', ['transitive.dll'])
    pe(search / 'transitive.dll')
    result = bundle(stage, search)
    assert result.returncode == 0, result.stdout + result.stderr
    assert {p.name for p in stage.iterdir()} == {'game.exe', 'SDL3.dll', 'transitive.dll'}


@pytest.mark.parametrize('bad_arch', [False, True])
def test_missing_or_wrong_architecture_dependency_is_rejected(tmp_path, bad_arch):
    stage = tmp_path / 'runtime'; search = tmp_path / 'toolchain'
    stage.mkdir(); search.mkdir()
    pe(stage / 'game.exe', ['dependency.dll'])
    if bad_arch:
        pe(search / 'dependency.dll', machine=0x14c)
    result = bundle(stage, search)
    assert result.returncode != 0


def test_archive_entries_are_portable_and_match_all_staged_bytes(tmp_path):
    stage = tmp_path / 'runtime with spaces'; stage.mkdir()
    (stage / 'assets').mkdir()
    (stage / 'assets/payload.txt').write_bytes(b'\x00\x1a\r\n\xff')
    archive = tmp_path / 'runtime.zip'
    script = tmp_path / 'archive.ps1'
    script.write_text('param($Engine, $Stage, $Archive)\n'
                      '. (Join-Path $Engine "tools/release/RuntimeRelease.ps1")\n'
                      'New-RuntimeReleaseArchive -StageDir $Stage -ArchivePath $Archive\n')
    subprocess.run([str(POWERSHELL), '-NoProfile', '-NonInteractive', '-File', str(script),
                    '-Engine', str(ROOT), '-Stage', str(stage), '-Archive', str(archive)], check=True)
    with zipfile.ZipFile(archive) as zipped:
        assert zipped.namelist() == ['assets/payload.txt']
        assert zipped.read('assets/payload.txt') == (stage / 'assets/payload.txt').read_bytes()
