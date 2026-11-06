#!/usr/bin/env python3
"""Prepare a reviewed copy of flat legacy state. Never edits the source or sends orders."""
import argparse
import copy
import json
import math
from pathlib import Path
import sys


def legacy_name(symbol):
    # Deterministic model of the observed Latin byte uppercase conversion, then Qt replacement.
    damaged = bytes(b - 32 if 0xE0 <= b <= 0xFE and b != 0xF7 else b
                    for b in symbol.encode('utf-8'))
    return damaged.decode('utf-8', errors='replace')


def repair_copy(records, exchange_info, positions, orders):
    """All snapshots are explicit inputs. Ambiguity/exposure refuses the entire repair."""
    if not isinstance(records, list) or not isinstance(exchange_info, dict):
        raise ValueError('状态必须是数组，exchangeInfo 必须是交易所返回的对象')
    market = exchange_info.get('symbols')
    if not isinstance(market, list) or not market:
        raise ValueError('缺少完整交易所品种列表')
    if not isinstance(positions, list) or not isinstance(orders, list):
        raise ValueError('持仓及普通/条件委托快照必须是数组，错误响应不能当成空列表')
    aliases = {}
    for row in market:
        symbol = row.get('symbol') if isinstance(row, dict) else None
        if not isinstance(symbol, str) or not symbol or '\ufffd' in symbol:
            raise ValueError('交易所品种列表包含无效名称')
        if row.get('contractType') != 'PERPETUAL' or row.get('quoteAsset') not in ('USDT', 'USDC'):
            continue
        alias = legacy_name(symbol)
        if alias != symbol:
            aliases.setdefault(alias, set()).add(symbol)

    exposed = set()
    for position in positions:
        if not isinstance(position, dict) or not isinstance(position.get('symbol'), str):
            raise ValueError('持仓快照条目无效')
        try:
            amount = float(position['positionAmt'])
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError('持仓快照缺少有效 positionAmt') from exc
        if not math.isfinite(amount):
            raise ValueError('持仓数量不是有限值')
        if amount != 0:
            exposed.add(position['symbol'])
    for order in orders:
        if not isinstance(order, dict) or not isinstance(order.get('symbol'), str):
            raise ValueError('委托快照条目无效')
        exposed.add(order['symbol'])  # Conservative: supplied orders must all be resolved first.

    result = copy.deepcopy(records)
    changes, used = [], set()
    for row in result:
        if not isinstance(row, dict) or not isinstance(row.get('symbol'), str) or not row['symbol']:
            raise ValueError('状态条目缺少品种')
        old = row['symbol']
        if '\ufffd' in old:
            matches = aliases.get(old, set())
            if len(matches) != 1:
                raise ValueError(f'{old} 无法唯一对应交易所品种，原文件保持不变')
            target = next(iter(matches))
            # Lossy names with holdings/unknown orders require a separate account/order audit.
            # Refuse instead of resetting position, stop IDs, re-entry or monitoring state.
            numeric = ('pos', 'qty', 'reserved_risk', 'reserved_margin')
            if 'pos' not in row or 'qty' not in row or any(not isinstance(row.get(k, 0), (int, float)) or isinstance(row.get(k, 0), bool)
                   or not math.isfinite(row.get(k, 0)) or row.get(k, 0) != 0 for k in numeric):
                raise ValueError(f'{old} 有持仓或风险预留，需先核对归属，不能自动改名')
            if any(row.get(k) for k in ('pending', 'open_uncertain', 'uncertain_client_id',
                                       'disaster_stop_id', 'ds_vanished_id')) or target in exposed:
                raise ValueError(f'{old} 有持仓/委托证据，需先核对归属，不能自动改名')
            row['symbol'] = target
            changes.append((old, target))
        if row['symbol'] in used:
            raise ValueError(f'{row["symbol"]} 存在重复条目，不能合并或丢弃断点')
        used.add(row['symbol'])
    return result, changes


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', required=True, type=Path)
    parser.add_argument('--exchange-info', required=True, type=Path)
    parser.add_argument('--positions', required=True, type=Path)
    parser.add_argument('--open-orders', required=True, type=Path,
                        help='合并普通及条件委托的完整数组；仅普通委托列表不完整')
    parser.add_argument('--output', type=Path, help='可选：生成待核对副本和原始备份；绝不覆盖源文件')
    args = parser.parse_args()
    raw = args.state.read_bytes()
    records = json.loads(raw.decode('utf-8-sig'))
    repaired, changes = repair_copy(records, read_json(args.exchange_info),
                                    read_json(args.positions), read_json(args.open_orders))
    for old, new in changes:
        print(f'{old} -> {new}')
    print(f'可确认 {len(changes)} 项；来源文件未修改。快照不能代替替换前的实时对账。')
    if args.output and changes:
        destination = args.output.resolve()
        backup = destination.with_name(destination.name + '.original.bak')
        sources = {p.resolve() for p in (args.state, args.exchange_info, args.positions, args.open_orders)}
        if destination in sources or backup in sources or destination.exists() or backup.exists():
            raise ValueError('输出或备份已存在/指向输入，拒绝覆盖')
        if args.state.read_bytes() != raw:
            raise ValueError('来源文件已变化，请重新取快照；未生成修复文件')
        with backup.open('xb') as stream:
            stream.write(raw)
        with destination.open('x', encoding='utf-8') as stream:
            json.dump(repaired, stream, ensure_ascii=False, indent=2, allow_nan=False)
        print(f'待核对副本：{destination}\n原始备份：{backup}')


if __name__ == '__main__':
    # Windows pipe output may default to a legacy code page that cannot encode
    # the very Chinese names this tool must report. Keep reports UTF-8 even when
    # redirected, and configure stderr as well for refusal/error messages.
    sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')
    sys.stderr.reconfigure(encoding='utf-8', errors='backslashreplace')
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(f'停止：{error}') from error
