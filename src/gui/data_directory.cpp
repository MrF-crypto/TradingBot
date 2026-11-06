#include "gui/data_directory.h"
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

namespace ccg {
namespace {
QFileInfoList entries(const QString& directory) {
    return QDir(directory).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}

bool writable_directory(const QString& directory) {
    QTemporaryFile probe(QDir(directory).filePath(".TradingBot-write-XXXXXX"));
    return probe.open() && probe.write("1", 1) == 1 && probe.flush();
}

bool regular_directory(const QString& path, QString& error) {
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isDir())) {
        error = "数据目录必须是普通文件夹，请核对：\n" + path;
        return false;
    }
    if (info.exists() && !QDir(path).isReadable()) {
        error = "数据目录不可读，未改用空白目录：\n" + path;
        return false;
    }
    return true;
}

constexpr auto kProfileFile = ".tradingbot-profile.json";
bool read_profile(const QString& directory, PreparedDataDirectory& result) {
    result.credentialSlot.clear();
    const auto path = QDir(directory).filePath(kProfileFile);
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) return true;
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly)) {
        result.error = "无法读取配置身份，未载入其他 API：\n" + path;
        return false;
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    const auto id = doc.object().value("id").toString();
    const auto uuid = QUuid::fromString(id);
    if (!doc.isObject() || doc.object().value("version").toInt() != 1 || uuid.isNull() ||
        uuid.toString(QUuid::WithoutBraces) != id.toLower()) {
        result.error = "配置身份文件损坏，未载入其他 API：\n" + path;
        return false;
    }
    result.credentialSlot = "binance-api/" + uuid.toString(QUuid::WithoutBraces);
    return true;
}

bool create_profile(PreparedDataDirectory& result) {
    if (!QDir().mkpath(result.path)) { result.error = "无法创建数据目录：\n" + result.path; return false; }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto contents = QJsonDocument(QJsonObject{{"version", 1}, {"id", id}}).toJson();
    QSaveFile file(QDir(result.path).filePath(kProfileFile));
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit()) {
        result.error = "无法保存全新配置，尚未载入 API：\n" + result.path;
        return false;
    }
    result.credentialSlot = "binance-api/" + id;
    return true;
}

bool lock_directory(PreparedDataDirectory& result, const QString& path) {
    auto lock = std::make_unique<QLockFile>(path);
    // No age-only takeover of a running process, including a long journal copy.
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        result.error = "数据目录正在使用或无法加锁，请关闭使用该目录的旧程序后重试：\n" + path;
        return false;
    }
    result.locks.push_back(std::move(lock));
    return true;
}

bool copy_tree(const QString& source, const QString& destination, QString& error) {
    if (!QDir(source).isReadable()) {
        error = "旧数据目录不可读，未迁移：\n" + source;
        return false;
    }
    for (const auto& entry : entries(source)) {
        const auto name = entry.fileName();
        if (name == "ccbot.lock" || name == ".write_test" || name == ".TradingBot-startup.lock") continue;
        const auto target = QDir(destination).filePath(name);
        if (entry.isSymLink() || (!entry.isFile() && !entry.isDir())) {
            error = "旧数据中包含链接或特殊文件，未迁移，请先核对：\n" + entry.absoluteFilePath();
            return false;
        }
        if (entry.isDir()) {
            if (!QDir().mkpath(target)) { error = "无法创建迁移目录：\n" + target; return false; }
            if (!copy_tree(entry.absoluteFilePath(), target, error)) return false;
        } else {
            if (!QFile::copy(entry.absoluteFilePath(), target) || QFileInfo(target).size() != entry.size() ||
                QFileInfo(entry.absoluteFilePath()).lastModified() != entry.lastModified()) {
                error = "文件复制失败或复制期间发生变化，原数据保持不变：\n" + entry.absoluteFilePath();
                return false;
            }
        }
    }
    return true;
}
} // namespace

QString macos_portable_data_path(const QString& executableDirectory) {
    QDir directory(QDir(executableDirectory).absolutePath());
    // .app/Contents/MacOS is an implementation path; the user sees the .app itself.
    if (directory.dirName() == "MacOS") {
        auto parent = directory;
        if (parent.cdUp() && parent.dirName() == "Contents" && parent.cdUp() &&
            parent.dirName().endsWith(".app", Qt::CaseInsensitive) && parent.cdUp())
            directory = parent;
    }
    return QDir::cleanPath(directory.filePath("data"));
}

