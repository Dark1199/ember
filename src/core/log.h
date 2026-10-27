// SPDX-License-Identifier: MIT
#pragma once

#include <cstdarg>
#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace ember {

enum class LogLevel { Info, Warning, Error };

inline void Log(LogLevel level, const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
#if defined(__ANDROID__)
    const int priority = level == LogLevel::Error     ? ANDROID_LOG_ERROR
                         : level == LogLevel::Warning ? ANDROID_LOG_WARN
                                                      : ANDROID_LOG_INFO;
    __android_log_print(priority, "ember", "%s", buffer);
#else
    std::fprintf(level == LogLevel::Info ? stdout : stderr, "[ember] %s\n", buffer);
#endif
}

} // namespace ember

#define EMBER_LOG(...) ::ember::Log(::ember::LogLevel::Info, __VA_ARGS__)
#define EMBER_WARN(...) ::ember::Log(::ember::LogLevel::Warning, __VA_ARGS__)
#define EMBER_ERROR(...) ::ember::Log(::ember::LogLevel::Error, __VA_ARGS__)
