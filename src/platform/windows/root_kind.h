#pragma once
// Which archive roots on Windows are refused before anything of them is read
// (decision 0029): one on a network share, which is no volume of this
// computer, and one written in the \\?\ or \\.\ form. Windows reports no
// volume for a share (GetVolumeNameForVolumeMountPointW fails with Win32
// error 87), and the C++ library reads the drive letter inside "\\?\D:\Photos"
// as a root of its own, so the root is not found on its volume. Plain C++
// without Windows headers, so it is tested on every OS, like the drive
// letters made with subst.
#include "platform/platform_services.h"

#include <functional>
#include <string_view>

namespace va::platform::windows {

// Whether the drive letter `drive` ("Z:") is a network drive, as
// GetDriveTypeW reports it for "Z:\" (DRIVE_REMOTE).
using NetworkDriveQuery = std::function<bool(std::wstring_view drive)>;

// The kind of `root`, a root as the user interfaces make it (a drive letter
// made with subst already resolved, decision 0027), with '\' or '/':
//  - a network path, "\\server\share\..." or "//server/share", is NetworkShare;
//  - a path in the \\?\ or \\.\ form is NetworkShare when what follows names a
//    share ("\\?\UNC\server\share", in any case) or a network drive
//    ("\\?\Z:\..."), otherwise DevicePath ("\\?\D:\Photos",
//    "\\?\Volume{...}\", "\\.\D:\Photos");
//  - a path from the root of a drive letter is NetworkShare when that letter
//    is a network drive;
//  - anything else is Usable.
// Only the text is read, and `network_drive` is asked only about a drive
// letter at the start: a network path is told without asking anything, so a
// share that does not answer is refused at once.
RootKind root_kind(std::wstring_view root, const NetworkDriveQuery& network_drive);

} // namespace va::platform::windows
