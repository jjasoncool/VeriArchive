#pragma once
// Which archive roots on Windows are refused before anything of them is read
// (decision 0029): one on a network share, which is no volume of this
// computer, and one written in the \\?\ or \\.\ form, but for a volume's GUID
// path ("\\?\Volume{...}\"), the only name of a volume that has no drive
// letter or folder path. Windows reports no volume for a share
// (GetVolumeNameForVolumeMountPointW fails with Win32 error 87), and the C++
// library reads the drive letter inside "\\?\D:\Photos" as a root of its own,
// so the root is not found on its volume. Plain C++ without Windows headers,
// so it is tested on every OS, like the drive letters made with subst.
#include "platform/platform_services.h"

#include <cstddef>
#include <functional>
#include <string_view>

namespace va::platform::windows {

// Whether the drive letter `drive` ("Z:") is a network drive, as
// GetDriveTypeW reports it for "Z:\" (DRIVE_REMOTE).
using NetworkDriveQuery = std::function<bool(std::wstring_view drive)>;

// Whether `path`, what follows "\\?\", starts with a volume's name by its
// GUID as mountvol lists it, "Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
// ("Volume" and the GUID's digits in any case), followed by a separator or
// nothing (decision 0029, point 3).
bool names_volume_guid(std::wstring_view path);

// The length of that name: "Volume{", the GUID's 36 characters and "}".
inline constexpr std::size_t volume_guid_name_length = 7 + 36 + 1;

// The kind of `root`, a root as the user interfaces make it (a drive letter
// made with subst already resolved, decision 0027), with '\' or '/':
//  - a network path, "\\server\share\..." or "//server/share", is NetworkShare;
//  - a path in the \\?\ or \\.\ form is NetworkShare when what follows names a
//    share ("\\?\UNC\server\share", in any case) or a network drive
//    ("\\?\Z:\..."); Usable when it is a volume's GUID path in the \\?\ form,
//    as mountvol lists it ("\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\",
//    "Volume" in any case, followed by a separator or nothing), or a folder
//    below one; otherwise DevicePath ("\\?\D:\Photos", "\\.\D:\Photos",
//    "\\.\Volume{...}\", a GUID cut short);
//  - a path from the root of a drive letter is NetworkShare when that letter
//    is a network drive;
//  - anything else is Usable.
// Only the text is read, and `network_drive` is asked only about a drive
// letter at the start: a network path is told without asking anything, so a
// share that does not answer is refused at once.
RootKind root_kind(std::wstring_view root, const NetworkDriveQuery& network_drive);

} // namespace va::platform::windows
