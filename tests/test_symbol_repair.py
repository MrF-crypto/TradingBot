import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('repair', Path(__file__).parents[1] / 'tools/repair_symbol_state.py')
repair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repair)


class SymbolRepairTest(unittest.TestCase):
    def setUp(self):
        self.names = ['币安人生USDT', '牛来USDT', '龙虾USDT']
        self.aliases = ['Ÿ�Ů�ĺ�ǔ�USDT', 'ǉ�Ɲ�USDT', 'ɾ�ș�USDT']
        self.catalog = {'symbols': [dict(symbol=s, contractType='PERPETUAL', quoteAsset='USDT') for s in self.names]}
        self.state = [dict(symbol=s, pos=0, qty=0, reentry_after_ms=1800000000000,
                           monitor_checkpoint={'cursor': 1799996400000, 'trend_bars': 9},
                           auto_config={'template': {'tf_enabled': True}}, realized_pnl=12.3) for s in self.aliases]

    def test_exact_observed_aliases_and_preserved_checkpoints(self):
        self.assertEqual([repair.legacy_name(s) for s in self.names], self.aliases)
        original = copy.deepcopy(self.state)
        result, changes = repair.repair_copy(self.state, self.catalog, [], [])
        self.assertEqual(len(changes), 3)
        self.assertEqual(self.state, original)
        for before, after, symbol in zip(self.state, result, self.names):
            expected = dict(before, symbol=symbol)
            self.assertEqual(after, expected)

    def test_exposure_never_auto_repaired(self):
        for field, value in [('pos', 2), ('qty', 1), ('open_uncertain', True),
                             ('reserved_risk', 10), ('disaster_stop_id', '123'),
                             ('uncertain_client_id', 'old-opening'), ('qty', float('nan'))]:
            state = copy.deepcopy(self.state)
            state[0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                repair.repair_copy(state, self.catalog, [], [])
        for positions, orders in [([{'symbol': self.names[0], 'positionAmt': '-1'}], []),
                                  ([], [{'symbol': self.names[0], 'algoId': '123'}]),
                                  ({'code': -1}, []), ([], {'code': -1})]:
            with self.assertRaises(ValueError):
                repair.repair_copy(self.state, self.catalog, positions, orders)

    def test_missing_and_duplicate_identity_refused(self):
        with self.assertRaises(ValueError):
            repair.repair_copy(self.state, {'symbols': self.catalog['symbols'][:1]}, [], [])
        with self.assertRaises(ValueError):
            repair.repair_copy(self.state + [{'symbol': self.names[0]}], self.catalog, [], [])
        ambiguous = copy.deepcopy(self.catalog)
        other = chr(ord('龙') + 1) + '虾USDT'
        self.assertEqual(repair.legacy_name(other), self.aliases[2])
        ambiguous['symbols'].append(dict(symbol=other, contractType='PERPETUAL', quoteAsset='USDT'))
        with self.assertRaises(ValueError):
            repair.repair_copy(self.state, ambiguous, [], [])
        clean = [{'symbol': self.names[0], 'pos': 2, 'qty': 1, 'disaster_stop_id': '123'}]
        self.assertEqual(repair.repair_copy(clean, self.catalog, [], []), (clean, []))

    def test_cli_only_writes_new_copy_and_exact_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            state = folder / 'sar_bots.json'
            original = json.dumps(self.state, ensure_ascii=False).encode('utf-8')
            state.write_bytes(original)
            (folder / 'market.json').write_text(json.dumps(self.catalog), encoding='utf-8')
            (folder / 'positions.json').write_text('[]', encoding='utf-8')
            (folder / 'orders.json').write_text('[]', encoding='utf-8')
            command = [sys.executable, str(Path(repair.__file__)), '--state', str(state),
                       '--exchange-info', str(folder / 'market.json'),
                       '--positions', str(folder / 'positions.json'),
                       '--open-orders', str(folder / 'orders.json')]
            dry = subprocess.run(command, capture_output=True)
            self.assertEqual(dry.returncode, 0, dry.stderr)
            self.assertEqual(len(list(folder.iterdir())), 4)
            output = folder / 'reviewed.json'
            completed = subprocess.run(command + ['--output', str(output)], capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(state.read_bytes(), original)
            self.assertEqual((folder / 'reviewed.json.original.bak').read_bytes(), original)
            self.assertEqual([x['symbol'] for x in json.loads(output.read_text(encoding='utf-8'))], self.names)
            before = output.read_bytes()
            refused = subprocess.run(command + ['--output', str(output)], capture_output=True)
            self.assertNotEqual(refused.returncode, 0)
            self.assertEqual(output.read_bytes(), before)
            refused = subprocess.run(command + ['--output', str(state)], capture_output=True)
            self.assertNotEqual(refused.returncode, 0)
            self.assertEqual(state.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
