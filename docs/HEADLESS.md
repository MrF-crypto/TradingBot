# ccbot_headless：无图形界面版本

配置文件驱动，不依赖 Qt、不需要显示器，可以在 Linux 服务器上完全后台运行。核心策略引擎
（趋势跟踪、行情链路自愈、账户级闸门、webhook 提醒）跟图形界面版共用同一份代码，
行为完全一致，只是把"填表单"换成了"写配置文件"。

## 构建（Linux）

```bash
git clone https://github.com/MrF-crypto/TradingBot.git
cd TradingBot
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg根目录>/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=x64-linux
cmake --build build --config Release --target ccbot_headless
```

> `vcpkg.json` 里 Qt 依赖标了 `"platform": "windows | osx"`，Linux 上不会尝试装 Qt，只会装
> `ccbot_headless` 需要的 simdjson / curl / mbedtls，构建比图形界面版快很多。

生成的可执行文件在 `build/ccbot_headless`。

## 配置文件

复制 `config.example.json` 改成自己的，默认路径是运行目录下的 `config.json`，也可以指定别的路径：

```bash
./ccbot_headless /path/to/my_config.json
```

顶层字段：

| 字段 | 说明 |
|---|---|
| `api_key` / `api_secret` | Binance API 凭证，**明文存在配置文件里**，务必 `chmod 600` 并且不要提交到git（`.gitignore` 已经排除了 `config.json`，只有 `config.example.json` 模板会被提交） |
| `testnet` | `true`=连测试网，`false`=连真实账户，强烈建议先用 `true` 跑通 |
| `account_mode` | `"futures"`（默认）=普通合约账户，走 `fapi.binance.com`；`"portfolio_margin"`=统一账户，走 `papi.binance.com`。**统一账户没有测试网**，填了 `testnet: true` 会被忽略并告警。账户在币安开通统一账户后，普通合约的 API 端点就失效了，两者不能混用同一个账户的 Key |
| `max_total_margin` | 账户总保证金上限（USDT），`0`=不限，同图形界面版设置里的那个 |
| `alert_webhook` | 企业微信/飞书/Telegram webhook，行情链路持续异常、对账不一致、账户接口失败、资金费成本跨阈值时推送 |
| `max_open_positions` | 同时持仓的品种数上限，`0`=不限。与 `max_total_margin` 是两道不同的闸：前者管"总共投多少钱"，这个管"同时压在几个品种上" |
| `state_path` | 仓位运行时状态落盘路径，重启续跑用，默认 `ccbot_state.json` |
| `log_path` | 日志文件路径，留空则只输出到 stdout（配合 `journalctl`/`docker logs` 更方便） |
| `trend_bots` | 策略数组，见下一节（旧键名 `sar_bots` 仍然有效） |

> ⚠ **v5.0.0：旧的 `bots` 数组（网格 DCA）不再运行。** 它不会被静默忽略——启动时会明确报出
> "配置里有 N 个 bots 条目，但网格DCA 已整体移除，这些条目【完全不会运行】"。
>
> 静默忽略才是危险的：升级后配置文件原样放着、进程正常启动、日志一切正常，而那些品种其实
> 一个都没在跑，要等到某天去交易所对账才发现。趋势策略请配在 `trend_bots` 里。
> DCA 的实测结论见 [NEGATIVE_RESULTS.md](NEGATIVE_RESULTS.md)——代码删了，花钱买来的结论不删。

## 趋势策略（`trend_bots`）

本版有**三个趋势策略**，靠 `strategy` 字段选，一个 bot 一个策略。

数组名 v5.9.9 起是 `trend_bots`。**旧名 `sar_bots` 继续有效**，启动时只提示一句建议改名 ——
老部署的配置文件里就是这个名字，直接废掉等于让升级上来的人"进程正常启动、一个品种都不跑"。
两个键**同时出现会报错**：合并会让同一品种被配两次，任选一个会让另一半静默不运行。

状态落盘在 `<state_path>.sar`（后缀**故意保留**：改了名，升级上来的部署会读不到重启前的仓位状态）。重启会恢复持仓、开仓价、止损线、加速因子、
连续反手计数与统计，并在启动时与交易所对账一次，运行中每分钟再对一次。

三个策略共享同一套**出场语义**：只有追踪止损、**没有固定止盈**、止损线是棘轮的、
数据断流时沿用最后一条有效线。风险形状也一样：**胜率低（30~40%）、每笔都止损、
盈利来自少数几笔跑很远的单子、致命场景是震荡市连续磨损**。判据是 `胜率 × 盈亏比`，不是胜率。

