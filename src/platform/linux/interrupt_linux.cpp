#include "platform/interrupt.h"

#include <atomic>
#include <csignal>

namespace va::platform {
namespace {

std::atomic<CancelToken*> interrupt_target{nullptr};

// Only an atomic store happens here, which is safe inside a signal handler.
void on_signal(int)
{
    if (auto* token = interrupt_target.load()) {
        token->cancel();
    }
}

} // namespace

void cancel_on_interrupt(CancelToken& token)
{
    interrupt_target.store(&token);
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

} // namespace va::platform
