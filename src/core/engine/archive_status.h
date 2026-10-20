#pragma once
// What the user sees about each archive (R-21, §B17): never a bare "integrity
// status", but the last FULL verify and the last verify of any mode, each with
// its result and date, plus OVERDUE flags from policy (R-105).
#include "common/config.h"
#include "core/engine/recorded_manifest.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <chrono>
#include <optional>
#include <vector>

namespace va {

struct ArchiveStatus {
    ArchiveRecord archive;
    std::optional<ManifestHeader> header;     // latest revision, from its manifest
    // The latest revision's manifest cannot be used (decision 0028): the
    // archive is listed with this problem, and without a header.
    std::optional<UnusableManifest> unusable_manifest;
    // The archive is retired and that manifest is lost (decision 0028, point
    // 9; lost_by_a_retired_archive): it is listed as retired with its
    // manifest lost, which needs nothing of the operator any more, and
    // matching no longer takes it for a possible owner of a folder or volume.
    bool manifest_lost = false;
    // Every other recorded revision whose manifest cannot be used, read as
    // matching reads them for the earlier carriers (decision 0004): line 1
    // alone. Only where `StatusSelection::earlier_revisions` asks for them.
    std::vector<UnusableManifest> earlier_unusable;
    std::optional<VerifyRunRecord> last_full; // last FULL run that reached a result
    std::optional<VerifyRunRecord> last_any;  // last run of any mode that reached a result
    std::optional<VerifyRunRecord> last_run;  // last run, INCOMPLETE included
    bool full_overdue = false;
    bool any_overdue = false;
    bool label_shared = false;                // R-88: another archive has the same label
};

// Which statuses archive_statuses reads: every archive's, or only that of the
// archive the operator named (`archive show <uuid>`, `history <uuid>`); and
// each one's header, or none where nothing shown comes from a header (a
// history, a list of labels to choose from). A manifest is read only for a
// header that is shown.
struct StatusSelection {
    std::optional<Uuid> only;
    bool headers = true;
    // Also the manifests of the archive's other revisions (`archive show`),
    // which only matching reads otherwise (decision 0028).
    bool earlier_revisions = false;
};

// INCOMPLETE runs make no claim, so they never reset the overdue clock. A never
// verified archive is overdue once its registration is older than the limit.
// Retired archives are never overdue (decision 0003). Each header is read from
// the latest revision's manifest (load_recorded_header), never taken from the
// index's copy (R-14): line 1 alone, or the whole manifest, read as `repo
// rebuild-index` reads one, where the last rebuild skipped it (its mark,
// skipped_manifest_key). An archive whose latest manifest cannot be used (it
// is missing, cannot be read, has changed, or the index says otherwise of it
// or has no record of it) is listed with that problem (unusable_manifest) and
// without a header, like every other archive: one damaged archive never stops
// the list (decision 0028); a retired archive whose latest manifest is lost
// is listed so as well, with its manifest lost (manifest_lost). Only the
// database's failures fail it. Whether a label is shared (R-88) is judged
// among every archive, whatever `selection` reads.
std::expected<std::vector<ArchiveStatus>, IoError> archive_statuses(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const Policy& policy,
    std::chrono::sys_seconds now, const StatusSelection& selection = {});

// Whether `archive` is retired and its latest manifest is lost (decision
// 0028, point 9), judged as archive_statuses judges it for the lists
// (ArchiveStatus::manifest_lost), so that what the Archive List shows of the
// archive and of its unfinished verifications agrees: line 1 alone, or the
// whole manifest where the last `repo rebuild-index` skipped it, and the whole
// manifest behind an index that seems to be all that is wrong
// (lost_by_a_retired_archive). Such an archive's interrupted verifications
// can never be resumed (decision 0022, point 5). False for an archive that is
// not retired, without reading its manifest. Fails only where the database
// cannot be read.
std::expected<bool, IoError> retired_with_manifest_lost(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const ArchiveRecord& archive);

// "ARCH-0007", or "ARCH-0007 (28ff645e)" when the label is shared (R-88).
std::string display_label(const ArchiveStatus& status);

} // namespace va
