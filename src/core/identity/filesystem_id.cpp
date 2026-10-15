#include "core/identity/filesystem_id.h"

#include <format>

namespace va {
namespace {

std::string lowercase(std::string_view text)
{
    std::string result(text);
    for (auto& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

std::string hex_digits_only(std::string_view text)
{
    std::string result;
    for (const char character : lowercase(text)) {
        if ((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')) {
            result += character;
        }
    }
    return result;
}

} // namespace

std::string normalise_filesystem_type(std::string_view raw)
{
    const auto type = lowercase(raw);
    if (type == "ntfs3" || type == "ntfs-3g" || type == "ntfs") {
        return "ntfs";
    }
    if (type == "fat" || type == "fat12" || type == "fat16" || type == "fat32" ||
        type == "msdos" || type == "vfat") {
        return "vfat";
    }
    if (type == "exfat" || type == "exfat-fuse" || type == "fuse.exfat") {
        return "exfat";
    }
    return type;
}

FilesystemId normalise_filesystem_id(std::string_view filesystem_type, std::string_view raw)
{
    if (filesystem_type == "ntfs") {
        const auto hex = hex_digits_only(raw);
        if (hex.size() == 16) {
            return {hex, hex.substr(8)};
        }
        if (hex.size() == 8) {
            return {"", hex};
        }
        return {hex, ""};
    }
    if (filesystem_type == "exfat" || filesystem_type == "vfat") {
        const auto hex = hex_digits_only(raw);
        if (hex.size() == 8) {
            return {hex, hex};
        }
        return {hex, ""};
    }
    return {lowercase(raw), ""};
}

std::string kernel_filesystem_id(std::uint64_t fsid)
{
    return std::format("fsid-{:016x}", fsid);
}

bool is_kernel_filesystem_id(std::string_view filesystem_uuid)
{
    return filesystem_uuid.starts_with("fsid-");
}

std::uint64_t combine_kernel_filesystem_id(std::int32_t first, std::int32_t second)
{
    // Each word goes through uint32_t first: a negative int converted straight
    // to a 64-bit unsigned value would fill the high half with ones.
    return std::uint64_t{static_cast<std::uint32_t>(first)} |
           (std::uint64_t{static_cast<std::uint32_t>(second)} << 32);
}

std::optional<std::string> kernel_filesystem_id_for(std::string_view filesystem_type,
    std::uint64_t fsid)
{
    const bool follows_uuid = filesystem_type == "ext2" || filesystem_type == "ext3" ||
                              filesystem_type == "ext4" || filesystem_type == "btrfs";
    if (!follows_uuid || fsid == 0) {
        return std::nullopt;
    }
    return kernel_filesystem_id(fsid);
}

std::optional<std::string> nvme_wwn_from_windows_serial(std::string_view serial)
{
    if (!serial.ends_with('.') || serial.find('_') == std::string_view::npos) {
        return std::nullopt;
    }
    std::string hex;
    for (const char character : serial) {
        if (character == '_' || character == '.') {
            continue;
        }
        const bool digit = (character >= '0' && character <= '9') ||
                           (character >= 'a' && character <= 'f') ||
                           (character >= 'A' && character <= 'F');
        if (!digit) {
            return std::nullopt;
        }
        hex += character;
    }
    if (hex.size() != 16 && hex.size() != 32) {
        return std::nullopt;
    }
    return "eui." + lowercase(hex);
}

} // namespace va
