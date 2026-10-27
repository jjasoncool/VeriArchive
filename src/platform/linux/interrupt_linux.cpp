#include "platform/interrupt.h"

#include <signal.h>

#include <array>
#include <atomic>

namespace va::platform {
namespace {

std::atomic<CancelToken*> interrupt_target{nullptr};
std::atomic<int> handlers_running{0};
static_assert(std::atomic<CancelToken*>::is_always_lock_free && std::atomic<int>::is_always_lock_free,
    "a signal handler may use only lock-free atomics");

// Ctrl+C; a request to end, as at shutdown; the terminal closed or the user
// logged out (decision 0022, amended 2026-10-27).
constexpr std::array<int, 3> interrupt_signals{SIGINT, SIGTERM, SIGHUP};
std::array<struct sigaction, 3> earlier_actions{};
std::array<bool, 3> taken{};

// Only atomics, which a signal handler may use. The count tells the end of
// the operation, on another thread, that this handler still reads the token.
void on_signal(int)
{
    handlers_running.fetch_add(1);
    if (auto* token = interrupt_target.load()) {
        token->cancel();
    }
    handlers_running.fetch_sub(1);
}

} // namespace

CancelOnInterrupt::CancelOnInterrupt(CancelToken& token)
{
    interrupt_target.store(&token);
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    for (std::size_t index = 0; index < interrupt_signals.size(); ++index) {
        // A signal the process was started to ignore stays ignored: nohup
        // keeps an operation running after the terminal closes, and a shell
        // ignores Ctrl+C for a command it runs in the background.
        struct sigaction earlier {};
        taken[index] = sigaction(interrupt_signals[index], nullptr, &earlier) == 0 && earlier.sa_handler != SIG_IGN &&
                       sigaction(interrupt_signals[index], &action, &earlier_actions[index]) == 0;
    }
}

CancelOnInterrupt::~CancelOnInterrupt()
{
    for (std::size_t index = 0; index < interrupt_signals.size(); ++index) {
        if (taken[index]) {
            sigaction(interrupt_signals[index], &earlier_actions[index], nullptr);
            taken[index] = false;
        }
    }
    interrupt_target.store(nullptr);
    // A handler that began before, on another thread, ends in a moment.
    while (handlers_running.load() != 0) {
    }
}

} // namespace va::platform
