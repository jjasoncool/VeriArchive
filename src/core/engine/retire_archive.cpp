#include "core/engine/retire_archive.h"

#include <nlohmann/json.hpp>

namespace va {
namespace {

std::unexpected<RetireFailure> fail(RetireFailure::Kind kind, std::string message)
{
    return std::unexpected(RetireFailure{kind, std::move(message)});
}

// The archive's unfinished registration: the plan its key holds (decision
// 0022), whose revision is not recorded. A plan whose revision is recorded is
// done with (decision 0023), and a value that is no plan is no
// registration's. Read in the caller's transaction.
struct UnfinishedRegistration {
    std::uint32_t revision = 0;
    std::string root;
};

std::expected<std::optional<UnfinishedRegistration>, IoError> unfinished_registration(
    ArchiveStore& store, const Uuid& archive_uuid)
{
    const auto stored = store.setting("registration:" + archive_uuid);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    const auto plan = nlohmann::json::parse(*stored, nullptr, false);
    const auto revision = plan.is_object() ? plan.find("revision") : plan.end();
    if (stored->empty() || !plan.is_object() || revision == plan.end() || !revision->is_number_unsigned()) {
        return std::optional<UnfinishedRegistration>{};
    }
    UnfinishedRegistration registration;
    registration.revision = revision->get<std::uint32_t>();
    const auto recorded = store.revision_recorded(archive_uuid, registration.revision);
    if (!recorded) {
        return std::unexpected(recorded.error());
    }
    if (*recorded) {
        return std::optional<UnfinishedRegistration>{};
    }
    const auto root = plan.find("root");
    registration.root = root != plan.end() && root->is_string() ? root->get<std::string>() : std::string{};
    return std::optional<UnfinishedRegistration>(std::move(registration));
}

} // namespace

std::expected<Retirement, RetireFailure> retire_archive(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const RetireRequest& request)
{
    using Kind = RetireFailure::Kind;
    const auto& uuid = request.archive_uuid;
    const auto found = store.find_archive(uuid);
    if (!found) {
        return fail(Kind::Repository, found.error().message);
    }
    if (!*found) {
        return fail(Kind::Refused, "no archive " + uuid);
    }
    const auto archive = **found;
    Retirement retirement{archive.archive_uuid, archive.label, archive.current_revision, std::nullopt,
        archive.status == "RETIRED"};

    // Lost, as a registration over a lost manifest tells one (decision 0028,
    // point 8): the manifest of the archive's revision checked as a
    // verification of the archive by its uuid checks it, cannot be used, and
    // not only the index is wrong of it, which a rebuild repairs. A revision
    // older than one recorded (R-15) is no manifest that cannot be used.
    if (archive.current_revision > 0) {
        const auto checked =
            check_recorded_manifest(store, repository_files, layout, uuid, archive.current_revision);
        if (!checked) {
            const auto& failure = checked.error();
            if (failure.unusable) {
                if (!only_the_index_is_wrong(*failure.unusable)) {
                    retirement.lost_manifest = *failure.unusable;
                }
            } else if (failure.failure_class != "ManifestRevisionRollback") {
                return fail(Kind::Repository, failure.message);
            }
        }
    }
    // Retired before: there is nothing to change, nor to confirm.
    if (retirement.already_retired) {
        return retirement;
    }
    // Decision 0028, point 9: the archive's latest manifest is lost. Retired,
    // the archive is no longer taken for a possible owner of a folder or
    // volume, can no longer be verified and takes no new revision; unless the
    // operator confirmed that, nothing is written.
    if (retirement.lost_manifest && !request.confirmed) {
        return std::unexpected(
            RetireFailure{Kind::Unconfirmed, "run the command again with --confirm to retire the archive", retirement});
    }

    std::optional<RetireFailure> refused;
    const auto written = store.write_transaction([&]() -> std::expected<void, IoError> {
        const auto now = store.find_archive(uuid);
        if (!now) {
            return std::unexpected(now.error());
        }
        if (!*now) {
            refused = RetireFailure{Kind::Refused, "no archive " + uuid};
            return {};
        }
        if (retirement.lost_manifest) {
            // Still at the revision whose manifest was found lost: another
            // operation may have recorded a later one meanwhile, whose
            // manifest can be used.
            if ((*now)->current_revision != archive.current_revision) {
                refused = RetireFailure{Kind::Refused,
                    "archive " + uuid + " now has revision " + std::to_string((*now)->current_revision) +
                        ", not revision " + std::to_string(archive.current_revision) +
                        "; nothing was changed (run the command again)"};
                return {};
            }
            // A registration of the archive that is unfinished would give it
            // a new revision, which a retired archive takes no more; one made
            // for an older revision can no longer finish (decision 0030). In
            // the words of a registration that meets them (decision 0025).
            const auto unfinished = unfinished_registration(store, uuid);
            if (!unfinished) {
                return std::unexpected(unfinished.error());
            }
            if (*unfinished) {
                const auto& registration = **unfinished;
                const auto later = store.later_revision(uuid, registration.revision - 1);
                if (!later) {
                    return std::unexpected(later.error());
                }
                refused = RetireFailure{Kind::Refused,
                    *later ? "the unfinished registration of archive " + uuid + " (root \"" + registration.root +
                                 "\") would create revision " + std::to_string(registration.revision) +
                                 ", but the archive now has a later revision (" + std::to_string(**later) +
                                 "); the registration can no longer finish: abandon it with `archive "
                                 "abandon-registration " +
                                 uuid + "`"
                           : "an unfinished registration of archive " + uuid + " (root \"" + registration.root +
                                 "\") is creating revision " + std::to_string(registration.revision) +
                                 "; nothing was changed (finish it with `register --resume` first, or abandon it "
                                 "with `archive abandon-registration " +
                                 uuid + "`)"};
                return {};
            }
        }
        auto retired = **now;
        retired.status = "RETIRED";
        return store.update_archive(retired);
    });
    if (!written) {
        return fail(Kind::Repository, written.error().message);
    }
    if (refused) {
        return std::unexpected(std::move(*refused));
    }
    // As the archive lists show it now: retired, with its manifest lost.
    if (retirement.lost_manifest) {
        retirement.lost_manifest->retired = true;
    }
    return retirement;
}

} // namespace va