PreparedDataDirectory prepare_macos_data_directory(const QString& executableDirectory,
                                                   const QString& oldAppData,
                                                   const DataDirectoryOptions& options) {
    PreparedDataDirectory result;
    result.path = macos_portable_data_path(executableDirectory);
    const bool translocated = QDir::fromNativeSeparators(executableDirectory).contains("/AppTranslocation/");
    if (!translocated && !regular_directory(result.path, result.error)) return result;
    const bool portablePopulated = !translocated && QFileInfo::exists(result.path) && !entries(result.path).isEmpty();
    // A previously selected location takes precedence over creating another empty
    // account when macOS changes the app's temporary launch path or the app moves.
    if (!portablePopulated && !options.rememberedDirectory.isEmpty()) {
        result.path = QDir::cleanPath(options.rememberedDirectory);
        if (!QDir::isAbsolutePath(result.path) || !QFileInfo::exists(result.path)) {
            result.error = "上次使用的数据目录已移动或不可用，请先恢复该目录：\n" + result.path;
            return result;
        }
    } else if (!portablePopulated &&
               (translocated || !writable_directory(QFileInfo(result.path).absolutePath()))) {
        if (options.fallbackDirectory.isEmpty()) {
            result.error = "程序旁的目录不可写，请选择可写的数据目录：\n" + QFileInfo(result.path).absolutePath();
            return result;
        }
        result.path = QDir::cleanPath(options.fallbackDirectory);
        if (!QDir::isAbsolutePath(result.path) || !QDir().mkpath(QFileInfo(result.path).absolutePath())) {
            result.error = "无法创建数据存放位置：\n" + result.path;
            return result;
        }
    }
    if (!regular_directory(result.path, result.error)) return result;
    if (result.path != macos_portable_data_path(executableDirectory))
        result.note = "运行数据保存在 " + result.path + "；下次启动继续使用此目录";
    const auto parent = QFileInfo(result.path).absolutePath();
    const bool parentWritable = writable_directory(parent);
    const bool populated = QFileInfo::exists(result.path) && !entries(result.path).isEmpty();
    if (!parentWritable && (!populated || !writable_directory(result.path))) {
        result.error = "数据目录不可写，未切换账户状态：\n" + result.path;
        return result;
    }
    const auto lockPath = parentWritable ? QDir(parent).filePath(".TradingBot-data.lock")
                                        : QDir(result.path).filePath(".TradingBot-startup.lock");
    if (!lock_directory(result, lockPath)) return result;
    if (!regular_directory(result.path, result.error)) return result;
    if (QDir::cleanPath(options.rememberedDirectory) == result.path && !options.rememberedCredentialSlot.isEmpty()) {
        if (!read_profile(result.path, result)) return result;
        if (result.credentialSlot != options.rememberedCredentialSlot) {
            result.error = "已保存的配置身份与数据目录不一致，未载入其他 API：\n" + result.path;
            return result;
        }
    }

    // A populated portable directory is authoritative. Never merge two accounts
    // or fill missing state from an older system directory after it starts trading.
    // Recheck after taking the lock: another startup may have initialized this
    // directory after our preflight, then exited before we acquired the lock.
    if (QFileInfo::exists(result.path) && !entries(result.path).isEmpty()) {
        if (options.mode == DataStartMode::Fresh) {
            result.error = "该目录已有数据，全新配置不会覆盖它：\n" + result.path;
            return result;
        }
        if (!writable_directory(result.path))
            result.error = "data 目录不可写，未切换到其他数据目录：\n" + result.path;
        else read_profile(result.path, result);
        return result;
    }
    if (options.mode == DataStartMode::Fresh) {
        create_profile(result);
        return result;
    }

    // v1.3.3 used an existing bundle-local data directory ahead of AppDataLocation.
    const QStringList candidates{QDir(executableDirectory).filePath("data"), oldAppData};
    for (const auto& candidate : candidates) {
        if (candidate.isEmpty() || QDir::cleanPath(candidate) == result.path) continue;
        const QFileInfo info(candidate);
        if (!info.exists()) continue;
        if (info.isSymLink() || !info.isDir()) {
            result.error = "旧数据目录类型异常，未创建空白账户状态：\n" + candidate;
            return result;
        }
        if (!QDir(candidate).isReadable()) {
            result.error = "旧数据目录不可读，未创建空白账户状态：\n" + candidate;
            return result;
        }
        // Even an empty legacy bundle directory was authoritative in v1.3.3.
        // Falling through could resurrect a different account from system storage.
        result.source = candidate;
        break;
    }
    if (!result.source.isEmpty() && options.mode == DataStartMode::Ask) {
        result.needsChoice = true;
        return result;
    }
    if (result.source.isEmpty()) {
        create_profile(result);
        return result;
    }
    const auto source = result.source;
    if (!lock_directory(result, QDir(source).filePath("ccbot.lock")) || !read_profile(source, result)) return result;
    QTemporaryDir staging(QDir(parent).filePath(".TradingBot-migrate-XXXXXX"));
    if (!staging.isValid()) { result.error = "无法创建数据迁移暂存目录：\n" + parent; return result; }
    if (!copy_tree(source, staging.path(), result.error) || !read_profile(staging.path(), result)) return result;
    // The staging directory is published only when every file has copied. A failed
    // attempt cannot leave credentials beside a missing position checkpoint.
    if (QFileInfo::exists(result.path) && !QDir().rmdir(result.path)) {
        result.error = "迁移期间 data 已出现内容，未覆盖任何文件：\n" + result.path;
        return result;
    }
    if (!QDir().rename(staging.path(), result.path)) {
        result.error = "迁移完成但无法启用 data，原数据保持不变：\n" + result.path;
        return result;
    }
    staging.setAutoRemove(false);
    result.note = "已完整复制旧数据到 " + result.path + "（原目录保留：" + source + "）";
    return result;
}
} // namespace ccg
