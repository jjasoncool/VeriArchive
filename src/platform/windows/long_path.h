#pragma once
// The \\?\ form of a Windows path (R-23), in which the Windows file functions
// take a path of any length. Windows reads a path in that form exactly as
// given, so '/' is made '\' and "." and ".." are resolved here first. Worked
// out on the text alone, without the C++ library, which reads a network path
// ("\\server\share") or a \\?\ path as one on the current drive. Plain C++
// without Windows headers, so it is tested on every OS, like the drive letters
// made with subst.
#include <optional>
#include <string>
#include <string_view>

namespace va::platform::windows {

// A volume's GUID path in the \\?\ form ("\\?\Volume{...}\Photos", decision
// 0029, point 3), written in one form: the prefix with '\', the name
// "Volume{...}" as written, then what follows it as a path from the root of a
// drive letter is made (see long_path): every run of separators one '\', "."
// dropped and ".." dropping the name before it, never the volume's root
// folder, and a separator at the end kept, also one alone, which makes the
// path the volume's root folder. Windows reads a path in the \\?\ form exactly
// as given, without resolving "." and "..", while the place of a root on its
// volume is told from the path made normal: made so, the root read is the
// root identified. Nothing for any other path, also one in the \\?\ form or
// in the \\.\ form that names no volume by its GUID.
std::optional<std::wstring> normal_volume_guid_path(std::wstring_view path);

// `path` in the form the Windows file functions take at any length. A path
// from the root of a drive letter gets the \\?\ prefix: "C:\Data\a\..\b"
// gives "\\?\C:\Data\b". A network path gets the \\?\UNC\ prefix and keeps
// its server and share: "\\server\share\a\..\b" gives
// "\\?\UNC\server\share\b". Both read '/' as '\' (R-40), and "." and ".." are
// resolved, never above "C:\" or the share; a path that ends with a
// separator keeps it. Any other path only has its '/' made '\', and Windows
// reads it as usual: one that starts with \\?\ or \\.\ already, a relative
// one, "\Data" and "C:Data" on the current drive, and "\\server" without a
// share.
std::wstring long_path(std::wstring_view path);

} // namespace va::platform::windows