```json
"trend_bots": [
  { "symbol": "BTCUSDT", "strategy": "trend_following", "interval": "4h",
    "budget_usdt": 500, "leverage": 3,
    "donchian_period": 20, "atr_period": 14, "atr_mult": 3.0 }
]
```

### 选哪个

| `strategy` | 入场 | 止损 | 适合 |
|---|---|---|---|
| `trend_following`（默认） | 价格创 N 根新高/新低 | `k × ATR` 的 Chandelier 棘轮线 | 4h 以上。信号最少、对参数最不敏感 |
| `psar` | 价格站上/跌破前一根极值；之后**只由翻转产生** | SAR 值本身，随趋势加速贴近价格 | 有明确单边的品种。震荡市会连续翻转 |
| `bare_k` | 盘中即时 或 收盘突破前一根高/低 | 最近 N 根的摆动极值 | 短周期。最快也最吵 |

### 共享字段

| 字段 | 默认 | 说明 |
|---|---|---|
| `symbol` | — | 必填 |
| `strategy` | `trend_following` | `trend_following` / `psar` / `bare_k`。填别的会**拒绝启动**，不会默默跑趋势跟踪 |
| `budget_usdt` | `1000` | 每笔仓位的**名义价值**。三个策略都是单仓位、都不加仓，这就是全部 |
| `leverage` | `3` | |
| `interval` | `4h` | 信号K线周期。可选 `1m` `3m` `5m` `15m` `30m` `1h` `2h` `4h` `6h` `8h` `12h` `1d`（币安没有 10m 这一档） |
| `direction` | `both` | 允许开仓的方向：`both`=多空 / `long`=仅多 / `short`=仅空。不允许的方向不开仓；要反手到不允许的方向时只平不反手（`psar` 同样）。已有仓位照常用止损线管着 |
| `reverse` | `none` | 亏损止损后怎么办。`none`=只平掉，回到正常入场流程（方向不限，同向信号先来就同向再进）；`immediate`=立即反手开反向仓，不查信号。**盈利出场永不反手**——力竭不等于反转（`psar` 例外，见下）|
| `max_consecutive_reverses` | `0` | 连续反手上限，`0`=不限。真趋势不需要连续反手，连续反手本身就是「现在是震荡市」的信号 |
| `cooldown_bars` | `0` | 触顶后冷却多少根**K线**（不是 tick） |
| `signal_max_age_sec` | `0` | 信号快照的保质期。过期后**不开新仓**，但已持仓的止损线仍然有效。`0`=**自动**取 `max(60秒, 2.5×K线周期)`——1m 给 150 秒，4h 给 10 小时。填死值只在你明确知道想要什么时才有必要 |
| `size_mode` | `notional` | 仓位算法。`notional`=固定名义；`risk`=**固定单笔风险**：名义 = `risk_usdt ÷ 入场价到止损线的距离%`。用的是**当前策略算出来的那条线**（趋势跟踪 k×ATR、SAR 首根 SAR 值、裸K 摆动极值），不是某个固定的 ATR 倍数 |
| `risk_usdt` | `0` | `size_mode=risk` 时**必填**：单次止损愿亏多少钱。账户 10000U、每次探测愿亏 1% ⇒ 填 100。此时 `budget_usdt` 变成**名义上限**（止损距离极小时兜住公式算出的天量仓位） |
| `use_disaster_stop` | `false` | **强烈建议开。** 把棘轮止损线镜像成交易所上的 `STOP_MARKET + closePosition` 单——唯一的进程外保护。这套策略的全部保护就是那条活在本进程里的止损线，程序崩了就什么都不剩。挂不上会重试 13 次（3 次快速 + 7 次退避 + 3 次最后机会），第 10 次起界面标红「此仓位无交易所侧保护」，第 13 次仍失败就**立刻平掉刚开的仓**；连续 2 次这样平仓会熔断停掉该 bot |
| `disaster_stop_buffer_pct` | `1.0` | 挂单价 = 止损线再外扩这么多%（多头往下、空头往上）。**必须 > 0**，填 0 会被配置校验拒绝。理由：交易所用连续标记价触发而本地每 3 秒采样，挂在线上会让交易所抢先触发，于是正常止损变成「外部平仓 → 对账停 bot」，一次例行出场变成需要人工介入的事件。开仓时冻结初始灾难止损，不跟随本地移动止损重挂 |
| `roi_trail` / `roi_trail_start` / `roi_trail_back` | `false` / `10` / `5` | 收益率追踪止盈（三个策略都能用）。收益率 = 浮动盈亏 ÷ 保证金，和 GUI 主表「收益率」列同一个数（带杠杆、保证金按现价算）。持仓期最高收益率到过 `roi_trail_start`% 后，从最高点回撤 `roi_trail_back` 个**百分点**就平（例：10 / 5 ⇒ 最高到过 +15% 后回到 +10% 就走）。和 k×ATR 止损线同时生效，谁先触发听谁的。智能模式使用趋势跟踪模板的 `template.roi_trail*` 参数 |

