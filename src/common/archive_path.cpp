#include "common/archive_path.h"

#include "common/hex.h"
#include "common/utf8.h"

#include <vector>

namespace va {
namespace {

IoError invalid_path(std::string message)
{
    return {IoErrorKind::Other, 0, std::move(message)};
}

// Splits `relative` into components, dropping "." and empty components and
// rejecting "..". Works on any string type (bytes or UTF-16 units).
template <typename String, typename IsSeparator>
std::expected<std::vector<String>, IoError> split_components(
    const String& relative,
    IsSeparator is_separator)
{
    if (!relative.empty() && is_separator(relative.front())) {
        return std::unexpected(invalid_path("ArchivePath must be relative"));
    }

    std::vector<String> components;
    String current;
    const auto finish_component = [&]() -> bool {
        if (current.size() == 2 && current[0] == '.' && current[1] == '.') {
            return false;
        }
        if (!current.empty() && !(current.size() == 1 && current[0] == '.')) {
            components.push_back(current);
        }
        current.clear();
        return true;
    };

    for (const auto unit : relative) {
        if (is_separator(unit)) {
            if (!finish_component()) {
                return std::unexpected(invalid_path("ArchivePath cannot contain .."));
            }
        } else {
            current += unit;
        }
    }
    if (!finish_component()) {
        return std::unexpected(invalid_path("ArchivePath cannot contain .."));
    }
    return components;
}

std::string utf16le_hex(const std::vector<std::u16string>& components)
{
    std::string bytes;
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (index > 0) {
            bytes += '/';
            bytes += '\0';
        }
        for (const char16_t unit : components[index]) {
            bytes += static_cast<char>(unit & 0xFF);
            bytes += static_cast<char>(unit >> 8);
        }
    }
    return to_hex(bytes);
}

} // namespace

std::expected<ArchivePath, IoError> archive_path_from_utf16(std::u16string_view relative)
{
    if (relative.size() >= 2 && relative[1] == u':') {
        return std::unexpected(invalid_path("ArchivePath must be relative"));
    }
    const auto components = split_components(
        std::u16string(relative), [](char16_t unit) { return unit == u'/' || unit == u'\\'; });
    if (!components) {
        return std::unexpected(components.error());
    }

    ArchivePath result;
    bool lossless = true;
    for (const auto& component : *components) {
        bool component_lossless = true;
        if (!result.p.empty()) {
            result.p += '/';
        }
        result.p += utf16_to_utf8(component, &component_lossless);
        lossless = lossless && component_lossless;
    }
    if (!lossless) {
        result.enc = "utf16le";
        result.raw = utf16le_hex(*components);
    }
    return result;
}

std::expected<ArchivePath, IoError> archive_path_from_bytes(std::string_view relative)
{
    const auto components =
        split_components(std::string(relative), [](char unit) { return unit == '/'; });
    if (!components) {
        return std::unexpected(components.error());
    }

    ArchivePath result;
    std::string joined;
    for (const auto& component : *components) {
        if (!joined.empty()) {
            joined += '/';
        }
        joined += component;
    }
    if (is_valid_utf8(joined)) {
        result.p = joined;
    } else {
        result.p = utf8_lossy(joined);
        result.enc = "bytes";
        result.raw = to_hex(joined);
    }
    return result;
}

std::expected<ArchivePath, IoError> archive_path_from_native(const std::filesystem::path& relative)
{
    if (relative.has_root_path()) {
        return std::unexpected(invalid_path("ArchivePath must be relative"));
    }
    const auto& native = relative.native();
    using native_char = std::filesystem::path::value_type;
    if constexpr (sizeof(native_char) == sizeof(char16_t)) {
        return archive_path_from_utf16(std::u16string(native.begin(), native.end()));
    } else {
        return archive_path_from_bytes(std::string(native.begin(), native.end()));
    }
}

std::expected<std::filesystem::path, IoError> archive_path_to_native(const ArchivePath& value)
{
    using native_char = std::filesystem::path::value_type;
    constexpr bool wide_platform = sizeof(native_char) == sizeof(char16_t);

    std::filesystem::path::string_type native;
    if (value.enc.empty()) {
        native = path_from_utf8(value.p).native();
    } else {
        if (value.enc != (wide_platform ? "utf16le" : "bytes")) {
            return std::unexpected(IoError{IoErrorKind::NotSupported, 0,
                "name was recorded with another platform's encoding: " + value.enc});
        }
        const auto bytes = from_hex(value.raw);
        if (!bytes || (wide_platform && bytes->size() % 2 != 0)) {
            return std::unexpected(invalid_path("invalid raw ArchivePath"));
        }
        const std::size_t unit_size = wide_platform ? 2 : 1;
        for (std::size_t index = 0; index < bytes->size(); index += unit_size) {
            unsigned unit = std::to_integer<unsigned>((*bytes)[index]);
            if (wide_platform) {
                unit |= std::to_integer<unsigned>((*bytes)[index + 1]) << 8;
            }
            native += static_cast<native_char>(unit);
        }
    }

    // Rebuild component by component; a component that the native path parser
    // would split further (for example a Linux name containing '\' read on
    // Windows) cannot exist here and is rejected.
    std::filesystem::path result;
    const auto components =
        split_components(native, [](native_char unit) { return unit == native_char('/'); });
    if (!components || components->empty()) {
        return std::unexpected(invalid_path("invalid ArchivePath"));
    }
    for (const auto& component : *components) {
        const std::filesystem::path native_component(component);
        if (component.find(native_char('\0')) != std::filesystem::path::string_type::npos ||
            native_component.has_root_path() || native_component.has_parent_path()) {
            return std::unexpected(invalid_path("invalid ArchivePath"));
        }
        result /= native_component;
    }
    return result;
}

} // namespace va
