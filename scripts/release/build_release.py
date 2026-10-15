#!/usr/bin/env python3
"""Build a public starter release from tracked host sources and two launchers.

Never traverses the local game/build/report trees. The game executable, assets,
generated code, profiles and language banks are deliberately outside this pack.
"""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import tarfile
import zipfile

VERSION = re.compile(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9]+(?:\.[A-Za-z0-9]+)*)?")
ROOT_FILES = {'.gitattributes', '.gitignore', 'CMakeLists.txt', 'LICENSE', 'NOTICE',
              'README.md', 'CHANGELOG.md', 'Build BLACK.cmd', 'Play BLACK.sh',
              'BLACK Settings.sh', 'Install Linux Shortcuts.sh', 'Build BLACK.sh'}
SOURCE_DIRS = {'cmake', 'docs', 'LICENSES', 'localization', 'patches', 'scripts', 'src',
               'subtitles', 'tests', 'translations'}
PRIVATE_DIRS = {'game', 'local', 'save', 'reports', 'third_party', '.work', '.venv',
                '.wine', '.proton', 'releases', 'recomp'}
BINARY_SUFFIXES = {'.exe', '.dll', '.pdb', '.bin', '.xbe', '.iso', '.xiso', '.zip',
                   '.7z', '.wav', '.xmv', '.ssh', '.bks', '.obj', '.lib', '.a'}
LAUNCHERS = ('BLACK PC Launcher.exe', 'BLACK PC Settings.exe')
SECRET = re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,}|'
                    rb'AKIA[A-Z0-9]{16}|sk-(?:proj-)?[A-Za-z0-9_-]{40,}|'
                    rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----)')


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe_file(path, root):
    """Reject every symlink/reparse ancestor without following it."""
    path = Path(os.path.abspath(path))
    root = Path(os.path.abspath(root))
    path.relative_to(root)
    for item in [path, *path.parents]:
        metadata = item.lstat()
        if stat.S_ISLNK(metadata.st_mode) or getattr(metadata, 'st_file_attributes', 0) & 0x400:
            raise ValueError(f'Refusing linked/reparse path: {item}')
        if item == root:
            break
    if not path.is_file():
        raise ValueError(f'Not a regular file: {path}')
    return path


def collect_sources(root):
    entries = {}
    for raw in git(root, 'ls-files', '-z').split(b'\0'):
        if not raw:
            continue
        name = raw.decode('utf-8')
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or '\\' in name:
            raise ValueError(f'Unsafe tracked path: {name}')
        if path.parts[0] not in SOURCE_DIRS and name not in ROOT_FILES:
            continue
        if path.parts[0] in PRIVATE_DIRS or path.parts[:2] == ('src', 'recomp') or any(part.startswith('build-') for part in path.parts[:-1]):
            raise ValueError(f'Private path is tracked: {name}')
        if path.suffix.lower() in BINARY_SUFFIXES or path.name.startswith('.env') or path.name == 'black_xmv_idct.inc':
            raise ValueError(f'Private/binary file is tracked: {name}')
        data = safe_file(root / name, root).read_bytes()
        if SECRET.search(data):
            raise ValueError(f'Possible secret in {name}; refusing release')
        # Shell entry points must execute after extraction on a Linux filesystem.
        if path.suffix == '.sh' and b'\r' in data:
            raise ValueError(f'Shell script needs LF line endings: {name}')
        entries[name] = data
    missing = ROOT_FILES - entries.keys()
    if missing:
        raise ValueError(f'Required files are not tracked: {sorted(missing)}')
    return entries


def build(root, launchers, output, version, allow_dirty=False):
    if not VERSION.fullmatch(version):
        raise ValueError('Use a semantic version such as 0.1.0-alpha.1')
    commit = git(root, 'rev-parse', 'HEAD').decode().strip()
    dirty = bool(git(root, 'status', '--porcelain', '--untracked-files=normal').strip())
    if dirty and not allow_dirty:
        raise ValueError('Commit the release sources first, or use --allow-dirty for local validation only')
    entries = collect_sources(root)
    for name in LAUNCHERS:
        data = safe_file(launchers / name, launchers).read_bytes()
        if not data.startswith(b'MZ'):
            raise ValueError(f'Not a compiled Windows launcher: {name}')
        entries[name] = data
    marker = {'version': version, 'kind': 'public-starter', 'source_commit': commit,
              'source_dirty': dirty, 'platforms': ['windows-x86_64', 'linux-x86_64-wine'],
              'retail_xbe_sha256': 'DF2739C372D254A90AEECCF5971D12097F22D89FBB90DB021FDA96DD4DEB68F6',
              'game_executable_included': False, 'game_files_included': False,
              'files': {name: digest(data) for name, data in sorted(entries.items())}}
    entries['black-release.json'] = (json.dumps(marker, indent=2, sort_keys=True) + '\n').encode()
    entries['SHA256SUMS'] = ''.join(f'{digest(data)}  {name}\n' for name, data in sorted(entries.items())).encode()
    prefix = f'BLACK-{version}'
    output.mkdir(parents=True, exist_ok=True)
    zip_path = output / f'{prefix}-starter.zip'
    tar_path = output / f'{prefix}-starter.tar.gz'
    checksum_path = output / f'{prefix}-SHA256SUMS.txt'
    for path in (zip_path, tar_path, checksum_path):
        if path.exists():
            raise ValueError(f'Release output already exists; preserve it and use another directory: {path}')
    with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(entries.items()):
            item = zipfile.ZipInfo(f'{prefix}/{name}', (1980, 1, 1, 0, 0, 0))
            item.create_system = 3
            item.external_attr = (0o100755 if name.endswith('.sh') else 0o100644) << 16
            item.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(item, data)
    with tar_path.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', filename='', mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as archive:
            for name, data in sorted(entries.items()):
                item = tarfile.TarInfo(f'{prefix}/{name}')
                item.size = len(data)
                item.mode = 0o755 if name.endswith('.sh') else 0o644
                item.mtime = 0
                archive.addfile(item, io.BytesIO(data))
    checksum_path.write_text(''.join(f'{digest(path.read_bytes())}  {path.name}\n'
                                    for path in (zip_path, tar_path)), encoding='utf-8', newline='\n')
    print(f'Public starter {version}: {len(entries)} files; no game executable or retail assets')
    print(zip_path)
    print(tar_path)
    print(checksum_path)
    return zip_path, tar_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[2]
    parser.add_argument('--version', required=True)
    parser.add_argument('--launchers', type=Path, default=root / 'build-release-launchers')
    parser.add_argument('--output', type=Path, default=root / 'releases')
    parser.add_argument('--allow-dirty', action='store_true', help='Local validation only; manifest records this')
    args = parser.parse_args()
    build(root, args.launchers.absolute(), args.output.absolute(), args.version, args.allow_dirty)


if __name__ == '__main__':
    main()
