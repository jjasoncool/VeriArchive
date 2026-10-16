#include "core/engine/recorded_manifest.h"

#include "common/json_error.h"
#include "common/utf8.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"

#include <algorithm>
#include <format>

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
    // them gives entries. (The reader tells a read of the entries that gave
    // other bytes, also ones that do not parse, from them: BodyMismatch.)
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
// up to its body's hash (R-14, R-62, R-15), and the record. `latest`: the
// revision is the archive's latest, as the archive's record says it.
struct OpenedManifest {
    ManifestReader reader;
    RevisionRecord record;
    bool latest = true;
};

// `unusable` as a manifest of the archive's latest revision or of an earlier one.
UnusableManifest of_revision(UnusableManifest unusable, bool latest)
{
    unusable.latest = latest;
    return unusable;
}

std::expected<OpenedManifest, ManifestLoadFailure> open_recorded(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const Uuid& archive_uuid,
    std::uint32_t revision)
{
    const auto record = store.revision(archive_uuid, revision);
    const auto all = store.revisions(archive_uuid);
    const auto archive = store.find_archive(archive_uuid);
    if (!record || !all || !archive) {
        return fail("RepositoryError", (!record ? record.error() : !all ? all.error() : archive.error()).message);
    }
    // The archive's latest revision, whose lost manifest has a last resort
    // (decision 0028, point 8); an archive without a record has no earlier one.
    const bool latest = !*archive || revision >= (*archive)->current_revision;
    if (!*record) {
        // The archive's revision, as its record says it, of which the index
        // has no record (decision 0028): its manifest cannot be used, and
        // what the file at its place is says how to fix it.
        if (*archive && revision > 0 && (*archive)->current_revision == revision) {
            return fail(manifest_not_indexed(repository_files, layout, archive_uuid, revision, true));
        }
        return fail("RepositoryError", "revision " + std::to_string(revision) + " is not recorded");
    }

    const auto path = layout.root / path_from_utf8((*record)->manifest_relpath);
    // Line 1 is compared with the record before it is parsed (decision 0028):
    // a header changed into text that no longer parses has changed as well.
    auto reader = ManifestReader::open(repository_files, path, (*record)->header_blake3);
    if (!reader) {
        return fail(of_revision(unusable_manifest(**record, reader.error(), false), latest));
    }
    // Line 1 is the one recorded, and the body the one it names (R-62): the
    // manifest is intact, and what still differs is the index.
    if (!names_what_was_recorded(**record, reader->header())) {
        return fail(of_revision(unusable_manifest(**record, UnusableManifest::Kind::IndexDiffers), latest));
    }
    // The index's copy of the header is the manifest's own (CLAUDE.md rule 3).
    if (reader->header_line() != (*record)->header_json) {
        return fail(of_revision(unusable_manifest(**record, UnusableManifest::Kind::IndexDiffers), latest));
    }
    const auto highest = std::max_element(all->begin(), all->end(),
        [](const RevisionRecord& left, const RevisionRecord& right) { return left.revision < right.revision; });
    if (highest != all->end() && revision < highest->revision) {
        return fail("ManifestRevisionRollback",
            "revision " + std::to_string(revision) + " is older than the known revision " +
                std::to_string(highest->revision));
    }
    return OpenedManifest{std::move(*reader), **record, latest};
}

} // namespace

std::string failure_class_of(const UnusableManifest& unusable)
{
    return unusable.kind == UnusableManifest::Kind::Changed ? "ManifestHashMismatch" : "RepositoryError";
}

std::string_view kind_code(UnusableManifest::Kind kind)
{
    switch (kind) {
    case UnusableManifest::Kind::Missing:
        return "missing";
    case UnusableManifest::Kind::Unreadable:
        return "unreadable";
    case UnusableManifest::Kind::Changed:
        return "changed";
    case UnusableManifest::Kind::NotIndexed:
        return "not_indexed";
    case UnusableManifest::Kind::IndexDiffers:
        break;
    }
    return "index_differs";
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
    case UnusableManifest::Kind::NotIndexed: {
        const auto no_record = "the repository's index has no record of " + manifest;
        switch (unusable.file) {
        case UnusableManifest::File::Missing:
            return no_record + ", which is missing";
        case UnusableManifest::File::Unreadable:
            return no_record + ", which cannot be read: " + unusable.cause;
        case UnusableManifest::File::Readable:
            break;
        }
        return no_record;
    }
    case UnusableManifest::Kind::IndexDiffers:
        break;
    }
    return "the repository's index differs from the manifest of revision " + revision + " (" + unusable.manifest + ")";
}

