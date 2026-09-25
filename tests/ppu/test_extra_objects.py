"""Build/run the ROM-free extra-object PPU contract on Windows or Unix."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

def main():
    root=Path(__file__).resolve().parents[2]
    cc='C:/msys64/mingw64/bin/gcc.exe' if os.name=='nt' else shutil.which('cc')
    with tempfile.TemporaryDirectory(prefix='ppu-extra-') as tmp:
        exe=Path(tmp)/('test.exe' if os.name=='nt' else 'test')
        subprocess.run([cc,'-std=c11','-O2','-DSNESRECOMP_REVERSE_DEBUG=0',
            '-Irunner/src','-Irunner/src/snes','tests/ppu/ppu_extra_object_test.c',
            'runner/src/snes/ppu.c','runner/src/snes/ppu_legacy.c','-o',str(exe)],cwd=root,check=True)
        subprocess.run([str(exe)],check=True)

if __name__=='__main__':main()
