import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent
commit = 'f63beccfb1cf6ecbd74f0a5ed1a6cdcb90cba854'
path = root / 'assets-final/BUILD_VERIFICATION.json'
manifest = json.loads(path.read_text(encoding='utf-8'))
assert manifest['version'] == 'v1.3.4' and manifest['source_commit'] == commit
assert len(manifest['assets']) == 5
runs = json.loads((root / 'runs.json').read_text())
evidence = []
for label, config in runs.items():
    run = json.loads(subprocess.check_output([
        'gh', 'run', 'view', str(config['id']), '--repo', 'MrF-crypto/TradingBot',
        '--json', 'headSha,status,conclusion,url,jobs'], text=True, encoding='utf-8', timeout=50))
    assert run['headSha'] == commit and run['status'] == 'completed' and run['conclusion'] == 'success', label
    assert len(run['jobs']) == config['jobs'] and all(j['conclusion'] == 'success' for j in run['jobs']), label
    for job in run['jobs']:
        steps = {s['name']: s['conclusion'] for s in job['steps']}
        required = (['Run all test suites', 'Test legacy symbol recovery', 'Package', 'Leak check (hard gate)', 'Record source and dependencies', 'Upload artifact']
                    if label != 'ASan + UBSan / TSan' else ['Suite coverage (hard gate)', 'Run suites under ' + job['name']])
        assert all(steps.get(name) == 'success' for name in required), (label, job['name'], steps)
    evidence.append({'platform': label, 'url': run['url'], 'conclusion': 'success',
                     'jobs': [{'name': j['name'], 'url': j['url'], 'conclusion': j['conclusion']} for j in run['jobs']]})
package_runs = {e['url'] for e in evidence[:3]}
assert all(a['workflow_url'] in package_runs for a in manifest['assets'])
manifest['ci'] = evidence
path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
body_path = root / 'release-body.md'
body = body_path.read_text(encoding='utf-8')
pending = '发布前检查结果及 CI 链接会在发布时补充。'
assert pending in body
summary = ('发布源码：[`' + commit[:7] + '`](https://github.com/MrF-crypto/TradingBot/commit/' + commit + ')。'
           '以下原生 CI 与 sanitizer 检查均在该提交通过：\n\n' +
           '\n'.join('- [' + e['platform'] + '](' + e['url'] + ')：通过。' for e in evidence) +
           '\n\n桌面平台各 18 套测试、Linux 各 17 套；ASan/UBSan 覆盖 17 套、TSan 覆盖 7 套并发测试。'
           '五个附件均通过源码提交号、内嵌版本、CPU 架构、依赖清单、许可证和运行数据排除检查。')
body_path.write_text(body.replace(pending, summary), encoding='utf-8')
print('Verified release evidence for', commit)
