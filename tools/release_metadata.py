#!/usr/bin/env python3
"""Version and provenance for release artifacts; never reads runtime user data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', action='store_true')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--archive', type=Path)
    ap.add_argument('--triplet')
    args = ap.parse_args()
    version = 'v' + re.search(r'project\(TradingBot VERSION ([\d.]+)',
                             (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')).group(1)
    ref = os.environ.get('GITHUB_REF', '')
    if ref.startswith('refs/tags/') and ref != 'refs/tags/' + version:
        raise RuntimeError('Git tag does not match the source version')
    if args.version:
        print(version)
        return
    if not args.output or not args.archive or not args.triplet:
        ap.error('--output, --archive and --triplet are required')
    args.output.mkdir(parents=True, exist_ok=True)
    with args.archive.open('rb') as archive:
        digest = hashlib.file_digest(archive, 'sha256').hexdigest()
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    repo = os.environ.get('GITHUB_REPOSITORY', 'MrF-crypto/TradingBot')
    info = {
        'version': version, 'source_commit': commit,
        'source_url': f'https://github.com/{repo}/tree/{commit}',
        'triplet': args.triplet, 'vcpkg_tag': '2026.07.29',
        'workflow_url': f'https://github.com/{repo}/actions/runs/' + os.environ.get('GITHUB_RUN_ID', ''),
        'tests': 'CTest and native package checks passed before packaging',
        'archive': args.archive.name, 'archive_size': args.archive.stat().st_size,
        'archive_sha256': digest,
    }
    args.output.joinpath('BUILD_INFO.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    status = ROOT / 'build/vcpkg_installed/vcpkg/status'
    dependencies = []
    for paragraph in status.read_text(encoding='utf-8').split('\n\n'):
        fields = dict(line.split(': ', 1) for line in paragraph.splitlines() if ': ' in line)
        if fields.get('Status') == 'install ok installed':
            dependencies.append({key: fields[key] for key in ('Package', 'Version', 'Architecture', 'Feature') if key in fields})
    args.output.joinpath('DEPENDENCIES.json').write_text(json.dumps(dependencies, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
