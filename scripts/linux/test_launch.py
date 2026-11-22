#!/usr/bin/env python3
"""Run real shell entry points against a recording launcher."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LaunchTests(unittest.TestCase):
    def test_build_entry_point_accepts_sh_and_preserves_arguments(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'BLACK spaces $ quote" percent%'
            (root / 'scripts').mkdir(parents=True)
            shutil.copyfile(ROOT / 'Build BLACK.sh', root / 'Build BLACK.sh')
            record = Path(temp) / 'build-record.json'
            (root / 'scripts/setup-linux.sh').write_text(
                '#!/bin/bash\n'
                'python3 - "$@" <<\'PY\'\n'
                'import json, os, sys\nfrom pathlib import Path\n'
                'Path(os.environ["RECORD"]).write_text(json.dumps(sys.argv[1:]))\n'
                'PY\nexit 23\n', encoding='utf-8')
            env = dict(os.environ, RECORD=str(record))
            arguments = ['--game-only', 'argument with spaces', 'literal $value']
            for shell in ['sh', 'bash']:
                result = subprocess.run([shell, str(root / 'Build BLACK.sh'), *arguments],
                                        cwd=temp, env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 23, result.stderr)
                self.assertEqual(json.loads(record.read_text()), arguments)

    @unittest.skipUnless(shutil.which('gio'), 'gio is needed to exercise desktop entry parsing')
    def test_desktop_shortcuts_escape_actual_folder(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'BLACK spaces $ quote" slash\\ percent%'
            (root / 'scripts/linux').mkdir(parents=True)
            shutil.copyfile(ROOT / 'scripts/linux/install-shortcuts.py', root / 'scripts/linux/install-shortcuts.py')
            record = Path(temp) / 'desktop-record'
            for name in ['Play BLACK.sh', 'BLACK Settings.sh']:
                (root / name).write_text('#!/bin/bash\nprintf success > "$RECORD"\n', encoding='utf-8')
            subprocess.run(['python3', str(root / 'scripts/linux/install-shortcuts.py')], check=True, capture_output=True)
            env = dict(os.environ, RECORD=str(record))
            for name in ['Play BLACK.desktop', 'BLACK Settings.desktop']:
                record.unlink(missing_ok=True)
                subprocess.run(['gio', 'launch', str(root / name)], env=env, check=True, capture_output=True)
                deadline = time.monotonic() + 5
                while not record.exists() and time.monotonic() < deadline:
                    time.sleep(0.05)
                self.assertEqual(record.read_text(), 'success')

    def test_entry_points_open_the_launcher_in_its_folder(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'BLACK spaces $ quote" percent%'
            (root / 'scripts/linux').mkdir(parents=True)
            for name in ['Play BLACK.sh', 'BLACK Settings.sh', 'scripts/linux/launch.sh']:
                shutil.copyfile(ROOT / name, root / name)
            launcher = root / 'BLACK Launcher'
            record = Path(temp) / 'record.json'
            launcher.write_text('#!/usr/bin/env python3\nimport json,os,sys\nfrom pathlib import Path\n'
                                'Path(os.environ["RECORD"]).write_text(json.dumps({"args":sys.argv[1:],'
                                '"cwd":os.getcwd()}))\n', encoding='utf-8')
            launcher.chmod(0o755)
            env = dict(os.environ, RECORD=str(record))
            for script, args in [('Play BLACK.sh', ['--play']), ('BLACK Settings.sh', [])]:
                subprocess.run(['bash', str(root / script)], cwd=temp, env=env, check=True)
                result = json.loads(record.read_text())
                self.assertEqual(result['args'], args)
                self.assertEqual(result['cwd'], str(root))
            for script, options, expected in [
                    ('Play BLACK.sh', ['--fullscreen'], ['--play', '--fullscreen']),
                    ('BLACK Settings.sh', ['--import', 'My disc.iso'], ['--import', 'My disc.iso'])]:
                subprocess.run(['bash', str(root / script), *options], cwd=temp, env=env, check=True)
                self.assertEqual(json.loads(record.read_text()), {'args': expected, 'cwd': temp})

    def test_a_missing_launcher_is_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'scripts/linux').mkdir(parents=True)
            for name in ['Play BLACK.sh', 'scripts/linux/launch.sh']:
                shutil.copyfile(ROOT / name, root / name)
            result = subprocess.run(['bash', str(root / 'Play BLACK.sh')], capture_output=True, text=True,
                                    env=dict(os.environ, PATH='/usr/bin:/bin'))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('launcher is missing', result.stderr)


if __name__ == '__main__':
    unittest.main()
