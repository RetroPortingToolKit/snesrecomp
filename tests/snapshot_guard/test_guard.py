"""Exercise the optional state envelope without a ROM or SDL."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class GuardTest(unittest.TestCase):
    def test_envelope(self):
        cc = 'C:/msys64/mingw64/bin/gcc.exe' if os.name == 'nt' else shutil.which('cc')
        self.assertTrue(cc, 'A C11 compiler is required')
        with tempfile.TemporaryDirectory(prefix='snes-state-guard-') as build:
            exe = Path(build) / ('guard.exe' if os.name == 'nt' else 'guard')
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'runner/src'),
                str(ROOT / 'runner/src/snapshot_guard.c'),
                str(ROOT / 'runner/src/crc32.c'),
                str(Path(__file__).with_name('guard_test.c')), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    unittest.main()