为什么 `size_mode=risk` 值得开：同一份名义，在 4h 波动 1.6% 的 LTC 和 5.3% 的 COTI 上，
单次止损亏的钱差 3.3 倍。固定名义 = 风险全压在高波动那几个品种上，「铺开品种分散风险」就此失效。
这是趋势跟踪的「单位」概念。

### ① `trend_following` 专属

| 字段 | 默认 | 说明 |
|---|---|---|
| `donchian_period` | `20` | 唐奇安通道周期（趋势跟踪原版 20）。上破 N 根最高实体顶→做多，下破 N 根最低实体底→做空（实体 = max/min(开, 收)，**不看影线**：一根长上影会把上沿抬到针尖、错过好价格）。通道**排除当前未收盘K线**，否则「价格≥上沿」恒成立、信号恒真 |
| `atr_period` | `14` | ATR 周期，Wilder 平滑（与 TradingView/币安同口径）。**只有这个策略用 ATR** |
| `atr_mult` | `3.0` | **止损距离 = k × ATR**（Chandelier Exit）。棘轮，只朝有利方向移动。k 越小假突破越多，越大回吐越多。经典值 2.5~3.5 |
| `squeeze_box_atr` | `0`（关） | 横盘过滤：突破时通道宽度（上沿−下沿）≤ 这么多倍 ATR 才开仓。ATR 只用已收盘K线。例 `4` |
| `trend_er_min` | `0`（关） | 箱体过宽时的例外：通道窗口的效率比（净涨跌 ÷ 来回总路程，0~1）≥ 这个值、且净走向与突破同向 ⇒ 当作趋势延续放行；宽且来回扫的照样拦。只在 `squeeze_box_atr > 0` 时生效。随机走 12 根平均约 0.23，建议从 `0.4` 起步 |
| `lock_atr` | `false` | 出场优化：止损距离用 min(开仓时 ATR, 当前 ATR)，暴涨时 ATR 放大不会把止损放宽 |
| `accel_exit` | `false` | 出场优化：浮盈（按开仓时 ATR 计）≥ 6 ATR 时 k 降到 2，≥ 10 ATR 时降到 1.5，只降不升。仍是追踪止损，不是固定止盈 |
| `breakout_vol_ratio` | `0`（关） | 放量过滤：当前根**已成交**量 ≥ 通道内各根均量的这么多倍才开仓（不按剩余时间外推，量攒够那一刻入场）。例 `1.5` |
| `once_per_bar` | `false` | 每根K线最多开一次：同一根里出场后（止盈、止损、假突破都算）不再开，等下一根。不开的话平仓后价格还在通道外，几秒后就原地再开一笔 |
| `max_chase_atr` | `0`（关） | 追高过滤：价格离被突破的通道边沿超过这么多倍 ATR 就不开，回到以内再开。例 `1.5` |
| `max_init_loss_pct` | `0`（关） | 初始止损上限：开仓时止损最多离开仓价这么多 %（按价格），k×ATR 更近时用 k×ATR，之后照常追踪。例 `6` |
| `expand_long` / `expand_long_min` | `false` / `2.5` | 多单要波动放大（只管做多）：这根K线开盘到现在的振幅 ÷（已收盘 ATR × √已走比例）≥ `expand_long_min` 才开多。已走比例按至少 1/4 根算；没到就等，本根内波动起来照样开 |
| `breakeven` / `breakeven_start` | `false` / `6` | 保本止损：最高收益率（同 `roi_trail`，带杠杆）到过 `breakeven_start` 后，止损抬到开仓价 + 0.1% 手续费，只上不下。开着 `roi_trail` 时 `breakeven_start` 必须小于 `roi_trail_start`，否则启动报错。交易所硬止损不跟着挪 |

两项各自独立，可只开一项。只拦通道突破入场，不拦亏损止损后的立即反手。

**选 `atr_mult` 前先看实测 ATR**：GUI 版的表有一列 ATR%，悬停即给出当前 k 对应的实际止损距离。
跨品种唯一可比的口径是 `atr/price`，所以 BTC 和新上山寨能共用同一组参数。

