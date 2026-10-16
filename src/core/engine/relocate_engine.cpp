#include "core/engine/relocate_engine.h"

#include "common/time.h"
#include "common/utf8.h"
#include "common/uuid.h"
#include "core/engine/comparison.h"
#include "core/engine/heartbeat.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/registration_attempt.h"
#include "core/identity/volume_json.h"
#include "core/manifest/manifest_writer.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <set>

namespace va {
namespace {

using Kind = RelocateFailure::Kind;

std::unexpected<RelocateFailure> fail(Kind kind, std::string message, std::vector<VerifyItemRecord> items = {})
{
    return std::unexpected(RelocateFailure{kind, std::move(message), std::move(items)});
}

// Members of a record read without exceptions: another type (a damaged
// record) reads as missing.
std::string text_member(const nlohmann::json& object, const char* name)
{
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::optional<std::uint64_t> unsigned_member(const nlohmann::json& object, const char* name)
{
    const auto found = object.find(name);
    if (found == object.end() || !found->is_number_unsigned()) {
        return std::nullopt;
    }
    return found->get<std::uint64_t>();
}

// The archive's unfinished relocation, as its record says (decision 0026).
struct Unfinished {
    nlohmann::json record;
    std::uint32_t revision = 0;  // the revision it verifies against; it creates the next one
    std::string run_id;          // its checkpoints, and the id of its FULL verification
    std::string root;            // where its latest attempt read
    bool differs = false;        // an attempt found that the new carrier does not hold the recorded content
};

// Nothing when the setting holds no record, or a value that is no relocation's.
std::optional<Unfinished> unfinished_from(const std::string& stored)
{
    auto record = nlohmann::json::parse(stored, nullptr, false);
    if (stored.empty() || !record.is_object()) {
        return std::nullopt;
    }
    const auto revision = unsigned_member(record, "revision");
    auto run_id = text_member(record, "run_id");
    if (!revision || run_id.empty()) {
        return std::nullopt;
    }
    auto root = text_member(record, "root");
    const bool differs = !text_member(record, "differs").empty();
    return Unfinished{
        std::move(record), static_cast<std::uint32_t>(*revision), std::move(run_id), std::move(root), differs};
}

// Why a relocation whose new carrier was found to differ is neither resumed
// nor replaced by a new one: it can never finish, and only the operator
// abandons it (decision 0026).
RelocateFailure differs_refusal(const Uuid& uuid, const std::string& root)
{
    return RelocateFailure{RelocateFailure::Kind::Refused,
        "the unfinished relocation of archive " + uuid + " (to \"" + root +
            "\") found that the new carrier does not hold the recorded content and cannot be resumed; abandon it "
            "with `archive abandon-relocation " +
            uuid + "`, then relocate again",
        {}};
}

// Whether `volume` and the folder `root_subpath` on it are where the
// relocation started (decision 0026): the volume its record saw then, as
// identity matching tells it (§B10: MATCH, or MATCH_WEAK where a serial or
// the filesystem id cannot be read), and the same folder. Its checkpoints say
// which entries of that folder matched; on another volume or in another
// folder, or on a volume formatted or replaced since, they say nothing.
bool started_here(const nlohmann::json& record, const VolumeInfo& volume, const std::string& root_subpath)
{
    try {
        const auto evidence = record.find("evidence");
        if (evidence == record.end() || !evidence->is_object()) {
            return false;
        }
        const auto seen = evidence->find("volume");
        if (seen == evidence->end() || !seen->is_object()) {
            return false;
        }
        ArchiveCandidate start;
        start.archive_uuid = text_member(record, "archive_uuid");
        start.root_subpath = text_member(record, "root_subpath");
        start.identity = volume_from_json(*seen);
        const MediaEvidence now{current_timestamp(), volume, std::nullopt};
        const auto match = match_identity(now, root_subpath, std::span(&start, 1));
        return match.result == MatchResult::Match || match.result == MatchResult::MatchWeak;
    } catch (const std::exception&) {
        return false; // a damaged record
    }
}

std::string still_running_message(const AttemptProcess& attempt)
{
    return "this relocation is still running in process " + std::to_string(attempt.process.id) + " (" +
           (attempt.process.program.empty() ? std::string("?") : attempt.process.program) +
           ") on this computer; resume it once that process has ended";
}

} // namespace

std::expected<RelocateOutcome, RelocateFailure> relocate_archive(
    RelocateServices& services,
    const RelocateRequest& request,
    CancelToken& cancel,
    IProgressSink& progress)
{
    auto& store = services.store;
    const auto found = store.find_archive(request.archive_uuid);
    if (!found || !*found) {
        return fail(Kind::Refused, "no archive " + request.archive_uuid);
    }
    auto archive = **found;
    if (archive.status == "RETIRED") {
        return fail(Kind::Refused, "a retired archive takes no new revisions (decision 0003)");
    }

    const auto volume = services.media.volume_for_path(request.root);
    if (!volume) {
        return fail(Kind::Media, "cannot identify the volume of the new root: " + volume.error().message);
    }
    const auto root_subpath = root_subpath_of(request.root, *volume);
    if (!root_subpath) {
        return fail(Kind::Media, root_subpath.error().message);
    }
    const MediaEvidence evidence{current_timestamp(), *volume, std::nullopt};

    // Decision 0001: the new root may not overlap another archive on its volume.
    const auto candidates = load_candidates(store, services.repository_files, services.layout);
    if (!candidates) {
        return fail(Kind::Repository, candidates.error().message);
    }
    MatchResult own_match = MatchResult::Unknown;
    for (const auto& candidate : *candidates) {
        const auto match = match_identity(evidence, candidate.root_subpath, std::span(&candidate, 1));
        if (candidate.archive_uuid == archive.archive_uuid) {
            own_match = match.result;
            continue;
        }
        const bool same_volume = match.result == MatchResult::Match ||
                                 match.result == MatchResult::MatchWeak ||
                                 match.result == MatchResult::Migrated;
        if (same_volume && roots_overlap(candidate.root_subpath, *root_subpath)) {
            return fail(Kind::Refused, "the new root overlaps archive " + candidate.archive_uuid +
                                           " on the same volume (decision 0001)");
        }
    }

    const auto manifest = load_recorded_manifest(store, services.repository_files, services.layout,
        archive.archive_uuid, archive.current_revision);
    if (!manifest) {
        return fail(Kind::Repository, manifest.error().failure_class + ": " + manifest.error().message);
    }

    const auto uuid = archive.archive_uuid;
    const auto previous_revision = archive.current_revision;
    const auto creates = previous_revision + 1;
    const auto key = relocation_key(uuid);
    // What there is to read, as a verification counts it (decision 0022):
    // the files (and streams) whose content has hashes, which are what the
    // checkpoints count; an entry unreadable at registration has none.
    std::uint64_t files_total = 0;
    std::uint64_t bytes_total = 0;
    for (const auto& entry : manifest->entries) {
        if ((entry.type == 'f' || entry.type == 's') && !entry.b3.empty()) {
            ++files_total;
            bytes_total += entry.size.value_or(0);
        }
    }
    // Which process runs this attempt, recorded as a registration's attempt
    // records it (decision 0024). When this process cannot be read, the
    // record names none, and nothing can tell whether this attempt still runs.
    std::optional<AttemptProcess> this_attempt;
    if (const auto self = services.processes.current()) {
        this_attempt = AttemptProcess{*self, services.host.host};
    }
    const auto resume_hint = "continue it with `archive relocate " + uuid + " <root_path> --resume`";
    // An attempt that a failure stops (its scan, its manifest, its record)
    // keeps the relocation's record and checkpoints as an interrupted one
    // does, and its failure says how to go on once the cause is dealt with
    // (decision 0026); it keeps its kind (exit 11), as registration's does.
    const auto stopped_by = [&](Kind kind, const std::string& failure) {
        return fail(kind, failure + "; the relocation keeps its saved progress: " + resume_hint);
    };

    // The relocation begins, or takes over the archive's unfinished one, in
    // one write transaction before anything of the media is read (decision
    // 0026). An archive has one unfinished relocation at a time: a relocation
    // that is not a resume is refused while another is unfinished; a resume
    // goes on only while the archive is at the revision the relocation
    // verifies, the process that ran the attempt before has ended, and its new
    // root is where it started. A record whose revision another operation has
    // recorded can never finish: it goes, with its checkpoints. One whose new
    // carrier was found to differ can never finish either, but it stays for
    // the operator to abandon: both are refused, saying so.
    nlohmann::json record;
    std::string run_id;
    Timestamp started;          // when the relocation's first attempt started
    Timestamp attempt_started;  // when this attempt started
    std::uint64_t resumes = 0;
    std::optional<RelocateFailure> refused;
    const auto claimed = [&]() -> std::expected<void, IoError> {
        if (auto begun = store.begin_waiting(record_lock_wait, cancel); !begun) {
            return begun;
        }
        const auto stored = store.setting(key);
        if (!stored) {
            return std::unexpected(stored.error());
        }
        auto unfinished = unfinished_from(*stored);
        bool dead = false;
        if (unfinished) {
            const auto taken = store.revision_recorded(uuid, unfinished->revision + 1);
            if (!taken) {
                return std::unexpected(taken.error());
            }
            dead = *taken;
            if (dead) {
                if (auto removed = store.remove_relocation(uuid, unfinished->run_id); !removed) {
                    return removed;
                }
            }
        }
        if (!request.resume) {
            if (unfinished && !dead && unfinished->differs) {
                refused = differs_refusal(uuid, unfinished->root);
                store.rollback();
                return {};
            }
            if (unfinished && !dead) {
                refused = RelocateFailure{Kind::Refused,
                    "an unfinished relocation of archive " + uuid + " (to \"" + unfinished->root +
                        "\") exists; resume it with `archive relocate " + uuid +
                        " <root_path> --resume`, or abandon it with `archive abandon-relocation " + uuid + "`",
                    {}};
                store.rollback();
                return {};
            }
            run_id = generate_uuid();
            started = current_timestamp();
            attempt_started = started;
            record = nlohmann::json{{"archive_uuid", uuid}, {"label", archive.label},
                {"revision", previous_revision}, {"run_id", run_id}, {"started", started},
                {"root", path_to_utf8(request.root)}, {"root_subpath", *root_subpath},
                {"evidence", {{"seen_at", evidence.seen_at}, {"volume", volume_to_json(*volume)}}},
                {"files_total", files_total}, {"bytes_total", bytes_total}, {"updated", started}};
            if (this_attempt) {
                record_attempt(record, *this_attempt);
            }
            if (auto saved = store.set_setting(key, record.dump()); !saved) {
                return saved;
            }
            return store.commit();
        }
        if (!unfinished) {
            refused = RelocateFailure{Kind::Refused, "archive " + uuid + " has no unfinished relocation to resume", {}};
            store.rollback();
            return {};
        }
        if (dead) {
            refused = RelocateFailure{Kind::Refused,
                "revision " + std::to_string(unfinished->revision + 1) + " of archive " + uuid +
                    " was recorded by another operation; the unfinished relocation can no longer finish, and its "
                    "saved progress was removed",
                {}};
            return store.commit(); // the removal above
        }
        // Its new carrier did not hold the recorded content: what matched
        // there says nothing of a carrier copied again (decision 0026).
        if (unfinished->differs) {
            refused = differs_refusal(uuid, unfinished->root);
            store.rollback();
            return {};
        }
        // R-104: it resumes only while the revision it verifies is unchanged.
        if (unfinished->revision != previous_revision) {
            refused = RelocateFailure{Kind::Refused,
                "archive " + uuid + " is at revision " + std::to_string(previous_revision) + ", not at revision " +
                    std::to_string(unfinished->revision) +
                    " that its unfinished relocation verifies; it cannot be resumed (abandon it with `archive "
                    "abandon-relocation " +
                    uuid + "`)",
                {}};
            store.rollback();
            return {};
        }
        // Decision 0024: never under an attempt whose process still runs.
        if (const auto judged = judge_attempt(unfinished->record, services.processes, services.host.host);
            judged.state == AttemptState::Running) {
            refused = RelocateFailure{Kind::Refused, still_running_message(*judged.attempt), {}};
            store.rollback();
            return {};
        }
        if (!started_here(unfinished->record, *volume, *root_subpath)) {
            refused = RelocateFailure{Kind::Refused,
                "the volume or folder of this root is not the one the unfinished relocation of archive " + uuid +
                    " started on (\"" + unfinished->root + "\"); resume it there, or abandon it with `archive "
                    "abandon-relocation " +
                    uuid + "` and start again",
                {}};
            store.rollback();
            return {};
        }
        // Working again: the stop of the attempt before no longer holds, and
        // the record names this attempt's process and where it reads.
        record = std::move(unfinished->record);
        run_id = unfinished->run_id;
        started = text_member(record, "started");
        resumes = unsigned_member(record, "resumes").value_or(0) + 1;
        record.erase("stopped");
        record.erase("process");
        if (this_attempt) {
            record_attempt(record, *this_attempt);
        }
        record["root"] = path_to_utf8(request.root);
        record["resumes"] = resumes;
        attempt_started = current_timestamp();
        record["updated"] = attempt_started;
        if (auto saved = store.update_setting(key, record.dump()); !saved) {
            return saved;
        }
        return store.commit();
    }();
    if (!claimed) {
        store.rollback();
        if (cancel.cancelled()) {
            return fail(Kind::Incomplete, "relocation was interrupted; nothing was changed");
        }
        return fail(Kind::Repository, "cannot start the relocation: " + claimed.error().message);
    }
    if (refused) {
        return std::unexpected(*refused);
    }
    if (started.empty()) {
        started = current_timestamp(); // a record without its start (damaged)
    }

    // R-104: the entries whose content matched before, which are not read
    // again. When they cannot be read, everything is read.
    std::set<std::string> content_verified;
    if (request.resume) {
        if (auto saved = store.progress(run_id)) {
            content_verified = std::move(*saved);
        }
    }

    // The record says while the relocation works that it does ("updated",
    // decision 0022), written only while it is this relocation's record and
    // the revision it creates is not recorded.
    Heartbeat heartbeat(store, key, std::move(record), progress);
    heartbeat.only_while_relocating(uuid, run_id, creates);
    heartbeat.beat();
    // However this attempt returns without finishing, the record says last
    // that it stopped, so the Archive List shows it stopped at once; not once
    // the relocation has ended (recorded, refused for good, or gone).
    struct StopMark {
        Heartbeat& heartbeat;
        bool ended = false;
        ~StopMark()
        {
            if (!ended) {
                heartbeat.stop();
            }
        }
    } stop_mark{heartbeat};

    ComparisonInput input;
    input.root = request.root;
    input.filesystem_type = volume->filesystem_type;
    input.rotational = request.rotational;
    if (!input.rotational) {
        for (const auto& device : volume->devices) {
            if (device.rotational) {
                input.rotational = input.rotational.value_or(false) || *device.rotational;
            }
        }
    }
    input.host_os = services.host.os;
    input.header = &manifest->header;
    input.recorded = &manifest->entries;
    input.content_verified = std::move(content_verified);
    // R-104: the matched entries are saved every 1000 entries or 60 seconds.
    // Each checkpoint looks first, in its own transaction, whether the
    // revision this relocation creates is recorded (by another operation) or
    // its record is gone (abandoned); then it stops, saving nothing. A
    // checkpoint that cannot be saved is rolled back, and its entries are
    // tried again with the next one.
    bool lost = false;      // another operation recorded the revision
    bool abandoned = false; // the operator abandoned the relocation meanwhile
    std::vector<std::string> unsaved;
    input.checkpoint = [&](const std::vector<std::string>& keys) {
        unsaved.insert(unsaved.end(), keys.begin(), keys.end());
        const auto saved = store.write_transaction([&]() -> std::expected<void, IoError> {
            const auto taken = store.revision_recorded(uuid, creates);
            if (!taken) {
                return std::unexpected(taken.error());
            }
            if (*taken) {
                lost = true;
                return {};
            }
            const auto ours = store.relocation_recorded(uuid, run_id);
            if (!ours) {
                return std::unexpected(ours.error());
            }
            if (!*ours) {
                abandoned = true;
                return {};
            }
            return store.add_progress(run_id, unsaved);
        });
        if (saved && !lost && !abandoned) {
            unsaved.clear();
        }
        return !lost && !abandoned;
    };
    // The first entry that the new carrier does not hold as recorded ends the
    // relocation (below), however this attempt ends. Its record says so at
    // once ("differs"), and again with every later write of this attempt (its
    // "updated", its stop mark), each made when the database allows: a
    // record left behind, as when the process is killed before the end or
    // the record cannot be removed then, is never resumed (decision 0026).
    // The comparison reads on, so that every entry that differs is listed.
    bool differs = false;
    const auto mark_differs = [&] {
        if (!differs) {
            differs = true;
            heartbeat.set("differs", current_timestamp());
            heartbeat.beat();
        }
    };
    input.on_item = [&](const VerifyItemRecord& item, bool fails) {
        // A name that cannot exist here is a warning to a verification, but
        // it would drop out of the new revision (below). An entry that could
        // not be read differs only once the comparison has read everything
        // (below): it is not marked as it is found.
        if ((fails && !item.status.starts_with("Unreadable(")) || item.status == "Unrepresentable") {
            mark_differs();
        }
    };
    auto comparison = compare_with_manifest(services.archive_files, input, cancel, heartbeat);

    // The revision was recorded by another operation, which may have left
    // this record (one of an older version): it can never finish, and goes.
    const auto lost_to_another = [&] {
        stop_mark.ended = true;
        (void)store.write_transaction([&]() -> std::expected<void, IoError> {
            const auto taken = store.revision_recorded(uuid, creates);
            if (!taken) {
                return std::unexpected(taken.error());
            }
            return *taken ? store.remove_relocation(uuid, run_id) : std::expected<void, IoError>{};
        });
        return fail(Kind::Refused, "revision " + std::to_string(creates) + " of archive " + uuid +
                                       " was recorded by another operation while this relocation ran; "
                                       "nothing was changed");
    };
    const auto abandoned_meanwhile = [&] {
        stop_mark.ended = true;
        return fail(Kind::Refused, "the unfinished relocation of archive " + uuid +
                                       " was abandoned while this attempt ran; nothing was changed");
    };
    // An interrupted attempt (cancelled, its device removed) is recorded in
    // the history as an interrupted verification is (R-104): an INCOMPLETE
    // FULL verification of the revision it compared the new carrier with,
    // under a run id of its own (the relocation's run_id is the FULL
    // verification that records the next revision), from when this attempt
    // started, counting what its comparison counted (the entries matched by
    // the attempts before included, as a resumed verification counts them),
    // with its items, the new carrier's evidence, and the relocation it
    // belongs to (decision 0026). Appended to the hash chain as every run is
    // (R-90). Written when the database allows, as the stop mark: the
    // relocation goes on from its record and checkpoints either way.
    const auto record_interruption = [&](const ComparisonResult& compared) {
        VerifyRunRecord run;
        run.run_id = generate_uuid();
        run.archive_uuid = uuid;
        run.revision = previous_revision;
        run.mode = "FULL";
        run.started = attempt_started;
        run.finished = current_timestamp();
        run.result = "INCOMPLETE";
        run.coverage_bytes = compared.coverage_bytes;
        run.counts_json = nlohmann::json{{"items", compared.counts}, {"relocation", run_id}}.dump();
        run.host = services.host.host;
        run.app_version = services.host.app_version;
        auto items = compared.items;
        for (auto& item : items) {
            item.run_id = run.run_id;
        }
        (void)store.write_transaction([&]() -> std::expected<void, IoError> {
            EvidenceRecord seen{uuid, run.run_id, evidence.seen_at, volume_to_json_text(*volume),
                std::string(to_string(own_match)), "relocation interrupted"};
            if (auto inserted = store.insert_evidence(seen); !inserted) {
                return inserted;
            }
            if (auto appended = store.append_verify_run(run, services.repository_uuid); !appended) {
                return std::unexpected(appended.error());
            }
            return store.insert_verify_items(items);
        });
    };
    if (lost) {
        return lost_to_another();
    }
    if (abandoned) {
        return abandoned_meanwhile();
    }
    // The new carrier must hold exactly the recorded content. Names that cannot
    // exist there would silently drop out of the baseline, so they block too.
    // That ends the relocation, also when this attempt was interrupted after
    // it found such an entry: what the operator does next (copy again)
    // changes the carrier, so what matched before says nothing any more, and
    // a new relocation reads it all (decision 0026). An entry that could not
    // be read ends it only once the comparison has read everything: a disk
    // that drops off usually fails the read in flight with an I/O error
    // before the next open finds the device removed. Such an attempt is
    // interrupted (below), and a resume reads the entry again, as it never
    // matched. Its record and checkpoints go in a transaction that waits for
    // the write lock, as its record would (decision 0023): another operation
    // may hold the lock for longer than a statement waits, and a record left
    // behind would let `--resume` accept a second copy without reading again
    // what matched in this one. Where they still cannot go, the record keeps
    // its mark, which the stop mark writes once more.
    const bool carrier_differs = differs || (comparison.content_failure && !comparison.incomplete);
    if (carrier_differs) {
        mark_differs();
        // Interrupted after it found such an entry, the attempt read the new
        // carrier as an interrupted verification reads its carrier: it is
        // recorded INCOMPLETE as every interrupted attempt is (below), with
        // the entries found so far, which claims nothing for the archive.
        if (comparison.incomplete) {
            record_interruption(comparison);
        }
        const auto removed = [&]() -> std::expected<void, IoError> {
            if (auto begun = store.begin_waiting(record_lock_wait, cancel); !begun) {
                return begun;
            }
            if (auto gone = store.remove_relocation(uuid, run_id); !gone) {
                return gone;
            }
            return store.commit();
        }();
        if (!removed) {
            // Still there: the operator abandons it, so that the next
            // relocation reads the whole carrier.
            store.rollback();
            return fail(Kind::Differs,
                "the new carrier does not hold the recorded content; nothing was changed, but the relocation's "
                "saved progress could not be removed: " +
                    removed.error().message + "; abandon the relocation with `archive abandon-relocation " + uuid +
                    "` before relocating again",
                comparison.items);
        }
        stop_mark.ended = true;
        return fail(Kind::Differs,
            "the new carrier does not hold the recorded content; nothing was changed (use "
            "`register --new-revision` to re-baseline changed content)",
            comparison.items);
    }
    if (comparison.incomplete) {
        record_interruption(comparison);
        return fail(Kind::Incomplete, "relocation was interrupted; " + resume_hint);
    }
    if (!comparison.scan_error.empty()) {
        return stopped_by(Kind::Media, "cannot scan the new root: " + comparison.scan_error);
    }
    // Writing the manifest and the index of a large archive takes a while
    // without progress reports: it starts from a fresh "updated".
    heartbeat.beat();

    // Write r(N+1): recorded hashes, the new carrier's names, metadata and identity.
    RelocateOutcome outcome;
    outcome.archive_uuid = uuid;
    outcome.label = archive.label;
    outcome.revision = creates;
    outcome.result = comparison.warning ? "PASSED_WITH_WARNINGS" : "PASSED";
    outcome.run_id = run_id;
    outcome.counts = comparison.counts;
    outcome.items = comparison.items;
    outcome.resumed = request.resume;
    for (auto& item : outcome.items) {
        item.run_id = outcome.run_id;
    }

    ManifestHeader header = manifest->header;
    header.revision = outcome.revision;
    header.supersedes = previous_revision;
    header.created = current_timestamp();
    header.app_version = services.host.app_version;
    header.os = services.host.os;
    header.host = services.host.host;
    header.identity = *volume;
    header.root_subpath = *root_subpath;
    header.root_hint = path_to_utf8(request.root);

    outcome.manifest = manifest_path(services.layout.manifests(), uuid, outcome.revision);
    std::error_code directory_error;
    std::filesystem::create_directories(outcome.manifest.parent_path(), directory_error);
    ManifestWriter writer(services.writer, services.repository_files, outcome.manifest);
    for (const auto& entry : comparison.current_entries) {
        if (auto added = writer.add(entry); !added) {
            return stopped_by(Kind::Repository, "cannot write the manifest: " + added.error().message);
        }
    }
    // Written and flushed without a lock; not at its name yet.
    auto written = writer.finish(header);
    if (!written) {
        return stopped_by(Kind::Repository, "cannot write the manifest: " + written.error().message);
    }

    // As in registration (decision 0023): another operation, such as a
    // registration of the archive's next revision, may record the same
    // revision first. In one write transaction the revision is found not
    // recorded, the record is written, and the manifest is renamed to its name
    // last, so a recorded manifest is never replaced. The relocation's own
    // record and checkpoints go in the same transaction (decision 0026).
    bool cancelled_waiting = false;         // cancelled while it waited for the write lock
    std::optional<std::string> registering; // the root of a registration that has saved files of the revision
    const auto recorded = [&]() -> std::expected<void, IoError> {
        // The record of a large archive holds the write lock for seconds:
        // this one waits for it rather than fail.
        if (auto begun = store.begin_waiting(record_lock_wait, cancel); !begun) {
            // Locked for longer still, or cancelled while it waited. Whoever
            // held the lock may have recorded the revision, and another
            // operation taken it since, or the operator abandoned the
            // relocation.
            if (const auto known = store.revision_recorded(uuid, creates); known && *known) {
                lost = true;
            } else if (const auto ours = store.relocation_recorded(uuid, run_id); known && ours && !*ours) {
                abandoned = true;
            }
            cancelled_waiting = cancel.cancelled();
            return begun;
        }
        const auto known = store.revision_recorded(uuid, creates);
        if (!known) {
            return std::unexpected(known.error());
        }
        if (*known) {
            lost = true;
            return std::unexpected(IoError{IoErrorKind::Other, 0, "the revision is recorded"});
        }
        const auto ours = store.relocation_recorded(uuid, run_id);
        if (!ours) {
            return std::unexpected(ours.error());
        }
        if (!*ours) {
            abandoned = true;
            return std::unexpected(IoError{IoErrorKind::Other, 0, "the relocation was abandoned"});
        }
        // An unfinished registration creating the same revision (`register
        // --new-revision`) checkpoints the files it has hashed under it
        // (R-104). Recorded over them, they would join this revision's index,
        // or fail its insert on their keys: the relocation leaves them to the
        // registration, and stops.
        const auto checkpointed = store.has_file_rows(uuid, creates);
        if (!checkpointed) {
            return std::unexpected(checkpointed.error());
        }
        if (*checkpointed) {
            const auto plan = store.registration_plan(uuid, creates);
            registering = plan && *plan ? **plan : std::string{};
            return std::unexpected(IoError{IoErrorKind::Other, 0, "a registration creates the revision"});
        }
        // One that has hashed nothing yet can never finish once this revision
        // is recorded: its plan goes with the record, and it stops when it
        // finds the revision recorded.
        if (auto removed = store.remove_registration_plan(uuid, creates); !removed) {
            return removed;
        }
        archive.current_revision = creates;
        archive.root_subpath = *root_subpath;
        if (auto updated = store.update_archive(archive); !updated) {
            return updated;
        }
        RevisionRecord revision{uuid, creates, "manifests/" + uuid + "/" + path_to_utf8(outcome.manifest.filename()),
            written->header_line, written->header_blake3, written->header.body_blake3, "", header.created};
        if (auto inserted = store.insert_revision(revision); !inserted) {
            return inserted;
        }
        if (auto indexed = store.insert_file_rows(uuid, creates, comparison.current_entries); !indexed) {
            return indexed;
        }
        EvidenceRecord evidence_record{uuid, outcome.run_id, evidence.seen_at, volume_to_json_text(*volume),
            std::string(to_string(own_match)), "relocated"};
        if (auto inserted = store.insert_evidence(evidence_record); !inserted) {
            return inserted;
        }
        // The verification that accepted the carrier is a FULL verify of the
        // new revision. It ran from the relocation's start; a resumed one
        // says how often it was resumed: the entries matched before each
        // resume were not read again (R-104). Its counts and coverage are the
        // whole relocation's; the INCOMPLETE runs of the attempts before it,
        // which lie within its time, say when each of them read.
        VerifyRunRecord run;
        run.run_id = outcome.run_id;
        run.archive_uuid = uuid;
        run.revision = creates;
        run.mode = "FULL";
        run.started = started;
        run.finished = current_timestamp();
        run.result = outcome.result;
        run.coverage_bytes = comparison.coverage_bytes;
        nlohmann::json counts{{"items", outcome.counts}, {"relocated_from_revision", previous_revision}};
        if (resumes > 0) {
            counts["resumed"] = resumes;
        }
        run.counts_json = counts.dump();
        run.host = services.host.host;
        run.app_version = services.host.app_version;
        if (auto appended = store.append_verify_run(run, services.repository_uuid); !appended) {
            return std::unexpected(appended.error());
        }
        if (auto items = store.insert_verify_items(outcome.items); !items) {
            return items;
        }
        // Finished: its record and its checkpoints go.
        if (auto removed = store.remove_relocation(uuid, run_id); !removed) {
            return removed;
        }
        if (auto named = written->file->commit(); !named) {
            return named;
        }
        return store.commit();
    }();
    if (!recorded) {
        store.rollback();
        if (lost) {
            return lost_to_another();
        }
        if (abandoned) {
            return abandoned_meanwhile();
        }
        if (registering) {
            // The registration keeps the revision (decision 0023) until it is
            // finished, or the operator abandons it, as when its root is gone
            // (decision 0025). This relocation keeps its saved progress for a
            // resume once the registration is abandoned; once the
            // registration records the revision, the relocation can never
            // finish, and its record goes (decision 0026).
            return fail(Kind::Refused, "an unfinished registration of archive " + uuid + " (root \"" + *registering +
                                           "\") is creating revision " + std::to_string(creates) +
                                           "; this relocation recorded nothing (finish that registration with "
                                           "`register --resume`, then relocate again; or abandon it with `archive "
                                           "abandon-registration " +
                                           uuid + "`, then continue this relocation with `archive relocate " + uuid +
                                           " <root_path> --resume`)");
        }
        if (cancelled_waiting) {
            record_interruption(comparison);
            return fail(Kind::Incomplete, "relocation was interrupted; " + resume_hint);
        }
        return stopped_by(Kind::Repository, "cannot record the relocation: " + recorded.error().message);
    }
    stop_mark.ended = true;
    return outcome;
}

} // namespace va
