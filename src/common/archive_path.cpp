#include "common/archive_path.h"
#include "common/utf8.h"

namespace va {
std::expected<ArchivePath, IoError> archive_path_from_native(
    const std::filesystem::path& relative)
{
    if (relative.has_root_path()) {
        return std::unexpected(
            IoError{IoErrorKind::Other, 0, "ArchivePath must be relative"});
    }

    ArchivePath result;
    for (const auto& component : relative) {
        if (component == ".") {
            continue;
        }
        if (component == "..") {
            return std::unexpected(
                IoError{IoErrorKind::Other, 0, "ArchivePath cannot contain .."});
        }
        if (!result.p.empty()) {
            result.p += '/';
        }
        result.p += path_to_utf8(component);
    }
    return result;
}

std::expected<std::filesystem::path, IoError> archive_path_to_native(
    const ArchivePath& value)
{
    if (!value.enc.empty()) {
        return std::unexpected(IoError{
            IoErrorKind::NotSupported,
            0,
            "raw encoded names require platform conversion",
        });
    }

    std::filesystem::path result;
    std::size_t begin = 0;
    while (begin <= value.p.size()) {
        const auto end = value.p.find('/', begin);
        const auto component = value.p.substr(begin, end - begin);
        const auto native_component = path_from_utf8(component);
        if (component.empty() || component == ".." || component == "." ||
            component.find('\0') != std::string::npos || native_component.has_root_path() ||
            native_component.has_parent_path()) {
            return std::unexpected(
                IoError{IoErrorKind::Other, 0, "invalid ArchivePath"});
        }

        result /= native_component;
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

} // namespace va