### ② `psar` 专属

`SAR(下一根) = SAR + AF × (EP − SAR)`，其中 EP 是持仓期极值（多=最高、空=最低）。
**AF 只在刷新 EP 的那一根递增**，封顶 `af_max`——这是「趋势走得越强 SAR 收得越快」的全部来源，
每根都加的话 AF 三四根就封顶、SAR 立刻贴上价格、一点正常回调就出场。
Wilder 的夹逼规则也实现了：SAR 不得落进**前两根**的价格范围内。

| 字段 | 默认 | 说明 |
|---|---|---|
| `af_start` | `0.02` | 加速因子初值（Wilder 原版）。调大 = 一开仓就贴得紧 = 更容易被正常回调扫掉 |
| `af_step` | `0.02` | 每次刷新 EP 时 AF 加多少 |
| `af_max` | `0.20` | AF 上限。封顶后 SAR 每根固定吃掉「当前距 EP」的 20% |

⚠ **`reverse` 对这个策略是写死的 `immediate`，配了也会被覆盖。** PSAR 的定义就是
stop-and-reverse：触及 SAR 就平仓并立即反向，**盈利触及也翻**。它的入场信号**只有**翻转
这一个来源，不翻就永远空仓——所以「保守版 PSAR」不是更保守，是根本不交易。

⚠ 代价是它在震荡市里会**一直翻下去没有刹车**，这是 PSAR 最著名的弱点而不是实现缺陷。
不设 `max_consecutive_reverses` 时启动会明确告警一次，建议设 2~3。

### ③ `bare_k` 专属

| 字段 | 默认 | 说明 |
|---|---|---|
| `bare_entry` | `break_prev` | `immediate`=**盘中即时**：实时价 > 本根开盘价 ⇒ 做多，< ⇒ 做空，不等收盘；`break_prev`=**等收盘突破**：本根收盘价 > 上一根最高价 ⇒ 做多，< 上一根最低价 ⇒ 做空 |
| `once_per_bar` | `false` | 每根K线最多开一次：同一根里被止损出场后不再重新入场，等下一根 |
| `swing_bars` | `3` | 止损用最近几根的最低/最高价。信号根**不算**在内。⚠ 这个数直接决定持仓时长：随机游走下平均持仓约 N+1 根，N=3 配短周期时交易次数会非常高 |
| `bare_vol_ratio` | `0`（关） | 只对 `break_prev` 生效：收盘突破时这根K线的量 ≥ 前 20 根均量的这么多倍才开仓，挡掉横盘里"收盘蹭过前高、量平平"的假信号。5m 建议从 `1.5`~`2.0` 起步。不拦止损后的反手 |
| `signal_body_atr` | `0`（关） | 信号K力度，单位为同周期 ATR14（只用已收盘K线）。`break_prev`：信号K实体 ≥ x×ATR 且阴阳与方向一致，建议 `0.6`；`immediate`：实时价离本根开盘价 ≥ x×ATR 才算信号（开盘价附近的死区，治来回翻转），建议 `0.3`~`0.5` |
| `chop_er_max` | `0`（关） | 震荡态：最近 20 根的效率比 < 这个值时，平常的入场信号不算、止损后只平不反手（无视 `reverse`），只认收盘站上/跌破最近 20 根高低点的箱体突破（`immediate` 看实时价），量比与力度照样要过。读数缺失按震荡处理。随机走 20 根平均约 0.18，建议 `0.3` |

`break_prev` 比「刚收盘那根是阳线就做多」严格得多——下跌趋势里的一根阳线只是噪音，
而突破前一根的高点是真正的动能信号。旧的阳线规则已在 v5.1 删除。

⚠ `immediate` **天然抖**：新K线刚开盘时价格 ≈ 开盘价，微小波动会让多空条件反复翻转。
配上 `reverse: "immediate"` 就是一根K线内来回开平好几次，每次两笔手续费。
这个组合在启动时会告警并建议开 `once_per_bar`——它是这个模式唯一的抖动护栏。

### 从 v5.0.x 升级

旧键仍然认，但**每一处行为变化都会在启动日志里明确报出来**，不会静默生效：

