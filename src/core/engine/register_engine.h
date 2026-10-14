#pragma once
// Registration: scan an archive root, hash every file, write the first (or a
// new) revision's manifest, and index it in the repository.
#include "core/engine/candidates.h"
#include "core/interfaces.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <optional>
#include <string>
#include <vector>

namespace va {

struct RegisterRequest {
    std::filesystem::path root;
    std::optional<std::string> label;       // default: next repository label
    bool new_revision = false;              // R-97: re-baseline an existing archive
    bool force_new_archive = false;         // R-97: register again as a separate archive
    std::optional<Uuid> copy_of;            // decision 0005
    std::optional<bool> rotational;         // --io hdd|ssd overrides detection (R-111)
    bool resume = false;                    // R-104: continue an interrupted registration
    bool write_media_copy = false;          // R-89: also write .veriarchive/ on the media (off by default)
};

struct RegisterServices {
    IFileSystemAccess& archive_files;       // reads the archive media
    IFileSystemAccess& repository_files;    // reads back repository files
    IFileWriter& writer;                    // writes repository files
    IFileWriter& media_writer;              // writes the archive media, only with write_media_copy (R-03)
    IMediaEnumerator& media;
    ArchiveStore& store;
    RepositoryLayout layout;
    HostInfo host;
};

struct RegisterOutcome {
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 0;
    ManifestCounts counts;
    std::vector<Entry> unreadable;          // decision 0010: listed, exit code 2
    std::vector<ArchivePath> skipped_mount_points;
    std::vector<std::string> warnings;
    std::filesystem::path manifest;
    std::filesystem::path media_copy;       // set when the media copy was written (R-89)
};

struct RegisterFailure {
    enum class Kind {
        Invalid,     // a value of the request is malformed, e.g. the label (exit 10)
        Refused,     // needs an operator decision or flag (exit 12)
        Media,       // the root or its volume cannot be read (exit 11)
        Repository,  // manifest or database could not be written (exit 11)
        Incomplete,  // cancelled or the device disappeared (exit 4)
    };
    Kind kind;
    std::string message;
};

std::expected<RegisterOutcome, RegisterFailure> register_archive(
    RegisterServices& services,
    const RegisterRequest& request,
    CancelToken& cancel,
    IProgressSink& progress);

} // namespace va
