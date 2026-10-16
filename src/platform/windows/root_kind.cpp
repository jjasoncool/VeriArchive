#include "platform/windows/root_kind.h"

#include <string>

namespace va::platform::windows {
namespace {

bool is_separator(wchar_t character)
{
    return character == L'\\' || character == L'/';
}

bool is_letter(wchar_t character)
{
    return (character >= L'A' && character <= L'Z') || (character >= L'a' && character <= L'z');
}

wchar_t upper(wchar_t character)
{
    return character >= L'a' && character <= L'z' ? static_cast<wchar_t>(character - L'a' + L'A') : character;
}

// "UNC" followed by a separator or nothing, in any case (names in the NT
// namespace ignore case): the share behind "\\?\UNC\server\share".
bool names_unc(std::wstring_view path)
{
    return path.size() >= 3 && upper(path[0]) == L'U' && upper(path[1]) == L'N' && upper(path[2]) == L'C' &&
           (path.size() == 3 || is_separator(path[3]));
}

// A path from the root of a drive letter ("Z:\...", or "Z:" alone) whose
// letter is a network drive.
bool on_network_drive(std::wstring_view path, const NetworkDriveQuery& network_drive)
{
    if (path.size() < 2 || !is_letter(path[0]) || path[1] != L':' || (path.size() > 2 && !is_separator(path[2]))) {
        return false;
    }
    return network_drive(std::wstring{upper(path[0]), L':'});
}

} // namespace

RootKind root_kind(std::wstring_view root, const NetworkDriveQuery& network_drive)
{
    const bool two_separators = root.size() >= 2 && is_separator(root[0]) && is_separator(root[1]);
    if (two_separators && root.size() >= 4 && (root[2] == L'?' || root[2] == L'.') && is_separator(root[3])) {
        // The \\?\ or \\.\ form: a share behind it is refused as a share,
        // which it is in any form; anything else for its form.
        const auto rest = root.substr(4);
        return names_unc(rest) || on_network_drive(rest, network_drive) ? RootKind::NetworkShare
                                                                       : RootKind::DevicePath;
    }
    if (two_separators) {
        return RootKind::NetworkShare;
    }
    return on_network_drive(root, network_drive) ? RootKind::NetworkShare : RootKind::Usable;
}

} // namespace va::platform::windows
