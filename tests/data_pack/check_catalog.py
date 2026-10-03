"""Exercise the compiled catalog with independently authored folder/ZIP fixtures."""
import copy
import hashlib
import json
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile

PROBE = Path(sys.argv.pop(1)).resolve()
PAYLOAD = b'semantic resource data'
BASE = dict(format='snesrecomp.data-pack', version=1, game='test-game', id='sample',
            title='A Sample', base_rom_sha256='0'*64, requires=['test.known'],
            payload=dict(format='test.data', file='data.bin', sha256=hashlib.sha256(PAYLOAD).hexdigest()))

class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='data-pack-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def folder(self, name='sample', manifest=None):
        path = self.root/name; path.mkdir()
        (path/'pack.json').write_text(json.dumps(BASE if manifest is None else manifest))
        (path/'data.bin').write_bytes(PAYLOAD)
        (path/'cover.txt').write_bytes(b'cover')
        return path

    def archive(self, name='sample.zip', prefix='', manifest=None, extras=()):
        path = self.root/name
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
            z.writestr(prefix+'pack.json', json.dumps(BASE if manifest is None else manifest))
            z.writestr(prefix+'data.bin', PAYLOAD)
            z.writestr(prefix+'cover.txt', b'cover')
            for entry, data in extras: z.writestr(entry, data)
        return path

    def scan(self, asset=None):
        out = subprocess.check_output([str(PROBE), str(self.root)]+([asset] if asset else []), text=True)
        return out

    def test_missing_directory_has_no_side_effects(self):
        missing = self.root/'absent'
        self.assertEqual(subprocess.check_output([str(PROBE), str(missing)], text=True), '')
        self.assertFalse(missing.exists())

    def test_folder_and_zip_equal(self):
        folder = self.folder()
        expected = self.scan('cover.txt')
        self.assertIn(f'PACK\tsample\t{len(PAYLOAD)}', expected)
        self.assertIn('ASSET\t5', expected)
        shutil.rmtree(folder)
        self.archive(prefix='wrapped/')
        self.assertEqual(self.scan('cover.txt'), expected)

    def test_zip_root_and_rename_stable(self):
        path = self.archive()
        expected = self.scan()
        path.rename(self.root/'different-name.zip')
        self.assertEqual(self.scan(), expected)

    def test_multiple_ids_sorted_and_duplicate_all_refused(self):
        a = copy.deepcopy(BASE);a['id']='z-last'
        self.folder('a', a)
        b = copy.deepcopy(BASE);b['id']='a-first'
        self.archive('z.zip', manifest=b)
        packs = [s for s in self.scan().splitlines() if s.startswith('PACK')]
        self.assertEqual([s.split('\t')[1] for s in packs], ['a-first','z-last'])
        self.archive('duplicate.zip', manifest=a)
        out = self.scan()
        self.assertIn('Duplicate pack ID',out)
        self.assertNotIn('PACK\tz-last',out)
        self.assertIn('PACK\ta-first',out)

    def test_incompatible_packs_do_not_hide_valid_siblings(self):
        self.folder()
        for field, value in [('game','another-game'),('base_rom_sha256','f'*64),
                             ('requires',['unknown']),('version',2),('id','../bad')]:
            m = copy.deepcopy(BASE);m[field]=value
            path=self.archive('bad.zip',manifest=m)
            out=self.scan()
            self.assertEqual(out.count('PACK\t'),1)
            self.assertIn('ERROR\t',out)
            path.unlink()

    def test_bad_payload_hash(self):
        folder=self.folder();(folder/'data.bin').write_bytes(b'changed')
        self.assertIn('SHA-256 mismatch',self.scan())
        self.assertNotIn('PACK\t',self.scan())

    def test_duplicate_json_keys_and_nul(self):
        folder=self.folder()
        for text in ('{"id":"a",'+json.dumps(BASE)[1:], json.dumps(BASE).replace('A Sample','A\\u0000Sample')):
            (folder/'pack.json').write_text(text)
            self.assertNotIn('PACK\t',self.scan())

    def test_zip_slip_and_absolute_and_backslash(self):
        for name in ('../escape', '/absolute', 'C:/escape', '..\\escape', 'a/../b', 'x/CON', 'x/alias.'):
            path=self.archive(extras=[(name,b'x')])
            self.assertNotIn('PACK\t',self.scan(),name)
            path.unlink()
        self.assertFalse((self.root.parent/'escape').exists())

    def test_zip_links_and_case_alias(self):
        link=zipfile.ZipInfo('link');link.create_system=3;link.external_attr=(stat.S_IFLNK|0o777)<<16
        path=self.archive(extras=[(link,b'../outside')])
        self.assertIn('links',self.scan());path.unlink()
        self.archive(extras=[('DATA.BIN',b'collision')])
        self.assertIn('Duplicate ZIP path',self.scan())

    def test_asset_sandbox_and_bound(self):
        folder=self.folder();(folder/'large.bin').write_bytes(bytes(17))
        self.assertIn('ERROR\t',self.scan('../escape'))
        self.assertIn('size limit',self.scan('large.bin'))
        self.assertIn('Missing pack file',self.scan('missing.bin'))

    def test_folder_symlink_escape(self):
        folder=self.folder();outside=self.root/'outside';outside.write_bytes(PAYLOAD)
        (folder/'data.bin').unlink()
        try:(folder/'data.bin').symlink_to(outside)
        except OSError:self.skipTest('symlink privilege unavailable')
        self.assertIn('symlink escapes',self.scan())

    def test_multiple_manifests_refused(self):
        self.archive(extras=[('second/pack.json',b'{}')])
        self.assertIn('exactly one pack.json',self.scan())

    def materialize(self):
        out=subprocess.check_output([str(PROBE),str(self.root),'cover.txt',str(self.root/'.cache')],text=True)
        self.assertNotIn('ERROR',out)
        return Path(next(s.split('\t',1)[1] for s in out.splitlines() if s.startswith('DIR\t')))

    def test_materialized_folder_and_wrapped_zip(self):
        folder=self.folder()
        self.assertEqual(self.materialize(),folder.resolve())
        self.assertFalse((self.root/'.cache').exists())
        shutil.rmtree(folder)
        self.archive(prefix='wrapped/',extras=[('wrapped/music/song.pcm',b'MSU1'+bytes(100000))])
        root=self.materialize()
        self.assertEqual((root/'data.bin').read_bytes(),PAYLOAD)
        self.assertEqual((root/'music/song.pcm').stat().st_size,100004)
        timestamp=(root/'data.bin').stat().st_mtime_ns
        self.assertEqual(self.materialize(),root)
        self.assertEqual((root/'data.bin').stat().st_mtime_ns,timestamp)

    def test_hidden_cache_directory_is_not_a_pack(self):
        self.folder('.cache')
        self.assertEqual(self.scan(),'')

    def test_materialized_cache_never_discovers_removed_or_changed_pack(self):
        archive=self.archive();first=self.materialize();archive.unlink()
        self.assertEqual(self.scan(),'')
        m=copy.deepcopy(BASE);m['id']='replacement'
        self.archive(manifest=m)
        second=self.materialize()
        self.assertNotEqual(first,second)
        self.assertIn('PACK\treplacement',self.scan())
        self.assertNotIn('PACK\tsample',self.scan())

    def test_materialized_cache_payload_tampering_rejected(self):
        self.archive();root=self.materialize();(root/'data.bin').write_bytes(b'wrong')
        out=subprocess.check_output([str(PROBE),str(self.root),'cover.txt',str(self.root/'.cache')],text=True)
        self.assertIn('Cached payload differs',out)
        self.assertNotIn('DIR\t',out)

    def test_changed_and_removed_archive_no_cache(self):
        path=self.archive();self.assertIn('PACK\tsample',self.scan());path.unlink()
        m=copy.deepcopy(BASE);m['id']='replacement'
        path=self.archive(manifest=m)
        self.assertIn('PACK\treplacement',self.scan());path.unlink()
        self.assertEqual(self.scan(),'')
        self.assertEqual(list(self.root.iterdir()),[])

if __name__ == '__main__':unittest.main()
