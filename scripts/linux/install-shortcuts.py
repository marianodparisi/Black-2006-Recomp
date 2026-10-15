#!/usr/bin/env python3
"""Create local desktop launchers bound to this extracted folder."""
from pathlib import Path
import sys


def desktop_quote(value):
    # Desktop Entry Exec quoting is not shell quoting. Double % prevents field
    # code expansion in paths; escape reserved characters inside double quotes.
    value = str(value).replace("%", "%%")
    for char in ('\\', '"', '`', '$'):
        value = value.replace(char, '\\' + char)
    return '"' + value.replace('\\', '\\\\') + '"'


def main():
    root = Path(__file__).resolve().parents[2]
    if any(char in str(root) for char in '\n\r'):
        sys.exit('Desktop shortcuts cannot be created in a path containing a newline.')
    for name, script in [('Play BLACK', 'Play BLACK.sh'), ('BLACK Settings', 'BLACK Settings.sh')]:
        path = root / (name + '.desktop')
        path.write_text('[Desktop Entry]\nType=Application\nVersion=1.0\n'
                        f'Name={name}\nExec=bash {desktop_quote(root / script)}\n'
                        'Terminal=false\nCategories=Game;\n', encoding='utf-8')
        path.chmod(0o755)
        print(path)
    print('Open these shortcuts in your file manager. Allow launching if prompted.\n'
          'Run this script again after moving the folder.')


if __name__ == '__main__':
    main()
