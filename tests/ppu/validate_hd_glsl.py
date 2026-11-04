"""Compile/link the real HD shader strings with Khronos glslangValidator.

This optional developer check needs glslangValidator, not an OpenGL context.
It catches language errors that a permissive GPU driver can otherwise hide.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def shader_strings(source, start, end):
    body = source.split(start, 1)[1].split(end, 1)[0]
    return ''.join(json.loads(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', body))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--validator', default=shutil.which('glslangValidator'))
    args = parser.parse_args()
    if not args.validator:
        parser.error('Install Khronos glslang or pass --validator; game builds do not require it')
    root = Path(__file__).resolve().parents[2]
    source = (root / 'runner/src/desktop/hd_gpu.c').read_text(encoding='utf-8')
    vertex = shader_strings(source, 'static const char *vertex_source =',
                            'static const char *fragment_source =')
    fragment = shader_strings(source, 'static const char *fragment_source =',
                              '/* One double-precision')
    coordinate = shader_strings(source, 'static const char *coordinate_source =',
                                'static GLuint hd_shader')
    with tempfile.TemporaryDirectory(prefix='snes-hd-glsl-') as directory:
        folder = Path(directory)
        vert = folder / 'hd.vert'
        vert.write_bytes(vertex.encode('utf-8'))
        for name, prefix, body in (
                ('register', '#version 330 core\n', fragment),
                ('affine', '#version 400 core\n#define HD_AFFINE 1\n', fragment),
                ('coordinate', '#version 400 core\n#define HD_AFFINE 1\n', coordinate)):
            frag = folder / (name + '.frag')
            frag.write_bytes((prefix + body).encode('utf-8'))
            subprocess.run([args.validator, '-l', str(vert), str(frag)], check=True)
            print('PASS HD GLSL compile/link:', name, flush=True)


if __name__ == '__main__':
    main()
