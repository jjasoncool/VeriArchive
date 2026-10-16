#pragma once
// Drive letters made with subst (`subst S: D:\Data`, decision 0027). Windows
// reports no volume for such a letter: GetVolumePathNameW stops at the folder
// it is given, and GetVolumeNameForVolumeMountPointW then fails (Win32 error
// 4390). A path through one is therefore resolved to the folder the letter
// stands for before its volume is looked up. Plain C++ without Windows
// headers, so it is tested on every OS, like the Linux mountinfo parser.
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace va::platform::windows {

// The folder a drive letter made with subst stands for, from what
// QueryDosDeviceW reports for the letter: "\??\D:\Data" gives "D:\Data", and
// "\??\UNC\server\share\dir" (a folder on a network share) gives
// "\\server\share\dir". Nothing for any other device, such as a volume
// ("\Device\HarddiskVolume3") or a network drive ("\Device\LanmanRedirector\...").
std::optional<std::wstring> subst_target(std::wstring_view device);

// What QueryDosDeviceW reports for a drive such as "S:": the first (current)
// target in its list, or nothing when the drive is not defined.
using DosDeviceQuery = std::function<std::optional<std::wstring>(std::wstring_view drive)>;

// `path` with its drive letter replaced by the folder the letter stands for
// when the letter was made with subst, again while the result starts with
// such a letter (one made for a folder on another). Only paths that start with
// a drive letter and a separator ("S:\...", "S:/...") are resolved; any other
// path, and a chain of letters that never ends (a loop), comes back unchanged.
// "S:\" itself gives the folder without a trailing separator, unless that
// folder is a drive root ("D:\").
std::wstring resolve_subst(std::wstring_view path, const DosDeviceQuery& query);

} // namespace va::platform::windows
