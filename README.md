# TradingBot · 趋势跟踪

面向 Binance USDT-M 永续合约的 C++20 交易框架。Qt6 桌面版与配置驱动的 headless 版共用引擎和智能执行层。基于 [CCBot v5.11.0](https://github.com/MrF-crypto/CCBot) 整理和升级，作为独立项目从 **v1.0.0** 开始发布。

## 下载与运行

从 [Releases](https://github.com/MrF-crypto/TradingBot/releases) 下载对应平台包。Windows x64 只提供 `TradingBot.exe`、实际依赖的 DLL、`platforms/`、许可证及空 `data/`，完整解压后运行 GUI；不要只复制 EXE。macOS Intel / Apple Silicon 包顶层只有 `TradingBot.app` 和空 `data/`，许可证位于应用包内。无头程序只面向 Linux x64 / ARM64，包内附配置模板、简要运行说明及许可证。桌面包不再包含无头程序、配置模板、工具脚本或整套文档。平台要求见 [平台说明](docs/PLATFORMS.zh-CN.md)。

macOS 桌面版优先使用 `TradingBot.app` 同级 `data/`；只读临时运行目录下自动选择可写位置并记住。首次发现旧记录时可选“全新配置”或“恢复旧数据”，全新配置直接填写 API，旧记录保留。通过“设置 → 打开数据文件夹”查看实际位置，更新和备份时保留完整文件夹。API 凭证仍保存在 macOS 钥匙串，换电脑需要重新输入。

桌面版首次启动后设置测试网账户。无界面版将 `config.example.json` 复制为 `config.json` 并填写本机配置，参考 [Headless 说明](docs/HEADLESS.md)。下载文件可用同一 Release 的 `SHA256SUMS.txt` 核验。

```powershell
.\TradingBot.exe --check-runtime
```

Linux 使用 `./ccbot_headless --version` 检查版本。这两个检查不会读取账户配置或提交订单。当前修复见 [v1.4.3 发布说明](docs/releases/v1.4.3.md)，首版变化与验证范围见 [v1.0.0 发布说明](docs/releases/v1.0.0.md)。

## 智能模式

智能模式只保留**趋势跟踪**：

```text
扫描与异动唤醒 → 硬过滤 → 统一候选池 → 自身趋势许可 → 突破确认 → 组合预算 → 初始止损与追踪退出
```

v1.1.0 起通过硬过滤的品种全部放入统一候选池，不打分、不分多空榜、不设候选数量配额；表格按品种名称显示。每个候选可向上突破做多、向下突破做空，实际入场仍需满足自身趋势许可、突破确认和风险预算。多个信号竞争有限预算时，由实际处理到的有效开仓请求申请预算，不再优先高分币。

智能模板默认使用 1h 信号、EMA20/60 的方向/斜率/分离度、12 根高低价唐奇安通道收盘突破、ATR14 与 3 倍 ATR 初始/移动止损。上涨许可做多、下跌许可做空，震荡或过渡等待。另可显式选择盘中突破或固定 15m / 30m 周期作独立实验。

常规扫描为每小时整点后 1 分钟；另每分钟轻量观察双向价格异动，最多每五分钟触发一次合并重扫。异动只唤醒扫描，不越过信号与风控。默认单笔计划风险 1U、组合 6U、同向 4U、单笔名义上限 100U，费用/滑点估算 20bps；这些是可修改的研究初值，不适配任何特定账户规模。

在途开仓计入预算；状态不明的委托保留预留，核对原订单终态与新鲜仓位后才释放。平仓不受新仓闸门限制。收盘确认模式可在加入候选后参与仍有效的已确认突破，盘中模式仍需先回到通道内上膛。明确不合格的候选暂停新仓，持仓与在途委托继续管理；重启恢复的自动 bot 等待本轮过滤通过。智能模式默认关闭。

v1.1.1 增加智能 bot 平仓后等待：完全止盈或止损后保留监控，必须等下一根完整 K 线收盘形成新的突破才可再次入场。例如 1h 周期 13:20 平仓，最早在 15:00 使用 14:00–15:00 的收盘信号。时间到但没有新突破继续空仓；首次选择盘中触发也不能绕过再次入场的收盘确认。等待记录随状态保存，换参数、候选重扫和重启不清除。

v1.2.0 增加可选的“满仓换仓”（智能模式设置 → 风控 → 智能持仓），默认关闭。满仓时，新收盘突破可申请替换持续失去趋势许可的旧自动持仓；先平后开并重新检查信号、追价及风险预算。没有每日换仓次数或额外换仓间隔，任务串行执行。旧仓至少持有 6 根完整 K 线、连续 3 根失去原方向许可、收盘位于快 EMA 不利一侧，且最近 3 根没有新的有利收盘极值。正常趋势仓、手动仓及异常待核对仓不参与替换。v1.3.0 起保存观察断点，重启和断线后按历史 K 线补齐进度；不直接把离线时间算作趋势失效。

第一版实现与边界见 [趋势跟踪 V1](docs/TREND_FOLLOWING_V1.zh-CN.md)。单元/场景测试不构成盈利验证；历史回放、交易成本及样本外验证尚待开展。

原智能裸K埋伏、波动顺势及其设置、调度和执行分支已移除。**手动 bot 的趋势跟踪、抛物线 SAR、纯裸K继续保留**；手动纯裸K与已移除的智能埋伏是不同功能。


v1.3.0 增加断点续监控和候选详情。原状态文件自动保存换仓计数、有利收盘极值、趋势记录、已处理 K 线、入场准备状态及最近 40 条事件；连接恢复后先核对仓位，分批补齐缺失 K 线，再使用新鲜信号。历史订单不会重放，保存的止损线不会因恢复而清零。首次从旧版升级，没有断点的已有持仓尝试从开仓后的历史 K 线重建观察；缺少历史数据时显示等待，不猜测进度。

打开“智能模式设置 → 统一候选 · 监控详情”，可按有信号、入场受阻、持仓、可被替换、等待再入场或数据异常筛选。选中品种查看趋势、形态、条件数值、换仓理由、恢复进度与事件；未连接时可查看保存状态。升级使用原来的数据目录，具体说明见 [趋势跟踪使用说明](docs/TREND_FOLLOWING_V1.zh-CN.md)。

## 组合利润保护与 BTC 积累

智能设置新增“利润保护 · BTC 积累”页，两项功能均默认关闭。组合保护按多、空方向分别记录净收益峰值，无需等持仓数减少。首版要求智能模板为 1h；本轮固定候选至少 10 个、同一根已收盘数据覆盖至少 90%。缺失品种按保守上下界处理，不能被当作转弱。研究初值：净收益达到 100 U 后启动保护，广度低于 45% 或较峰值下降 15 个百分点连续确认 2 根后暂停同向新增风险；此时峰值回撤 20% 则退出该方向，硬回撤 35% 不等待广度。退出期间暂停所有智能新增风险，原有单币止损继续工作。受保护方向在广度回到 60% 连续 2 根后恢复，并要求新收盘突破。峰值不按充值、划转或日界线重置；组合实时曲线包含已实现、未实现和估算退出费用，仍存在滑点与离线期间无法执行的限制。

BTC 积累首版采用普通主网、单向持仓、单资产 USDT 合约账户的“利润划转到现货 → 买入 BTC → 独立持有”。不支持 PM 自动划转、测试网钱包划转或 1× 永续积累。默认按累计已结算空头净收益的 30% 分配，亏损先回补，同时受整体策略已实现收益与最低保留合约权益限制；首次资金核对保存合约净权益作为默认保留额，可在设置中显式调整。单批至少 50 U、最多 500 U，默认等待 BTC 1h EMA20/60 上涨许可。IOC 限价约束最大价格偏离，批次内留 1% 余量容纳费用与取整，实际支出以成交为准。

利润源仅包含首次启用后新开的智能仓位。部分平仓等整笔结清后计入可购买的空头收益，合约成交明细核实实际手续费，持仓期间的非本程序成交会阻止该笔自动分配。资金费在平仓至少 5 分钟后核对，之后 7 天内每小时复核；晚到的调整只改变剩余额度，不自动卖出已买 BTC。非 USDT 合约手续费、非 BTC/USDT 现货手续费尚无可靠换算时暂停购买并说明原因。需要现货交易和内部划转权限，不需要提现权限。

账本保存在原数据目录下的 `ccg_smart_state.json.profit`（headless 为其 smart 状态文件加 `.profit`），备份完整 data 目录即可。关闭功能只暂停新购买，已经提交的请求继续查单；不自动卖币。重启恢复峰值、广度确认进度、累计使用额度、划转意图和订单 ID，超时不盲目重发。账户凭证变更、账本损坏或未能唯一确认的划转需要核对原记录，不能删除账本来“重试”。修改分配比例会按新比例重新计算累计上限；已经使用的额度不会清零。现货 BTC 的价值仍会波动，购买 BTC 不等于锁住 USDT 利润。

重启后的成交按合约持仓周期配对，不依赖临时机器人编号。已继承的持仓数量也会保存；外部平仓或手动改仓造成数量与成交记录不一致时，暂停该方向组合回撤计算与新增风险，保留原有单币止损，并在状态中提示核对。划转超时只核查原请求附近的记录；无法唯一确认时保留预留额度，后续同金额的人工划转不会被当作这次请求成功。

## 框架

| 模块 | 职责 |
| --- | --- |
| `src/core/smart_mode.*` | 硬过滤、ATR 特征、统一候选池与智能配置 |
| `src/app/smart_runner.*` | 市场扫描、执行候选池计划、同步自动 bot 参数 |
| `src/core/trend_following.h`、`market_wake.h` | 因果趋势状态、突破确认、异动去重与限频 |
| `src/core/trend_decision.h` | 入场、出场、止损与反手的纯决策 |
| `src/core/trend_engine.*` | 仓位、风控闸门、异步执行、成交和对账 |
| `src/net/trading_client.*` | Binance REST、签名、限流与订单确认 |
| `src/net/rest_price_feed.*` | REST 行情、新鲜度保护与备用价格 |
| `src/gui/`、`src/headless/` | 两种运行入口、界面与状态恢复 |
| `src/core/journal.*` | 交易、扫描、候选池和过滤记录 |

行情采用 REST 轮询。网络失败与“没有仓位 / 标的不合格”分开处理。本地止损只向有利方向移动；交易所灾难止损保留开仓时的初始保护位置，自动 bot 强制开启。

## 构建与测试

需要 C++20 编译器、CMake 3.20+、vcpkg。核心依赖为 simdjson、curl、MbedTLS 和线程库，GUI 另需 Qt6 Widgets。

Windows 桌面版示例：

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -DCCBOT_WERROR=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Linux 构建 headless 与测试时，同时关闭 CMake GUI 目标和 vcpkg 默认 GUI 特性：

```bash
cmake -S . -B build/headless -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-linux -DCCBOT_BUILD_GUI=OFF -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON -DCCBOT_WERROR=ON
cmake --build build/headless --config Release
ctest --test-dir build/headless -C Release --output-on-failure
```

Linux 默认不构建 GUI；macOS 分别构建 Intel 与 Apple Silicon 的 `.app`，Linux 分别构建 x64 与 ARM64 headless。完整构建命令、运行库检查、数据目录及验证边界见 [平台说明](docs/PLATFORMS.zh-CN.md)。Qt 关闭默认特性，只启用 Widgets、线程、正则表达式和 PNG。所有测试已注册到 CTest，原有测试可执行文件和 sanitizer 流水线继续保留。

```text
python tools/check_suite_coverage.py
```

## 配置与迁移

桌面版通过 GUI 配置。Linux 无头版复制 `config.example.json` 为本机 `config.json`，填写测试网凭证，再选择启用智能模式或配置手动 bot。字段见 [Headless 说明](docs/HEADLESS.md)。

- 新配置使用 `"strategy": "trend_following"`；旧 `"turtle"` 名称作为输入别名兼容。
- 智能 `"strategy": "bare"`、`"vol"` 明确拒绝，不会自动改成趋势跟踪；手动 `bare_k` 继续有效。
- 旧状态若仍有已移除智能策略的持仓，程序拒绝恢复和启动交易，保留原状态文件。请使用旧版管理至平仓后再升级。
- GUI 数据位于可执行文件旁的 `data/`，headless 使用配置中的状态路径。凭证、配置、持仓、成交和日志不得随源码或发布包上传。

本程序连接真实合约订单接口。首次运行请使用测试网验证；交易所止损与本地风控不保证避免滑点或损失。

## 文档与发布

- [框架与升级基线](docs/ARCHITECTURE_BASELINE.zh-CN.md)
- [Headless 配置、运行与部署](docs/HEADLESS.md)
- `.github/workflows/`：Windows、macOS、Linux 构建检查与 sanitizer；标签构建保留 CI 产物，正式 Release 附件在核验后单独发布。
- `tools/leak_check.py`：发布包白名单和内嵌版本核验。
- `docs/BACKTEST.md`、`docs/NEGATIVE_RESULTS.md`、`docs/DYNAMIC_W.md` 为上游历史研究记录，不代表当前可执行功能。

许可证为 [MIT](LICENSE)，保留上游版权与许可声明。

当前源码 **v1.4.3** 增加杠杆超时查询恢复，修复写入开仓意图及利润归集网络等待后的最终检查，并清理无调用的旧代码和多余系统库链接。详见 [v1.4.3 发布说明](docs/releases/v1.4.3.md)。继续包含此前的成交核对、利润保护、BTC 积累及断点恢复功能。Windows、macOS 仅提供 GUI 及必要运行文件，无头程序只面向 Linux。正式下载以 GitHub Release 为准；`BUILD_VERIFICATION.json` 随 Release 单独提供源码、CI、依赖和附件摘要，`SHA256SUMS.txt` 用于核验压缩包。
