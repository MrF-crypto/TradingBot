#!/usr/bin/env python3
"""Final payload gate, after licenses are added. Never accepts runtime user data."""
import argparse
import json
from pathlib import Path


def license_file(parts):
    return (len(parts) == 3 and parts[0] == 'licenses' and
            (parts[2] == 'copyright' or parts[1:] == ('TradingBot', 'LICENSE')))


def validate(root: Path, platform: str):
    root = root.resolve()
    if not root.is_dir():
        raise ValueError('Package directory missing')
    desktop = platform != 'linux'
    required = {
        'windows': ('TradingBot.exe', 'platforms/qwindows.dll', 'licenses/TradingBot/LICENSE'),
        'macos': ('TradingBot.app/Contents/MacOS/TradingBot', 'TradingBot.app/Contents/Info.plist',
                  'TradingBot.app/Contents/Resources/licenses/TradingBot/LICENSE'),
        'linux': ('ccbot_headless', 'config.example.json', 'README.md', 'licenses/TradingBot/LICENSE'),
    }[platform]
    for name in required:
        if not (root / name).is_file():
            raise ValueError(f'Missing required file: {name}')
    if desktop:
        data = root / 'data'
        if not data.is_dir() or data.is_symlink() or any(data.iterdir()):
            raise ValueError('Desktop package requires a completely empty data directory')
    for path in root.rglob('*'):
        rel = path.relative_to(root)
        parts = rel.parts
        if path.is_symlink() and not path.resolve().is_relative_to(root):
            raise ValueError(f'Escaping package symlink: {rel}')
        if desktop and any('ccbot_headless' in part.lower() for part in parts):
            raise ValueError(f'Headless entry in desktop package: {rel}')
        if platform == 'windows':
            allowed_dir = parts[0] in ('data', 'platforms', 'licenses')
            allowed = ((len(parts) == 1 and (parts[0] == 'TradingBot.exe' or path.suffix.lower() == '.dll')) or
                       parts == ('platforms', 'qwindows.dll') or license_file(parts))
        elif platform == 'macos':
            allowed_dir = parts[0] in ('data', 'TradingBot.app')
            license_parts = parts[3:] if parts[:3] == ('TradingBot.app', 'Contents', 'Resources') else ()
            allowed = (license_file(license_parts) or rel.as_posix() in {
                'TradingBot.app/Contents/MacOS/TradingBot', 'TradingBot.app/Contents/Info.plist',
                'TradingBot.app/Contents/PkgInfo', 'TradingBot.app/Contents/_CodeSignature/CodeResources',
                'TradingBot.app/Contents/Resources/qt.conf',
            } or (parts[0] == 'TradingBot.app' and path.suffix in ('.dylib', '.so')))
        else:
            allowed_dir = parts[0] == 'licenses'
            allowed = rel.as_posix() in ('ccbot_headless', 'config.example.json', 'README.md') or license_file(parts)
        if path.is_dir():
            if not allowed_dir:
                raise ValueError(f'Unexpected directory: {rel}')
        elif not path.is_file() or not allowed:
            raise ValueError(f'Unexpected file: {rel}')
    if platform == 'linux':
        config = json.loads((root / 'config.example.json').read_text(encoding='utf-8'))
        if config.get('api_key') != '你的Binance API Key' or config.get('api_secret') != '你的Binance API Secret':
            raise ValueError('Linux package contains a non-template credential')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('package', type=Path)
    parser.add_argument('--platform', choices=('windows', 'macos', 'linux'), required=True)
    args = parser.parse_args()
    validate(args.package, args.platform)
    print(f'{args.platform} final package layout passed')
