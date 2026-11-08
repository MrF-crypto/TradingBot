#!/usr/bin/env python3
"""Keep the transitive DLL imports of the GUI and its Windows platform plugin."""
import argparse
import os
from pathlib import Path
import re
import subprocess


def required_dlls(root: Path, imports, system_dll):
    dlls = {}
    for path in root.rglob('*.dll'):
        name = path.name.lower()
        if name in dlls or path.is_symlink():
            raise ValueError(f'Duplicate or linked DLL: {name}')
        dlls[name] = path
    queue = [root / 'TradingBot.exe', root / 'platforms/qwindows.dll']
    needed = {'qwindows.dll'}
    while queue:
        executable = queue.pop()
        if not executable.is_file():
            raise ValueError(f'Missing runtime: {executable.name}')
        for name in imports(executable):
            name = name.lower()
            if name in dlls:
                if name not in needed:
                    needed.add(name)
                    queue.append(dlls[name])
            elif not system_dll(name):
                raise ValueError(f'Unresolved DLL import: {executable.name} -> {name}')
    return needed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('package', type=Path)
    parser.add_argument('--dumpbin', type=Path, required=True)
    args = parser.parse_args()
    root = args.package.resolve()
    def imports(path):
        output = subprocess.check_output([str(args.dumpbin), '/nologo', '/dependents', str(path)],
                                         text=True, errors='replace', timeout=30)
        return re.findall(r'^\s*([\w.+-]+\.dll)\s*$', output, re.MULTILINE | re.IGNORECASE)
    system = Path(os.environ['WINDIR']) / 'System32'
    needed = required_dlls(root, imports, lambda name: name.startswith(('api-ms-', 'ext-ms-')) or (system / name).is_file())
    for path in root.glob('*.dll'):
        if path.name.lower() not in needed:
            if path.resolve().parent != root or path.is_symlink():
                raise ValueError('Refusing to remove a DLL outside the package root')
            path.unlink()
            print(f'Removed unused runtime: {path.name}')
    print(f'Kept {len(needed)} required application/plugin DLLs')


if __name__ == '__main__':
    main()
