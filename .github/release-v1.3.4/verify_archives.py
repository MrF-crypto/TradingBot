import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import sys
import zipfile

root = Path(__file__).resolve().parent
commit = sys.argv[1]
partial = '--partial' in sys.argv[2:]
assets = root / 'assets-final'
expected = {
    'tradingbot-v1.3.4-win64.zip': ('x64-windows', ['TradingBot.exe', 'ccbot_headless.exe'], 'pe', 0x8664),
    'tradingbot-v1.3.4-linux-x64.zip': ('x64-linux', ['ccbot_headless'], 'elf', 62),
    'tradingbot-v1.3.4-linux-arm64.zip': ('arm64-linux', ['ccbot_headless'], 'elf', 183),
    'tradingbot-v1.3.4-macos-x64.zip': ('x64-osx', ['TradingBot.app/Contents/MacOS/TradingBot', 'ccbot_headless'], 'macho', 0x1000007),
    'tradingbot-v1.3.4-macos-arm64.zip': ('arm64-osx', ['TradingBot.app/Contents/MacOS/TradingBot', 'ccbot_headless'], 'macho', 0x100000c),
}
verified = []
for name, (triplet, executables, kind, machine) in expected.items():
    archive = assets / name
    if partial and not archive.exists():
        continue
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None, name
        names = z.namelist()
        assert len(names) == len(set(names)), (name, 'duplicate archive entries')
        for member in names:
            p = PurePosixPath(member.replace('\\', '/'))
            assert not p.is_absolute() and '..' not in p.parts, (name, member)
            assert not set(p.parts).intersection({'.git', 'data', '.env'}), (name, member)
            assert p.name not in {'config.json', 'sar_bots.json', 'ccg_creds.dat', 'ccbot_state.json', 'ccg_settings.json'}, (name, member)
        info = json.loads(z.read('BUILD_INFO.json'))
        assert info['source_commit'] == commit and info['version'] == 'v1.3.4', (name, info)
        assert info['triplet'] == triplet, (name, info)
        assert 'docs/releases/v1.3.4.md' in names and 'tools/repair_symbol_state.py' in names, name
        assert json.loads(z.read('DEPENDENCIES.json')), name
        assert any(n.startswith('licenses/') and not n.endswith('/') for n in names), name
        if kind == 'pe':
            assert {'vcruntime140.dll', 'msvcp140.dll'}.issubset({n.lower() for n in names}), name
        for exe in executables:
            if kind != 'pe':
                assert (z.getinfo(exe).external_attr >> 16) & 0o111, (name, exe, 'not executable')
            binary = z.read(exe)
            assert b'v1.3.4' in binary, (name, exe, 'version')
            if kind == 'pe':
                assert binary[:2] == b'MZ'
                offset = struct.unpack_from('<I', binary, 0x3c)[0]
                assert binary[offset:offset+4] == b'PE\0\0'
                actual = struct.unpack_from('<H', binary, offset + 4)[0]
            elif kind == 'elf':
                assert binary[:6] == b'\x7fELF\x02\x01'
                actual = struct.unpack_from('<H', binary, 18)[0]
            else:
                assert binary[:4] == b'\xcf\xfa\xed\xfe'
                actual = struct.unpack_from('<I', binary, 4)[0]
            assert actual == machine, (name, exe, actual, machine)
        print(name, 'verified', len(names), 'entries')
    verified.append({'name': name, 'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
                     'bytes': archive.stat().st_size, 'triplet': triplet,
                     'workflow_url': info['workflow_url']})
if partial:
    print('Partial check only; release manifest is not generated until all five packages exist.')
    sys.exit(0)
(assets / 'SHA256SUMS.txt').write_text(''.join(f"{a['sha256']}  {a['name']}\n" for a in verified), encoding='ascii')
(assets / 'BUILD_VERIFICATION.json').write_text(json.dumps({'version': 'v1.3.4', 'source_commit': commit,
    'checks': ['archive CRC', 'source provenance', 'binary version and architecture', 'dependency manifest', 'licenses', 'runtime-data exclusion'],
    'assets': verified}, indent=2) + '\n', encoding='utf-8')
