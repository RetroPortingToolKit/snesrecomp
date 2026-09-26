"""Write the shared data-pack envelope; game tools own semantic qualification."""
import hashlib
import json
from pathlib import Path
import re
import zipfile


def manifest_bytes(*, game, ident, title, base_sha256, payload_format,
                   payload_name, payload, requires=()):
    if not re.fullmatch(r'[a-z0-9][a-z0-9._-]{0,62}', ident):
        raise ValueError('invalid pack ID')
    if not re.fullmatch(r'[0-9a-f]{64}', base_sha256):
        raise ValueError('invalid base ROM digest')
    if not payload_name or Path(payload_name).name != payload_name or ':' in payload_name or '\\' in payload_name:
        raise ValueError('payload must have a portable basename')
    if payload_name.lower() in ('pack.json', 'readme.txt'):
        raise ValueError('payload name is reserved')
    manifest = dict(format='snesrecomp.data-pack', version=1, game=game, id=ident,
                    title=title, base_rom_sha256=base_sha256, requires=list(requires),
                    payload=dict(format=payload_format, file=payload_name,
                                 sha256=hashlib.sha256(payload).hexdigest()))
    return (json.dumps(manifest, indent=2)+'\n').encode('utf-8')


def write_pack(output, *, game, ident, title, base_sha256, payload_format,
               payload_name, payload, requires=(), readme=''):
    encoded = manifest_bytes(game=game, ident=ident, title=title, base_sha256=base_sha256,
                             payload_format=payload_format, payload_name=payload_name,
                             payload=payload, requires=requires)
    files = {'pack.json': encoded, payload_name: payload}
    if readme:
        files['README.txt'] = readme.encode('utf-8')
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    # No silent overwrite of an installed pack or the source file.
    if output.suffix.lower() == '.zip':
        with zipfile.ZipFile(output, 'x', zipfile.ZIP_DEFLATED) as z:
            for name, data in files.items():
                z.writestr(name, data)
    else:
        output.mkdir()
        for name, data in files.items():
            (output/name).write_bytes(data)
    return json.loads(encoded)
