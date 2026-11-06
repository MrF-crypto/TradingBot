#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QLockFile>
#include <QStandardPaths>
#include <QDir>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include "gui/main_window.h"
#include "gui/data_directory.h"
#include <QVariant>
#include "version.h"
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string_view(argv[1]) == "--version") {
        std::cout << "TradingBot " << ccbot::kVersion << std::endl;
        return 0;
    }
    QApplication::setStyle("Fusion");  // 让深色 QSS 完全接管，避免下拉框/勾选框等控件
                                       // 回退到系统原生浅色渲染，出现"一亮一暗"的效果
    QApplication app(argc, argv);
    // 离线检查 Qt 平台插件及依赖，不创建主窗口、不读取账户数据。
    if (argc > 1 && std::string_view(argv[1]) == "--check-runtime") {
        std::cout << "TradingBot " << ccbot::kVersion << " | Qt " << qVersion()
                  << " | runtime OK" << std::endl;
        return 0;
    }
    app.setApplicationName("TradingBot");
    // ⚠ 不要改这个名字：回退模式（程序目录不可写）下的数据目录路径由它决定，
    // 改了等于让老用户的凭证与持仓跟踪失联。它不上界面，用户看不到
    app.setOrganizationName("CCGMonitor");
#ifdef Q_OS_MAC
    // Native CI can verify the actual bundle path without preparing directories,
    // reading account state, accessing Keychain or starting the main window.
    if (argc > 1 && std::string_view(argv[1]) == "--print-data-directory") {
        std::cout << ccg::macos_portable_data_path(QCoreApplication::applicationDirPath()).toStdString() << std::endl;
        return 0;
    }
#endif

    // 窗口左上角 / 任务栏 / Alt-Tab 的图标。Qt 不会去读 exe 内嵌的 ICON 资源，
    // 得显式设一次。七个尺寸都塞进同一个 QIcon，让 Qt 按场景挑最合适的那张——
    // 只给一张大图让它自己缩，小尺寸下会糊（图标集本来就是分尺寸出图的，
    // ≤32px 用的是收到脸部的窄裁切，见 tools/make_icons.py）
    {
        QIcon ic;
        for (int px : {16, 24, 32, 48, 64, 128, 256})
            ic.addFile(QString(":/logo/logo_%1.png").arg(px));
        app.setWindowIcon(ic);
    }

    // 单实例锁：同一台机器双开会各自独立决策、对同一账户重复下单。
    // QLockFile 自带陈旧锁检测（崩溃残留的锁会被自动接管），正常运行的实例则拒绝二开。
    // 锁放数据目录（便携模式=程序目录data/）：同一份数据目录只允许一个实例
#ifdef Q_OS_MAC
    // Remember the real data path separately from account data: Gatekeeper may
    // assign a different read-only AppTranslocation path on the next launch.
    QSettings startupSettings;
    ccg::DataDirectoryOptions dataOptions;
    dataOptions.fallbackDirectory = QDir::home().filePath("TradingBot/data");
    dataOptions.rememberedDirectory = startupSettings.value("startup/dataDirectory").toString();
    dataOptions.rememberedCredentialSlot = startupSettings.value("startup/credentialSlot").toString();
    const auto oldAppData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    auto dataDirectory = ccg::prepare_macos_data_directory(QCoreApplication::applicationDirPath(), oldAppData, dataOptions);
    if (dataDirectory.needsChoice) {
        QMessageBox choose(QMessageBox::Question, "启动 TradingBot",
            "发现以前保存的数据。\n\n全新配置：重新填写 API，旧数据和旧凭证保留。\n"
            "恢复旧数据：继续使用原配置和监控断点。\n\n"
            "全新配置不会自动接管交易所已有仓位。\n\n数据将保存到：\n" + dataDirectory.path,
            QMessageBox::NoButton);
        auto* fresh = choose.addButton("全新配置", QMessageBox::ActionRole);
        auto* restore = choose.addButton("恢复旧数据", QMessageBox::AcceptRole);
        choose.addButton(QMessageBox::Cancel);
        choose.setDefaultButton(restore);
        choose.exec();
        if (choose.clickedButton() != fresh && choose.clickedButton() != restore) return 0;
        dataOptions.mode = choose.clickedButton() == fresh ? ccg::DataStartMode::Fresh : ccg::DataStartMode::Restore;
        dataDirectory = {}; // Release the preparation lock before rechecking and applying the choice.
        dataDirectory = ccg::prepare_macos_data_directory(QCoreApplication::applicationDirPath(), oldAppData, dataOptions);
    }
    if (!dataDirectory.error.isEmpty()) {
        QMessageBox::critical(nullptr, "无法准备数据目录", dataDirectory.error);
        return 1;
    }
    startupSettings.setValue("startup/dataDirectory", dataDirectory.path);
    startupSettings.setValue("startup/credentialSlot", dataDirectory.credentialSlot);
    startupSettings.sync();
    if (startupSettings.status() != QSettings::NoError) {
        QMessageBox::critical(nullptr, "无法保存启动设置", "无法记住数据目录，尚未载入账户：\n" + dataDirectory.path);
        return 1;
    }
    app.setProperty("tradingbotDataDirectory", dataDirectory.path);
    app.setProperty("tradingbotDataDirectoryNote", dataDirectory.note);
    app.setProperty("tradingbotCredentialSlot", dataDirectory.credentialSlot);
    app.setProperty("tradingbotSetupOnMissingCredentials", true);
#endif
    QString lockDir = ccg::MainWindow::portable_data_dir();
    QLockFile instanceLock(lockDir + "/ccbot.lock");
    if (!instanceLock.tryLock(100)) {
        QMessageBox::critical(nullptr, "TradingBot",
            "程序已经在运行中（同一账户双开会重复下单）。\n"
            "如果确认没有其他实例，删除锁文件后重试:\n" + lockDir + "/ccbot.lock");
        return 1;
    }

#ifdef Q_OS_MAC
    QFont f("Menlo", 12);      // macOS 没有 Consolas，Menlo 是系统等宽字体
#else
    QFont f("Consolas", 10);
#endif
    app.setFont(f);

    ccg::MainWindow w;
    w.show();
    return app.exec();
}
