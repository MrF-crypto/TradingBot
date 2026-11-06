"""Publish only the five verified artifacts from the fixed v1.3.4 source commit."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
REPO = 'MrF-crypto/TradingBot'
COMMIT = 'f63beccfb1cf6ecbd74f0a5ed1a6cdcb90cba854'
TAG = 'v1.3.4'
RUNS = json.loads((ROOT / 'runs.json').read_text(encoding='utf-8'))

def run(*command):
    return subprocess.check_output(command, text=True, encoding='utf-8', timeout=180)

def api(path, *arguments):
    return json.loads(run('gh', 'api', f'repos/{REPO}/{path}', *arguments))

# No tag/release mutations until CI, downloaded contents and provenance pass.
for label, config in RUNS.items():
    record = api(f'actions/runs/{config["id"]}')
    assert record['head_sha'] == COMMIT and record['conclusion'] == 'success'
    assert record['status'] == 'completed', (label, record['status'])

assets = ROOT / 'assets-final'
assets.mkdir(exist_ok=True)
downloads = [
    (42217221730, 'tradingbot-win64'),
    (42217221696, 'tradingbot-macos-x64'),
    (42217221696, 'tradingbot-macos-arm64'),
    (42217221709, 'tradingbot-linux-x64'),
    (42217221709, 'tradingbot-linux-arm64'),
]

def download(item):
    number, name = item
    run('gh', 'run', 'download', str(number), '--repo', REPO,
        '--name', name, '--dir', str(assets))
    print(f'Downloaded {name}', flush=True)

with ThreadPoolExecutor(max_workers=3) as pool:
    list(pool.map(download, downloads))
print(run(sys.executable, str(ROOT / 'verify_archives.py'), COMMIT), flush=True)
print(run(sys.executable, str(ROOT / 'finalize_evidence.py')), flush=True)

assert not any(item['tag_name'] == TAG for item in api('releases?per_page=100')), 'Release already exists'
tag = api('git/refs', '--method', 'POST', '-f', 'ref=refs/tags/' + TAG, '-f', 'sha=' + COMMIT)
assert tag['object']['sha'] == COMMIT
files = sorted(str(path) for path in assets.iterdir() if path.is_file())
assert len(files) == 7
print(run('gh', 'release', 'create', TAG, *files, '--repo', REPO, '--verify-tag',
          '--target', COMMIT, '--draft', '--title', 'v1.3.4 — 中文合约行情与 macOS 数据目录修复',
          '--notes-file', str(ROOT / 'release-body.md')), flush=True)
print(run(sys.executable, str(ROOT / 'verify_uploaded_assets.py')), flush=True)
run('gh', 'release', 'edit', TAG, '--repo', REPO, '--draft=false', '--latest')
print(run(sys.executable, str(ROOT / 'verify_uploaded_assets.py')), flush=True)
release = api('releases/tags/' + TAG)
assert not release['draft'] and not release['prerelease'] and release['published_at']
print('Published: ' + release['html_url'], flush=True)
