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
    case platform::RootKind::DevicePath:
        // The four characters of the prefix as typed: "\\?\", "\\.\", "//?/".
        return "write the archive root without " + path_to_utf8(root).substr(0, 4) +
               " in front (for example D:\\Photos)";
    case platform::RootKind::Usable:
        break;
    }
    return std::nullopt;
}

} // namespace va::app
