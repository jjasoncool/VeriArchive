#include "platform/interrupt.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

namespace va::platform {
namespace {

std::atomic<CancelToken*> interrupt_target{nullptr};

BOOL WINAPI on_console_event(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        if (auto* token = interrupt_target.load()) {
            token->cancel();
            return TRUE; // handled: keep running until the engine stops cleanly
        }
    }
    return FALSE;
}

} // namespace

void cancel_on_interrupt(CancelToken& token)
{
    interrupt_target.store(&token);
    SetConsoleCtrlHandler(on_console_event, TRUE);
}

} // namespace va::platform
