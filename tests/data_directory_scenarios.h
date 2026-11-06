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
    const auto restore = [](const QString& app, const QString& old) {
        ccg::DataDirectoryOptions options; options.mode = ccg::DataStartMode::Restore;
        return ccg::prepare_macos_data_directory(app, old, options);
    };
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/中文 程序/TradingBot.app/Contents/MacOS";
        check_path(QDir().mkpath(appDir), "fixture directory");
        const auto target = test.path() + "/中文 程序/data";
        check_path(ccg::macos_portable_data_path(appDir) == target, "data is beside .app, including Unicode and spaces");
        check_path(ccg::macos_portable_data_path(test.path() + "/bare") == test.path() + "/bare/data", "unbundled executable uses sibling data");
        auto ready = restore(appDir, test.path() + "/absent");
        check_path(ready.error.isEmpty() && ready.path == target && QDir(target).exists(), "new installation creates portable data");
        check_path(!QDir(appDir + "/data").exists(), "new app bundle remains unchanged");
        auto second = restore(appDir, test.path() + "/absent");
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
            auto ready = restore(appDir, old);
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
        auto again = restore(appDir, old);
        check_path(again.error.isEmpty() && again.note.isEmpty() && read(again.path + "/sar_bots.json") == "new-account-state" &&
            !QFile::exists(again.path + "/unexpected.json"), "existing portable data is authoritative on later starts");
    }
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        const auto old = test.path() + "/system";
        put(appDir + "/data/sar_bots.json", "legacy-bundle");
        put(old + "/sar_bots.json", "different-system-data");
        auto ready = restore(appDir, old);
        check_path(ready.error.isEmpty() && read(ready.path + "/sar_bots.json") == "legacy-bundle",
            "legacy bundle data keeps v1.3.3 source precedence");
    }
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        const auto old = test.path() + "/system";
        QDir().mkpath(appDir + "/data");
        put(old + "/sar_bots.json", "unrelated-system-account");
        auto ready = restore(appDir, old);
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
        auto ready = restore(appDir, old);
        check_path(!ready.error.isEmpty() && !QDir(ready.path).exists() && read(old + "/sar_bots.json") == "held-position",
            "busy source is not copied or replaced with an empty account");
    }
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        QDir().mkpath(appDir); put(test.path() + "/pkg/data", "not-a-directory");
        auto ready = restore(appDir, test.path() + "/absent");
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
        auto ready = restore(appDir, old);
        check_path(!ready.error.isEmpty() && !QDir(ready.path).exists() && read(old + "/sar_bots.json") == "checkpoint",
            "failed migration never publishes a partial data directory");
        check_path(QDir(test.path() + "/pkg").entryList({".TradingBot-migrate-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
            "failed staging tree is cleaned, original files stay intact");
    }
#endif
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        const auto old = test.path() + "/system";
        QDir().mkpath(appDir); put(old + "/sar_bots.json", "old-position-and-checkpoint");
        ccg::DataDirectoryOptions options;
        auto pending = ccg::prepare_macos_data_directory(appDir, old, options);
        check_path(pending.error.isEmpty() && pending.needsChoice && pending.source == old &&
            !QDir(pending.path).exists() && read(old + "/sar_bots.json") == "old-position-and-checkpoint",
            "first launch asks before importing old data and performs no account writes");
        pending = {};
        options.mode = ccg::DataStartMode::Fresh;
        auto fresh = ccg::prepare_macos_data_directory(appDir, old, options);
        const auto slot = fresh.credentialSlot;
        check_path(fresh.error.isEmpty() && !fresh.needsChoice && slot.startsWith("binance-api/") &&
            !QFile::exists(fresh.path + "/sar_bots.json") && read(old + "/sar_bots.json") == "old-position-and-checkpoint",
            "fresh profile skips legacy migration and has independent persistent credentials");
        const auto data = fresh.path;
        fresh = {};
        options.mode = ccg::DataStartMode::Ask;
        auto resumed = ccg::prepare_macos_data_directory(appDir, old, options);
        check_path(resumed.error.isEmpty() && !resumed.needsChoice && resumed.credentialSlot == slot &&
            !QFile::exists(data + "/sar_bots.json"), "fresh profile survives restart without resurrecting old data or keys");
        resumed = {};
        put(data + "/.tradingbot-profile.json", "broken");
        auto broken = ccg::prepare_macos_data_directory(appDir, old, options);
        check_path(!broken.error.isEmpty() && broken.credentialSlot.isEmpty(), "damaged profile identity blocks credential loading");
    }
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/AppTranslocation/random/d/TradingBot.app/Contents/MacOS";
        QDir().mkpath(appDir);
        ccg::DataDirectoryOptions options;
        options.fallbackDirectory = test.path() + "/home/TradingBot/data";
        auto fresh = ccg::prepare_macos_data_directory(appDir, test.path() + "/missing", options);
        const auto slot = fresh.credentialSlot;
        check_path(fresh.error.isEmpty() && fresh.path == options.fallbackDirectory && !slot.isEmpty() &&
            !QDir(ccg::macos_portable_data_path(appDir)).exists(), "translocated startup uses writable data without modifying the bundle");
        options.rememberedDirectory = fresh.path;
        options.rememberedCredentialSlot = slot;
        fresh = {};
        const auto movedApp = test.path() + "/moved/TradingBot.app/Contents/MacOS";
        QDir().mkpath(movedApp);
        auto moved = ccg::prepare_macos_data_directory(movedApp, test.path() + "/missing", options);
        check_path(moved.error.isEmpty() && moved.path == options.rememberedDirectory && moved.credentialSlot == slot &&
            !QDir(ccg::macos_portable_data_path(movedApp)).exists(), "remembered data prevents a second blank profile after app relocation");
        moved = {};
        check_path(QFile::remove(options.rememberedDirectory + "/.tradingbot-profile.json"), "missing identity fixture");
        put(options.rememberedDirectory + "/sar_bots.json", "retained-state-with-missing-identity");
        auto missingIdentity = ccg::prepare_macos_data_directory(movedApp, test.path() + "/missing", options);
        check_path(!missingIdentity.error.isEmpty(), "lost profile marker cannot fall back to legacy API credentials");
        missingIdentity = {};
        check_path(QFile::remove(options.rememberedDirectory + "/sar_bots.json"), "empty remembered directory fixture");
        auto emptied = ccg::prepare_macos_data_directory(movedApp, test.path() + "/missing", options);
        check_path(!emptied.error.isEmpty(), "empty remembered profile is not silently reinitialized");
        emptied = {};
        options.rememberedDirectory = test.path() + "/lost-data";
        auto lost = ccg::prepare_macos_data_directory(movedApp, test.path() + "/missing", options);
        check_path(!lost.error.isEmpty() && !QDir(ccg::macos_portable_data_path(movedApp)).exists(),
            "missing remembered data does not silently reset monitoring");
    }
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        QDir().mkpath(appDir);
        auto first = ccg::prepare_macos_data_directory(appDir, test.path() + "/missing");
        const auto slot = first.credentialSlot;
        const auto data = first.path;
        first = {};
        put(data + "/sar_bots.json", "existing-held-position");
        ccg::DataDirectoryOptions options; options.mode = ccg::DataStartMode::Fresh;
        auto refused = ccg::prepare_macos_data_directory(appDir, test.path() + "/missing", options);
        check_path(!refused.error.isEmpty() && read(data + "/sar_bots.json") == "existing-held-position",
            "fresh selection cannot overwrite an existing account");
        refused = {};
        options.mode = ccg::DataStartMode::Ask;
        options.rememberedDirectory = test.path() + "/unrelated";
        auto existing = ccg::prepare_macos_data_directory(appDir, test.path() + "/missing", options);
        check_path(existing.error.isEmpty() && existing.path == data && existing.credentialSlot == slot,
            "populated sidecar remains authoritative over unrelated remembered location");
    }
