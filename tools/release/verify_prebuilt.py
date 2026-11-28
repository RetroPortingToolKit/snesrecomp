"""Verify a qualified native Linux build before reusing its ELF."""
from pathlib import Path
import hashlib, json, re, subprocess, sys
artifact, expected, receipt, version, repo, target, config, lua, sdl, raster = sys.argv[1:]
def require(value, message):
    if not value: raise SystemExit('prebuilt: ' + message)
def git(path):
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
data = Path(artifact).read_bytes()
record = json.loads(Path(receipt).read_text())
identity = record['buildIdentity']
require(re.fullmatch('[0-9a-f]{64}', expected), 'invalid expected SHA-256')
require(hashlib.sha256(data).hexdigest() == expected == record['artifactSha256'], 'artifact hash mismatch')
require(data[:6] == b'\x7fELF\x02\x01' and int.from_bytes(data[18:20], 'little') == 62, 'expected Linux x86-64 ELF')
require(version not in ('', 'dev') and identity['target'] == target and identity['version'] == version and version.encode() in data, 'target/version mismatch')
require(identity['titleSource'] == git(repo), 'title source mismatch')
require(identity['sdkSource'] == git(Path(repo) / 'snesrecomp'), 'SDK source mismatch')
require(identity['uiSource'] == git(Path(repo) / 'recomp-ui'), 'UI source mismatch')
profile = identity['profile']
require(not raster or profile['raster'] == raster, 'native raster mismatch')
require(profile['frame'] == 'HLE' and profile['apuTime'] == 'NATIVE' and profile['dspMix'] == 'NATIVE', 'native provider mismatch')
require(profile['executionDefault'] in ('on', 'force') and profile['host'] == 'Linux-x86_64', 'execution profile mismatch')
require(profile['trace'] is (config == 'debug') and profile['lua'] is (lua == 'ON') and profile['sdl'] == sdl, 'host configuration mismatch')
require(re.fullmatch('[0-9a-f]{64}', identity['generatedCorpusSha256']) and
        re.fullmatch('[0-9a-f]{64}', identity['reusedObjectsSha256']) and identity['compiledTUs'], 'missing build inputs')
canonical = json.dumps(identity, sort_keys=True, separators=(',', ':')).encode()
require(hashlib.sha256(canonical).hexdigest() == record['buildIdentitySha256'], 'build identity mismatch')
print('Qualified native ELF identity verified:', expected)
