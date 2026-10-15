#!/usr/bin/env python3
"""Release content, checksums, modes and accidental private-file regressions."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('build_release', Path(__file__).with_name('build_release.py'))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
spec = importlib.util.spec_from_file_location('build_provenance', Path(__file__).parents[1] / 'linux/write_build_provenance.py')
provenance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provenance)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp_path = Path(tempfile.mkdtemp(prefix='black-release-test-'))
        self.root = self.temp_path / 'repo'
        self.root.mkdir()
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        subprocess.run(['git', '-C', str(self.root), 'config', 'core.autocrlf', 'false'], check=True)
        for name in release.ROOT_FILES:
            (self.root / name).write_bytes(b'# host source\n')
        for name in ['src/main.c', 'localization/game/es.jsonl', 'scripts/linux/launch.sh']:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'# host source\n')
        self.launchers = self.temp_path / 'launchers'
        self.launchers.mkdir()
        for name in release.LAUNCHERS:
            (self.launchers / name).write_bytes(b'MZ synthetic authored launcher')
        self.stage()
        subprocess.run(['git', '-C', str(self.root), '-c', 'user.name=Release Test',
                        '-c', 'user.email=release-test@example.invalid', 'commit', '-qm', 'fixture'], check=True)

    def tearDown(self):
        # Keep Windows Git fixtures for review; never recursively remove a
        # Windows checkout without its separately reviewed reparse audit.
        if os.name != 'nt':
            shutil.rmtree(self.temp_path)

    def stage(self):
        subprocess.run(['git', '-C', str(self.root), 'add', '.'], check=True)

    def test_public_contents_checksums_and_modes(self):
        # These untracked retail/private files must never be traversed or packed.
        for name in ['game/default.xbe', 'game/language/strings/MainUS.bin',
                     'src/recomp/gen/recomp_0.c', 'local/xbox-launcher.json', 'save/progress.bin']:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('private canary', encoding='utf-8')
        artifacts = release.build(self.root, self.launchers, self.root / 'out', '0.1.0-alpha.1', True)
        with zipfile.ZipFile(artifacts[0]) as archive:
            files = {name.split('/', 1)[1]: archive.read(name) for name in archive.namelist()}
            self.assertNotIn('game/default.xbe', files)
            self.assertNotIn('src/recomp/gen/recomp_0.c', files)
            self.assertNotIn('local/xbox-launcher.json', files)
            self.assertIn('localization/game/es.jsonl', files)
            self.assertEqual(set(name for name in files if name.endswith('.exe')), set(release.LAUNCHERS))
            for line in files['SHA256SUMS'].decode().splitlines():
                expected, name = line.split('  ', 1)
                self.assertEqual(expected, hashlib.sha256(files[name]).hexdigest())
            marker = json.loads(files['black-release.json'])
            # An extracted starter has no Git checkout. Its build report must
            # bind the packed revision rather than fail after compilation.
            extracted = self.temp_path / 'extracted'
            extracted.mkdir()
            (extracted / 'black-release.json').write_bytes(files['black-release.json'])
            self.assertFalse((extracted / '.git').exists())
            self.assertEqual(provenance.source_revision(extracted), marker['source_commit'])
            self.assertEqual(provenance.source_revision(self.root), marker['source_commit'])
            self.assertFalse(marker['game_executable_included'])
            self.assertFalse(marker['game_files_included'])
            for name, expected in marker['files'].items():
                self.assertEqual(expected, hashlib.sha256(files[name]).hexdigest())
            self.assertEqual(archive.getinfo('BLACK-0.1.0-alpha.1/Play BLACK.sh').external_attr >> 16 & 0o777, 0o755)
        with tarfile.open(artifacts[1]) as archive:
            self.assertTrue(all(member.isfile() for member in archive))
            self.assertEqual(archive.getmember('BLACK-0.1.0-alpha.1/Play BLACK.sh').mode, 0o755)
            self.assertEqual({m.name.split('/', 1)[1]: archive.extractfile(m).read() for m in archive}, files)

    def test_tracked_generated_source_rejected(self):
        path = self.root / 'src/recomp/gen/recomp_0.c'
        path.parent.mkdir(parents=True)
        path.write_text('generated code', encoding='utf-8')
        self.stage()
        with self.assertRaisesRegex(ValueError, 'Private path'):
            release.collect_sources(self.root)

    def test_tracked_bank_and_secret_rejected(self):
        path = self.root / 'src/retail.bin'
        path.write_bytes(b'retail canary')
        self.stage()
        with self.assertRaisesRegex(ValueError, 'Private/binary'):
            release.collect_sources(self.root)
        path.unlink()
        (self.root / 'src/main.c').write_text('gh' + 'p_' + 'A' * 36, encoding='utf-8')
        self.stage()
        with self.assertRaisesRegex(ValueError, 'Possible secret'):
            release.collect_sources(self.root)

    def test_shell_crlf_rejected(self):
        (self.root / 'Play BLACK.sh').write_bytes(b'#!/bin/bash\r\n')
        with self.assertRaisesRegex(ValueError, 'LF line endings'):
            release.collect_sources(self.root)

    def test_clean_release_requires_commit(self):
        (self.root / 'README.md').write_text('edited', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'Commit the release sources'):
            release.build(self.root, self.launchers, self.root / 'out', '0.1.0')

    def test_symlink_rejected(self):
        if os.name == 'nt':
            self.skipTest('Symlink fixture runs in Linux CI without Windows privilege requirements')
        target = self.temp_path / 'private.txt'
        target.write_text('private canary', encoding='utf-8')
        (self.root / 'src/main.c').unlink()
        (self.root / 'src/main.c').symlink_to(target)
        with self.assertRaisesRegex(ValueError, 'linked/reparse'):
            release.collect_sources(self.root)


if __name__ == '__main__':
    unittest.main()
