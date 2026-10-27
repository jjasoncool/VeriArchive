#include "platform/interrupt.h"
#include "platform/interrupt_target.h"
#include "platform/windows/interrupt_win.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <iostream>

namespace va::platform {
namespace {

InterruptTarget& target()
{
    static InterruptTarget operation;
    return operation;
}

std::atomic<HWND> session_window{nullptr};

constexpr wchar_t session_window_class[] = L"VeriArchiveSessionEnd";

BOOL WINAPI on_console_event(DWORD event)
{
    return windows::console_event(event) != 0 ? TRUE : FALSE;
}

LRESULT CALLBACK session_window_procedure(HWND window, UINT message, WPARAM ending, LPARAM how)
{
    if (message == WM_ENDSESSION) {
        // The session ends (signing out, shutting down, or a restart that an
        // installer asked for): Windows may end the process as soon as this
        // returns. WM_QUERYENDSESSION, before, lets it end (DefWindowProcW),
        // as it may still be called off.
        if (ending != FALSE) {
            try {
                (void)target().interrupt(true);
            } catch (...) {
            }
        }
        return 0;
    }
    return DefWindowProcW(window, message, ending, how);
}

DWORD WINAPI session_window_thread(void* ready)
{
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = session_window_procedure;
    window_class.hInstance = instance;
    window_class.lpszClassName = session_window_class;
    HWND window = nullptr;
    if (RegisterClassExW(&window_class) != 0) {
        // Top-level and never shown: Windows sends the session's end to such
        // a window, not to a message-only one.
        window = CreateWindowExW(0, session_window_class, L"VeriArchive", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
            nullptr, instance, nullptr);
    }
    session_window.store(window);
    SetEvent(static_cast<HANDLE>(ready));
    if (window == nullptr) {
        return 0;
    }
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        DispatchMessageW(&message);
    }
    return 0;
}

// Once per process, with the first operation: the console control handler,
// and the thread of the window that hears of the session's end. A window that
// cannot be made leaves the console's events, which still cover Ctrl+C,
// Ctrl+Break and the window closed.
void listen_once()
{
    static const bool listening = [] {
        SetConsoleCtrlHandler(on_console_event, TRUE);
        // Kept for the process's life: the thread sets it, however late.
        const auto ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ready == nullptr) {
            return false;
        }
        const auto thread = CreateThread(nullptr, 0, session_window_thread, ready, 0, nullptr);
        if (thread == nullptr) {
            return false;
        }
        CloseHandle(thread);
        WaitForSingleObject(ready, 5000);
        return true;
    }();
    (void)listening;
}

} // namespace

namespace windows {

int console_event(unsigned long event)
{
    try {
        switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            return target().interrupt(false) ? 1 : 0;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            return target().interrupt(true) ? 1 : 0;
        default:
            return 0;
        }
    } catch (...) {
        return 0;
    }
}

void* session_end_window()
{
    return session_window.load();
}

} // namespace windows

CancelOnInterrupt::CancelOnInterrupt(CancelToken& token)
{
    target().begin(token);
    listen_once();
}

CancelOnInterrupt::~CancelOnInterrupt()
{
    // What the command said of the operation reaches a file or a pipe it was
    // sent to before a held interrupt lets Windows end the process.
    std::cout.flush();
    std::cerr.flush();
    target().end();
}

} // namespace va::platform
