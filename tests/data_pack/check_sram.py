"""Compile the actual SRAM routines without the unrelated full runner graph."""
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'runner/src/common_rtl.c').read_text()
names=('void RtlSetSaveRoot(', 'const char *RtlSaveRoot(', 'void RtlEnsureSaveDir(',
       'void RtlSramFilePath(', 'int RtlTryWriteSram(', 'void RtlWriteSram(')
parts=[]
for name in names:
    start=source.index(name)
    opening=source.index('{',start)
    end=source.index('}',opening)+1 if name == names[1] else source.index('\n}',opening)+2
    parts.append(source[start:end])
prefix = '#include "common_rtl.h"\n#ifdef _WIN32\n#include <io.h>\n#include <direct.h>\n#else\n#include <unistd.h>\n#include <sys/stat.h>\n#endif\nuint8 *g_sram; int g_sram_size; static char s_save_root[96]="saves";\n'
with tempfile.TemporaryDirectory(prefix='sram-check-') as temp:
    temp=Path(temp)
    (temp/'sram.c').write_text(prefix+'\n'.join(parts))
    exe=temp/'check.exe'
    subprocess.run([sys.argv[1],'-std=c11','-I'+str(root/'runner/src'),
                    str(root/'tests/data_pack/sram_test.c'),str(temp/'sram.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=temp,check=True)
