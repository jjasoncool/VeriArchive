#include "core/engine/archive_status.h"

#include "common/time.h"
#include "core/engine/recorded_manifest.h"
#include "core/repository/database_state.h"

#include <map>
#include <set>

namespace va {
namespace {

bool older_than(const std::string& timestamp, int days, std::chrono::sys_seconds now)
{
    const auto when = parse_timestamp(timestamp);
    return when && now - *when > std::chrono::days(days);
}

// The latest manifest of `archive` as the archive lists read it: its header,
// or why it cannot be used; an archive whose first registration never
// finished (revision 0) has neither.
struct LatestManifest {
    std::optional<ManifestHeader> header;
    std::optional<UnusableManifest> unusable;
    bool lost = false;  // the archive is retired and that manifest is lost (decision 0028, point 9)
};

// Reads it from the manifest (R-14), never from the index's copy of its
// header: line 1 alone, or, where the last `repo rebuild-index` skipped it
// (`whole`, its mark), as the rebuild reads it, whole.
std::expected<LatestManifest, IoError> latest_manifest(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const ArchiveRecord& archive, bool whole)
{
    LatestManifest latest;
    const auto revision = store.revision(archive.archive_uuid, archive.current_revision);
    if (!revision) {
        return std::unexpected(revision.error());
    }
    if (*revision) {
        auto header = load_recorded_header(repository_files, layout, **revision, whole);
        if (header) {
            latest.header = std::move(*header);
        } else {
            latest.unusable = std::move(header.error());
        }
    } else if (archive.current_revision > 0) {
        // The archive's record says a revision of which the index has no
        // record, as where `repo rebuild-index` could not read its manifest:
        // that manifest cannot be used either (decision 0028).
        latest.unusable =
            manifest_not_indexed(repository_files, layout, archive.archive_uuid, archive.current_revision, whole);
    }
    // A retired archive takes no new revision, not even over its lost
    // manifest, and where that manifest is lost, the archive is retired with
    // its manifest lost (decision 0028, point 9), judged as matching judges
    // it: behind an index that seems to be all that is wrong, the whole
    // manifest, and `unusable` says what that read found.
    if (latest.unusable) {
        latest.unusable->retired = archive.status == "RETIRED";
        const auto lost = lost_by_a_retired_archive(store, repository_files, layout, *latest.unusable);
        if (!lost) {
            return std::unexpected(lost.error());
        }
        latest.lost = *lost;
    }
    return latest;
}

} // namespace

std::expected<std::vector<ArchiveStatus>, IoError> archive_statuses(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const Policy& policy,
    std::chrono::sys_seconds now, const StatusSelection& selection)
{
    // A database that `repo rebuild-index` made again and has not filled
    // lists no archive: it says so rather than show an empty repository, as
    // to a window opened before it was made (decision 0036).
    const auto unfilled = store.setting(replaced_database_key);
    if (!unfilled) {
        return std::unexpected(unfilled.error());
    }
    if (!unfilled->empty()) {
        return std::unexpected(IoError{IoErrorKind::Other, 0,
            unusable_database_message(layout, DatabaseState::Unfilled)});
    }
    const auto archives = store.archives();
    if (!archives) {
        return std::unexpected(archives.error());
    }
    std::map<std::string, int> label_uses;
    for (const auto& archive : *archives) {
        ++label_uses[archive.label];
    }
    // The archives whose revision's manifest the last `repo rebuild-index`
    // skipped (decision 0028): their manifest is read as the rebuild reads
    // it, whole, as line 1 alone says nothing of a body or an entry that it
    // could not read, until a rebuild reads it and so removes the mark.
    std::set<Uuid> skipped;
    if (selection.headers) {
        const auto marks = store.settings_with_prefix(skipped_manifest_prefix);
        if (!marks) {
            return std::unexpected(marks.error());
        }
        for (const auto& [key, value] : *marks) {
            skipped.insert(key.substr(skipped_manifest_prefix.size()));
        }
    }

    std::vector<ArchiveStatus> statuses;
    for (const auto& archive : *archives) {
        if (selection.only && archive.archive_uuid != *selection.only) {
            continue;
        }
        ArchiveStatus status;
        status.archive = archive;
        status.label_shared = label_uses[archive.label] > 1;

        if (selection.headers) {
            auto latest =
                latest_manifest(store, repository_files, layout, archive, skipped.contains(archive.archive_uuid));
            if (!latest) {
                return std::unexpected(latest.error());
            }
            status.header = std::move(latest->header);
            status.unusable_manifest = std::move(latest->unusable);
            status.manifest_lost = latest->lost;
        }
        if (selection.earlier_revisions) {
            // The other revisions' manifests, which matching reads where the
            // archive's latest carrier does not decide (decision 0004): one
            // that cannot be used stops it (decision 0028, point 4).
            const auto revisions = store.revisions(archive.archive_uuid);
            if (!revisions) {
                return std::unexpected(revisions.error());
            }
            for (const auto& revision : *revisions) {
                if (revision.revision == archive.current_revision) {
                    continue;
                }
                if (auto header = load_recorded_header(repository_files, layout, revision); !header) {
                    auto unusable = std::move(header.error());
                    unusable.latest = false;
                    status.earlier_unusable.push_back(std::move(unusable));
                }
            }
        }

        const auto runs = store.verify_runs(archive.archive_uuid);
        if (!runs) {
            return std::unexpected(runs.error());
        }
        for (const auto& run : *runs) {
            status.last_run = run;
            if (run.result == "INCOMPLETE") {
                continue;
            }
            status.last_any = run;
            if (run.mode == "FULL") {
                status.last_full = run;
            }
        }

        if (archive.status != "RETIRED") {
            status.full_overdue = older_than(
                status.last_full ? status.last_full->finished : archive.created,
                policy.full_verify_max_age_days, now);
            status.any_overdue = older_than(
                status.last_any ? status.last_any->finished : archive.created,
                policy.any_verify_max_age_days, now);
        }
        statuses.push_back(std::move(status));
    }
    return statuses;
}

std::expected<bool, IoError> retired_with_manifest_lost(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const ArchiveRecord& archive)
{
    if (archive.status != "RETIRED") {
        return false;
    }
    // As the lists read it: whole where the last rebuild skipped it, as they
    // show "Manifest lost: " for it then.
    const auto mark = store.setting(skipped_manifest_key(archive.archive_uuid));
    if (!mark) {
        return std::unexpected(mark.error());
    }
    const auto latest = latest_manifest(store, repository_files, layout, archive, !mark->empty());
    if (!latest) {
        return std::unexpected(latest.error());
    }
    return latest->lost;
}

std::string display_label(const ArchiveStatus& status)
{
    if (!status.label_shared) {
        return status.archive.label;
    }
    return status.archive.label + " (" + status.archive.archive_uuid.substr(0, 8) + ")";
}

} // namespace va
