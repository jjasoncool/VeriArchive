#pragma once
// Ctrl-C (Windows console control events, Linux SIGINT/SIGTERM) requests
// cancellation instead of killing the process, so a long run ends as
// INCOMPLETE with its checkpoints saved and can be resumed (R-94, R-104).
#include "core/interfaces.h"

namespace va::platform {

// `token` must outlive the process's use of it; one token at a time.
void cancel_on_interrupt(CancelToken& token);

} // namespace va::platform
