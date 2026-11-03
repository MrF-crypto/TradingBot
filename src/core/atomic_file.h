#pragma once
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ccbot {
struct SaveResult {
    bool ok = false;
    std::string error;
};

// Unique sibling, complete write, durable flush, then atomic replacement.
// The caller orders snapshots; this function never truncates the previous checkpoint.
inline SaveResult write_atomic_file(const std::string& filename, std::string_view data) {
    const std::filesystem::path target(std::u8string(filename.begin(), filename.end()));
    static std::atomic<unsigned long long> sequence{0};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    auto temporary = target;
    temporary += ".tmp." + std::to_string(process) + "." + std::to_string(++sequence);
    const auto fail = [&](const std::string& operation, const std::error_code& error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return SaveResult{false, operation + ": " + error.message()};
    };
#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {false, "创建临时文件: " + std::error_code(GetLastError(), std::system_category()).message()};
    size_t offset = 0;
    while (offset < data.size()) {
        DWORD count = 0;
        if (!WriteFile(file, data.data() + offset,
                       static_cast<DWORD>(std::min<size_t>(data.size() - offset, 1024 * 1024)), &count, nullptr) || count == 0) {
            const auto error = std::error_code(GetLastError(), std::system_category());
            CloseHandle(file);
            return fail("写入断点", error);
        }
        offset += count;
    }
    if (!FlushFileBuffers(file)) {
        const auto error = std::error_code(GetLastError(), std::system_category());
        CloseHandle(file);
        return fail("同步断点", error);
    }
    CloseHandle(file);
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return fail("替换断点", std::error_code(GetLastError(), std::system_category()));
#else
    const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) return {false, "创建临时文件: " + std::error_code(errno, std::generic_category()).message()};
    size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::write(file, data.data() + offset, std::min<size_t>(data.size() - offset, 1024 * 1024));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const auto error = std::error_code(count == 0 ? EIO : errno, std::generic_category());
            ::close(file);
            return fail("写入断点", error);
        }
        offset += static_cast<size_t>(count);
    }
    if (::fsync(file) != 0) {
        const auto error = std::error_code(errno, std::generic_category());
        ::close(file);
        return fail("同步断点", error);
    }
    if (::close(file) != 0) return fail("关闭断点", std::error_code(errno, std::generic_category()));
    if (::rename(temporary.c_str(), target.c_str()) != 0)
        return fail("替换断点", std::error_code(errno, std::generic_category()));
    const auto parent = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
    const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) return {false, "打开断点目录: " + std::error_code(errno, std::generic_category()).message()};
    const int synced = ::fsync(directory);
    const int saved_errno = errno;
    ::close(directory);
    if (synced != 0) return {false, "同步断点目录: " + std::error_code(saved_errno, std::generic_category()).message()};
#endif
    return {true, {}};
}
} // namespace ccbot
