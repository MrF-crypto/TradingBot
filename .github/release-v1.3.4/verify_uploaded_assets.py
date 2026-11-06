import hashlib
import json
from pathlib import Path
import subprocess

repo = 'MrF-crypto/TradingBot'
root = Path(__file__).resolve().parent / 'assets-final'
manifest = json.loads((root / 'BUILD_VERIFICATION.json').read_text(encoding='utf-8'))

def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', 'repos/' + repo + '/' + path], text=True, encoding='utf-8'))

matches = [r for r in api('releases') if r['tag_name'] == 'v1.3.4']
assert len(matches) == 1
release = api('releases/' + str(matches[0]['id']))
assert release['tag_name'] == 'v1.3.4' and not release['prerelease']
assert '发布前检查结果及 CI 链接会在发布时补充' not in release['body']
ref = api('git/ref/tags/v1.3.4')['object']
if ref['type'] == 'tag':
    ref = api('git/tags/' + ref['sha'])['object']
assert ref['type'] == 'commit' and ref['sha'] == manifest['source_commit']
expected = {a['name'] for a in manifest['assets']} | {'SHA256SUMS.txt', 'BUILD_VERIFICATION.json'}
assert {a['name'] for a in release['assets']} == expected
for asset in release['assets']:
    file = root / asset['name']
    digest = 'sha256:' + hashlib.sha256(file.read_bytes()).hexdigest()
    assert asset['state'] == 'uploaded' and asset['size'] == file.stat().st_size, asset['name']
    assert asset['digest'] == digest, (asset['name'], asset.get('digest'), digest)
    print(asset['name'], 'remote SHA256 verified')
print(json.dumps({'url': release['html_url'], 'draft': release['draft'],
                  'published_at': release['published_at'], 'source_commit': ref['sha']}, indent=2))
