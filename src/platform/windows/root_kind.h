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

// The type GetDriveTypeW reports for the drive letter `drive` ("K:") as
// "K:\"; drive_no_root_dir where no volume has that letter.
using DriveTypeQuery = std::function<unsigned(std::wstring_view drive)>;
inline constexpr unsigned drive_no_root_dir = 1;  // DRIVE_NO_ROOT_DIR

// Why `root`, which GetFileAttributesW did not find (`error`, its Win32
// error), is not there (decision 0029, point 7): NotAttached where it starts
// with a drive letter that no volume has (`drive_type` gives
// drive_no_root_dir), or one whose drive has no medium in it
// (ERROR_NOT_READY, an empty card reader); Missing where it is not found
// (ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_INVALID_DRIVE,
// ERROR_INVALID_NAME, ERROR_BAD_PATHNAME); otherwise Unknown, which the
// volume lookup reports. `drive_type` is asked only about a drive letter at
// the start.
RootPresence absent_root(std::wstring_view root, unsigned long error, const DriveTypeQuery& drive_type);

} // namespace va::platform::windows
