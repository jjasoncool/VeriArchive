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

bool is_hex(wchar_t character)
{
    return (character >= L'0' && character <= L'9') || (character >= L'A' && character <= L'F') ||
           (character >= L'a' && character <= L'f');
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

// "Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" followed by a separator or
// nothing, "Volume" in any case: a volume's GUID path behind "\\?\", as
// mountvol lists it, the only name of a volume that has no drive letter or
// folder path (decision 0029, point 3).
bool names_volume_guid(std::wstring_view path)
{
    constexpr std::wstring_view volume = L"VOLUME{";
    constexpr std::size_t guid_length = 36;  // 32 hex digits and 4 dashes
    static_assert(volume.size() + guid_length + 1 == volume_guid_name_length);
    const auto end = volume.size() + guid_length;
    if (path.size() <= end || path[end] != L'}' || (path.size() > end + 1 && !is_separator(path[end + 1]))) {
        return false;
    }
    for (std::size_t index = 0; index < volume.size(); ++index) {
        if (upper(path[index]) != volume[index]) {
            return false;
        }
    }
    for (std::size_t index = 0; index < guid_length; ++index) {
        const auto character = path[volume.size() + index];
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash ? character != L'-' : !is_hex(character)) {
            return false;
        }
    }
    return true;
}

RootPresence absent_root(std::wstring_view root, unsigned long error, const DriveTypeQuery& drive_type)
{
    // Win32 error codes, as winerror.h gives them.
    constexpr unsigned long file_not_found = 2;
    constexpr unsigned long path_not_found = 3;
    constexpr unsigned long invalid_drive = 15;
    constexpr unsigned long not_ready = 21;
    constexpr unsigned long invalid_name = 123;
    constexpr unsigned long bad_pathname = 161;
    const bool drive_letter =
        root.size() >= 2 && is_letter(root[0]) && root[1] == L':' && (root.size() == 2 || is_separator(root[2]));
    if (drive_letter) {
        if (drive_type(std::wstring{upper(root[0]), L':'}) == drive_no_root_dir || error == not_ready) {
            return RootPresence::NotAttached;
        }
    }
    switch (error) {
    case file_not_found:
    case path_not_found:
    case invalid_drive:
    case not_ready:
    case invalid_name:
    case bad_pathname:
        return RootPresence::Missing;
    default:
        return RootPresence::Unknown;
    }
}

RootKind root_kind(std::wstring_view root, const NetworkDriveQuery& network_drive)
{
    const bool two_separators = root.size() >= 2 && is_separator(root[0]) && is_separator(root[1]);
    if (two_separators && root.size() >= 4 && (root[2] == L'?' || root[2] == L'.') && is_separator(root[3])) {
        // The \\?\ or \\.\ form: a share behind it is refused as a share,
        // which it is in any form; a volume's GUID path in the \\?\ form, as
        // Windows lists it, is taken; anything else is refused for its form.
        const auto rest = root.substr(4);
        if (names_unc(rest) || on_network_drive(rest, network_drive)) {
            return RootKind::NetworkShare;
        }
        return root[2] == L'?' && names_volume_guid(rest) ? RootKind::Usable : RootKind::DevicePath;
    }
    if (two_separators) {
        return RootKind::NetworkShare;
    }
    return on_network_drive(root, network_drive) ? RootKind::NetworkShare : RootKind::Usable;
}

} // namespace va::platform::windows