| 旧写法 | 迁移结果 | 告警 |
|---|---|---|
| `mode: "donchian"` | `strategy: "trend_following"` | 有（行为不变） |
| `mode: "bar"` | `strategy: "bare_k"` + `bare_entry: "break_prev"` | 有，**入场规则变了**：旧的阳线入场已删除，新规则严格得多，信号会明显变少 |
| `allow_reverse: true` + `reverse_needs_signal: false` | `reverse: "immediate"` | 有（行为不变） |
| `allow_reverse: true` + `reverse_needs_signal: true` | `reverse: "none"` | 有，**行为有差别**：新枚举里没有「反手但等反向信号」这一档，`none` 是保护性更强的那边，差别是现在同向信号先来也会再进一次 |
| `pyramid_max_adds` / `pyramid_step_atr` | 忽略 | 有（未知键告警）。金字塔加仓已整体移除，三个策略都不加仓 |

新键在场时旧键**完全不起作用**——两套键同时被尊重是最难查的一类 bug。

**对账的判定规则**（刻意保守——这套策略没有「层」这种可供收敛的中间结构）：

| 情况 | 处理 |
|---|---|
| 本地有仓、交易所没有 | 清空本地并**停止** bot。分不清是人工平的还是被强平的，后者继续开仓是往坑里跳 |
| 本地空仓、交易所有仓 | **停止** bot 并告警。最危险的一种——不停的话下一个突破信号会再开一笔，净敞口翻倍且原来那笔没有止损线守着 |
| 本地数量 > 交易所 | 外部部分平仓：数量收敛到交易所值，止损线与开仓价保留 |
| 本地数量 < 交易所 | 仅告警，不动本地。那笔不是自己开的，止损线基准不成立 |
| 方向不一致 | **停止** bot。任何自动收敛都是在猜 |

在途（有订单未回执）的 bot 一律跳过对账——订单可能已在交易所生效而本地还没入账，此刻比对必然误判。

SAR 的平仓也写进 `ccbot_trades.csv`，方向列是 `sar_long` / `sar_short`，层数列恒为 0。

平仓明细会追加写入运行目录的 `ccbot_trades.csv`（时间/品种/方向/原因/开平价/数量/盈亏/层数），
方便做周期统计分析。

程序启动后，配置文件里的 bot **立即开始监控**（没有GUI版"先停止等手动开启"那一步）；如果
`state_path` 里有上次落盘的仓位状态（品种+方向能对上配置文件），会先恢复仓位再继续，不会
重新从零开首仓。

v2.4 起的安全行为：

- **单实例锁**：启动时在 `<state_path>.lock` 写入 PID，已有活着的实例时拒绝启动
  （双开同账户会重复下单）。程序崩溃残留的锁会被自动接管，无需手动清理
- **启动对账**：恢复仓位后会拉取交易所实际持仓核对——外部手动平过仓的 bot 本地状态
  自动清空并停止（webhook 告警），数量对不上的自动收敛到交易所值
- **状态原子写盘**：先写 `.tmp` 再改名，崩溃在写文件中途不会损坏状态文件

## 智能模式（`smart_mode`）

只保留趋势跟踪。扫描满足条件的 USDT 永续候选；目前沿用 BTC/ETH、稳定币、黄金代币与非加密标的排除规则。`trend_bots` 可留空，智能模式默认关闭；完整示例见 `config.example.json`。

候选组织可选 `mixed`（强/弱/中间/异动分组）或 `breakout`（多/空评分名单），两者都不替代标的自身趋势判定。`dir_long`、`dir_short` 是最终交易方向权限。旧“过热仅空”已取消。

核心流程与时序见 [趋势跟踪 V1](TREND_FOLLOWING_V1.zh-CN.md)。`smart_mode.template` 新字段：

| 字段 | 默认 | 含义 |
| --- | --- | --- |
| `tf_enabled` | `true` | 启用已收盘均线趋势过滤；关闭可做旧规则基准对照 |
| `tf_close_confirm` | `true` | 收盘突破；`false` 为盘中触发，方向仍来自已收盘数据 |
| `tf_reversal_exit` | `true` | 确认反向趋势时平仓，不立即反手 |
| `tf_fast_period` / `tf_slow_period` | `20` / `60` | 快慢 EMA；慢线最大 200，必须大于快线 |
| `tf_slope_bars` | `3` | 慢线斜率回看 |
| `tf_min_slope_atr` | `0.02` | 每根慢线变动 / 参考 ATR 的方向阈值 |
| `tf_min_separation_atr` | `0.10` | 快慢均线方向间距 / ATR |
| `tf_breakout_buffer_atr` | `0.10` | 旧通道外的最小突破幅度 |
| `tf_min_close_location` | `0.60` | 收盘位于信号振幅中的顺势位置；空头镜像 |
| `tf_short_vol_ratio` | `0` | 空头量比要求；0 不要求放量。多头使用 `breakout_vol_ratio` |

