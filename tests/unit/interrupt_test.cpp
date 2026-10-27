// An interrupt on the command line cancels the operation that runs, as the
// GUI's Cancel does, so that it ends recorded and resumable (R-94, R-104);
// closing the console window, signing out and shutting down are interrupts
// too (decision 0022, amended 2026-10-27).
#include "test_support.h"

#include "platform/interrupt.h"
#include "platform/interrupt_target.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <thread>

#ifdef _WIN32
#include "platform/windows/interrupt_win.h"
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#endif

namespace {

using namespace std::chrono_literals;

// Whether `condition` holds within 10 seconds.
bool eventually(const std::function<bool()>& condition)
{
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > until) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// `interrupt`, run on a thread of its own as the system runs it, cancels the
// operation at once and returns only once `end` has ended the operation.
void holds_until_the_end(const std::function<bool()>& interrupt, const va::CancelToken& token,
    const std::function<void()>& end)
{
    std::atomic<bool> returned{false};
    std::atomic<bool> handled{false};
    std::thread system([&] {
        handled = interrupt();
        returned = true;
    });
    CHECK(eventually([&] { return token.cancelled(); }));
    std::this_thread::sleep_for(200ms);
    CHECK(!returned);
    end();
    system.join();
    CHECK(returned && handled);
}

// Plain C++, on every OS: an interrupt cancels the operation that runs; one
// that the system follows by ending the process (`hold`) returns once the
// operation has ended; while none runs, an interrupt cancels nothing and
// holds nothing.
void an_interrupt_cancels_and_may_hold_until_the_operation_ends()
{
    va::platform::InterruptTarget target;
    va::CancelToken token;
    CHECK(!target.interrupt(false) && !target.interrupt(true) && !token.cancelled());
    target.begin(token);
    CHECK(target.interrupt(false) && token.cancelled());
    token.reset();
    holds_until_the_end([&] { return target.interrupt(true); }, token, [&] { target.end(); });
    token.reset();
    CHECK(!target.interrupt(false) && !target.interrupt(true) && !token.cancelled());
}

#ifdef _WIN32
// With what Windows calls: Ctrl+C and Ctrl+Break cancel the operation and
// return; the console window closed, signing out and shutting down cancel it
// and return once it has ended, as Windows then ends the process. The
// session's end reaches it through a hidden top-level window as well, as
// Windows tells a program that loads user32.dll of it only so; a session end
// that is called off cancels nothing. Before and after an operation nothing
// is handled, and Windows ends the process as it would without VeriArchive.
void windows_interrupts_cancel_the_operation()
{
    using va::platform::windows::console_event;
    va::CancelToken token;
    CHECK(console_event(CTRL_C_EVENT) == 0 && console_event(CTRL_CLOSE_EVENT) == 0 && !token.cancelled());
    {
        const va::platform::CancelOnInterrupt interrupt(token);
        for (const DWORD event : {CTRL_C_EVENT, CTRL_BREAK_EVENT}) {
            token.reset();
            CHECK(console_event(event) != 0 && token.cancelled());
        }
    }
    token.reset();
    CHECK(console_event(CTRL_BREAK_EVENT) == 0 && !token.cancelled());
    for (const DWORD event : {CTRL_CLOSE_EVENT, CTRL_LOGOFF_EVENT, CTRL_SHUTDOWN_EVENT}) {
        token.reset();
        std::optional<va::platform::CancelOnInterrupt> interrupt(std::in_place, token);
        holds_until_the_end([&] { return console_event(event) != 0; }, token, [&] { interrupt.reset(); });
    }

    token.reset();
    std::optional<va::platform::CancelOnInterrupt> interrupt(std::in_place, token);
    const auto window = static_cast<HWND>(va::platform::windows::session_end_window());
    DWORD process = 0;
    CHECK(window != nullptr && IsWindow(window) && !IsWindowVisible(window) &&
          GetAncestor(window, GA_PARENT) == GetDesktopWindow() && GetWindowThreadProcessId(window, &process) != 0 &&
          process == GetCurrentProcessId());
    if (window == nullptr) {
        return;
    }
    // Answered at once: a window that held here would hold the session's end
    // for nothing (and the test would wait 5 seconds, not for ever).
    const auto answer = [&](UINT message, WPARAM ending) {
        DWORD_PTR result = 0xFFFF;
        return SendMessageTimeoutW(window, message, ending, ENDSESSION_LOGOFF, SMTO_BLOCK, 5000, &result) != 0
                   ? std::optional<DWORD_PTR>(result)
                   : std::nullopt;
    };
    CHECK(answer(WM_QUERYENDSESSION, 0) == std::optional<DWORD_PTR>(TRUE) && !token.cancelled());
    CHECK(answer(WM_ENDSESSION, FALSE) == std::optional<DWORD_PTR>(0) && !token.cancelled());
    holds_until_the_end(
        [&] { return SendMessageW(window, WM_ENDSESSION, TRUE, ENDSESSION_LOGOFF) == 0; }, token,
        [&] { interrupt.reset(); });
}
#else
// SIGINT (Ctrl+C), SIGTERM (as at shutdown) and SIGHUP (the terminal closed)
// cancel the operation that runs, each restored to its earlier action once
// the operation ends; a signal the process was started to ignore (nohup)
// stays ignored. Each signal is raised only while VeriArchive handles it.
void linux_signals_cancel_the_operation()
{
    va::CancelToken token;
    const auto action_of = [](int signal_number) {
        struct sigaction now {};
        sigaction(signal_number, nullptr, &now);
        return now.sa_handler;
    };
    for (const int signal_number : {SIGINT, SIGTERM, SIGHUP}) {
        struct sigaction earlier {};
        struct sigaction plain {};
        plain.sa_handler = SIG_DFL;
        sigemptyset(&plain.sa_mask);
        sigaction(signal_number, &plain, &earlier);
        token.reset();
        {
            const va::platform::CancelOnInterrupt interrupt(token);
            raise(signal_number);
            CHECK(token.cancelled());
        }
        CHECK(action_of(signal_number) == SIG_DFL);

        struct sigaction ignored {};
        ignored.sa_handler = SIG_IGN;
        sigemptyset(&ignored.sa_mask);
        sigaction(signal_number, &ignored, nullptr);
        token.reset();
        {
            const va::platform::CancelOnInterrupt interrupt(token);
            raise(signal_number);
            CHECK(!token.cancelled());
        }
        CHECK(action_of(signal_number) == SIG_IGN);
        sigaction(signal_number, &earlier, nullptr);
    }
}
#endif

} // namespace

void interrupt_tests()
{
    an_interrupt_cancels_and_may_hold_until_the_operation_ends();
#ifdef _WIN32
    windows_interrupts_cancel_the_operation();
#else
    linux_signals_cancel_the_operation();
#endif
}
