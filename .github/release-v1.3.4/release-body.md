TradingBot v1.3.4 修复中文合约持续无行情的问题，细化行情异常提示，并让 macOS 桌面运行数据统一存放在 `.app` 同级的 `data/`。

### 主要变化

- 品种名称只转换 ASCII 英文字母，保留中文 UTF-8 字节，避免系统字符环境破坏行情订阅、缓存及恢复时的品种名称。
- 行情提示区分部分品种异常、全部品种异常和请求中断；悬浮详情显示具体品种及报价更新时间。有效报价的品种继续受各自信号和风控闸门约束。
- 旧存档已有乱码时停止恢复并保留原文件，不自动删除记录或猜测品种。附离线核对工具，仅为已核实空仓、无未决委托且唯一匹配的记录生成待审核副本和原文件备份。
- macOS 配置、持仓、监控断点、日志和 journal 统一存入 `TradingBot.app` 同级的 `data/`。首次迁移完整复制旧数据，保留原目录；复制完成后整体启用，文件锁阻止同时使用同一数据目录。

### 下载

| 文件 | 适用平台 | 内容 |
| --- | --- | --- |
| `tradingbot-v1.3.4-win64.zip` | Windows x64 | 桌面版、headless、运行库 |
| `tradingbot-v1.3.4-macos-x64.zip` | Intel Mac | `.app`、headless |
| `tradingbot-v1.3.4-macos-arm64.zip` | Apple Silicon Mac | `.app`、headless |
| `tradingbot-v1.3.4-linux-x64.zip` | Linux x64 | headless |
| `tradingbot-v1.3.4-linux-arm64.zip` | Linux ARM64 | headless |

macOS 运行基线为 15.0，应用使用临时签名，未做 Apple Developer ID 公证。Linux 以 Ubuntu 24.04 为基线，不适用于 Alpine/musl。所有包附配置模板、文档、离线恢复工具与依赖许可证。

### 升级

关闭旧程序并备份完整数据后再更新。Windows 保留原 `data/`。Mac 用户把 ZIP 原文件拷到 Mac 后解压至可写文件夹；首次启动且同级 `data/` 不存在或为空时，优先复制本应用包内旧 `Contents/MacOS/data/`，否则复制旧 Application Support 目录，原目录保留。已有非空同级 `data/` 直接沿用，不混合其他目录的数据。

如果实际数据在另一个旧 `.app` 或便携文件夹中，请先把完整 `data/` 复制到新版 `.app` 同级。API 凭证仍在系统钥匙串中，复制 `data/` 不会导出钥匙串。headless 继续按配置指定路径运行。

启动后核对持仓、止损保护、行情和监控进度。若提示旧存档中文乱码，本地策略尚未启动；该检查不会撤销交易所已有硬止损。保留原文件，按包内 `docs/SYMBOL_RECOVERY.zh-CN.md` 核对，不要删除整个数据目录。

### 验证与追溯

发布前检查结果及 CI 链接会在发布时补充。

新增回归覆盖中文名称与字符环境、部分行情异常、损坏存档拒绝、恢复工具及 macOS 完整迁移、源目录保留、已有数据优先、并发锁、复制失败和链接拒绝。各平台另通过 4 项恢复工具测试；所有委托回归使用模拟接口。

使用 `SHA256SUMS.txt` 核验下载；各包中的 `BUILD_INFO.json` 记录源码提交和 CI，`DEPENDENCIES.json` 记录依赖。`BUILD_VERIFICATION.json` 汇总发布核验结果。完整变更见 [版本说明](https://github.com/MrF-crypto/TradingBot/blob/v1.3.4/docs/releases/v1.3.4.md)。
