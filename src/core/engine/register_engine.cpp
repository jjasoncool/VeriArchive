#include "core/engine/register_engine.h"

#include "common/time.h"
#include "common/utf8.h"
#include "common/unicode.h"
#include "common/uuid.h"
#include "core/hash/hash_pipeline.h"
#include "core/engine/heartbeat.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/registration_attempt.h"
#include "core/engine/transfer.h"
#include "core/identity/volume_json.h"
#include "core/write_file.h"
#include "core/manifest/manifest_writer.h"
#include "core/scan/scanner.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <regex>
#include <thread>

namespace va {
namespace {

std::unexpected<RegisterFailure> fail(RegisterFailure::Kind kind, std::string message)
{
    return std::unexpected(RegisterFailure{kind, std::move(message)});
}

std::string describe(const ArchiveCandidate& candidate)
{
    return candidate.archive_uuid +
           (candidate.root_subpath.empty() ? " (volume root)" : " (root \"" + candidate.root_subpath + "\")");
}

// HDDs get one reader; so do devices whose type is unknown, because many USB
// bridges hide it and parallel reads would make a disk seek (R-110, R-111).
std::optional<bool> rotational_of(const VolumeInfo& volume, std::optional<bool> override_value)
{
    if (override_value) {
        return override_value;
    }
    bool any_known = false;
    for (const auto& device : volume.devices) {
        if (device.rotational) {
            if (*device.rotational) {
                return true;
            }
            any_known = true;
        }
    }
    return any_known ? std::optional<bool>(false) : std::nullopt;
}

// An unfinished registration (R-104) is described in settings under
// "registration:<archive_uuid>"; its hashed entries are cached in the files
// table under the revision being created.
std::string pending_key(const Uuid& archive_uuid)
{
    return "registration:" + archive_uuid;
}

struct Plan {
    Uuid archive_uuid;
    std::uint32_t revision = 1;
    std::string label;
    std::optional<Uuid> copy_of;
    Timestamp created;
    bool new_archive = true;
};

// Two attempts of one registration can run at once (`register --resume` while
// an attempt still runs, or two resumes), and another operation can record the
// revision a registration creates (a relocation of the archive while
// `register --new-revision` of it is unfinished). The first to record the
// revision wins. An attempt that finds it recorded stops, and touches nothing
// of it; it cannot tell who recorded it (decision 0023).
std::unexpected<RegisterFailure> already_recorded(const Plan& plan)
{
    return fail(RegisterFailure::Kind::Refused,
        "revision " + std::to_string(plan.revision) + " of archive " + plan.archive_uuid +
            " is already recorded (by another attempt of this registration or by another operation, such as a "
            "relocation); this attempt recorded nothing");
}

// An attempt of a registration whose plan names a process that still runs it
// on this computer: `register --resume` waits for that process to end
// (decision 0024), so two attempts of one registration do not run at once
// where it can tell.
std::unexpected<RegisterFailure> still_running(const AttemptProcess& attempt)
{
    return fail(RegisterFailure::Kind::Refused,
        "this registration is still running in process " + std::to_string(attempt.process.id) + " (" +
            (attempt.process.program.empty() ? std::string("?") : attempt.process.program) +
            ") on this computer; resume it once that process has ended");
}

// The operator abandoned the registration while this attempt ran, where its
// process could not be seen (another computer sharing the repository, decision
// 0025): its plan is gone, or the key holds a later registration's plan.
std::unexpected<RegisterFailure> abandoned_meanwhile(const Plan& plan)
{
    return fail(RegisterFailure::Kind::Refused,
        "the unfinished registration of archive " + plan.archive_uuid +
            " was abandoned while this attempt ran; this attempt recorded nothing");
}

// Another registration of the archive is unfinished: the plan its key holds
// (decision 0025), and the root that registration reads.
struct UnfinishedRegistration {
    std::string root;
    std::uint32_t revision = 0;
};

// The archive's unfinished registration, if its key holds one: a plan whose
// revision is not recorded. A plan whose revision is recorded is done with
// (decision 0023), and a value that is no plan is no registration's.
std::expected<std::optional<UnfinishedRegistration>, IoError> unfinished_registration(
    ArchiveStore& store, const Uuid& archive_uuid)
{
    const auto stored = store.setting(pending_key(archive_uuid));
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

// A registration that is not a resume never replaces the plan of another
// unfinished registration of its archive (decision 0025), such as one of the
// same volume reached through another path (a second mount point): the
// attempt running under that plan would stop as if it had been abandoned. In
// the words of a relocation that meets it (decision 0023).
std::unexpected<RegisterFailure> another_registration(const Uuid& archive_uuid,
    const UnfinishedRegistration& registration)
{
    return fail(RegisterFailure::Kind::Refused,
        "an unfinished registration of archive " + archive_uuid + " (root \"" + registration.root +
            "\") is creating revision " + std::to_string(registration.revision) +
            "; nothing was changed (finish it with `register --resume` first, or abandon it with "
            "`archive abandon-registration " +
            archive_uuid + "`)");
}

// A plan whose revision is recorded can never be finished. An attempt of the
// registration that records it removes it, and so do a relocation and an
// import (decision 0030); an operation of an older version leaves it, and
// then each resume would stop at its start and the root would take no other
// registration. So the attempt that finds the revision recorded removes the
// plan, if the key still holds the plan of that revision: by then it may hold
// the plan of the archive's next registration, which stays (decision 0023).
// Best effort: a plan left behind goes with the next attempt that finds its
// revision recorded.
void remove_dead_plan(ArchiveStore& store, const Plan& plan)
{
    (void)store.write_transaction([&]() -> std::expected<void, IoError> {
        const auto recorded = store.revision_recorded(plan.archive_uuid, plan.revision);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
        return *recorded ? store.remove_registration_plan(plan.archive_uuid, plan.revision)
                         : std::expected<void, IoError>{};
    });
}

// R-89: identity.json goes on the media before the scan. The attempts of one
// registration write the same bytes, and may run at once (decision 0023); two
// that replace the file at the same moment fail on Windows (MoveFileExW,
// access denied). So an attempt writes it only while the media does not hold
// these bytes, and after a failed write looks again a moment later: by then
// the other attempt's write is done.
std::expected<void, IoError> write_identity_file(RegisterServices& services,
    const std::filesystem::path& media_directory, const std::string& identity)
{
    const auto path = media_directory / "identity.json";
    std::expected<void, IoError> written;
    for (int tries = 0;; ++tries) {
        if (const auto on_media = read_whole_file(services.archive_files, path); on_media && *on_media == identity) {
            return {};
        }
        if (tries == 3) {
            return written;
        }
        written = services.media_writer.create_directories(media_directory);
        if (written) {
            written = write_file_atomically(services.media_writer, path, identity);
        }
        if (written) {
            return written;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

nlohmann::json plan_json(const Plan& plan, const std::filesystem::path& root)
{
    nlohmann::json json{{"archive_uuid", plan.archive_uuid}, {"revision", plan.revision},
        {"label", plan.label}, {"created", plan.created}, {"new_archive", plan.new_archive},
        {"root", path_to_utf8(root)}};
    if (plan.copy_of) {
        json["copy_of"] = *plan.copy_of;
    }
    return json;
}

// The unfinished registration of `root`, if there is one. Roots are compared
// as the folders they name: a plan that an older version recorded for a root
// typed with a trailing separator ("D:\Photos\") is the plan of "D:\Photos".
std::expected<std::optional<Plan>, IoError> find_pending(ArchiveStore& store, const std::filesystem::path& root)
{
    const auto pending = store.settings_with_prefix("registration:");
    if (!pending) {
        return std::unexpected(pending.error());
    }
    const auto folder = [](const std::filesystem::path& path) {
        return unicode_fold(path_to_utf8(without_trailing_separators(path.lexically_normal())));
    };
    const auto wanted = folder(root);
    for (const auto& [key, value] : *pending) {
        const auto json = nlohmann::json::parse(value, nullptr, false);
        if (!json.is_object() || !json.contains("root") ||
            folder(path_from_utf8(json["root"].get<std::string>())) != wanted) {
            continue;
        }
        Plan plan;
        plan.archive_uuid = json.value("archive_uuid", std::string{});
        plan.revision = json.value("revision", 1u);
        plan.label = json.value("label", std::string{});
        plan.created = json.value("created", std::string{});
        plan.new_archive = json.value("new_archive", true);
        if (json.contains("copy_of")) {
            plan.copy_of = json["copy_of"].get<std::string>();
        }
        return std::optional<Plan>(plan);
    }
    return std::optional<Plan>{};
}

std::expected<std::optional<ArchiveCandidate>, RegisterFailure> check_existing_archives(
    RegisterServices& services, std::vector<ArchiveCandidate>& candidates, const MediaEvidence& evidence,
    const std::string& root_subpath, const RegisterRequest& request)
{
    std::optional<ArchiveCandidate> target;
    for (auto& candidate : candidates) {
        // Only an archive whose root overlaps this one can refuse it
        // (decision 0001); its earlier carriers are read where its latest
        // carrier does not match this volume (decision 0004).
        if (!roots_overlap(candidate.root_subpath, root_subpath)) {
            continue;
        }
        const auto matched = match_recorded(services.store, services.repository_files, services.layout, evidence,
            candidate.root_subpath, std::span(&candidate, 1));
        if (!matched) {
            return fail(RegisterFailure::Kind::Repository, matched.error().message);
        }
        const auto& match = *matched;
        const bool same_volume = match.result == MatchResult::Match ||
                                 match.result == MatchResult::MatchWeak ||
                                 match.result == MatchResult::Migrated;
        const bool previous_carrier = match.result == MatchResult::PreviousCarrier;
        if (!same_volume && !previous_carrier) {
            continue;
        }
        if (unicode_fold(candidate.root_subpath) != unicode_fold(root_subpath)) {
            return fail(RegisterFailure::Kind::Refused,
                "the root overlaps registered archive " + describe(candidate) +
                    "; archive roots on one volume may not contain each other (decision 0001)");
        }
        if (previous_carrier) {
            if (!request.force_new_archive) {
                return fail(RegisterFailure::Kind::Refused,
                    "this media is an earlier carrier of archive " + describe(candidate) +
                        "; pass --force-new-archive to register it separately (decision 0004)");
            }
            continue;
        }
        if (match.result == MatchResult::Migrated) {
            if (!request.force_new_archive) {
                return fail(RegisterFailure::Kind::Refused,
                    "this filesystem belongs to archive " + describe(candidate) +
                        " but the device differs (MIGRATED); use `archive relocate`, or pass "
                        "--force-new-archive");
            }
            continue;
        }
        if (request.new_revision) {
            if (candidate.retired) {
                return fail(RegisterFailure::Kind::Refused,
                    "archive " + describe(candidate) + " is retired and takes no new revisions (decision 0003)");
            }
            if (target) {
                return fail(RegisterFailure::Kind::Refused,
                    "several archives match this root (AMBIGUOUS); resolve the clone first");
            }
            target = candidate;
        } else if (!request.force_new_archive) {
            return fail(RegisterFailure::Kind::Refused,
                "already registered as archive " + describe(candidate) +
                    "; pass --new-revision to re-baseline it or --force-new-archive (R-97)");
        }
    }
    if (request.new_revision && !target) {
        return fail(RegisterFailure::Kind::Refused, "--new-revision: no registered archive at this root");
    }
    return target;
}

} // namespace

std::expected<RegisterOutcome, RegisterFailure> register_archive(
    RegisterServices& services,
    const RegisterRequest& request,
    CancelToken& cancel,
    IProgressSink& progress)
{
    using Kind = RegisterFailure::Kind;
    auto& store = services.store;
    if (request.label && !std::regex_match(*request.label, std::regex("ARCH-[0-9]{4,}"))) {
        return fail(Kind::Invalid, "labels have the form ARCH-NNNN");
    }
    if (request.copy_of && !is_canonical_uuid(*request.copy_of)) {
        return fail(Kind::Invalid, "--copy-of needs an archive uuid");
    }

    const auto volume = services.media.volume_for_path(request.root);
    if (!volume) {
        return fail(Kind::Media, "cannot identify the volume of the root: " + volume.error().message);
    }
    const auto root_subpath = root_subpath_of(request.root, *volume);
    if (!root_subpath) {
        return fail(Kind::Media, root_subpath.error().message);
    }
    const MediaEvidence evidence{current_timestamp(), *volume, std::nullopt};

    const auto pending = find_pending(store, request.root);
    if (!pending) {
        return fail(Kind::Repository, pending.error().message);
    }

    // Which process runs this attempt, recorded in its plan (decision 0024).
    // When this process cannot be read, the plan names none, as an older
    // version's does, and nothing can tell whether this attempt still runs.
    std::optional<AttemptProcess> this_attempt;
    if (const auto self = services.processes.current()) {
        this_attempt = AttemptProcess{*self, services.host.host};
    }
    const auto no_resumable = [] {
        return fail(Kind::Refused, "there is no interrupted registration of this root to resume");
    };

    // Decide which archive and revision this registration creates.
    Plan plan;
    nlohmann::json plan_record;            // the plan as this attempt writes it
    std::optional<ArchiveRecord> existing; // archive receiving a new revision
    std::map<std::string, Entry> cached;   // R-104: hashes from the interrupted attempt
    if (request.resume) {
        if (!*pending) {
            return no_resumable();
        }
        plan = **pending;
        // Before it reads anything of the media, this attempt takes the
        // registration over, in one write transaction (decision 0024): the
        // plan must still be the one found; an attempt before it whose process
        // still runs on this computer keeps it; otherwise the plan names this
        // attempt's process from now on. Two resumes started at once cannot
        // both go on: the second finds the first's process running.
        std::optional<RegisterFailure> refused;
        bool dead_plan = false; // the plan's revision is recorded: the plan goes
        const auto claimed = [&]() -> std::expected<void, IoError> {
            if (auto begun = store.begin_waiting(record_lock_wait, cancel); !begun) {
                return begun;
            }
            // A plan found with its revision recorded was read just before
            // another attempt recorded it, or another operation recorded the
            // revision and left the plan, which goes now (decision 0023).
            const auto recorded = store.revision_recorded(plan.archive_uuid, plan.revision);
            if (!recorded) {
                return std::unexpected(recorded.error());
            }
            if (*recorded) {
                dead_plan = true;
                if (auto removed = store.remove_registration_plan(plan.archive_uuid, plan.revision); !removed) {
                    return removed;
                }
                return store.commit();
            }
            const auto stored = store.setting(pending_key(plan.archive_uuid));
            if (!stored) {
                return std::unexpected(stored.error());
            }
            auto record = nlohmann::json::parse(*stored, nullptr, false);
            const auto planned = store.registration_planned(plan.archive_uuid, plan.revision, plan.created);
            if (!planned) {
                return std::unexpected(planned.error());
            }
            if (!*planned || !record.is_object()) {
                // Finished, abandoned or replaced since it was found.
                refused = no_resumable().error();
                store.rollback();
                return {};
            }
            if (const auto judged = judge_attempt(record, services.processes, services.host.host);
                judged.state == AttemptState::Running) {
                refused = still_running(*judged.attempt).error();
                store.rollback();
                return {};
            }
            // Working again: the stop of the attempt before no longer holds.
            // The totals of the attempt before show until this one's scan has
            // counted them (decision 0022).
            record.erase("stopped");
            record.erase("process");
            if (this_attempt) {
                record_attempt(record, *this_attempt);
            }
            if (request.write_media_copy) {
                record["media_copy"] = true; // decision 0025: an attempt writes identity.json on the media
            }
            record["updated"] = current_timestamp();
            if (auto saved = store.update_setting(pending_key(plan.archive_uuid), record.dump()); !saved) {
                return saved;
            }
            plan_record = std::move(record);
            return store.commit();
        }();
        if (!claimed) {
            store.rollback();
            if (cancel.cancelled()) {
                return fail(Kind::Incomplete,
                    "registration was cancelled; continue with `register <root_path> --resume`");
            }
            return fail(Kind::Repository, claimed.error().message);
        }
        if (dead_plan) {
            return already_recorded(plan);
        }
        if (refused) {
            return std::unexpected(*refused);
        }
        if (!plan.new_archive) {
            const auto found = store.find_archive(plan.archive_uuid);
            if (!found || !*found) {
                return fail(Kind::Repository, "the archive of the interrupted registration is missing");
            }
            existing = **found;
        }
        const auto rows = store.file_rows(plan.archive_uuid, plan.revision);
        if (!rows) {
            return fail(Kind::Repository, rows.error().message);
        }
        for (const auto& row : *rows) {
            cached.emplace(entry_key(row), row);
        }
    } else {
        // Its root takes no other registration until it is finished or
        // given up (decision 0025).
        if (*pending) {
            return fail(Kind::Refused, "an interrupted registration of this root exists (archive " +
                                           (*pending)->archive_uuid +
                                           "); finish it with --resume, or abandon it with "
                                           "`archive abandon-registration " +
                                           (*pending)->archive_uuid + "`");
        }
        auto candidates = load_candidates(store, services.repository_files, services.layout);
        if (!candidates) {
            return fail(Kind::Repository, candidates.error().message);
        }
        const auto checked = check_existing_archives(services, *candidates, evidence, *root_subpath, request);
        if (!checked) {
            return std::unexpected(checked.error());
        }
        const std::optional<ArchiveCandidate>& target = *checked;
        plan.created = current_timestamp();
        plan.copy_of = request.copy_of;
        if (target) {
            const auto found = store.find_archive(target->archive_uuid);
            if (!found || !*found) {
                return fail(Kind::Repository, "the archive to re-baseline is missing from the database");
            }
            existing = **found;
            plan.new_archive = false;
            plan.archive_uuid = existing->archive_uuid;
            plan.revision = existing->current_revision + 1;
            plan.label = existing->label;
            if (!plan.copy_of) {
                // A copy stays a copy (decision 0005): as the latest
                // revision's manifest says, never the index's copy of it.
                const auto previous = store.revision(existing->archive_uuid, existing->current_revision);
                if (!previous) {
                    return fail(Kind::Repository, previous.error().message);
                }
                if (*previous) {
                    const auto header =
                        load_recorded_header(services.repository_files, services.layout, **previous);
                    if (!header) {
                        return fail(Kind::Repository, header.error().message);
                    }
                    plan.copy_of = header->copy_of;
                }
            }
        } else {
            plan.archive_uuid = generate_uuid();
            if (request.label) {
                plan.label = *request.label;
            } else {
                auto label = store.next_label();
                if (!label) {
                    return fail(Kind::Repository, label.error().message);
                }
                plan.label = *label;
            }
        }
        // Recorded before any work, so an interruption leaves a resumable
        // registration. The plan names the process that runs this attempt
        // (decision 0024), and whether an attempt writes on the media
        // (decision 0025).
        plan_record = plan_json(plan, request.root);
        if (this_attempt) {
            record_attempt(plan_record, *this_attempt);
        }
        if (request.write_media_copy) {
            plan_record["media_copy"] = true;
        }
        std::optional<RegisterFailure> refused;
        const auto recorded = [&]() -> std::expected<void, IoError> {
            if (auto begun = store.begin(); !begun) {
                return begun;
            }
            // One unfinished registration per archive (decision 0025): the
            // key may hold the plan of another registration of it, reached
            // through another root path than this one, whose attempt may run.
            // Looked for in the transaction that writes this plan, so two
            // registrations started at once cannot both write theirs.
            const auto other = unfinished_registration(store, plan.archive_uuid);
            if (!other) {
                return std::unexpected(other.error());
            }
            if (*other) {
                refused = another_registration(plan.archive_uuid, **other).error();
                store.rollback();
                return {};
            }
            if (plan.new_archive) {
                ArchiveRecord archive{plan.archive_uuid, plan.label, plan.created, 0,
                    "REGISTRATION_INCOMPLETE", *root_subpath, ""};
                if (auto inserted = store.insert_archive(archive); !inserted) {
                    return inserted;
                }
            }
            if (auto saved = store.set_setting(pending_key(plan.archive_uuid), plan_record.dump()); !saved) {
                return saved;
            }
            return store.commit();
        }();
        if (!recorded) {
            store.rollback();
            return fail(Kind::Repository, recorded.error().message);
        }
        if (refused) {
            return std::unexpected(*refused);
        }
    }

    // The plan says while the registration works that it does ("updated"), and
    // after the scan how much there is to do (decision 0022). It is written
    // only while it is this registration's plan and its revision is not
    // recorded (decisions 0023, 0025).
    Heartbeat heartbeat(store, pending_key(plan.archive_uuid), std::move(plan_record), progress);
    heartbeat.only_while_planned(plan.archive_uuid, plan.revision, plan.created);
    heartbeat.beat();
    // However this attempt returns without finishing (cancelled, the device
    // removed, a failure), the plan says last that it stopped ("stopped"), so
    // the Archive List shows it stopped at once and not working for minutes.
    // The write is in a destructor, which runs on every way out of this
    // function, so no return below can forget it. Once the registration is
    // recorded the mark is off: the record removed the plan, and the key is
    // the archive's, so a plan there by then belongs to the archive's next
    // registration (`register --new-revision` in another process), which this
    // attempt's record must not replace.
    struct StopMark {
        Heartbeat& heartbeat;
        bool recorded = false;
        ~StopMark()
        {
            if (!recorded) {
                heartbeat.stop();
            }
        }
    } stop_mark{heartbeat};
    // This attempt found the revision recorded: it stops, recording nothing.
    const auto lost_to_another = [&] {
        remove_dead_plan(store, plan);
        return already_recorded(plan);
    };

    // R-89: identity.json goes on the media before the scan, so the scan excludes
    // it (.veriarchive/) and it is never reported as New.
    const auto media_directory = request.root / ".veriarchive";
    if (request.write_media_copy) {
        const auto identity = identity_file_json(plan.archive_uuid, plan.label, plan.revision,
            *root_subpath, plan.created);
        if (const auto written_identity = write_identity_file(services, media_directory, identity);
            !written_identity) {
            return fail(Kind::Media, "cannot write the media copy: " + written_identity.error().message);
        }
    }

    // Scan, then hash every readable file not already hashed before an interruption.
    const auto interrupted = [&](const std::string& why) {
        return fail(Kind::Incomplete, why + "; continue with `register <root_path> --resume`");
    };
    ScanOptions scan_options;
    auto scanned = scan_tree(services.archive_files, request.root, scan_options, cancel, heartbeat);
    if (!scanned) {
        const auto kind = scanned.error().kind;
        if (kind == IoErrorKind::Cancelled || kind == IoErrorKind::DeviceRemoved) {
            return interrupted("registration was interrupted while scanning");
        }
        return fail(Kind::Media, "cannot scan the root: " + scanned.error().message);
    }

    std::vector<HashJob> jobs;
    // Every readable file, those an interrupted attempt hashed included: the
    // files done are counted from the checkpoints, which hold those too.
    std::uint64_t files_total = 0;
    std::uint64_t bytes_total = 0;
    for (std::size_t index = 0; index < scanned->entries.size(); ++index) {
        auto& entry = scanned->entries[index].entry;
        if (entry.type != 'f' || !entry.unreadable.empty()) {
            continue;
        }
        ++files_total;
        bytes_total += entry.size.value_or(0);
        const auto hit = cached.find(entry_key(entry));
        if (hit != cached.end() && !hit->second.b3.empty() && hit->second.size == entry.size &&
            hit->second.mtime_ns == entry.mtime_ns) {
            entry.b3 = hit->second.b3; // unchanged since the interrupted attempt hashed it
            entry.sha256 = hit->second.sha256;
            cached.erase(hit);
            continue;
        }
        // The scanned size lets progress count bytes (share done, time left).
        jobs.push_back({index, scanned->entries[index].native, entry.size.value_or(0)});
    }
    // The checkpoints left over are not reused: their files are gone, changed
    // or unreadable since the interrupted attempt hashed them. Counted as done
    // they would show work that is never done or done again (decision 0022),
    // so they go, with the first checkpoint below; only a re-read is lost.
    std::vector<Entry> stale;
    stale.reserve(cached.size());
    for (auto& [key, row] : cached) {
        stale.push_back(std::move(row));
    }
    cached.clear();

    // Hashed files not yet saved. A checkpoint that cannot be saved is rolled
    // back, so its open transaction cannot swallow the next checkpoints and
    // the final record; its files are tried again with the next checkpoint,
    // and so are the stale checkpoints to delete. The registration goes on:
    // checkpoints only spare re-reading after an interruption (R-104).
    //
    // Once another attempt of this registration, or another operation, has
    // recorded the revision, the rows under it are the index of the recorded
    // manifest, which no checkpoint may change: each checkpoint looks first,
    // in its own transaction, and this attempt stops when it finds the
    // revision recorded (decision 0023). Likewise once the operator has
    // abandoned the registration (decision 0025): its plan is gone, or the
    // key holds a later registration's plan, whose checkpoints this attempt's
    // rows must not join. flush_checkpoint() then returns false.
    std::vector<Entry> checkpoint;
    std::size_t hashed_since_checkpoint = 0;
    auto last_checkpoint = std::chrono::steady_clock::now();
    bool lost = false;      // the revision was recorded by another attempt or operation first
    bool abandoned = false; // the operator abandoned the registration meanwhile
    const auto flush_checkpoint = [&] {
        if ((!checkpoint.empty() || !stale.empty()) &&
            store.write_transaction([&]() -> std::expected<void, IoError> {
                const auto recorded = store.revision_recorded(plan.archive_uuid, plan.revision);
                if (!recorded) {
                    return std::unexpected(recorded.error());
                }
                if (*recorded) {
                    lost = true;
                    return {};
                }
                const auto planned = store.registration_planned(plan.archive_uuid, plan.revision, plan.created);
                if (!planned) {
                    return std::unexpected(planned.error());
                }
                if (!*planned) {
                    abandoned = true;
                    return {};
                }
                // Deleted first: a changed file hashed again has the same key.
                if (auto deleted = store.delete_file_rows(plan.archive_uuid, plan.revision, stale); !deleted) {
                    return deleted;
                }
                return store.upsert_file_rows(plan.archive_uuid, plan.revision, checkpoint);
            })) {
            checkpoint.clear();
            stale.clear();
        }
        hashed_since_checkpoint = 0;
        last_checkpoint = std::chrono::steady_clock::now();
        return !lost && !abandoned;
    };
    // The stale checkpoints go before the plan has the totals, so a reader
    // never sees more done than there is.
    if (!flush_checkpoint()) {
        return lost ? lost_to_another() : abandoned_meanwhile(plan);
    }
    heartbeat.set("files_total", files_total);
    heartbeat.set("bytes_total", bytes_total);
    heartbeat.beat();
    HashPipelineOptions hash_options;
    hash_options.readers = default_reader_count(rotational_of(*volume, request.rotational));
    bool device_removed = false;
    const auto hashed = hash_files(services.archive_files, jobs, hash_options, cancel, heartbeat,
        [&](HashResult&& result) {
            auto& entry = scanned->entries[result.id].entry;
            if (result.error) {
                if (result.error->kind == IoErrorKind::DeviceRemoved) {
                    device_removed = true;
                    return false;
                }
                entry.unreadable = unreadable_reason(*result.error); // decision 0010
                return true;
            }
            entry.b3 = std::move(result.b3);
            entry.sha256 = std::move(result.sha256);
            entry.size = result.size;
            checkpoint.push_back(entry); // R-104: every 1000 files or 60 seconds
            if (++hashed_since_checkpoint >= 1000 ||
                std::chrono::steady_clock::now() - last_checkpoint > std::chrono::seconds(60)) {
                // False stops hashing: the revision is recorded, or the
                // registration was abandoned.
                return flush_checkpoint();
            }
            return true;
        });
    flush_checkpoint();
    if (lost) {
        return lost_to_another();
    }
    if (abandoned) {
        return abandoned_meanwhile(plan);
    }
    if (!hashed || device_removed) {
        return interrupted(device_removed ? "the device disappeared during registration"
                                          : "registration was cancelled");
    }
    // Writing the manifest and the index of a large archive takes a while
    // without progress reports: it starts from a fresh "updated".
    heartbeat.beat();

    // Manifest first: it is the truth. The database index follows, in the
    // transaction that gives the manifest its name.
    RegisterOutcome outcome;
    outcome.archive_uuid = plan.archive_uuid;
    outcome.label = plan.label;
    outcome.revision = plan.revision;

    ManifestHeader header;
    header.archive_uuid = plan.archive_uuid;
    header.label = plan.label;
    header.revision = plan.revision;
    if (plan.revision > 1) {
        header.supersedes = plan.revision - 1;
    }
    header.copy_of = plan.copy_of;
    header.created = current_timestamp();
    header.app_version = services.host.app_version;
    header.os = services.host.os;
    header.host = services.host.host;
    header.identity = *volume;
    header.root_subpath = *root_subpath;
    header.root_hint = path_to_utf8(request.root);
    header.exclusions_version = default_exclusions_version;
    header.exclusions = scan_options.exclusions;

    outcome.manifest = manifest_path(services.layout.manifests(), plan.archive_uuid, plan.revision);
    std::error_code directory_error;
    std::filesystem::create_directories(outcome.manifest.parent_path(), directory_error);
    ManifestWriter manifest(services.writer, services.repository_files, outcome.manifest);
    std::vector<Entry> entries;
    entries.reserve(scanned->entries.size());
    for (auto& scanned_entry : scanned->entries) {
        if (auto added = manifest.add(scanned_entry.entry); !added) {
            return fail(Kind::Repository, "cannot write the manifest: " + added.error().message);
        }
        if (!scanned_entry.entry.unreadable.empty()) {
            outcome.unreadable.push_back(scanned_entry.entry);
        }
        entries.push_back(std::move(scanned_entry.entry));
    }
    // Written and flushed without a lock, which takes seconds or minutes for a
    // large archive; not at its name yet.
    auto written = manifest.finish(header);
    if (!written) {
        return fail(Kind::Repository, "cannot write the manifest: " + written.error().message);
    }
    outcome.counts = written->header.counts;
    outcome.skipped_mount_points = scanned->skipped_mount_points;

    // One step for every attempt of this registration (decision 0023): in one
    // write transaction the revision is found not recorded, the record is
    // written, and the manifest is renamed to its name last. No other attempt
    // can record the revision or rename a manifest there meanwhile, so a
    // recorded manifest is never replaced, and the revision is recorded with
    // the bytes this attempt wrote under its name. A crash between the rename
    // and the commit leaves a manifest that is not recorded, which the next
    // attempt replaces.
    bool cancelled_waiting = false; // cancelled while it waited for the write lock
    const auto record = [&]() -> std::expected<void, IoError> {
        // The record of a large archive, another attempt's among them, holds
        // the write lock for seconds: this one waits for it, and then finds
        // the revision recorded rather than the database locked.
        if (auto begun = store.begin_waiting(record_lock_wait, cancel); !begun) {
            // Locked for longer still, or cancelled while it waited. Whoever
            // held the lock may have recorded the revision, and another
            // operation taken it since, or the operator abandoned the
            // registration.
            if (const auto recorded = store.revision_recorded(plan.archive_uuid, plan.revision);
                recorded && *recorded) {
                lost = true;
            } else if (const auto planned =
                           store.registration_planned(plan.archive_uuid, plan.revision, plan.created);
                       recorded && planned && !*planned) {
                abandoned = true;
            }
            cancelled_waiting = cancel.cancelled();
            return begun;
        }
        const auto recorded = store.revision_recorded(plan.archive_uuid, plan.revision);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
        if (*recorded) {
            lost = true;
            return std::unexpected(IoError{IoErrorKind::Other, 0, "the revision is recorded"});
        }
        // Abandoned while this attempt ran (decision 0025): for a first
        // registration the archive's record went with its plan.
        const auto planned = store.registration_planned(plan.archive_uuid, plan.revision, plan.created);
        if (!planned) {
            return std::unexpected(planned.error());
        }
        if (!*planned) {
            abandoned = true;
            return std::unexpected(IoError{IoErrorKind::Other, 0, "the registration was abandoned"});
        }
        ArchiveRecord archive;
        if (existing) {
            archive = *existing;
        } else {
            archive = {plan.archive_uuid, plan.label, plan.created, 0, "", "", ""};
        }
        archive.current_revision = plan.revision;
        archive.status = "REGISTERED";
        archive.root_subpath = *root_subpath;
        if (auto updated = store.update_archive(archive); !updated) {
            return updated;
        }
        const auto relative = outcome.manifest.lexically_relative(services.layout.root);
        RevisionRecord revision{plan.archive_uuid, plan.revision, path_to_utf8(relative),
            written->header_line, written->header_blake3, written->header.body_blake3, "", header.created};
        std::replace(revision.manifest_relpath.begin(), revision.manifest_relpath.end(), '\\', '/');
        if (auto inserted = store.insert_revision(revision); !inserted) {
            return inserted;
        }
        if (auto cleared = store.delete_file_rows(plan.archive_uuid, plan.revision); !cleared) {
            return cleared;
        }
        if (auto indexed = store.insert_file_rows(plan.archive_uuid, plan.revision, entries); !indexed) {
            return indexed;
        }
        EvidenceRecord evidence_record{plan.archive_uuid, "", evidence.seen_at,
            volume_to_json_text(*volume), plan.new_archive ? "UNKNOWN" : "MATCH", "registered"};
        if (auto inserted = store.insert_evidence(evidence_record); !inserted) {
            return inserted;
        }
        if (auto removed = store.remove_setting(pending_key(plan.archive_uuid)); !removed) {
            return removed;
        }
        // An unfinished relocation of the archive that creates this revision
        // can never finish once it is recorded: its record and its saved
        // progress go with this record (decision 0026), as a plan goes with
        // the relocation that records its revision (decision 0023).
        if (auto removed = store.remove_relocation_creating(plan.archive_uuid, plan.revision); !removed) {
            return removed;
        }
        if (auto named = written->file->commit(); !named) {
            return named;
        }
        return store.commit();
    }();
    if (!record) {
        store.rollback();
        if (lost) {
            return lost_to_another(); // this attempt's manifest goes with `written`
        }
        if (abandoned) {
            return abandoned_meanwhile(plan);
        }
        if (cancelled_waiting) {
            return interrupted("registration was cancelled");
        }
        return fail(Kind::Repository, "cannot record the archive: " + record.error().message);
    }
    stop_mark.recorded = true;

    if (request.write_media_copy) {
        const auto copy = media_directory / outcome.manifest.filename();
        if (auto copied = copy_file(services.repository_files, services.media_writer, outcome.manifest, copy)) {
            outcome.media_copy = copy;
        } else {
            outcome.warnings.push_back("the manifest copy on the media could not be written: " +
                                       copied.error().message);
        }
    }

    std::size_t portability = 0;
    for (const auto& entry : entries) {
        portability += entry.warn.empty() ? 0 : 1;
    }
    if (portability > 0) {
        outcome.warnings.push_back(std::to_string(portability) + " entries carry portability warnings");
    }
    return outcome;
}

} // namespace va
