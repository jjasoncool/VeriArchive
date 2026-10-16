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
    return std::unexpected(ManifestLoadFailure{std::move(failure_class), std::move(message), std::nullopt});
}

// The manifest cannot be used (decision 0028): the failure says so in its
// plain words and with its failure class.
std::unexpected<ManifestLoadFailure> fail(const UnusableManifest& unusable)
{
    return std::unexpected(ManifestLoadFailure{failure_class_of(unusable), plain_message(unusable), unusable});
}

// What has changed, when a manifest is not the one recorded.
constexpr const char* first_line_differs = "its first line differs from what the repository recorded";
constexpr const char* body_differs = "its body does not match its header";
constexpr const char* read_otherwise = "it gave other bytes when it was read again";

UnusableManifest unusable_manifest(const RevisionRecord& record, UnusableManifest::Kind kind, std::string cause = {})
{
    return {kind, record.archive_uuid, record.revision, record.manifest_relpath, std::move(cause)};
}

// What a failure of the reader says of a recorded manifest. `entries`: the
// failure came while the entries were read, after the body was validated.
UnusableManifest unusable_manifest(const RevisionRecord& record, const ManifestFailure& failure, bool entries)
{
    using Kind = UnusableManifest::Kind;
    switch (failure.kind) {
    case ManifestFailure::Kind::Io:
        return failure.io_kind == IoErrorKind::NotFound ? unusable_manifest(record, Kind::Missing)
                                                        : unusable_manifest(record, Kind::Unreadable, failure.message);
    case ManifestFailure::Kind::HeaderMismatch:
        return unusable_manifest(record, Kind::Changed, first_line_differs);
    case ManifestFailure::Kind::BodyMismatch:
        return unusable_manifest(record, Kind::Changed, entries ? read_otherwise : body_differs);
    case ManifestFailure::Kind::Malformed:
        break;
    }
    // Bytes that are the ones recorded and still do not parse: no reading of
    // them gives entries.
    return unusable_manifest(record, Kind::Unreadable, failure.message);
}

// Whether the header parsed from a manifest's line 1 names the archive,
// revision and body that `record` was recorded with (R-14). Asked once that
// line has the BLAKE3 recorded for it: the line is then the one recorded, and
// it vouches for its body (R-60). A record that names another archive,
// revision or body contradicts the line it vouches for itself, so it is the
// index that differs from the manifest (decision 0028), and rebuilding the
// index from the manifest gives back the record written with it.
bool names_what_was_recorded(const RevisionRecord& record, const ManifestHeader& header)
{
    return header.body_blake3 == record.body_blake3 && header.archive_uuid == record.archive_uuid &&
           header.revision == record.revision;
}

// The recorded manifest of `revision`, opened and checked against its record
// up to its body's hash (R-14, R-62, R-15), and the record.
struct OpenedManifest {
    ManifestReader reader;
    RevisionRecord record;
};

std::expected<OpenedManifest, ManifestLoadFailure> open_recorded(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const Uuid& archive_uuid,
    std::uint32_t revision)
{
    const auto record = store.revision(archive_uuid, revision);
    const auto all = store.revisions(archive_uuid);
    if (!record || !*record || !all) {
        return fail("RepositoryError", "revision " + std::to_string(revision) + " is not recorded");
    }

    const auto path = layout.root / path_from_utf8((*record)->manifest_relpath);
    // Line 1 is compared with the record before it is parsed (decision 0028):
    // a header changed into text that no longer parses has changed as well.
    auto reader = ManifestReader::open(repository_files, path, (*record)->header_blake3);
    if (!reader) {
        return fail(unusable_manifest(**record, reader.error(), false));
    }
    // Line 1 is the one recorded, and the body the one it names (R-62): the
    // manifest is intact, and what still differs is the index.
    if (!names_what_was_recorded(**record, reader->header())) {
        return fail(unusable_manifest(**record, UnusableManifest::Kind::IndexDiffers));
    }
    // The index's copy of the header is the manifest's own (CLAUDE.md rule 3).
    if (reader->header_line() != (*record)->header_json) {
        return fail(unusable_manifest(**record, UnusableManifest::Kind::IndexDiffers));
    }
    const auto highest = std::max_element(all->begin(), all->end(),
        [](const RevisionRecord& left, const RevisionRecord& right) { return left.revision < right.revision; });
    if (highest != all->end() && revision < highest->revision) {
        return fail("ManifestRevisionRollback",
            "revision " + std::to_string(revision) + " is older than the known revision " +
                std::to_string(highest->revision));
    }
    return OpenedManifest{std::move(*reader), **record};
}

} // namespace

