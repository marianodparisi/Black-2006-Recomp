#!/usr/bin/env python3
"""Record Linux compiler, input and output bindings beside the private build."""
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_revision(root):
    if (root / '.git').exists():
        return subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    marker = root / 'black-release.json'
    if marker.is_file():
        revision = json.loads(marker.read_text())['source_commit']
        if not isinstance(revision, str) or not re.fullmatch(r'[0-9a-fA-F]{40}', revision):
            raise RuntimeError('Release manifest has an invalid source revision.')
        return revision
    # Unversioned source downloads can still build; do not invent a revision.
    return None


def main():
    root = Path(__file__).resolve().parents[2]
    build = root / 'build-linux-wine'
    sources = []
    for directory in ('src', 'cmake', 'scripts', 'patches', 'third_party/xboxrecomp/src', 'third_party/xboxrecomp/templates'):
        for path in sorted((root / directory).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                sources.append(path)
    tools = {}
    for program in ('clang-cl', 'lld-link', 'llvm-lib', 'mcs', 'mono', 'rustc', 'cargo', 'xwin', 'cmake', 'ninja'):
        resolved = shutil.which(program)
        if not resolved:
            raise RuntimeError(f'Missing build tool {program}')
        if program in ('rustc', 'cargo'):
            resolved = subprocess.check_output(['rustup', 'which', '--toolchain', '1.98.1', program], text=True).strip()
        resolved = Path(resolved).resolve()
        if resolved.read_bytes()[:4] != b'\x7fELF' and program != 'mcs':
            raise RuntimeError(f'Build tool is not Linux ELF: {resolved}')
        tools[program] = {'path': str(resolved), 'sha256': digest(resolved)}
        if program in ('clang-cl', 'lld-link', 'mcs', 'mono', 'rustc', 'cargo', 'xwin', 'cmake', 'ninja'):
            # LLVM selects clang-cl/lld-link behavior from argv[0], even though
            # their aliases resolve to the same underlying ELF driver.
            tools[program]['version'] = subprocess.check_output([shutil.which(program), '--version'], text=True).splitlines()[0]
    compiler_info = next((build / 'CMakeFiles').glob('*/CMakeCCompiler.cmake')).read_text()
    compiler = re.search(r'set\(CMAKE_C_COMPILER "([^"]+)"\)', compiler_info).group(1)
    if Path(compiler).resolve() != Path(tools['clang-cl']['path']):
        raise RuntimeError('The recorded CMake compiler differs from native Linux clang-cl.')
    marker = root / 'src/recomp/gen/post_transform_audit.json'
    data = {'host_system': platform.system(), 'host_machine': platform.machine(),
            'target': 'x86_64-pc-windows-msvc', 'windows_installation_used': False,
            'source_commit': source_revision(root),
            'retail_xbe_sha256': digest(root / 'game/default.xbe'), 'tools': tools,
            'sdk_versions': json.loads((root / '.work/linux-toolchain/sdk-versions.json').read_text()),
            'generated_audit_sha256': digest(marker),
            'inputs': {str(path.relative_to(root)): digest(path) for path in sources},
            'outputs': {str(path.relative_to(root)): digest(path) for path in [root / 'bin/black_xbox_recomp.exe',
                        root / 'BLACK PC Launcher.exe', root / 'BLACK PC Settings.exe']}}
    (build / 'linux-build-provenance.json').write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    print('Linux build provenance: ' + str(build / 'linux-build-provenance.json'))


if __name__ == '__main__':
    main()
