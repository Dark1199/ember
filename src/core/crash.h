// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ember {

/// Installs handlers for fatal signals (SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE) and for
/// uncaught C++ exceptions. Each one writes the reason and a raw backtrace (program counters) to
/// the log file set with SetLogFile, then lets the process die normally. Without this a native
/// crash on a phone without adb leaves no trace at all.
void InstallCrashHandlers();

} // namespace ember
