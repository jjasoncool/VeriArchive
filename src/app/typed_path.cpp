#include "app/typed_path.h"

#include "common/utf8.h"
#include "core/engine/candidates.h"
#include "platform/platform_services.h"

#include <system_error>

namespace va::app {
namespace {

// A Windows path that starts with two separators: a network path
// ("\\server\share\dir") or one such as "\\?\D:\Photos". The C++ library
// reads it as a path from the root of the current drive, so it would make
// "C:\server\share\dir" of it, and "\server\share\dir" when normalising it.
bool starts_with_two_separators(const std::filesystem::path& path)
{
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        const auto& native = path.native();
        const auto separator = [](auto character) { return character == '\\' || character == '/'; };
        return native.size() >= 2 && separator(native[0]) && separator(native[1]);
    }
    return false;
}

// Whether `text`, a root refused for being written in the \\?\ or \\.\ form,
// names a volume by its GUID behind its four-character prefix ("Volume{", in
// any case): a volume's GUID path written otherwise than in the \\?\ form
// that mountvol lists, which is taken (decision 0029, point 3), as with
// "\\.\" in front or with its GUID cut short.
bool names_a_volume_guid(std::string_view text)
{
    constexpr std::string_view volume = "volume{";
    if (text.size() < 4 + volume.size()) {
        return false;
    }
    for (std::size_t index = 0; index < volume.size(); ++index) {
        const char character = text[4 + index];
        const char lower = character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        if (lower != volume[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

std::filesystem::path typed_path(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const auto typed = path_from_utf8(text);
    if (starts_with_two_separators(typed)) {
        return typed;
    }
    std::error_code error;
    const auto absolute = std::filesystem::absolute(typed, error);
    return (error ? typed : absolute).lexically_normal();
}

std::filesystem::path typed_root(std::string_view text)
{
    return platform::resolve_subst_drive(without_trailing_separators(typed_path(text)));
}

std::optional<std::string> root_refusal(const std::filesystem::path& root)
{
    switch (platform::archive_root_kind(root)) {
    case platform::RootKind::NetworkShare:
        return "the archive root is on a network share; VeriArchive registers and verifies folders on volumes "
               "attached to this computer";
    case platform::RootKind::DevicePath: {
        const auto text = path_to_utf8(root);
        if (names_a_volume_guid(text)) {
            return R"(write a volume's GUID path exactly as mountvol lists it, with \\?\ in front)";
        }
        // The four characters of the prefix as typed: "\\?\", "\\.\", "//?/".
        return "write the archive root without " + text.substr(0, 4) + " in front (for example D:\\Photos)";
    }
    case platform::RootKind::Usable:
        break;
    }
    return std::nullopt;
}

std::optional<std::string> root_not_taken(const std::filesystem::path& root, std::string_view typed)
{
    if (auto refusal = root_refusal(root)) {
        return refusal;
    }
    // Asked of the platform: the C++ library cannot tell that a volume's GUID
    // path is a folder (decision 0029, point 3).
    if (!platform::is_directory(root)) {
        return "not a directory: " + std::string(typed);
    }
    return std::nullopt;
}

} // namespace va::app
