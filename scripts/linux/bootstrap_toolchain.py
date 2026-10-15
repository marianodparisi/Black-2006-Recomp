#!/usr/bin/env python3
"""Fetch native Linux Rust/xwin and a pinned Windows SDK; no Windows tools run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tarfile
import urllib.request

RUST = '1.98.1'
XWIN = '0.10.0'
XWIN_SHA256 = 'd870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88'
SDK = '10.0.26100'
CRT = '14.44.17.14'


def download(url, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.is_file():
        print(f'Downloading {url}', flush=True)
        temporary = destination.with_suffix(destination.suffix + '.download')
        with urllib.request.urlopen(url, timeout=120) as source, temporary.open('wb') as target:
            while chunk := source.read(1024 * 1024):
                target.write(chunk)
        temporary.replace(destination)
    return destination


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bootstrap(root):
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        raise RuntimeError('The Linux build currently requires an x86_64 Linux host.')
    tool_root = root / '.work/linux-toolchain'
    downloads = tool_root / 'downloads'
    binaries = tool_root / 'bin'
    binaries.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, CARGO_HOME=str(tool_root / 'cargo'), RUSTUP_HOME=str(tool_root / 'rustup'))
    rustup = tool_root / 'cargo/bin/rustup'
    if not rustup.is_file():
        rust_url = 'https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init'
        installer = download(rust_url, downloads / 'rustup-init')
        checksum = download(rust_url + '.sha256', downloads / 'rustup-init.sha256').read_text().split()[0]
        if sha256(installer) != checksum or installer.read_bytes()[:4] != b'\x7fELF':
            raise RuntimeError('Rustup installer checksum or Linux ELF format mismatch; preserve the downloads and retry in a fresh tool directory.')
        installer.chmod(0o755)
        subprocess.run([str(installer), '-y', '--no-modify-path', '--profile', 'minimal',
                        '--default-toolchain', RUST, '--target', 'x86_64-pc-windows-msvc'], env=environment, check=True)
    subprocess.run([str(rustup), 'toolchain', 'install', RUST, '--profile', 'minimal',
                    '--target', 'x86_64-pc-windows-msvc'], env=environment, check=True)
    archive = download(f'https://github.com/Jake-Shadle/xwin/releases/download/{XWIN}/xwin-{XWIN}-x86_64-unknown-linux-musl.tar.gz', downloads / 'xwin.tar.gz')
    if sha256(archive) != XWIN_SHA256:
        raise RuntimeError('xwin archive checksum mismatch; preserve it and retry in a fresh tool directory.')
    xwin = binaries / 'xwin'
    with tarfile.open(archive) as source:
        member = source.getmember(f'xwin-{XWIN}-x86_64-unknown-linux-musl/xwin')
        if not member.isfile():
            raise RuntimeError('xwin archive member is not a regular file.')
        data = source.extractfile(member).read()
        if data[:4] != b'\x7fELF':
            raise RuntimeError('xwin is not a native Linux executable.')
        xwin.write_bytes(data)
    xwin.chmod(0o755)
    sdk_root = tool_root / 'xwin'
    stamp = tool_root / 'sdk-versions.json'
    expected = {'xwin': XWIN, 'sdk': SDK, 'crt': CRT}
    if stamp.is_file() and json.loads(stamp.read_text()) != expected:
        raise RuntimeError('Existing SDK version stamp differs; preserve .work/linux-toolchain separately before rebuilding.')
    if not stamp.is_file() or not (sdk_root / 'sdk/lib/um/x86_64/kernel32.lib').is_file():
        subprocess.run([str(xwin), '--accept-license', '--arch', 'x86_64', '--sdk-version', SDK,
                        '--crt-version', CRT, '--cache-dir', str(tool_root / 'xwin-cache'),
                        'splat', '--output', str(sdk_root)], check=True)
        stamp.write_text(json.dumps(expected, indent=2) + '\n', encoding='utf-8')
    print(f'Linux toolchain ready: Rust {RUST}, xwin {XWIN}, SDK {SDK}, CRT {CRT}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    bootstrap(parser.parse_args().root.absolute())
