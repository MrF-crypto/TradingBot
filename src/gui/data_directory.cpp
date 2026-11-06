#include "gui/data_directory.h"
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QStringList>

namespace ccg {
namespace {
QFileInfoList entries(const QString& directory) {
    return QDir(directory).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
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
        if (name == "ccbot.lock" || name == ".write_test") continue;
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
                                                   const QString& oldAppData) {
    PreparedDataDirectory result;
    result.path = macos_portable_data_path(executableDirectory);
    const auto parent = QFileInfo(result.path).absolutePath();
    if (QFileInfo(result.path).isSymLink() || (QFileInfo::exists(result.path) && !QFileInfo(result.path).isDir())) {
        result.error = "data 必须是普通文件夹，请核对：\n" + result.path;
        return result;
    }
    if (QFileInfo::exists(result.path) && !QDir(result.path).isReadable()) {
        result.error = "data 目录不可读，未改用空白目录：\n" + result.path;
        return result;
    }
    QTemporaryFile writable(QDir(parent).filePath(".TradingBot-write-XXXXXX"));
    if (!writable.open() || writable.write("1", 1) != 1 || !writable.flush()) {
        result.error = "程序旁的目录不可写，无法使用同目录 data。请将整个程序文件夹移到可写位置后运行：\n" + parent;
        return result;
    }
    if (!lock_directory(result, QDir(parent).filePath(".TradingBot-data.lock"))) return result;

    // A populated portable directory is authoritative. Never merge two accounts
    // or fill missing state from an older system directory after it starts trading.
    if (QFileInfo::exists(result.path) && !entries(result.path).isEmpty()) {
        QTemporaryFile probe(QDir(result.path).filePath(".TradingBot-write-XXXXXX"));
        if (!probe.open() || probe.write("1", 1) != 1 || !probe.flush())
            result.error = "data 目录不可写，未切换到其他数据目录：\n" + result.path;
        return result;
    }

    // v1.3.3 used an existing bundle-local data directory ahead of AppDataLocation.
    QString source;
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
        source = candidate;
        break;
    }
    if (source.isEmpty()) {
        if (!QDir().mkpath(result.path)) result.error = "无法创建 data：\n" + result.path;
        return result;
    }
    if (!lock_directory(result, QDir(source).filePath("ccbot.lock"))) return result;
    QTemporaryDir staging(QDir(parent).filePath(".TradingBot-migrate-XXXXXX"));
    if (!staging.isValid()) { result.error = "无法创建数据迁移暂存目录：\n" + parent; return result; }
    if (!copy_tree(source, staging.path(), result.error)) return result;
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
