"""Exercise production CMake selections without a game, image or SDK build."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CMAKE = os.environ.get('CMAKE') or shutil.which('cmake')


@unittest.skipUnless(CMAKE, 'CMake is required for configure checks')
class NativeDefaultsTest(unittest.TestCase):
    def configure(self, windows=False, processor='x86_64', pointer=8, options=()):
        production = (ROOT / 'runner/runner.cmake').read_text(encoding='utf-8')
        # Evaluate the real selection/validation statements; the remaining
        # runner adds unrelated SDL/net dependencies and source lists.
        providers = production[:production.index('set(SNESRECOMP_INTERP_READ_IMPL')]
        start = production.index('set(SNESRECOMP_EXECUTION_DEFAULT ')
        end = production.index('# The TCP debug server', start)
        policy = production[start:end]
        with tempfile.TemporaryDirectory(prefix='snes-native-defaults-') as folder:
            work = Path(folder)
            (work / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(defaults NONE)\n'
                f'set(WIN32 {"TRUE" if windows else "FALSE"})\n'
                f'set(CMAKE_SYSTEM_PROCESSOR {processor})\nset(CMAKE_SIZEOF_VOID_P {pointer})\n'
                + providers + policy + '\nfile(WRITE "${CMAKE_BINARY_DIR}/selected.txt" '
                '"${SNESRECOMP_FRAME_IMPL};${SNESRECOMP_APU_TIME_IMPL};'
                '${SNESRECOMP_DSP_MIX_IMPL};${_SNESRECOMP_EXEC_DEFAULT};${_SNESRECOMP_CRC_DEFAULT}")\n',
                encoding='utf-8')
            run = subprocess.run([CMAKE, '-S', str(work), '-B', str(work / 'build'), *options],
                                 capture_output=True, text=True)
            selected = work / 'build/selected.txt'
            return run, selected.read_text().split(';') if selected.exists() else None

    def test_fresh_native_choices(self):
        for windows, processor, pointer, crc in ((True, 'AMD64', 8, 'SLICE8'),
                (False, 'x86_64', 8, 'BYTE'), (False, 'aarch64', 8, 'BYTE'),
                (False, 'armv7', 4, 'BYTE'), (True, 'x86', 4, 'BYTE')):
            with self.subTest(windows=windows, processor=processor):
                run, selected = self.configure(windows, processor, pointer)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertEqual(selected, ['HLE', 'NATIVE', 'NATIVE', 'on', crc])

    def test_explicit_reference_selections_are_preserved(self):
        run, selected = self.configure(options=('-DSNESRECOMP_FRAME_IMPL=LLE',
            '-DSNESRECOMP_APU_TIME_IMPL=LLE', '-DSNESRECOMP_DSP_MIX_IMPL=LLE',
            '-DSNESRECOMP_EXECUTION_DEFAULT=off'))
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(selected, ['LLE', 'LLE', 'LLE', 'off', 'BYTE'])

    def test_invalid_backend_is_rejected(self):
        run, selected = self.configure(options=('-DSNESRECOMP_DSP_MIX_IMPL=unknown',))
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('SNESRECOMP_DSP_MIX_IMPL must be LLE or NATIVE', run.stderr)
        self.assertIsNone(selected)


if __name__ == '__main__':
    unittest.main()
