import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from check_package_layout import validate
from trim_windows_runtime import required_dlls


class PackagePolicyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def put(self, name, content='fixture'):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8')

    def desktop(self, platform):
        (self.root / 'data').mkdir()
        if platform == 'windows':
            for name in ('TradingBot.exe', 'Qt6Core.dll', 'platforms/qwindows.dll', 'licenses/TradingBot/LICENSE'):
                self.put(name)
        else:
            for name in ('MacOS/TradingBot', 'Info.plist', 'Resources/licenses/TradingBot/LICENSE'):
                self.put('TradingBot.app/Contents/' + name)

    def test_desktop_minimal_payloads(self):
        self.desktop('windows')
        validate(self.root, 'windows')

    def test_macos_bundle_contains_licenses(self):
        self.desktop('macos')
        validate(self.root, 'macos')
        self.put('LICENSE')
        with self.assertRaises(ValueError):
            validate(self.root, 'macos')

    def test_windows_rejects_headless_and_unrelated_payload(self):
        self.desktop('windows')
        for name in ('ccbot_headless.exe', 'docs/HEADLESS.md', 'tools/helper.py', 'config.example.json', 'BUILD_INFO.json'):
            with self.subTest(name=name):
                self.put(name)
                with self.assertRaises(ValueError):
                    validate(self.root, 'windows')
                (self.root / name).unlink()
                if '/' in name:
                    (self.root / name).parent.rmdir()

    def test_macos_rejects_nested_headless(self):
        self.desktop('macos')
        self.put('TradingBot.app/Contents/MacOS/ccbot_headless')
        with self.assertRaises(ValueError):
            validate(self.root, 'macos')

    def test_desktop_rejects_any_data_including_hidden_files(self):
        self.desktop('windows')
        self.put('data/.tradingbot-profile.json')
        with self.assertRaises(ValueError):
            validate(self.root, 'windows')

    def test_linux_headless_template_only(self):
        for name in ('ccbot_headless', 'README.md', 'licenses/TradingBot/LICENSE'):
            self.put(name)
        template = {'api_key': '你的Binance API Key', 'api_secret': '你的Binance API Secret'}
        self.put('config.example.json', json.dumps(template))
        validate(self.root, 'linux')
        self.put('TradingBot.exe')
        with self.assertRaises(ValueError):
            validate(self.root, 'linux')
        (self.root / 'TradingBot.exe').unlink()
        self.put('config.example.json', json.dumps({'api_key': 'non-template', 'api_secret': ''}))
        with self.assertRaises(ValueError):
            validate(self.root, 'linux')

    def test_dll_closure_keeps_plugin_transitive_and_delay_imports(self):
        for name in ('TradingBot.exe', 'platforms/qwindows.dll', 'core.dll', 'nested.dll', 'delayed.dll', 'unused.dll'):
            self.put(name)
        imports = {'TradingBot.exe': ['core.dll', 'kernel32.dll'], 'qwindows.dll': ['delayed.dll'],
                   'core.dll': ['nested.dll'], 'nested.dll': ['core.dll'], 'delayed.dll': []}
        needed = required_dlls(self.root, lambda p: imports[p.name], lambda n: n == 'kernel32.dll')
        self.assertEqual(needed, {'qwindows.dll', 'core.dll', 'nested.dll', 'delayed.dll'})

    def test_dll_closure_rejects_missing_non_system_import(self):
        self.put('TradingBot.exe')
        self.put('platforms/qwindows.dll')
        with self.assertRaises(ValueError):
            required_dlls(self.root, lambda p: ['missing.dll'], lambda n: False)


if __name__ == '__main__':
    unittest.main()
