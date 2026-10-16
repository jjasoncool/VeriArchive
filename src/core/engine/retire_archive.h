#pragma once
// Retiring an archive (decision 0003): the operator no longer expects to
// verify it. It takes no new revision and keeps its history.
//
// Where the archive's latest manifest is lost and the device that held the
// archive is gone as well (broken, lost or wiped), no root is left for a
// registration over the lost manifest (decision 0028, point 8), and where
// the manifest's line 1 cannot be used, the lost manifest stopped every
// registration of a new folder or volume and every relocation for good. The
// operator declares it by retiring the archive (decision 0028, point 9):
// retired with its latest manifest lost, it is no longer taken for a possible
// owner of a folder or volume, so it stops nothing. As that cannot be seen
// from the archive's record, retiring an archive whose latest manifest is
// lost needs the operator's confirmation, after saying what it means.
#include "core/engine/recorded_manifest.h"
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace va {

struct RetireRequest {
    Uuid archive_uuid;
    // --confirm: needed only where the archive's latest manifest is lost.
    bool confirmed = false;
};

// The archive retired, or to be retired once confirmed.
struct Retirement {
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 0;  // the archive's revision
    // Its latest manifest, where it is lost (decision 0028, point 9): it
    // cannot be used, and not only the index is wrong of it. Its plain
    // message is the one of an archive as it is when this is read: before the
    // archive is retired, it names the last resort, a registration over the
    // lost manifest, which is the other way where the device is not gone.
    std::optional<UnusableManifest> lost_manifest;
    bool already_retired = false;  // nothing changed: the archive was retired before
};

struct RetireFailure {
    enum class Kind {
        Refused,      // nothing was changed: an unfinished registration of the archive, or it changed meanwhile (exit 12)
        Unconfirmed,  // its latest manifest is lost and --confirm was not given: nothing written (exit 12)
        Repository,   // the database, or a manifest, could not be read, or the database written (exit 11)
    };
    Kind kind;
    std::string message;
    // Unconfirmed: what retiring the archive would do once confirmed.
    std::optional<Retirement> retirement = std::nullopt;
};

// Retires the archive. Its latest manifest is checked as a verification of
// the archive by its uuid checks it (check_recorded_manifest): one that can
// be used, or of which only the index is wrong (a rebuild gives it back), is
// not lost, and the archive is retired as before, without a confirmation, as
// is an archive without a revision. Where the manifest is lost, it is retired
// only when `confirmed`, in one write transaction that finds the archive
// still at the revision whose manifest was checked, and no registration of it
// unfinished: that registration would give it a new revision, which a
// retired archive takes no more. An archive retired before is left as it is.
std::expected<Retirement, RetireFailure> retire_archive(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const RetireRequest& request);

} // namespace va