默认模板为 `interval=1h`、通道 12、ATR14、k=3、`size_mode=risk`、`risk_usdt=1`、`budget_usdt=100`、杠杆 3。箱体硬过滤和盈利加速收紧默认关闭；锁定 ATR、每根一次、追价上限 1.5ATR 保留。信号快照默认 120 秒有效，跨到下一根后旧突破确认不可开仓。高低价通道仅用于新的 `tf_enabled` 模型；手动旧规则仍使用原实体通道。

以下字段位于 `smart_mode` 根层：

| 字段 | 默认 | 含义 |
| --- | --- | --- |
| `max_positions` | `20` | 持仓及在途新仓数上限，0 关闭 |
| `max_total_risk_usdt` | `6` | 全部 bot 的组合初始计划风险预算，0 关闭 |
| `max_direction_risk_usdt` | `4` | 同向共享风险预算，0 关闭；不假设多个山寨币相互独立 |
| `risk_cost_bps` | `20` | 往返手续费与滑点估算，参与等风险数量与预算；不含持有期资金费 |
| `daily_loss_limit` / `float_loss_limit` | `0` / `0` | 既有当日/浮亏闸门，0 关闭 |
| `pause_new` | `false` | 暂停所有新开仓；保留平仓 |
| `event_poll_sec` | `60` | 轻量价格观察间隔，至少 30 秒 |
| `event_cooldown_sec` | `300` | 异动重扫冷却，不小于观察间隔 |
| `event_move_atr` / `event_min_move_pct` | `2` / `1` | 两次观察间变化须达到 `max(上次完整扫描 ATR% × 倍数, 百分比下限)` |

以上金额和因子是研究初值，不是已验证的仓位建议。组合预算包含手动 bot 和在途开仓；无明确初始止损的已有仓位会阻止新增风险。反手的平仓腿始终可执行，新开仓腿经过同一套账户闸门。实际滑点、跳价、资金费或停机可能使实际损失超过预算。

候选 ATR% 研究范围默认 0.1~8，24h 涨跌幅排除上限默认 0（关闭）；历史涨跌幅不直接产生反向信号。硬门槛、最低分、名额与 24h 驻留等池管理规则仍然生效。异动重扫不推进按小时累计的淘汰计数，也不会绕过固定信号周期。

旧 `weak_vol_ratio` 仅作为输入兼容：若没有 `template.tf_short_vol_ratio` 则迁移到新的空头量比。新保存不再输出旧字段。老配置显式设置的仓位、止损、量比等继续优先；缺失的新字段使用新默认。

### 单一策略与旧版迁移

智能模式只运行趋势跟踪，使用 `"strategy": "trend_following"`，也可以省略该字段。旧名称 `"turtle"` 作为输入别名兼容，保存配置时统一输出新名称。

原智能裸K埋伏、波动顺势已移除，`"strategy": "bare"` / `"vol"` 明确拒绝，不会静默按趋势跟踪执行。旧配置遗留的 `bare` / `vol` 参数块不再读取或保存。

旧状态文件若包含这两套策略的持仓（旧名单代码 K / V），启动将被拒绝，原状态文件保留。请使用旧版管理至平仓，再升级；旧空仓条目可以安全丢弃。关闭智能模式时，趋势跟踪已有持仓继续管理至平仓。

手动 `trend_bots` 的纯裸K（`bare_k`）和 SAR（`psar`）继续保留。

## 数据记录（`journal/`）

给实盘复盘、校准智能模式用。只写不读，不参与任何交易决策，写失败就丢掉那一行。
位置：headless 在状态文件同目录下的 `journal/`；GUI 在 `data/journal/`。时间一律 UTC，CSV 带 BOM，Excel 直接打开。

