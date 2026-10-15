#!/usr/bin/env python3
"""Compile both host launchers with native Linux Mono and prepare local locales."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

ENGLISH_SHA256 = 'B23EEB4AD86A72894B40E7239D0EA8842E5A6BC05936A936401B608070E8C7BB'


def prepare_languages(root):
    scripts = root / 'scripts'
    subprocess.run([sys.executable, str(scripts / 'localization/build_launcher_catalog.py'),
                    str(root / 'localization/launcher/en-pt-BR.jsonl'),
                    str(root / 'localization/launcher'), str(scripts / 'localization/launcher-locales.json')], check=True)
    english = root / 'local/language-banks/en-US/MainUS.bin'
    installed = root / 'game/language/strings/MainUS.bin'
    if not english.is_file() and installed.is_file():
        data = installed.read_bytes()
        if hashlib.sha256(data).hexdigest().upper() != ENGLISH_SHA256:
            raise RuntimeError('Restore the original English MainUS.bin under local/language-banks/en-US before building locales.')
        english.parent.mkdir(parents=True, exist_ok=True)
        temporary = english.with_name('MainUS.bin.launcher-tmp-' + uuid.uuid4().hex)
        try:
            temporary.write_bytes(data)
            # A hard link atomically publishes a complete bank without replacing
            # one another launcher/build may have preserved in the meantime.
            try:
                os.link(temporary, english)
            except FileExistsError:
                pass
        finally:
            temporary.unlink(missing_ok=True)
    if not english.is_file():
        print('No retail English bank supplied; skipping local game-language banks.')
        return
    for locale in ('pt-BR', 'es', 'fr', 'de', 'it'):
        output = root / f'local/language-banks/{locale}/MainUS.bin'
        if locale == 'pt-BR':
            builder = scripts / 'localization/build_pt_br_bank.py'
            source = root / 'translations/pt-BR/MainUS.json'
        else:
            builder = scripts / 'localization/build_mainus.py'
            source = root / f'localization/game/{locale}.jsonl'
        subprocess.run([sys.executable, str(builder), str(english), str(source), str(output)], check=True)


def build(root, output, prepare=True):
    compiler = shutil.which('mcs')
    if not compiler:
        raise RuntimeError('Install Mono development tools (mono-devel) to build both launchers on Linux.')
    if prepare:
        prepare_languages(root)
    scripts = root / 'scripts'
    catalog = scripts / 'localization/launcher-locales.json'
    sources = [scripts / name for name in ('game-language.cs', 'launcher-language.cs', 'launcher.cs', 'save-profile-tab.cs', 'profile_editor.cs')]
    output.mkdir(parents=True, exist_ok=True)
    for name, entry in [('BLACK PC Launcher.exe', 'BlackXboxLauncher.Program'), ('BLACK PC Settings.exe', 'BlackXboxLauncher.SettingsProgram')]:
        subprocess.run([compiler, '-nologo', '-target:winexe', '-platform:x64', '-optimize+',
                        '-sdk:4', '-main:' + entry, '-r:System.Windows.Forms', '-r:System.Drawing',
                        '-r:System.Web.Extensions', f'-resource:{catalog},BlackXboxLauncher.launcher-locales.json',
                        f'-out:{output / name}', *map(str, sources)], check=True)
        print(f'Built {output / name}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--skip-locales', action='store_true')
    args = parser.parse_args()
    build(args.root.absolute(), (args.output or args.root).absolute(), not args.skip_locales)