std::string plain_message(const UnusableManifest& unusable)
{
    // An index that differs from an intact manifest, or has no record of one
    // that reads as the manifest of its revision, is rebuilt from the
    // manifests (R-80): nothing of the manifest is lost.
    if (unusable.kind == UnusableManifest::Kind::IndexDiffers ||
        (unusable.kind == UnusableManifest::Kind::NotIndexed && unusable.file == UnusableManifest::File::Readable)) {
        return what_is_wrong_with(unusable) + "; rebuild the index with `repo rebuild-index`";
    }
    // A manifest that is missing, cannot be read or has changed is put back
    // first, with `import` of its file, wherever one is kept (R-86, R-87,
    // R-89): the import puts back the manifest recorded, or records it where
    // the index has no record of it (decision 0028, point 7).
    const auto revision = std::to_string(unusable.revision);
    auto message = what_is_wrong_with(unusable) + "; put it back with `import <folder>/" +
                   std::format("r{:04}.vam", unusable.revision) + "` from a backup of the repository, an export of revision " +
                   revision + ", or the media copy in the archive's .veriarchive folder, where registration wrote one";
    // Of the latest revision, the last resort where none of these holds it
    // (decision 0028, point 8). An earlier revision has another revision
    // after it already.
    if (unusable.latest) {
        message += "; if none of these holds it, register revision " + std::to_string(unusable.revision + 1) +
                   " from what the archive's root holds now with `register <root_path> --lost-manifest " +
                   unusable.archive_uuid + "`";
    }
    return message;
}

std::string manifest_relpath_of(const Uuid& archive_uuid, std::uint32_t revision)
{
    return "manifests/" + archive_uuid + "/" + std::format("r{:04}.vam", revision);
}

UnusableManifest manifest_not_indexed(IFileSystemAccess& repository_files, const RepositoryLayout& layout,
    const Uuid& archive_uuid, std::uint32_t revision, bool whole)
{
    using File = UnusableManifest::File;
    UnusableManifest unusable{UnusableManifest::Kind::NotIndexed, archive_uuid, revision,
        manifest_relpath_of(archive_uuid, revision), {}, File::Readable};
    const auto unreadable = [&](std::string cause) {
        unusable.file = File::Unreadable;
        unusable.cause = std::move(cause);
        return unusable;
    };
    const auto from_failure = [&](const ManifestFailure& failure) {
        if (failure.kind == ManifestFailure::Kind::Io && failure.io_kind == IoErrorKind::NotFound) {
            unusable.file = File::Missing;
            return unusable;
        }
        return unreadable(failure.message);
    };
    // No record vouches for it, so it is read as `repo rebuild-index` reads
    // a manifest: one that parses, of this archive and revision, and where
    // the whole of it is read, its body matching its header (R-62) and every
    // entry parsing, to the end of the body.
    const auto path = manifest_path(layout.manifests(), archive_uuid, revision);
    ManifestHeader header;
    if (whole) {
        auto reader = ManifestReader::open(repository_files, path);
        if (!reader) {
            return from_failure(reader.error());
        }
        if (reader->header().archive_uuid != archive_uuid || reader->header().revision != revision) {
            return unreadable("header does not match its file name");
        }
        // A manifest whose entries the rebuild cannot all read is one it
        // skips: rebuilding the index alone would not record it.
        for (;;) {
            auto next = reader->next();
            if (!next) {
                return unreadable(next.error().message);
            }
            if (!*next) {
                return unusable;
            }
        }
    } else {
        const auto line = ManifestReader::read_header_line(repository_files, path);
        if (!line) {
            return from_failure(line.error());
        }
        try {
            header = header_from_json(nlohmann::json::parse(*line));
        } catch (const std::exception& error) {
            return unreadable("invalid manifest header: " + json_error_text(error));
        }
    }
    if (header.archive_uuid != archive_uuid || header.revision != revision) {
        return unreadable("header does not match its file name");
    }
    return unusable;
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
            return fail(of_revision(unusable_manifest(opened->record, next.error(), true), opened->latest));
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

std::expected<ManifestHeader, UnusableManifest> load_recorded_header(IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const RevisionRecord& revision, bool whole)
{
    using Kind = UnusableManifest::Kind;
    if (whole) {
        // As open_recorded and load_recorded_manifest read it, without the
        // rollback check (R-15), which the lists do not make, and without
        // keeping the entries.
        auto reader = ManifestReader::open(
            repository_files, layout.root / path_from_utf8(revision.manifest_relpath), revision.header_blake3);
        if (!reader) {
            return std::unexpected(unusable_manifest(revision, reader.error(), false));
        }
        if (!names_what_was_recorded(revision, reader->header()) || reader->header_line() != revision.header_json) {
            return std::unexpected(unusable_manifest(revision, Kind::IndexDiffers));
        }
        for (;;) {
            auto next = reader->next();
            if (!next) {
                return std::unexpected(unusable_manifest(revision, next.error(), true));
            }
            if (!*next) {
                return reader->header();
            }
        }
    }
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
