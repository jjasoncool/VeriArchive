#include "core/identity/filesystem_id.h"

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
