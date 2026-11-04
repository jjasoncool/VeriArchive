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
// retired with its latest manifest lost, so that identity matching cannot use
// that manifest, it is no longer taken for a possible owner of a folder or
// volume, so it stops nothing. As that cannot be seen from the archive's
// record, retiring an archive whose latest manifest is lost so needs the
// operator's confirmation, after saying what it means.
//
// An archive is not retired while an operation that would give it a new
// revision is unfinished (decision 0046): a registration (decision 0025), a
// relocation (decision 0026) or a FULL verify that adds chunk hashes (decision
// 0033). Each of them would otherwise record its revision over the
// retirement; they finish, or are abandoned, first.
#include "core/engine/add_chunk_hashes.h"
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
    // Its latest manifest, where it is lost as matching judges a retired
    // archive's (decision 0028, point 9; lost_by_a_retired_archive): matching
    // cannot use it, and not only the index is wrong of it. Retired, the
    // archive leaves matching. Its plain message is the one of an archive as
    // it is when this is read: before the archive is retired, it names the
    // last resort, a registration over the lost manifest, which is the other
    // way where the device is not gone, and this retirement.
    std::optional<UnusableManifest> lost_manifest;
    bool already_retired = false;  // nothing changed: the archive was retired before
};

struct RetireFailure {
    enum class Kind {
        Refused,      // nothing was changed: an unfinished operation that would give the archive a new revision,
                      // or it changed meanwhile (exit 12)
        Unconfirmed,  // its latest manifest is lost and --confirm was not given: nothing written (exit 12)
        Repository,   // the database, or a manifest, could not be read, or the database written (exit 11)
    };
    Kind kind;
    std::string message;
    // Unconfirmed: what retiring the archive would do once confirmed.
    std::optional<Retirement> retirement = std::nullopt;
    // Refused because an operation that would give the archive a new
    // revision is unfinished: that operation, as the refusal found it, whose
    // ways on the windows name in their own words in place of the commands
    // that `message` names (app::refusal_in_window).
    std::optional<RefusingOperation> refusing = std::nullopt;
};

// What retiring the archive would do, read without writing anything, as
// retire_archive judges it: the archive, its revision, its latest manifest
// where it is lost so that retiring it needs the operator's confirmation, or
// that it is retired already; or why it would be refused now (an operation
// that would give it a new revision is unfinished, with `refusing`), or why it
// cannot be told (Repository). Never Unconfirmed: a Retirement with
// `lost_manifest` says that a confirmation is needed. What it reads can
// change before the retirement, which checks it all again.
std::expected<Retirement, RetireFailure> retirement_of(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const Uuid& archive_uuid);

// Retires the archive. Its latest manifest is judged as matching judges a
// retired archive's (load_candidate_pool, lost_by_a_retired_archive): line 1
// alone, and the whole manifest where line 1 alone says that only the index
// is wrong of it. One that matching can use (also one changed only in its
// body, which a verification fails on), or of which only the index is wrong
// (a rebuild gives it back), is not lost so: the archive stays as matching
// sees it, and is retired as before, without a confirmation, as is an
// archive without a revision. Where the manifest is lost so, it is retired
// only when `confirmed`. Either is retired in one write transaction that finds
// no operation unfinished that would give the archive a new revision, which a
// retired archive takes no more, and the archive whose manifest is lost still
// at the revision whose manifest was checked. An archive retired before is
// left as it is.
std::expected<Retirement, RetireFailure> retire_archive(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const RetireRequest& request);

} // namespace va
