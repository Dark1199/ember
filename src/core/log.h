// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <system_error>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace ember {

enum class LogLevel { Info, Warning, Error };

/// File descriptor of the on-device log file, or -1. Plain write(2) on an O_APPEND descriptor is
/// used (not stdio) so that lines are never buffered or lost, and so the crash handler can write
/// to the same file from a signal handler.
inline std::atomic<int>& LogFileDescriptor() noexcept {
    static std::atomic<int> fd{-1};
    return fd;
}

/// Seconds since the first log call; used to timestamp file lines.
inline double LogUptimeSeconds() noexcept {
    static const timespec start = [] {
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        return t;
    }();
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec - start.tv_sec) +
           static_cast<double>(now.tv_nsec - start.tv_nsec) * 1e-9;
}

/// Starts logging to `path` in addition to the platform log. An existing log is kept once as
/// "<path>.prev" so the previous run survives a crash-and-relaunch. Returns false if the file
/// cannot be created (logging then continues to logcat / stderr only).
inline bool SetLogFile(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path previous = path;
    previous += ".prev";
    std::filesystem::rename(path, previous, ec); // ignore failure: first run has no old log
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    const int old = LogFileDescriptor().exchange(fd);
    if (old >= 0) {
        ::close(old);
    }
    return true;
}

inline void Log(LogLevel level, const char* fmt, ...) {
    char message[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
#if defined(__ANDROID__)
    const int priority = level == LogLevel::Error     ? ANDROID_LOG_ERROR
                         : level == LogLevel::Warning ? ANDROID_LOG_WARN
                                                      : ANDROID_LOG_INFO;
    __android_log_print(priority, "ember", "%s", message);
#else
    std::fprintf(level == LogLevel::Info ? stdout : stderr, "[ember] %s\n", message);
#endif
    const int fd = LogFileDescriptor().load(std::memory_order_relaxed);
    if (fd >= 0) {
        char line[1100];
        const char tag = level == LogLevel::Error ? 'E' : level == LogLevel::Warning ? 'W' : 'I';
        const int n = std::snprintf(line, sizeof(line), "[%9.3f] %c %s\n", LogUptimeSeconds(), tag,
                                    message);
        if (n > 0) {
            const std::size_t length =
                n < static_cast<int>(sizeof(line)) ? static_cast<std::size_t>(n) : sizeof(line) - 1;
            [[maybe_unused]] const ssize_t written = ::write(fd, line, length);
        }
    }
}

} // namespace ember

#define EMBER_LOG(...) ::ember::Log(::ember::LogLevel::Info, __VA_ARGS__)
#define EMBER_WARN(...) ::ember::Log(::ember::LogLevel::Warning, __VA_ARGS__)
#define EMBER_ERROR(...) ::ember::Log(::ember::LogLevel::Error, __VA_ARGS__)
