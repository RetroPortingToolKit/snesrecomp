"""Guarded loaded-image generation never substitutes a patched disk image."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

from tools.image_overlay import apply_ips
from tools.smoke_sdk_generate import write_fixture_rom

ROOT = Path(__file__).resolve().parents[1]


def record(offset, data):
    return offset.to_bytes(3, 'big') + len(data).to_bytes(2, 'big') + data


@pytest.mark.parametrize('patch', [
    b'garbage', b'PATCH', b'PATCH\x00\x00\x00\x00',
    b'PATCH\x00\x00\x00\x00\x02\x01',
    b'PATCH' + record(0xFFFFFF, b'xx') + b'EOF',
    b'PATCH' + record(0, b'a') + record(0, b'b') + b'EOF',
])
def test_malformed_out_of_range_and_conflicting_ips_are_rejected(patch):
    original = b'original'
    with pytest.raises(ValueError):
        apply_ips(original, patch)
    assert original == b'original'


def test_native_stdin_matches_disk_for_binary_control_bytes(tmp_path):
    from tools.sdk_generate import ensure_tool_paths
    ensure_tool_paths(ROOT)
    from tools.v2_analyze import build_manifest_native
    rom = tmp_path / 'fixture.sfc'
    raw = bytearray(write_fixture_rom(rom))
    raw[0x100:0x106] = b'\x1a\r\n\x00\xff\x80'
    rom.write_bytes(raw)
    cfg = tmp_path / 'cfg'; cfg.mkdir()
    (cfg / 'bank00.cfg').write_text('bank = 0\nauto_vectors\n')
    before = hashlib.sha256(rom.read_bytes()).hexdigest()
    disk = build_manifest_native(rom_path=rom, cfg_dir=cfg)[0]
    stream = build_manifest_native(rom_path=rom, cfg_dir=cfg, rom_data=bytes(raw))[0]
    assert stream.to_dict() == disk.to_dict()
    assert hashlib.sha256(rom.read_bytes()).hexdigest() == before


def test_recipe_overlay_provenance_and_rejection_before_output(tmp_path):
    rom = tmp_path / 'fixture.sfc'
    original = write_fixture_rom(rom)
    before = hashlib.sha256(original).hexdigest()
    patch = b'PATCH' + record(0x100, b'\x1a\r\n\x00\xff\x80') + b'EOF'
    (tmp_path / 'overlay.ips').write_bytes(patch)
    (tmp_path / 'rom_identity.txt').write_text(f'expected_sha256 = {before}\n')
    cfg = tmp_path / 'recomp'; cfg.mkdir()
    (cfg / 'bank00.cfg').write_text('bank = 0\nauto_vectors\n')
    recipe = ('schema = 1\nidentity_file = "rom_identity.txt"\n'
              '[[image_overlays]]\nfile = "overlay.ips"\nsha256 = "' +
              hashlib.sha256(patch).hexdigest() + '"\n')
    (cfg / 'generate.toml').write_text(recipe)
    command = [sys.executable, str(ROOT / 'tools/regen_project.py'),
               '--project-root', str(tmp_path), '--rom', str(rom), '--no-tests']
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    metadata = json.loads((tmp_path / 'src/gen/.snesrecomp-image-view.json').read_text())
    assert metadata['original_sha256'] == before
    assert metadata['loaded_image_sha256'] == hashlib.sha256(apply_ips(original, patch)).hexdigest()
    assert metadata['overlays'] == [hashlib.sha256(patch).hexdigest()]
    assert len(metadata['plan_sha256']) == 64
    assert hashlib.sha256(rom.read_bytes()).hexdigest() == before
    published = (tmp_path / 'src/gen/dispatch_v2.c').read_bytes()
    (tmp_path / 'overlay.ips').write_bytes(b'invalid')
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode != 0
    assert (tmp_path / 'src/gen/dispatch_v2.c').read_bytes() == published
    assert hashlib.sha256(rom.read_bytes()).hexdigest() == before
