#include "platform/windows/subst_drive.h"

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

// "S:\..." or "S:/...": a path from the root of a drive.
bool starts_at_drive_root(std::wstring_view path)
{
    return path.size() >= 3 && is_letter(path[0]) && path[1] == L':' && is_separator(path[2]);
}

// Names in the NT namespace ("\??\UNC\...") are compared without regard to case.
bool starts_with_ignoring_case(std::wstring_view text, std::wstring_view prefix)
{
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (upper(text[index]) != upper(prefix[index])) {
            return false;
        }
    }
    return true;
}

// `folder` followed by `rest`, what a path holds after its drive letter.
std::wstring join(std::wstring folder, std::wstring_view rest)
{
    while (!rest.empty() && is_separator(rest.front())) {
        rest.remove_prefix(1);
    }
    if (folder.size() == 2) {
        folder += L'\\'; // "D:" stands for the root of D:
    }
    if (rest.empty()) {
        while (folder.size() > 3 && is_separator(folder.back())) {
            folder.pop_back(); // keep only a drive root's separator ("D:\")
        }
        return folder;
    }
    if (!is_separator(folder.back())) {
        folder += L'\\';
    }
    folder += rest;
    return folder;
}

} // namespace

std::optional<std::wstring> subst_target(std::wstring_view device)
{
    constexpr std::wstring_view dos_devices = L"\\??\\";
    constexpr std::wstring_view network = L"UNC\\";
    if (!device.starts_with(dos_devices)) {
        return std::nullopt;
    }
    const auto target = device.substr(dos_devices.size());
    if (starts_with_ignoring_case(target, network) && target.size() > network.size()) {
        return L"\\\\" + std::wstring(target.substr(network.size()));
    }
    if (target.size() >= 2 && is_letter(target[0]) && target[1] == L':' &&
        (target.size() == 2 || is_separator(target[2]))) {
        return std::wstring(target);
    }
    return std::nullopt;
}

std::wstring resolve_subst(std::wstring_view path, const DosDeviceQuery& query)
{
    std::wstring resolved(path);
    // Each step replaces one letter, so a chain longer than the alphabet is a loop.
    for (int step = 0; step < 26; ++step) {
        if (!starts_at_drive_root(resolved)) {
            return resolved;
        }
        const auto device = query(std::wstring{upper(resolved[0]), L':'});
        const auto folder = device ? subst_target(*device) : std::nullopt;
        if (!folder) {
            return resolved;
        }
        resolved = join(*folder, std::wstring_view(resolved).substr(3));
    }
    return std::wstring(path);
}

} // namespace va::platform::windows
