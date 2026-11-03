#pragma once
// When an operation of the window ends while no window of the program is the
// active one, the window says so in a notification of the system (decision
// 0043): the operator who left a FULL verification running for hours learns
// when it ended, and how.
//
// Plain C++ without GTK, so the unit tests can check it.
#include <string>
#include <string_view>

namespace va::gui {

// The notification's body: the first two lines of the operation's report that
// hold text, as shown in the view, which name the result and what it is of
// ("FULL VERIFY PASSED", then the archive's label, uuid and revision; a
// registration's label and revision, then what it hashed; a refusal's
// reason), and after them every line of a device alert, which app::alert_mark
// leads (decision 0044): an operator away from the computer learns of it too.
std::string ended_notification_body(std::string_view report);

} // namespace va::gui
