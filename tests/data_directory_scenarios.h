#pragma once
#include "gui/data_directory.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QLockFile>
#include <cstdio>

inline bool test_data_directory_scenarios() {
    bool ok = true;
    auto check_path = [&](bool passed, const char* what) {
        if (!passed) { std::printf("FAIL data directory: %s\n", what); ok = false; }
    };
    const auto put = [](const QString& path, const QByteArray& contents) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
    };
    const auto read = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/中文 程序/TradingBot.app/Contents/MacOS";
        check_path(QDir().mkpath(appDir), "fixture directory");
        const auto target = test.path() + "/中文 程序/data";
        check_path(ccg::macos_portable_data_path(appDir) == target, "data is beside .app, including Unicode and spaces");
        check_path(ccg::macos_portable_data_path(test.path() + "/bare") == test.path() + "/bare/data", "unbundled executable uses sibling data");
        auto ready = ccg::prepare_macos_data_directory(appDir, test.path() + "/absent");
        check_path(ready.error.isEmpty() && ready.path == target && QDir(target).exists(), "new installation creates portable data");
        check_path(!QDir(appDir + "/data").exists(), "new app bundle remains unchanged");
        auto second = ccg::prepare_macos_data_directory(appDir, test.path() + "/absent");
        check_path(!second.error.isEmpty(), "concurrent startup cannot race directory preparation");
    }
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        const auto old = test.path() + "/Application Support/CCGMonitor/TradingBot";
        QDir().mkpath(appDir);
        const QStringList files{"sar_bots.json", "ccg_smart.json", "ccg_smart_state.json", "ccg_funding.json",
            "ccg_trades.json", "ccg_trade_stats.json", "ccg_settings.json", "ccg_creds.dat",
            "logs/ccg_20261106.log", "journal/trades.csv", "journal/archive/scan.csv"};
        for (const auto& file : files) check_path(put(old + "/" + file, file.toUtf8() + "-fixture\n"), "source fixture");
        {
            auto ready = ccg::prepare_macos_data_directory(appDir, old);
            check_path(ready.error.isEmpty() && !ready.note.isEmpty(), "system data migrates in full");
            for (const auto& file : files) {
                check_path(read(ready.path + "/" + file) == file.toUtf8() + "-fixture\n", "every data file and nested journal copied exactly");
                check_path(read(old + "/" + file) == file.toUtf8() + "-fixture\n", "old data retained as backup");
            }
            check_path(!QFile::exists(ready.path + "/ccbot.lock"), "source process lock is not copied");
            QLockFile oldProcess(old + "/ccbot.lock"); oldProcess.setStaleLockTime(0);
            check_path(!oldProcess.tryLock(0), "old app cannot start on source during first migrated session");
        }
        put(test.path() + "/pkg/data/sar_bots.json", "new-account-state");
        put(old + "/unexpected.json", "must-not-merge");
        auto again = ccg::prepare_macos_data_directory(appDir, old);
        check_path(again.error.isEmpty() && again.note.isEmpty() && read(again.path + "/sar_bots.json") == "new-account-state" &&
            !QFile::exists(again.path + "/unexpected.json"), "existing portable data is authoritative on later starts");
    }
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        const auto old = test.path() + "/system";
        put(appDir + "/data/sar_bots.json", "legacy-bundle");
        put(old + "/sar_bots.json", "different-system-data");
        auto ready = ccg::prepare_macos_data_directory(appDir, old);
        check_path(ready.error.isEmpty() && read(ready.path + "/sar_bots.json") == "legacy-bundle",
            "legacy bundle data keeps v1.3.3 source precedence");
    }
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        const auto old = test.path() + "/system";
        QDir().mkpath(appDir + "/data");
        put(old + "/sar_bots.json", "unrelated-system-account");
        auto ready = ccg::prepare_macos_data_directory(appDir, old);
        check_path(ready.error.isEmpty() && !QFile::exists(ready.path + "/sar_bots.json"),
            "explicit empty legacy bundle data does not import a different system account");
    }
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        const auto old = test.path() + "/system";
        QDir().mkpath(appDir); put(old + "/sar_bots.json", "held-position");
        QLockFile active(old + "/ccbot.lock"); active.setStaleLockTime(0);
        check_path(active.tryLock(0), "source running fixture");
        auto ready = ccg::prepare_macos_data_directory(appDir, old);
        check_path(!ready.error.isEmpty() && !QDir(ready.path).exists() && read(old + "/sar_bots.json") == "held-position",
            "busy source is not copied or replaced with an empty account");
    }
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        QDir().mkpath(appDir); put(test.path() + "/pkg/data", "not-a-directory");
        auto ready = ccg::prepare_macos_data_directory(appDir, test.path() + "/absent");
        check_path(!ready.error.isEmpty() && read(ready.path) == "not-a-directory", "invalid destination is not overwritten or silently redirected");
    }
#ifndef Q_OS_WIN
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        const auto old = test.path() + "/system";
        QDir().mkpath(appDir); put(old + "/sar_bots.json", "checkpoint");
        put(test.path() + "/external", "outside");
        check_path(QFile::link(test.path() + "/external", old + "/linked"), "linked source fixture");
        auto ready = ccg::prepare_macos_data_directory(appDir, old);
        check_path(!ready.error.isEmpty() && !QDir(ready.path).exists() && read(old + "/sar_bots.json") == "checkpoint",
            "failed migration never publishes a partial data directory");
        check_path(QDir(test.path() + "/pkg").entryList({".TradingBot-migrate-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
            "failed staging tree is cleaned, original files stay intact");
    }
#endif
    return ok;
}
