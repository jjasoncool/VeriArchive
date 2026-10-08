#pragma once
// Relocation (decision 0004): the operator copied an archive to a new carrier
// with their own tools, or put its disk in a new enclosure (MIGRATED).
// VeriArchive copies nothing; it FULL-verifies the new carrier against the latest
// revision and, if every entry matches, writes the next revision with the same
// content hashes and the new carrier's identity, so the archive keeps its uuid
// and history and stays verifiable.
#include "core/engine/candidates.h"
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace va {

struct RelocateRequest {
    Uuid archive_uuid;
    std::filesystem::path root;             // the archive root on the new carrier
    std::optional<bool> rotational;
};

struct RelocateServices {
    IFileSystemAccess& archive_files;
    IFileSystemAccess& repository_files;
    IFileWriter& writer;
    IMediaEnumerator& media;
    ArchiveStore& store;
    RepositoryLayout layout;
    std::string repository_uuid;
    HostInfo host;
};

struct RelocateOutcome {
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 0;             // the new revision
    std::string result;                     // PASSED or PASSED_WITH_WARNINGS
    std::string run_id;                     // the FULL verification that accepted the carrier
    std::map<std::string, std::uint64_t> counts;
    std::vector<VerifyItemRecord> items;
    std::filesystem::path manifest;
};

struct RelocateFailure {
    enum class Kind { Refused, Differs, Media, Repository, Incomplete };
    Kind kind;
    std::string message;
    std::vector<VerifyItemRecord> items;    // for Differs: what does not match
};

std::expected<RelocateOutcome, RelocateFailure> relocate_archive(
    RelocateServices& services,
    const RelocateRequest& request,
    CancelToken& cancel,
    IProgressSink& progress);

} // namespace va
