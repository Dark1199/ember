// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/crash.h"

#include <signal.h>
#include <unistd.h>
#include <unwind.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>

#include "core/log.h"

namespace ember {
namespace {

// Only async-signal-safe calls below: write(2), _Unwind_Backtrace and manual formatting.

void WriteRaw(const char* text, std::size_t length) {
    const int fd = LogFileDescriptor().load(std::memory_order_relaxed);
    if (fd >= 0) {
        [[maybe_unused]] const ssize_t n = ::write(fd, text, length);
    }
}

void WriteStr(const char* text) {
    WriteRaw(text, std::strlen(text));
}

void WriteHex(std::uintptr_t value) {
    char buffer[2 + sizeof(value) * 2 + 1];
    char* out = buffer;
    *out++ = '0';
    *out++ = 'x';
    bool started = false;
    for (int shift = static_cast<int>(sizeof(value) * 8) - 4; shift >= 0; shift -= 4) {
        const unsigned nibble = static_cast<unsigned>((value >> shift) & 0xF);
        if (nibble != 0 || started || shift == 0) {
            *out++ = "0123456789abcdef"[nibble];
            started = true;
        }
    }
    WriteRaw(buffer, static_cast<std::size_t>(out - buffer));
}

struct TraceState {
    int frames = 0;
};

_Unwind_Reason_Code OnFrame(_Unwind_Context* context, void* arg) {
    auto* state = static_cast<TraceState*>(arg);
    const std::uintptr_t pc = _Unwind_GetIP(context);
    if (pc != 0 && state->frames < 40) {
        WriteStr("    #");
        WriteHex(static_cast<std::uintptr_t>(state->frames));
        WriteStr(" pc ");
        WriteHex(pc);
        WriteStr("\n");
        ++state->frames;
    }
    return state->frames >= 40 ? _URC_END_OF_STACK : _URC_NO_REASON;
}

void WriteBacktrace() {
    TraceState state;
    _Unwind_Backtrace(OnFrame, &state);
}

void OnSignal(int sig, siginfo_t* info, void*) {
    WriteStr("FATAL signal ");
    WriteHex(static_cast<std::uintptr_t>(sig));
    WriteStr(" (");
    switch (sig) {
    case SIGSEGV: WriteStr("SIGSEGV"); break;
    case SIGABRT: WriteStr("SIGABRT"); break;
    case SIGBUS: WriteStr("SIGBUS"); break;
    case SIGILL: WriteStr("SIGILL"); break;
    case SIGFPE: WriteStr("SIGFPE"); break;
    default: WriteStr("other"); break;
    }
    WriteStr(") fault address ");
    WriteHex(reinterpret_cast<std::uintptr_t>(info ? info->si_addr : nullptr));
    WriteStr("\n");
    WriteBacktrace();
    WriteStr("(pc values are offsets into the loaded libraries; see /proc/self/maps equivalents "
             "when symbolizing)\n");
    // Restore the default action and re-raise so the system still records the crash.
    signal(sig, SIG_DFL);
    raise(sig);
}

void OnTerminate() {
    EMBER_ERROR("FATAL: std::terminate (uncaught exception or noexcept violation)");
    if (const std::exception_ptr eptr = std::current_exception()) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            EMBER_ERROR("  uncaught exception: %s", e.what());
        } catch (...) {
            EMBER_ERROR("  uncaught non-standard exception");
        }
    }
    WriteBacktrace();
    std::abort();
}

} // namespace

void InstallCrashHandlers() {
    // Run handlers on their own stack so a stack overflow can still be reported.
    static char alt_stack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    sigaltstack(&ss, nullptr);

    struct sigaction action{};
    action.sa_sigaction = OnSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) {
        sigaction(sig, &action, nullptr);
    }
    std::set_terminate(OnTerminate);
}

} // namespace ember
