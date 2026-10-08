#include "core/engine/recorded_manifest.h"

#include "common/utf8.h"
#include "core/manifest/manifest_reader.h"

#include <algorithm>

namespace va {
namespace {

std::unexpected<ManifestLoadFailure> fail(std::string failure_class, std::string message)
{
    return std::unexpected(ManifestLoadFailure{std::move(failure_class), std::move(message)});
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
    if (reader->header_blake3() != (*record)->header_blake3 ||
        reader->header().body_blake3 != (*record)->body_blake3 ||
        reader->header().archive_uuid != archive_uuid || reader->header().revision != revision) {
        return fail("ManifestHashMismatch", "the manifest differs from the repository record");
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

} // namespace va
