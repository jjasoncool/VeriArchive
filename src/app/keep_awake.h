#pragma once
// An operation of the window keeps the computer awake while it runs (decision
// 0043): every operation that gui::Job runs. The command line holds none: it
// runs in scripts, in scheduled tasks and on servers, whose power settings
// decide (on Linux, `systemd-inhibit` wraps a command).
#include "app/text.h"
#include "platform/keep_awake.h"

#include <string_view>

namespace va::app {

// Why the computer is kept awake, as the system lists it (`powercfg
// /requests`, `systemd-inhibit --list`), in the interface's language.
inline constexpr std::string_view keep_awake_reason = "A long operation is running";

inline platform::KeepAwake keep_awake(const Text& text)
{
    return platform::KeepAwake(text(keep_awake_reason));
}

} // namespace va::app
