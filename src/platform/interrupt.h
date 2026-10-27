#pragma once
// An interrupt requests cancellation instead of ending the process, so that a
// long operation on the command line ends as one cancelled with the GUI's
// Cancel does: a verification or a relocation is recorded INCOMPLETE, and a
// registration keeps its saved progress, each to be resumed (R-94, R-104). On
// Windows the interrupts are Ctrl+C and Ctrl+Break, and as they do, the
// console window closed, the user signing out and the computer shutting down
// (decision 0022, amended 2026-10-27); on Linux SIGINT (Ctrl+C), SIGTERM (as
// at shutdown) and SIGHUP (the terminal closed, the user logged out), but for
// one the process was started to ignore (nohup).
#include "core/interfaces.h"

namespace va::platform {

// While it lives, an interrupt cancels `token`, which must outlive it; one at
// a time. Windows ends the process once its handler of a closed console
// window or of the session's end returns: that handler waits until this
// object goes, which the command does once it has recorded and said how the
// operation ended, within the time Windows gives (about 5 seconds for a
// closed console window). Before and after, an interrupt ends the process as
// it would without VeriArchive.
class CancelOnInterrupt {
public:
    explicit CancelOnInterrupt(CancelToken& token);
    ~CancelOnInterrupt();
    CancelOnInterrupt(const CancelOnInterrupt&) = delete;
    CancelOnInterrupt& operator=(const CancelOnInterrupt&) = delete;
};

} // namespace va::platform
