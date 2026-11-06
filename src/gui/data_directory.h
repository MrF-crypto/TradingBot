#pragma once
#include <QString>
#include <QLockFile>
#include <memory>
#include <vector>

namespace ccg {
// Owns startup/source locks until the application exits. No account files are parsed.
struct PreparedDataDirectory {
    QString path, note, error;
    std::vector<std::unique_ptr<QLockFile>> locks;
};

QString macos_portable_data_path(const QString& executableDirectory);
PreparedDataDirectory prepare_macos_data_directory(const QString& executableDirectory,
                                                   const QString& oldAppData);
} // namespace ccg
