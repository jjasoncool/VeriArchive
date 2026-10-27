#pragma once
// How Windows interrupts an operation of the command line (platform/
// interrupt.h), named here so that the tests can call what Windows calls.
// Without Windows headers; built only on Windows.

namespace va::platform::windows {

// The console control handler, as Windows calls it with `event`: Ctrl+C
// (CTRL_C_EVENT) and Ctrl+Break (CTRL_BREAK_EVENT) cancel the operation that
// runs and return at once; the console window closed (CTRL_CLOSE_EVENT), the
// user signing out (CTRL_LOGOFF_EVENT) and the computer shutting down
// (CTRL_SHUTDOWN_EVENT) cancel it and return once it has ended, as Windows
// ends the process when the handler returns. Nonzero when handled: zero when
// no operation runs, or for another event, which Windows then handles as it
// would without VeriArchive.
int console_event(unsigned long event);

// The hidden top-level window that hears of the session's end, made with the
// first operation: Windows tells a program that loads user32.dll, as this one
// does through its libraries, of signing out and shutting down by
// WM_QUERYENDSESSION and WM_ENDSESSION only, never by CTRL_LOGOFF_EVENT or
// CTRL_SHUTDOWN_EVENT. Its WM_ENDSESSION, when the session ends, cancels the
// operation that runs and returns once it has ended; WM_QUERYENDSESSION lets
// the session end. Null (a HWND) before the first operation, or where no
// window could be made.
void* session_end_window();

} // namespace va::platform::windows
