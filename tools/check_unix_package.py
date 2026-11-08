#!/usr/bin/env python3
"""Native CI gate: architecture, runtime dependencies and offline launch.

Checks a newly built package only. Never opens account configuration or starts trading.
Linux artifacts target Ubuntu 24.04 and compatible glibc systems, not musl/Alpine.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess


def run(*args, env=None):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT,
                                   timeout=60, env=env).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('package', type=Path)
    ap.add_argument('--platform', choices=('macos', 'linux'), required=True)
    ap.add_argument('--arch', choices=('x64', 'arm64'), required=True)
    args = ap.parse_args()
    root = args.package.resolve()
    executables = [root / ('TradingBot.app/Contents/MacOS/TradingBot' if args.platform == 'macos' else 'ccbot_headless')]
    if args.platform == 'macos' and list(root.rglob('ccbot_headless*')):
        raise RuntimeError('macOS desktop package must not include headless')

    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('QT_', 'QML', 'DYLD_', 'LD_LIBRARY_PATH', 'LD_PRELOAD'))}
    env['PATH'] = '/usr/bin:/bin:/usr/sbin:/sbin'
    env['QT_QPA_PLATFORM'] = 'cocoa' if args.platform == 'macos' else 'offscreen'
    for exe in executables:
        if not exe.is_file():
            raise RuntimeError(f'Missing executable: {exe}')
        kind = run('file', str(exe))
        expected = {'x64': ('x86_64', 'x86-64'), 'arm64': ('arm64', 'aarch64')}[args.arch]
        if not any(arch in kind for arch in expected):
            raise RuntimeError(f'Wrong architecture: {kind}')
        if args.platform == 'macos':
            # Default macOS vcpkg triplets use static libraries. If a shared Qt build
            # is used, reject references to a developer's absolute library paths.
            for dep in run('otool', '-L', str(exe)).splitlines()[1:]:
                name = dep.strip().split(' (', 1)[0]
                if not name.startswith(('/usr/lib/', '/System/Library/', '@')):
                    raise RuntimeError(f'Nonportable dependency: {name}')
            if any(p.startswith(('/opt/', '/Users/', '/usr/local/')) for p in
                   re.findall(r'path (.+?) \(offset', run('otool', '-l', str(exe)))):
                raise RuntimeError(f'Developer RPATH in {exe}')
        else:
            deps = run('ldd', str(exe))
            if 'not found' in deps:
                raise RuntimeError(deps)
            # Dependencies built by vcpkg must be static. Only standard glibc/C++
            # runtimes may remain; a library merely installed on the runner is not enough.
            allowed = re.compile(r'^(?:linux-vdso|ld-linux|lib(?:c|m|dl|pthread|rt|resolv|stdc\+\+|gcc_s))[-.]')
            for line in deps.splitlines():
                name = Path(line.strip().split()[0]).name
                if not allowed.match(name):
                    raise RuntimeError(f'Unbundled Linux dependency: {line}')
        print(kind)
        print(run(str(exe), '--version', env=env))
    if args.platform == 'macos':
        print(run(str(executables[0]), '--check-runtime', env=env))
        data_path = Path(run(str(executables[0]), '--print-data-directory', env=env))
        if data_path != root / 'data':
            raise RuntimeError(f'Mac data directory must be beside the app: {data_path}')
        if (root / 'data').exists() and any((root / 'data').iterdir()):
            raise RuntimeError('Offline package checks unexpectedly created runtime data')
        print(f'Portable data path verified: {data_path}')
    print('Native package checks passed (no account access).')


if __name__ == '__main__':
    main()
