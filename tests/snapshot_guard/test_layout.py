"""Compile the production layout query with a serializer/lock test double."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def test_layout():
    cc = ('C:/msys64/mingw64/bin/gcc.exe' if os.name == 'nt'
          else shutil.which('cc'))
    source = (ROOT/'runner/src/common_rtl.c').read_text()
    start = source.index('typedef struct MemorySli {')
    parts = [source[start:source.index('} MemorySli;', start)+len('} MemorySli;')]]
    for name in ('static void memory_sli_func(', 'size_t RtlSnapshotGuestLayout(',
                 'size_t RtlSnapshotGuestSize('):
        start = source.index(name)
        parts.append(source[start:source.index('\n}', start)+2])
    prefix = ('#include "common_rtl.h"\n#include "snes/snes.h"\n'
              '#include "snes/cpu.h"\n#include <string.h>\n'
              '#define RTL_SAV_VERSION 10\nextern Snes *g_snes;\n')
    with tempfile.TemporaryDirectory(prefix='snes-state-layout-') as temp:
        temp = Path(temp)
        (temp/'layout.c').write_text(prefix+'\n'.join(parts))
        exe = temp/('layout.exe' if os.name == 'nt' else 'layout')
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-missing-field-initializers',
                        '-I'+str(ROOT/'runner/src'), str(temp/'layout.c'),
                        str(Path(__file__).with_name('layout_test.c')),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    test_layout()
