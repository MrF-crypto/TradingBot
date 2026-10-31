# TradingBot · 趋势跟踪

面向 Binance USDT-M 永续合约的 C++20 交易框架。Qt6 桌面版与配置驱动的 headless 版共用引擎和智能执行层。基于 [CCBot v5.11.0](https://github.com/MrF-crypto/CCBot) 整理和升级，作为独立项目从 **v1.0.0** 开始发布。

## 下载与运行

从 [Releases](https://github.com/MrF-crypto/TradingBot/releases) 下载 Windows x64 便携包，完整解压到可写目录后运行 `TradingBot.exe`。DLL、`platforms/` 和许可证文件应与程序一起保留，不要只复制 EXE。首版同时包含 `ccbot_headless.exe`、配置模板和文档；Linux/macOS 可按下文从源码构建。

桌面版首次启动后设置测试网账户。无界面版将 `config.example.json` 复制为 `config.json` 并填写本机配置，参考 [Headless 说明](docs/HEADLESS.md)。下载文件可用同一 Release 的 `SHA256SUMS.txt` 核验。

```powershell
.\ccbot_headless.exe --version
.\TradingBot.exe --check-runtime
```

这两个检查不会读取账户配置或提交订单。首版变化与验证范围见 [v1.0.0 发布说明](docs/releases/v1.0.0.md)。

## 智能模式

智能模式只保留**趋势跟踪**：

```text
扫描与异动唤醒 → 候选排序 → 自身趋势许可 → 突破确认 → 组合预算 → 初始止损与追踪退出
```

智能模板默认使用 1h 信号、EMA20/60 的方向/斜率/分离度、12 根高低价唐奇安通道收盘突破、ATR14 与 3 倍 ATR 初始/移动止损。上涨许可做多、下跌许可做空，震荡或过渡等待；历史过热与相对强弱只参与分组排序，不预测反转。另可显式选择盘中突破或固定 15m / 30m 周期作独立实验。

常规扫描为每小时整点后 1 分钟；另每分钟轻量观察双向价格异动，最多每五分钟触发一次合并重扫。异动只唤醒扫描，不越过信号与风控。默认单笔计划风险 1U、组合 6U、同向 4U、单笔名义上限 100U，费用/滑点估算 20bps；这些是可修改的研究初值，不适配任何特定账户规模。

在途开仓计入预算；状态不明的委托保留预留，核对原订单终态与新鲜仓位后才释放。平仓不受新仓闸门限制。收盘确认模式可在加入候选后参与仍有效的已确认突破，盘中模式仍需先回到通道内上膛。持仓不会因排名下降被移除。智能模式默认关闭。

第一版实现与边界见 [趋势跟踪 V1](docs/TREND_FOLLOWING_V1.zh-CN.md)。单元/场景测试不构成盈利验证；历史回放、交易成本及样本外验证尚待开展。

原智能裸K埋伏、波动顺势及其设置、调度和执行分支已移除。**手动 bot 的趋势跟踪、抛物线 SAR、纯裸K继续保留**；手动纯裸K与已移除的智能埋伏是不同功能。

## 框架

| 模块 | 职责 |
| --- | --- |
| `src/core/smart_mode.*` | 特征、评分、分段、候选池与智能配置 |
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

仅构建 headless 与测试时，同时关闭 CMake GUI 目标和 vcpkg 默认 GUI 特性：

```powershell
cmake -S . -B build/headless -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake -DCCBOT_BUILD_GUI=OFF -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON -DCCBOT_WERROR=ON
cmake --build build/headless --config Release
ctest --test-dir build/headless -C Release --output-on-failure
```

Linux 默认不构建 GUI；macOS 按对应 vcpkg triplet 构建。Qt 关闭默认特性，只启用 Widgets、线程、正则表达式和 PNG，避免默认引入未使用的 Qt Network、SQL、测试库等模块。所有测试已注册到 CTest，原有测试可执行文件和 sanitizer 流水线继续保留。

```text
python tools/check_suite_coverage.py
```

## 配置与迁移

复制 `config.example.json` 为本机 `config.json`，填写测试网凭证，再选择启用智能模式或配置手动 bot。字段见 [Headless 说明](docs/HEADLESS.md)。

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