std::string failure_class_of(const UnusableManifest& unusable)
{
    return unusable.kind == UnusableManifest::Kind::Changed ? "ManifestHashMismatch" : "RepositoryError";
}

std::string what_is_wrong_with(const UnusableManifest& unusable)
{
    const auto revision = std::to_string(unusable.revision);
    const auto manifest = "the manifest of revision " + revision + " (" + unusable.manifest + ")";
    switch (unusable.kind) {
    case UnusableManifest::Kind::Missing:
        return manifest + " is missing";
    case UnusableManifest::Kind::Unreadable:
        return manifest + " cannot be read: " + unusable.cause;
    case UnusableManifest::Kind::Changed:
        return manifest + " has changed: " + unusable.cause;
    case UnusableManifest::Kind::IndexDiffers:
        break;
    }
    return "the repository's index differs from the manifest of revision " + revision + " (" + unusable.manifest + ")";
}

std::string plain_message(const UnusableManifest& unusable)
{
    // A manifest that is missing or has changed is only in the repository's
    // backup (R-86); an index that differs is rebuilt from the manifests (R-80).
    return what_is_wrong_with(unusable) + (unusable.kind == UnusableManifest::Kind::IndexDiffers
                                                  ? "; rebuild the index with `repo rebuild-index`"
                                                  : "; restore it from a backup of the repository");
}

std::expected<RecordedManifest, ManifestLoadFailure> load_recorded_manifest(
    ArchiveStore& store,
    IFileSystemAccess& repository_files,
    const RepositoryLayout& layout,
    const Uuid& archive_uuid,
    std::uint32_t revision)
{
    auto opened = open_recorded(store, repository_files, layout, archive_uuid, revision);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    RecordedManifest manifest;
    manifest.header = opened->reader.header();
    for (;;) {
        auto next = opened->reader.next();
        if (!next) {
            // The entries come from a second read of the manifest, checked at
            // the end of the body as the first was.
            return fail(unusable_manifest(opened->record, next.error(), true));
        }
        if (!*next) {
            return manifest;
        }
        manifest.entries.push_back(std::move(**next));
    }
}

std::expected<ManifestHeader, ManifestLoadFailure> check_recorded_manifest(
    ArchiveStore& store,
    IFileSystemAccess& repository_files,
    const RepositoryLayout& layout,
    const Uuid& archive_uuid,
    std::uint32_t revision)
{
    auto opened = open_recorded(store, repository_files, layout, archive_uuid, revision);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    return opened->reader.header();
}

std::expected<ManifestHeader, UnusableManifest> load_recorded_header(
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const RevisionRecord& revision)
{
    using Kind = UnusableManifest::Kind;
    const auto line =
        ManifestReader::read_header_line(repository_files, layout.root / path_from_utf8(revision.manifest_relpath));
    if (!line) {
        // A line 1 that ends without a newline, or an empty file, is not the
        // line recorded either (decision 0028).
        return std::unexpected(line.error().kind == ManifestFailure::Kind::Io
                                   ? unusable_manifest(revision, line.error(), false)
                                   : unusable_manifest(revision, Kind::Changed, first_line_differs));
    }
    // The record's hash first: line 1 of a changed manifest may not parse.
    const auto line_blake3 = blake3_hex(*line);
    if (line_blake3 != revision.header_blake3) {
        return std::unexpected(unusable_manifest(revision, Kind::Changed, first_line_differs));
    }
    ManifestHeader header;
    try {
        header = header_from_json(nlohmann::json::parse(*line));
    } catch (const std::exception& error) {
        return std::unexpected(
            unusable_manifest(revision, Kind::Unreadable, "invalid manifest header: " + json_error_text(error)));
    }
    // Line 1 is the one recorded: a record that names another archive,
    // revision or body, or a copy of the header that is not that line, is
    // the index differing from the manifest.
    if (!names_what_was_recorded(revision, header)) {
        return std::unexpected(unusable_manifest(revision, Kind::IndexDiffers));
    }
    if (*line != revision.header_json) {
        return std::unexpected(unusable_manifest(revision, Kind::IndexDiffers));
    }
    return header;
}

} // namespace va
