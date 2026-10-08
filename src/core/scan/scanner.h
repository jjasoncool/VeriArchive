#pragma once
// Scanner: walks an archive tree through IFileSystemAccess and produces the
// entries a manifest will contain, in canonical order. It reads link targets
// but no file content; content hashing is the hash pipeline's job.
#include "core/interfaces.h"
#include "core/manifest/entry.h"

#include <filesystem>
#include <string>
#include <vector>

namespace va {

// The default exclusion list, version 1 (R-54). Patterns match at the archive
// root only (R-55); a trailing '/' means "directory", '*' matches any text.
const std::vector<std::string>& default_exclusions();
inline constexpr int default_exclusions_version = 1;

struct ScannedEntry {
    Entry entry;                        // everything except content hashes of files
    std::filesystem::path native;       // absolute path used to open the file
};

struct ScanResult {
    std::vector<ScannedEntry> entries;  // canonical order (R-43)
    std::vector<ArchivePath> skipped_mount_points; // R-30: reported, not descended
};

struct ScanOptions {
    std::vector<std::string> exclusions = default_exclusions();
};

// Fails only when the root itself cannot be listed, the device disappears, or
// the scan is cancelled; every other problem becomes an unreadable entry
// (R-10, decisions 0010 and 0014).
std::expected<ScanResult, IoError> scan_tree(
    IFileSystemAccess& files,
    const std::filesystem::path& root,
    const ScanOptions& options,
    CancelToken& cancel,
    IProgressSink& progress);

// "permission", "media" or "other" for an I/O error (decision 0010).
std::string unreadable_reason(const IoError& error);

// Windows attribute bits meaning "content is not on this disk": reading would
// trigger a download that writes to the volume (decision 0009).
inline constexpr std::uint32_t recall_attribute_mask = 0x00001000u   // OFFLINE
                                                       | 0x00040000u // RECALL_ON_OPEN
                                                       | 0x00400000u; // RECALL_ON_DATA_ACCESS

// True for '/', 'X:/' and '//' targets (decision 0008).
bool is_absolute_link_target(std::string_view target);

} // namespace va
