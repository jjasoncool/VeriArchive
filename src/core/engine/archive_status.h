#pragma once
// What the user sees about each archive (R-21, §B17): never a bare "integrity
// status", but the last FULL verify and the last verify of any mode, each with
// its result and date, plus OVERDUE flags from policy (R-105).
#include "common/config.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <chrono>
#include <optional>
#include <vector>

namespace va {

struct ArchiveStatus {
    ArchiveRecord archive;
    std::optional<ManifestHeader> header;     // latest revision, from its manifest
    std::optional<VerifyRunRecord> last_full; // last FULL run that reached a result
    std::optional<VerifyRunRecord> last_any;  // last run of any mode that reached a result
    std::optional<VerifyRunRecord> last_run;  // last run, INCOMPLETE included
    bool full_overdue = false;
    bool any_overdue = false;
    bool label_shared = false;                // R-88: another archive has the same label
};

// INCOMPLETE runs make no claim, so they never reset the overdue clock. A never
// verified archive is overdue once its registration is older than the limit.
// Retired archives are never overdue (decision 0003). Each header is read from
// the latest revision's manifest (load_recorded_header): one that cannot be
// read there, or that the index holds otherwise, fails the list, naming the
// archive and the revision, rather than show the index's copy (R-14).
std::expected<std::vector<ArchiveStatus>, IoError> archive_statuses(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const Policy& policy,
    std::chrono::sys_seconds now);

// "ARCH-0007", or "ARCH-0007 (28ff645e)" when the label is shared (R-88).
std::string display_label(const ArchiveStatus& status);

} // namespace va
