"""Exercise the shipping executable without a GPU, a disc or the GUI."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

LAUNCHER = Path(sys.argv.pop(1)).resolve()


class LauncherCliTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.cwd = Path(temporary.name)
        self.project = self.cwd / 'BLACK portable ü'
        (self.project / 'scripts').mkdir(parents=True)
        (self.project / 'scripts/generate.py').touch()
        (self.project / 'CMakeLists.txt').touch()

    def cli(self, *args, success=True):
        command = [LAUNCHER, '--project-dir', self.project.name, *args]
        result = subprocess.run(command, cwd=self.cwd, capture_output=True, encoding='utf-8', timeout=10)
        self.assertEqual(result.returncode, 0 if success else 1, (result.stdout, result.stderr))
        return result

    def test_settings_persist_in_selected_project(self):
        local = self.project / 'local'
        local.mkdir()
        (local / 'controls.ini').write_text('[controller]\nLayout=2\n')
        self.cli('--set', 'FpsLimit=60', 'VSync=false', 'MenuLanguage=de', 'Bindings={"FIRE":"Mouse2"}')
        saved = json.loads(self.cli('--settings').stdout)
        self.assertEqual(saved['FpsLimit'], 60)
        self.assertIs(saved['VSync'], False)
        self.assertEqual(saved['MenuLanguage'], 'de')
        self.assertEqual(saved['Bindings'], {'FIRE': 'Mouse2'})
        self.assertIn('fps_limit=60', (local / 'video.ini').read_text())
        self.assertIn('[controller]\nLayout=2', (local / 'controls.ini').read_text())
        self.assertFalse((self.cwd / 'local').exists())

    def test_invalid_batch_has_no_side_effects(self):
        for bad in ['VSync=off', 'FpsLimit=42', 'FpsLimit=60.0', 'MenuLanguage=xx',
                    'SettingsVersion=1', 'NoSuchKey=10', 'Bindings={"not-an-action":"C"}']:
            with self.subTest(bad=bad):
                self.cli('--set', 'Threads=6', bad, success=False)
        self.assertFalse((self.project / 'local').exists())

    def test_invalid_commands_and_disc_report_errors_without_opening_a_window(self):
        self.assertIn('--project-dir', subprocess.check_output([LAUNCHER, '--help'], cwd=self.cwd, encoding='utf-8'))
        self.assertIn('not been built', self.cli('--play', '--fullscreen', '--no-dialogs', success=False).stderr)
        for args in [('--build', '--typo'), ('--play', '--typo'), ('--import',), ('--set',), ('--unknown',)]:
            self.cli(*args, success=False)
        self.assertIn('missing.iso', self.cli('--import', 'missing.iso', success=False).stderr)
        disc = self.cwd / 'Disc files'
        disc.mkdir()
        (disc / 'default.xbe').write_bytes(b'unsupported')
        self.cli('--import', disc.name, success=False)
        self.assertFalse((self.project / 'game').exists())
        invalid = subprocess.run([LAUNCHER, '--project-dir', self.project / 'scripts', '--settings'],
                                 cwd=self.cwd, capture_output=True, timeout=10)
        self.assertEqual(invalid.returncode, 1)


if __name__ == '__main__':
    unittest.main()