#ifndef Q_OS_WIN
    {
        QTemporaryDir test;
        const auto parent = test.path() + "/readonly";
        const auto appDir = parent + "/TradingBot.app/Contents/MacOS";
        QDir().mkpath(appDir);
        const auto permissions = QFileInfo(parent).permissions();
        check_path(QFile::setPermissions(parent, QFileDevice::ReadOwner | QFileDevice::ExeOwner), "readonly parent fixture");
        ccg::DataDirectoryOptions options;
        options.fallbackDirectory = test.path() + "/writable/data";
        auto ready = ccg::prepare_macos_data_directory(appDir, test.path() + "/missing", options);
        QFile::setPermissions(parent, permissions);
        check_path(ready.error.isEmpty() && ready.path == options.fallbackDirectory && !ready.credentialSlot.isEmpty(),
            "real readonly application parent automatically falls back for a new installation");
    }
    {
        QTemporaryDir test;
        const auto appDir = test.path() + "/pkg/TradingBot.app/Contents/MacOS";
        const auto data = test.path() + "/pkg/data";
        QDir().mkpath(appDir); put(data + "/sar_bots.json", "held-position");
        const auto permissions = QFileInfo(data).permissions();
        QFile::setPermissions(data, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        ccg::DataDirectoryOptions options; options.fallbackDirectory = test.path() + "/fallback/data";
        auto ready = ccg::prepare_macos_data_directory(appDir, test.path() + "/missing", options);
        QFile::setPermissions(data, permissions);
        check_path(!ready.error.isEmpty() && ready.path == data && !QDir(options.fallbackDirectory).exists() &&
            read(data + "/sar_bots.json") == "held-position", "readonly populated data never switches to a blank account");
    }
#endif
    return ok;
}