| 文件 | 每行是什么 | 用来回答 |
|---|---|---|
| `trades.csv` | 一笔平仓：开平时间、持仓小时、手动/自动、**开仓时所在段、名次、分数**、盈亏、估算手续费、净盈亏、**入场离通道边沿多少 ATR**、**浮盈亏最高 / 最低**（美元、%、ATR、R 四种单位）、**入场量比、入场箱体ATR、入场主动买入占比**（趋势跟踪：开仓那根 K 线里主动买入成交额 ÷ 成交额）、原因 | 哪一段赚钱；突破入场平均晚了多少；止损松紧（赚钱单最低到过几 R）、出场回吐多少（最高 vs 实际盈亏）；突破那根是不是真有人在买 |
| `scan-YYYYMMDD.csv` | 每轮扫描里**每个合格币**一行：所在段、段内名次、三种分数、7 个指标、收盘价、是否已有 bot、**主动买入占比**（最近 1 根 / 6 根 1h）、**持仓量**（时间、持仓价值 U、1h / 4h / 24h 变化%，按张数算） | 高分币后来是不是真的走得更好（拿后面快照里的收盘价算后续涨跌），权重怎么调；持仓量与主动买入能不能区分真假突破（只记录，不参与决策） |
| `filter-YYYYMMDD.csv` | 一次被过滤掉的突破（每 bot 每根K线最多一条）：方向、卡在哪一关、价格、说明 | 过滤挡掉的是坏单还是好单 |
| `pool.csv` | 候选池事件：加入 / 移出 / 换方向，带段、名次、原因 | 名单换得是不是太勤 |

按日的 scan / filter 文件保留 60 天并自动清理。CSV 表头变化时，旧文件会改名保存并创建新文件。

持仓量逐币拉取（`/futures/data/openInterestHist`，只给合格币，每轮扫描一次，单独限流不占 /fapi 权重）。拉不到的币那几列留空，不影响排名。
交易所只保留最近 30 天的持仓量历史，所以要靠这里每小时记下来。
升级后某个文件的列变多了：老文件改名为 `xxx-old-时间.csv` 留着，另起一份新的。

## 运行

```bash
./ccbot_headless config.json
```

日志会打印到 stdout（配了 `log_path` 的话同时落盘），`Ctrl+C`（或 `kill` 发 SIGTERM）会保存
当前仓位状态后干净退出。

## 用 systemd 常驻后台

```ini
# /etc/systemd/system/ccbot.service
[Unit]
Description=ccbot headless trading bot
After=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/ccbot
ExecStart=/opt/ccbot/ccbot_headless /opt/ccbot/config.json
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ccbot
journalctl -u ccbot -f
```

`config.json` 建议 `chmod 600`，`WorkingDirectory` 目录也建议只给运行用户权限，避免API Key被其他用户读到。

## 看门狗：发现"进程还在但不干活了"

交易系统最阴的故障不是崩溃——崩溃至少进程没了，`Restart=on-failure` 会拉起来。
真正危险的是**进程活着但循环停摆**：喂价全部返回 0、心跳一直失败、卡在某个网络调用上。
这种状态下仓位无人管理，而 `systemctl status` 显示一切正常。

程序每个 tick（3秒）会把当前时间戳写进 `<state_path>.alive`（默认
`ccbot_state.json.alive`）。**这个文件的年龄就是循环的心跳。** 正常退出时会删掉它。

### 方式一：systemd 定时器检查文件年龄

```ini
# /etc/systemd/system/ccbot-watchdog.service
[Unit]
Description=ccbot liveness check

[Service]
Type=oneshot
ExecStart=/opt/ccbot/watchdog.sh
```

```ini
# /etc/systemd/system/ccbot-watchdog.timer
[Unit]
Description=run ccbot liveness check every minute

[Timer]
OnBootSec=2min
OnUnitActiveSec=1min

[Install]
WantedBy=timers.target
```

```bash
#!/bin/bash
# /opt/ccbot/watchdog.sh —— 心跳文件超过 60 秒没更新就重启服务并告警
ALIVE=/opt/ccbot/ccbot_state.json.alive
HOOK='https://api.telegram.org/bot<TOKEN>/sendMessage?chat_id=<ID>'

# 文件不存在 = 进程正常退出或从未启动，交给 systemd 处理，这里不插手
[ -f "$ALIVE" ] || exit 0

AGE=$(( $(date +%s) - $(stat -c %Y "$ALIVE") ))
if [ "$AGE" -gt 60 ]; then
    curl -s -X POST "$HOOK" -H 'Content-Type: application/json'          -d "{\"text\":\"[watchdog] ccbot 心跳停止 ${AGE}s，正在重启\"}"
    systemctl restart ccbot
fi
```

```bash
sudo chmod +x /opt/ccbot/watchdog.sh
sudo systemctl enable --now ccbot-watchdog.timer
```

重启是安全的：启动时会从落盘状态恢复仓位、跟交易所对账、并重建交易所侧灾难止损单。

### 方式二：只告警不重启

把 `systemctl restart ccbot` 去掉即可。行情剧烈时自动重启会错过几十秒的判定窗口，
如果你更怕"自动操作在不该动的时候动手"，就只发告警、人工决定。

