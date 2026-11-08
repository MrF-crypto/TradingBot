# TradingBot Linux 无头版本

按 x64 / ARM64 选择对应程序包。预编译版本面向 Ubuntu 24.04 及兼容 glibc/C++ 运行库的系统。

首次使用：

```bash
cp config.example.json config.json
chmod 600 config.json
# 编辑 config.json，填写自己的 API 凭证和策略参数。
./ccbot_headless ./config.json
```

先使用测试网验证配置。`./ccbot_headless --version` 只检查版本，不加载账户或提交交易。

升级时先退出旧进程，保留已有配置与完整状态文件，包括 `state_path` 对应文件及其 `.sar`、`.smart`、`.smart.profit` 等附属文件，不要用模板覆盖原配置。不要运行两个实例管理同一账户。

本包只含程序、配置模板、本说明和许可证；完整配置说明、策略文档及修复工具在 [GitHub 项目](https://github.com/MrF-crypto/TradingBot)。发布页的 `SHA256SUMS.txt` 用于核对压缩包，`BUILD_VERIFICATION.json` 记录源码、原生 CI 与依赖信息。
