"""Exercise the shipping executable without a GPU, a disc or the GUI."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from threading import Barrier

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

    def test_explicit_default_binding_resets_an_override(self):
        self.cli('--set', 'Bindings={"FIRE":"Mouse2"}')
        self.cli('--set', 'Bindings={"FIRE":"Mouse1"}')
        self.assertEqual(json.loads(self.cli('--settings').stdout)['Bindings'], {})
        self.assertIn('FIRE=Mouse1', (self.project / 'local/controls.ini').read_text())

    def test_simultaneous_setting_changes_are_all_saved(self):
        wanted = dict(Threads=6, VSync=False, FpsLimit=60, MenuLanguage="de", SkipMovies=True,
                      InfiniteHealth=True, Subtitles=False, InputMode="controller", OutputWidth=1920)
        barrier = Barrier(len(wanted))

        def set_value(item):
            key, value = item
            barrier.wait(timeout=10)
            self.cli('--set', f'{key}={value if isinstance(value, str) else json.dumps(value)}')

        with ThreadPoolExecutor(max_workers=len(wanted)) as pool:
            list(pool.map(set_value, wanted.items()))
        saved = json.loads(self.cli('--settings').stdout)
        self.assertEqual({key: saved[key] for key in wanted}, wanted)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux executable lookup')
    def test_invoked_symlink_selects_its_own_starter(self):
        other = self.cwd / 'Other starter'
        shutil.copytree(self.project, other)
        real = other / LAUNCHER.name
        shutil.copy2(LAUNCHER, real)
        link = self.project / LAUNCHER.name
        link.symlink_to(real)
        env = dict(os.environ, PATH=str(self.project) + os.pathsep + os.environ['PATH'])
        for invocation in [str(link), link.name]:
            result = subprocess.run([invocation, '--set', 'Threads=7'], cwd=self.cwd, env=env,
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse((other / 'local').exists())
            self.assertEqual(json.loads(self.cli('--settings').stdout)['Threads'], 7)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux shortcut error dialog')
    def test_missing_project_uses_the_requested_play_error_mode(self):
        detached = self.cwd / LAUNCHER.name
        shutil.copy2(LAUNCHER, detached)
        dialog = self.cwd / 'zenity'
        dialog.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$DIALOG_RECORD"\n')
        dialog.chmod(0o755)
        record = self.cwd / 'dialog.txt'
        env = dict(os.environ, PATH=str(self.cwd) + os.pathsep + os.environ['PATH'], DIALOG_RECORD=str(record))
        for options in [['--no-dialogs'], []]:
            result = subprocess.run([detached, '--play', *options], cwd=self.cwd, env=env, capture_output=True,
                                    encoding='utf-8', timeout=10)
            self.assertEqual(result.returncode, 1)
            self.assertIn('project', result.stderr)
            if options:
                self.assertFalse(record.exists())
            else:
                deadline = time.monotonic() + 5
                while not record.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertIn('--error', record.read_text())

    @unittest.skipUnless(os.name == 'nt', 'Windows console attachment')
    def test_help_in_a_direct_terminal_invocation(self):
        # Give the probe its own hidden console. The actual launcher inherits no redirected pipes.
        info = subprocess.STARTUPINFO()
        info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        result = subprocess.run([sys.executable, __file__, LAUNCHER, '--probe-console'], cwd=self.cwd,
                                creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=info, timeout=15)
        self.assertEqual(result.returncode, 0)

    def test_invalid_batch_has_no_side_effects(self):
        for bad in ['VSync=off', 'FpsLimit=42', 'FpsLimit=60.0', 'MenuLanguage=xx',
                    'SettingsVersion=1', 'NoSuchKey=10', 'Bindings={"not-an-action":"C"}',
                    'Bindings={"FIRE":"Mouse1,NotAKey"}', 'Bindings={"FIRE":false}']:
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


def probe_windows_console():
    import ctypes
    from ctypes import wintypes
    result = subprocess.run([LAUNCHER, '--help'], timeout=10)
    if result.returncode:
        return 1
    kernel = ctypes.windll.kernel32
    kernel.GetStdHandle.restype = wintypes.HANDLE
    screen = ctypes.create_unicode_buffer(4096)
    count = wintypes.DWORD()
    ok = kernel.ReadConsoleOutputCharacterW(wintypes.HANDLE(kernel.GetStdHandle(-11)), screen, len(screen),
                                            0, ctypes.byref(count))
    return 0 if ok and '--project-dir' in screen[:count.value] else 1


if __name__ == '__main__':
    if sys.argv[1:] == ['--probe-console']:
        sys.exit(probe_windows_console())
    unittest.main()