## 程序自己会叫的几种情况

配了 `alert_webhook` 之后，这些事件会主动推送——不用盯日志：

| 事件 | 触发条件 |
|---|---|
| 启动连接失败 | 启动时拉不到账户 |
| 启动对账不一致 | 落盘仓位与交易所实际持仓对不上 |
| **运行中对账不一致** | 每分钟一轮。包括：交易所有持仓但**程序里没有任何 bot 对应**（连续 2 轮确认后报一次，没有止损保护，要手动处理）；硬止损在暂停/离线期间触发（按挂单价估算记账，计入当日亏损） |
| **智能模式异常** | 扫描失败、过半自动 bot 同时被判出局（疑似数据异常，这一轮不移除）、持仓中的币即将下架 |
| **行情停摆** | 某品种连续 1 分钟取不到价格，**且该品种有持仓** |
| **账户接口连续失败** | 心跳连续 3 次失败（约 3 分钟）；恢复后也会通知 |
| **uniMMR 接近强平** | 统一账户 uniMMR < 1.3（币安 1.05 起强制减仓） |
| **行情链路持续异常** | 连续约 3 分钟 `Health::healthy()` 为假（连续请求失败、缓存陈旧等）。**与上面那条"行情停摆"不是一回事**——备用价格可暂时维持单品种供价，但主标记价来源仍可能持续异常 |
| **进程退出** | 收到退出信号时，附带"还有几个品种有持仓" |

最后一条值得单独说：**进程一停，所有本地风控就停了**，只剩交易所侧的灾难止损单
（`use_disaster_stop`，见下）。这条消息本身就是"从现在起没人在管仓位"的信号。

## 资金费账本：一笔看不见的真实成本

永续合约每 8 小时结算一次资金费。**这是真实划走的现金，不是浮亏**——价格涨回来
也拿不回来。对"套住就长期持有"的用法，它是持有成本的全部定价。

0.01%/8h ≈ **年化 11%**。一个被套一年的多单，光资金费就吃掉 11%。

程序会自动记账，无需配置：

- **费率**约 5 分钟刷一次（公开接口，不占签名限流）
- **历史流水**约 1 小时同步一次（`/fapi/v1/income`，统一账户走 `/papi/v1/um/income`）
- 首次运行按最早那笔持仓的建仓时间往回补，7 天一窗分页，最多 30 窗（约 7 个月）
- 账本落在 `<state_path>.funding`，与 bot 生命周期无关（按**品种**记账，不按 bot——
  交易所是按"账户×品种"收费的，同品种多 bot 无法真实归属）

首次同步完成时日志报一行累计总额。（v5.10 之前每分钟每个持仓打一行"本轮已付 / 回本价"——
那个"本轮"从来没在平仓时清零过，实际是这个品种全部历史持仓的累计，数是错的，已删。）

年化持有成本超过 30% 会推一条告警（回落到 20% 以下解除）。**这条告警只告知成本变化，
不建议平仓**——持有决策是你的事，账本只负责让你看得见。

它不并进 `realized_pnl`：那个数是按平仓周期结算的，而资金费属于持有期。两个数分开
显示，口径才不会混。

## 唯一的进程外保护：`use_disaster_stop`

**这套策略的全部保护就是那条活在进程里的棘轮止损线。** 程序崩溃、断电、被 OOM
杀掉之后，它一点都不剩——仓位完全裸奔且没有任何底。

打开 `use_disaster_stop` 后，引擎在交易所挂保护委托。保护价按开仓时的初始止损再外扩并冻结，正常持仓中不随本地棘轮移动；平仓后撤销，重启后核对与恢复。进程停止期间只能保留这条初始保护，无法保证已获得的浮盈。触发不保证按触发价成交。

```json
{ "symbol": "BTCUSDT", "use_disaster_stop": true, "disaster_stop_buffer_pct": 1.0, ... }
```

**`disaster_stop_buffer_pct` 不能是 0。** 挂单价 = 止损线再往外扩这么多%（多头往下、
空头往上）。交易所用连续的标记价触发，而本地是采样的——挂在止损线【上】的话交易所
几乎总会先触发，于是主出场路径从"本地 reduceOnly 平仓"变成"交易所平掉、本地靠对账
才发现"，而对账发现外部平仓会**停掉那个 bot**。等于把一次正常的止损出场变成一次
需要人工介入的事件。留出缓冲之后：正常情况本地先平并撤掉这张单，只有进程真的不在了，
价格才会继续走到它上面。

它不参与常规交易，只在进程不在时兜底。


