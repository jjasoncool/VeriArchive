#include "core/engine/archive_status.h"

#include "common/time.h"
#include "core/engine/recorded_manifest.h"

#include <map>

namespace va {
namespace {

bool older_than(const std::string& timestamp, int days, std::chrono::sys_seconds now)
{
    const auto when = parse_timestamp(timestamp);
    return when && now - *when > std::chrono::days(days);
}

} // namespace

std::expected<std::vector<ArchiveStatus>, IoError> archive_statuses(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const Policy& policy,
    std::chrono::sys_seconds now, const StatusSelection& selection)
{
    const auto archives = store.archives();
    if (!archives) {
        return std::unexpected(archives.error());
    }
    std::map<std::string, int> label_uses;
    for (const auto& archive : *archives) {
        ++label_uses[archive.label];
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
            const auto revision = store.revision(archive.archive_uuid, archive.current_revision);
            if (!revision) {
                return std::unexpected(revision.error());
            }
            if (*revision) {
                auto header = load_recorded_header(repository_files, layout, **revision);
                if (header) {
                    status.header = std::move(*header);
                } else {
                    status.unusable_manifest = std::move(header.error());
                }
            } else if (archive.current_revision > 0) {
                // The archive's record says a revision of which the index has
                // no record, as where `repo rebuild-index` could not read its
                // manifest: that manifest cannot be used either (decision 0028).
                status.unusable_manifest = manifest_not_indexed(
                    repository_files, layout, archive.archive_uuid, archive.current_revision, false);
            }
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
                    status.earlier_unusable.push_back(std::move(header.error()));
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

std::string display_label(const ArchiveStatus& status)
{
    if (!status.label_shared) {
        return status.archive.label;
    }
    return status.archive.label + " (" + status.archive.archive_uuid.substr(0, 8) + ")";
}

} // namespace va
