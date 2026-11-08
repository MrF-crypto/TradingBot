#!/usr/bin/env python3
"""Offline package launch with development DLL/plugin search paths removed."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
version = sys.argv[2]
env = {k: v for k, v in os.environ.items() if not k.startswith(('QT_', 'QML'))}
env['PATH'] = str(Path(env['WINDIR']) / 'System32') + ';' + env['WINDIR']
if list(root.rglob('ccbot_headless*')):
    raise RuntimeError('Windows desktop package must not include headless')
for name, flag in [('TradingBot.exe', '--version'), ('TradingBot.exe', '--check-runtime')]:
    result = subprocess.run([str(root / name), flag], cwd=root, env=env,
                            capture_output=True, text=True, timeout=30, check=True)
    if version not in result.stdout:
        raise RuntimeError(f'{name} {flag}: version missing from output: {result.stdout!r}')
    print(result.stdout.strip())
if (root / 'data').exists() and any((root / 'data').iterdir()):
    raise RuntimeError('Offline package check unexpectedly created runtime data')
