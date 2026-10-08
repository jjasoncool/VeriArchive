#include "core/engine/archive_status.h"

#include "common/time.h"
#include "core/engine/candidates.h"

#include <map>

namespace va {
namespace {

bool older_than(const std::string& timestamp, int days, std::chrono::sys_seconds now)
{
    const auto when = parse_timestamp(timestamp);
    return when && now - *when > std::chrono::days(days);
}

} // namespace

std::expected<std::vector<ArchiveStatus>, IoError> archive_statuses(
    ArchiveStore& store, const Policy& policy, std::chrono::sys_seconds now)
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
        ArchiveStatus status;
        status.archive = archive;
        status.label_shared = label_uses[archive.label] > 1;

        const auto revision = store.revision(archive.archive_uuid, archive.current_revision);
        if (!revision) {
            return std::unexpected(revision.error());
        }
        if (*revision) {
            if (auto header = recorded_header(**revision)) {
                status.header = std::move(*header);
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
