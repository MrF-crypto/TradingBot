# 平台构建与运行

平台范围为 Windows、macOS 与 Linux，不包括 iPhone/iPad。桌面版和 headless 共用策略、订单、风控及监控断点逻辑；不同平台的凭证存储不能直接互换，换系统需要重新填写凭证。

| 平台 | 架构 | 运行入口 | CI 原生环境 |
| --- | --- | --- | --- |
| Windows | x64 | `TradingBot.exe`、`ccbot_headless.exe` | Windows / MSVC |
| macOS | Intel x64 | `TradingBot.app`、`ccbot_headless` | `macos-15-intel` |
| macOS | Apple Silicon ARM64 | `TradingBot.app`、`ccbot_headless` | `macos-15` |
| Linux | x64、ARM64 | `ccbot_headless` | Ubuntu 24.04 对应架构 |

macOS 流水线的应用部署目标为 14.4；这是当前 Qt 6.12 支持的最低运行版本。Qt 的应用包和部署机制见 [Qt macOS 文档](https://doc.qt.io/qt-6/macos.html)。Intel 和 Apple Silicon 使用各自的原生包，无需依赖 Rosetta。CI runner 架构以 [GitHub 官方表格](https://docs.github.com/en/actions/reference/runners/github-hosted-runners) 为准。

Linux 预编译包面向 Ubuntu 24.04 及具备对应 glibc/C++ 运行库的系统，不承诺兼容 Alpine/musl 或更旧发行版；其他发行版可从源码构建。Linux 本次交付入口为 headless，未验证 Linux Qt 桌面环境。

## macOS 源码构建

安装 Xcode 命令行开发工具、CMake、Ninja 与 vcpkg。下面以 Apple Silicon 为例；Intel 把 `arm64-osx` 改为 `x64-osx`。`VCPKG_ROOT` 指向本机 vcpkg 目录。

```bash
cmake -S . -B build/macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCCBOT_WERROR=ON \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4 \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-osx
cmake --build build/macos --parallel 4
ctest --test-dir build/macos --output-on-failure
cmake --install build/macos --prefix "$PWD/pkg"
codesign --force --deep --sign - pkg/TradingBot.app
python3 tools/check_unix_package.py pkg --platform macos --arch arm64
```

默认 vcpkg macOS triplet 静态链接第三方库，Qt 的静态插件由 CMake 导入。共享 Qt 的源码构建使用 Qt 部署脚本复制依赖。CI 检查包的架构、依赖路径、离线启动与应用包签名；未满足检查的产物不上传。临时签名不等于 Apple Developer ID 签名或公证；本项目本轮未接入 Apple 公证发布。

新安装的 `.app` 使用 Qt `AppDataLocation`（通常是 `~/Library/Application Support/CCGMonitor/TradingBot`）保存数据，不向应用包内部写入。程序文件升级时保留该目录。旧版可执行文件旁已有的 `data/` 继续使用；从旧裸可执行文件迁移到 `.app` 时，停机备份并把完整数据目录复制到新数据目录，核对持仓和恢复状态后再启用。

## Linux 源码构建

安装 C++20 编译器、CMake、Ninja、pkg-config 及 vcpkg 所需构建工具。ARM64 使用 `arm64-linux`，x64 使用 `x64-linux`。

```bash
cmake -S . -B build/linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCCBOT_WERROR=ON \
  -DCCBOT_BUILD_GUI=OFF -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-linux
cmake --build build/linux --parallel 4
ctest --test-dir build/linux --output-on-failure
cmake --install build/linux --prefix "$PWD/pkg"
python3 tools/check_unix_package.py pkg --platform linux --arch x64
```

正式运行的配置、信号处理和服务部署见 [HEADLESS.md](HEADLESS.md)。Linux CI 用静态 vcpkg 依赖，并检查剩余动态依赖是否仅为标准系统运行库；不会把仅存在于构建机器的第三方库误当成已部署。

Ubuntu 24.04 也可使用系统开发库构建，无需 vcpkg：

```bash
sudo apt-get install build-essential cmake ninja-build pkg-config \
  libcurl4-openssl-dev libsimdjson-dev libmbedtls-dev
cmake -S . -B build/linux-system -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCCBOT_BUILD_GUI=OFF -DCCBOT_WERROR=ON
cmake --build build/linux-system --parallel 4
ctest --test-dir build/linux-system --output-on-failure
```

这个构建会链接系统第三方运行库，只适合安装了对应运行库的机器；不要作为独立便携包分发。`check_unix_package.py` 会拒绝把它误当成静态依赖发布包。

## 验证边界

平台流水线统一运行 CTest：桌面构建 18 套（含离线 Qt 监控表测试），headless 构建 17 套。ASan/UBSan 登记 17 个核心套件，TSan 登记 7 个实际并发套件，包括新增的并发断点保存测试。登记和脚本检查不等同于运行成功，实际验证结果见对应版本说明及 CI 日志。

`--version` 和 GUI `--check-runtime` 不加载账户、不读取持仓文件、不发送订单。本地 Windows 验证不能替代 macOS、Linux ARM64 的原生测试。跨平台成品须在相应流水线通过后发布；包内 `BUILD_INFO.json` 可追溯到准确源码提交及原生 CI，Release 附件提供 SHA256 校验清单。
