"""Build and compare scalar/native DSP mixing with GCC or Clang, without a ROM."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
parser.add_argument('--build-dir', type=Path, default=root / 'build/audio/dsp-mix')
args = parser.parse_args()
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
base = [args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-Wno-unused-function',
        '-D_POSIX_C_SOURCE=200809L', '-DSNESRECOMP_TRACE=0',
        '-I' + str(root / 'runner/src'), '-I' + str(root / 'runner/src/snes'),
        '-ffunction-sections', '-fdata-sections']
source = root / 'runner/src/snes/dsp.c'
names = re.findall(r'^(?:bool|void|Dsp\*|Dsp \*|int16_t|uint32_t|uint8_t) '
                   r'(dsp_\w+)\(', source.read_text(), re.M)
assert 'dsp_init' in names
env = {key: value for key, value in os.environ.items()
       if not key.startswith('SNESRECOMP_')}
def run(command):
    subprocess.run(command, env=env, check=True)
for arm in ('reference', 'native'):
    defines = ['-DSNESRECOMP_DSP_NATIVE_MIX=' + str(int(arm == 'native'))]
    if arm == 'reference':
        defines += ['-D' + name + '=reference_' + name for name in names]
        defines += ['-DgaussValues=reference_gaussValues']
    run(base + defines + ['-c', str(source), '-o', str(build / (arm + '.o'))])
binary = build / ('dsp_mix_test.exe' if os.name == 'nt' else 'dsp_mix_test')
sources = [root / 'tests/audio/dsp_mix_test.c', build / 'reference.o',
           build / 'native.o', root / 'runner/src/snes/dsp_shadow.c',
           root / 'runner/src/snes/audio_shadow.c', root / 'runner/src/audio_trace.c']
gc = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
run(base + [str(path) for path in sources] + [gc, '-lm', '-o', str(binary)])
run([str(binary)])
