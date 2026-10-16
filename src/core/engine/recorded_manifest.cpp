#include "core/engine/recorded_manifest.h"

#include "common/json_error.h"
#include "common/utf8.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_reader.h"

#include <algorithm>

namespace va {
namespace {

std::unexpected<ManifestLoadFailure> fail(std::string failure_class, std::string message)
{
    return std::unexpected(ManifestLoadFailure{std::move(failure_class), std::move(message)});
}

constexpr const char* differs_from_record = "the manifest differs from the repository record";

// Whether a manifest's line 1, by its BLAKE3, and the header parsed from it
// are the ones `record` was recorded with: the same bytes, naming the same
// archive, revision and body (R-14).
bool recorded_as(const RevisionRecord& record, const std::string& header_blake3, const ManifestHeader& header)
{
    return header_blake3 == record.header_blake3 && header.body_blake3 == record.body_blake3 &&
           header.archive_uuid == record.archive_uuid && header.revision == record.revision;
}

// The index's copy of a revision's header is not its manifest's line 1,
// though the manifest is the one recorded: the index is wrong, and is
// rebuilt from the manifests (R-80).
std::string index_differs(const RevisionRecord& record)
{
    return "the repository's index differs from the manifest of revision " + std::to_string(record.revision) +
           " of archive " + record.archive_uuid + "; rebuild the index with `repo rebuild-index`";
}

} // namespace

std::expected<RecordedManifest, ManifestLoadFailure> load_recorded_manifest(
    ArchiveStore& store,
    IFileSystemAccess& repository_files,
    const RepositoryLayout& layout,
    const Uuid& archive_uuid,
    std::uint32_t revision)
{
    const auto record = store.revision(archive_uuid, revision);
    const auto all = store.revisions(archive_uuid);
    if (!record || !*record || !all) {
        return fail("RepositoryError", "revision " + std::to_string(revision) + " is not recorded");
    }

    const auto path = layout.root / path_from_utf8((*record)->manifest_relpath);
    auto reader = ManifestReader::open(repository_files, path);
    if (!reader) {
        return fail(reader.error().kind == ManifestFailure::Kind::BodyMismatch ? "ManifestHashMismatch"
                                                                              : "RepositoryError",
            reader.error().message);
    }
    if (!recorded_as(**record, reader->header_blake3(), reader->header())) {
        return fail("ManifestHashMismatch", differs_from_record);
    }
    // The index's copy of the header is the manifest's own (CLAUDE.md rule 3).
    if (reader->header_line() != (*record)->header_json) {
        return fail("RepositoryError", index_differs(**record));
    }
    const auto highest = std::max_element(all->begin(), all->end(),
        [](const RevisionRecord& left, const RevisionRecord& right) { return left.revision < right.revision; });
    if (highest != all->end() && revision < highest->revision) {
        return fail("ManifestRevisionRollback",
            "revision " + std::to_string(revision) + " is older than the known revision " +
                std::to_string(highest->revision));
    }

    RecordedManifest manifest;
    manifest.header = reader->header();
    for (;;) {
        auto next = reader->next();
        if (!next) {
            return fail("RepositoryError", next.error().message);
        }
        if (!*next) {
            return manifest;
        }
        manifest.entries.push_back(std::move(**next));
    }
}

std::expected<ManifestHeader, IoError> load_recorded_header(
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const RevisionRecord& revision)
{
    // The manifest is gone, cannot be read, or is not the one recorded: only
    // the repository's backup still holds it (R-86). Rebuilding the index
    // would take whatever the file holds now for the truth.
    const auto cannot_use = [&](const std::string& cause) {
        return std::unexpected(IoError{IoErrorKind::Other, 0,
            "cannot use the manifest of revision " + std::to_string(revision.revision) + " of archive " +
                revision.archive_uuid + ": " + cause + "; restore it from a backup of the repository"});
    };
    const auto line =
        ManifestReader::read_header_line(repository_files, layout.root / path_from_utf8(revision.manifest_relpath));
    if (!line) {
        return cannot_use(line.error().message);
    }
    // The record's hash first: line 1 of a changed manifest may not parse.
    const auto line_blake3 = blake3_hex(*line);
    if (line_blake3 != revision.header_blake3) {
        return cannot_use(differs_from_record);
    }
    ManifestHeader header;
    try {
        header = header_from_json(nlohmann::json::parse(*line));
    } catch (const std::exception& error) {
        return cannot_use("invalid manifest header: " + json_error_text(error));
    }
    if (!recorded_as(revision, line_blake3, header)) {
        return cannot_use(differs_from_record);
    }
    if (*line != revision.header_json) {
        return std::unexpected(IoError{IoErrorKind::Other, 0, index_differs(revision)});
    }
    return header;
}

} // namespace va
