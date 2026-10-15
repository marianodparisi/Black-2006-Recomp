#!/usr/bin/env python3
"""Run real shell entry points against a recording Wine/Proton command."""
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

    def test_wine_and_proton_share_paths_and_forward_arguments(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'BLACK spaces $ quote\" percent%'
            (root / 'scripts/linux').mkdir(parents=True)
            for name in ['Play BLACK.sh', 'BLACK Settings.sh', 'scripts/linux/launch.sh']:
                shutil.copyfile(ROOT / name, root / name)
            for name in ['BLACK PC Launcher.exe', 'BLACK PC Settings.exe']:
                (root / name).touch()
            stub = Path(temp) / 'record runner'
            record = Path(temp) / 'record.json'
            stub.write_text('#!/usr/bin/env python3\nimport json,os,sys\n'
                            'from pathlib import Path\n'
                            'Path(os.environ["RECORD"]).write_text(json.dumps({"args":sys.argv[1:],'
                            '"cwd":os.getcwd(),"prefix":os.environ.get("WINEPREFIX"),'
                            '"compat":os.environ.get("STEAM_COMPAT_DATA_PATH")}))\n', encoding='utf-8')
            stub.chmod(0o755)
            env = dict(os.environ, BLACK_WINE=str(stub), RECORD=str(record))
            for script, exe in [('Play BLACK.sh', 'BLACK PC Launcher.exe'), ('BLACK Settings.sh', 'BLACK PC Settings.exe')]:
                subprocess.run(['bash', str(root / script), '--preview', 'a path with spaces'], cwd=temp, env=env, check=True)
                result = json.loads(record.read_text())
                self.assertEqual(result['args'], [str(root / exe), '--preview', 'a path with spaces'])
                self.assertEqual(result['cwd'], str(root))
                self.assertEqual(result['prefix'], str(root / '.wine'))
            env.update(BLACK_PROTON=str(stub), STEAM_COMPAT_CLIENT_INSTALL_PATH=temp)
            for script, exe in [('Play BLACK.sh', 'BLACK PC Launcher.exe'), ('BLACK Settings.sh', 'BLACK PC Settings.exe')]:
                subprocess.run(['bash', str(root / script), '--check'], cwd=temp, env=env, check=True)
                result = json.loads(record.read_text())
                self.assertEqual(result['args'], ['run', str(root / exe), '--check'])
                self.assertEqual(result['compat'], str(root / '.proton'))


if __name__ == '__main__':
    unittest.main()
