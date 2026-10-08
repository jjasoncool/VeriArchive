#pragma once
// ArchivePath: how a file's location inside an archive is written in the manifest
// (HANDOFF §B2). It is always relative to the archive root and uses '/' between
// components, whatever the operating system.
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include "common/error.h"

namespace va {

struct ArchivePath {
    // UTF-8 path. When the native name is not valid Unicode this is a lossy
    // display form (U+FFFD replaces the bad parts) and `raw` is the identity.
    std::string p;
    // "" when `p` is exact; "bytes" (Linux) or "utf16le" (Windows) otherwise (R-41).
    std::string enc;
    // Lowercase hex of the native code units of the whole relative path, with
    // components joined by '/' (U+002F). Empty when `enc` is empty.
    std::string raw;
};

// Platform-neutral builders, usable (and testable) on every OS.
// UTF-16 input treats both '/' and '\' as separators (Windows rules); byte input
// treats only '/' as a separator, so '\' stays part of a Linux name (R-40).
std::expected<ArchivePath, IoError> archive_path_from_utf16(std::u16string_view relative);
std::expected<ArchivePath, IoError> archive_path_from_bytes(std::string_view relative);

// Uses the builder that matches this platform's native path encoding.
std::expected<ArchivePath, IoError> archive_path_from_native(const std::filesystem::path& relative);

// Fails with NotSupported when the name was recorded with another platform's raw
// encoding; callers report such entries as Unrepresentable (R-44).
std::expected<std::filesystem::path, IoError> archive_path_to_native(const ArchivePath& path);

} // namespace va
