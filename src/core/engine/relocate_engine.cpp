#include "core/engine/relocate_engine.h"

#include "common/time.h"
#include "common/utf8.h"
#include "common/uuid.h"
#include "core/engine/comparison.h"
#include "core/engine/recorded_manifest.h"
#include "core/identity/volume_json.h"
#include "core/manifest/manifest_writer.h"

namespace va {
namespace {

std::unexpected<RelocateFailure> fail(RelocateFailure::Kind kind, std::string message,
    std::vector<VerifyItemRecord> items = {})
{
    return std::unexpected(RelocateFailure{kind, std::move(message), std::move(items)});
}

} // namespace

std::expected<RelocateOutcome, RelocateFailure> relocate_archive(
    RelocateServices& services,
    const RelocateRequest& request,
    CancelToken& cancel,
    IProgressSink& progress)
{
    using Kind = RelocateFailure::Kind;
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
    const auto candidates = load_candidates(store);
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
    const auto started = current_timestamp();
    auto comparison = compare_with_manifest(services.archive_files, input, cancel, progress);
    if (comparison.incomplete) {
        return fail(Kind::Incomplete, "relocation was interrupted; nothing was changed");
    }
    if (!comparison.scan_error.empty()) {
        return fail(Kind::Media, "cannot scan the new root: " + comparison.scan_error);
    }
    // The new carrier must hold exactly the recorded content. Names that cannot
    // exist there would silently drop out of the baseline, so they block too.
    const bool unrepresentable = comparison.counts.contains("Unrepresentable");
    if (comparison.content_failure || unrepresentable) {
        return fail(Kind::Differs,
            "the new carrier does not hold the recorded content; nothing was changed (use "
            "`register --new-revision` to re-baseline changed content)",
            comparison.items);
    }

    // Write r(N+1): recorded hashes, the new carrier's names, metadata and identity.
    const auto previous_revision = archive.current_revision;
    RelocateOutcome outcome;
    outcome.archive_uuid = archive.archive_uuid;
    outcome.label = archive.label;
    outcome.revision = archive.current_revision + 1;
    outcome.result = comparison.warning ? "PASSED_WITH_WARNINGS" : "PASSED";
    outcome.run_id = generate_uuid();
    outcome.counts = comparison.counts;
    outcome.items = comparison.items;
    for (auto& item : outcome.items) {
        item.run_id = outcome.run_id;
    }

    ManifestHeader header = manifest->header;
    header.revision = outcome.revision;
    header.supersedes = archive.current_revision;
    header.created = current_timestamp();
    header.app_version = services.host.app_version;
    header.os = services.host.os;
    header.host = services.host.host;
    header.identity = *volume;
    header.root_subpath = *root_subpath;
    header.root_hint = path_to_utf8(request.root);

    outcome.manifest = manifest_path(services.layout.manifests(), archive.archive_uuid, outcome.revision);
    std::error_code directory_error;
    std::filesystem::create_directories(outcome.manifest.parent_path(), directory_error);
    ManifestWriter writer(services.writer, services.repository_files, outcome.manifest);
    for (const auto& entry : comparison.current_entries) {
        if (auto added = writer.add(entry); !added) {
            return fail(Kind::Repository, "cannot write the manifest: " + added.error().message);
        }
    }
    // Written and flushed without a lock; not at its name yet.
    auto written = writer.finish(header);
    if (!written) {
        return fail(Kind::Repository, "cannot write the manifest: " + written.error().message);
    }

    // As in registration (decision 0023): another operation, such as another
    // relocation of the archive or a registration of its next revision, may
    // record the same revision first. In one write transaction the revision is
    // found not recorded, the record is written, and the manifest is renamed
    // to its name last, so a recorded manifest is never replaced.
    bool lost = false;
    bool cancelled_waiting = false;         // cancelled while it waited for the write lock
    std::optional<std::string> registering; // the root of a registration that has saved files of the revision
    const auto recorded = [&]() -> std::expected<void, IoError> {
        // The record of a large archive holds the write lock for seconds:
        // this one waits for it rather than fail.
        if (auto begun = store.begin_waiting(record_lock_wait, cancel); !begun) {
            // Locked for longer still, or cancelled while it waited. Whoever
            // held the lock may have recorded the revision, and another
            // operation taken it since.
            if (const auto known = store.revision_recorded(archive.archive_uuid, outcome.revision); known && *known) {
                lost = true;
            }
            cancelled_waiting = cancel.cancelled();
            return begun;
        }
        const auto known = store.revision_recorded(archive.archive_uuid, outcome.revision);
        if (!known) {
            return std::unexpected(known.error());
        }
        if (*known) {
            lost = true;
            return std::unexpected(IoError{IoErrorKind::Other, 0, "the revision is recorded"});
        }
        // An unfinished registration creating the same revision (`register
        // --new-revision`) checkpoints the files it has hashed under it
        // (R-104). Recorded over them, they would join this revision's index,
        // or fail its insert on their keys: the relocation leaves them to the
        // registration, and stops.
        const auto checkpointed = store.has_file_rows(archive.archive_uuid, outcome.revision);
        if (!checkpointed) {
            return std::unexpected(checkpointed.error());
        }
        if (*checkpointed) {
            const auto plan = store.registration_plan(archive.archive_uuid, outcome.revision);
            registering = plan && *plan ? **plan : std::string{};
            return std::unexpected(IoError{IoErrorKind::Other, 0, "a registration creates the revision"});
        }
        // One that has hashed nothing yet can never finish once this revision
        // is recorded: its plan goes with the record, and it stops when it
        // finds the revision recorded.
        if (auto removed = store.remove_registration_plan(archive.archive_uuid, outcome.revision); !removed) {
            return removed;
        }
        archive.current_revision = outcome.revision;
        archive.root_subpath = *root_subpath;
        if (auto updated = store.update_archive(archive); !updated) {
            return updated;
        }
        RevisionRecord revision{archive.archive_uuid, outcome.revision,
            "manifests/" + archive.archive_uuid + "/" + path_to_utf8(outcome.manifest.filename()),
            written->header_line, written->header_blake3, written->header.body_blake3, "", header.created};
        if (auto inserted = store.insert_revision(revision); !inserted) {
            return inserted;
        }
        if (auto indexed = store.insert_file_rows(archive.archive_uuid, outcome.revision, comparison.current_entries);
            !indexed) {
            return indexed;
        }
        EvidenceRecord evidence_record{archive.archive_uuid, outcome.run_id, evidence.seen_at,
            volume_to_json_text(*volume), std::string(to_string(own_match)), "relocated"};
        if (auto inserted = store.insert_evidence(evidence_record); !inserted) {
            return inserted;
        }
        // The verification that accepted the carrier is a FULL verify of the new revision.
        VerifyRunRecord run;
        run.run_id = outcome.run_id;
        run.archive_uuid = archive.archive_uuid;
        run.revision = outcome.revision;
        run.mode = "FULL";
        run.started = started;
        run.finished = current_timestamp();
        run.result = outcome.result;
        run.coverage_bytes = comparison.coverage_bytes;
        run.counts_json = nlohmann::json{{"items", outcome.counts}, {"relocated_from_revision", previous_revision}}.dump();
        run.host = services.host.host;
        run.app_version = services.host.app_version;
        if (auto appended = store.append_verify_run(run, services.repository_uuid); !appended) {
            return std::unexpected(appended.error());
        }
        if (auto items = store.insert_verify_items(outcome.items); !items) {
            return items;
        }
        if (auto named = written->file->commit(); !named) {
            return named;
        }
        return store.commit();
    }();
    if (!recorded) {
        store.rollback();
        if (lost) {
            return fail(Kind::Refused, "revision " + std::to_string(outcome.revision) + " of archive " +
                                           archive.archive_uuid +
                                           " was recorded by another operation while this relocation ran; "
                                           "nothing was changed");
        }
        if (registering) {
            // The registration keeps the revision (decision 0023) until it is
            // finished, or the operator abandons it, as when its root is gone
            // (decision 0025).
            return fail(Kind::Refused, "an unfinished registration of archive " + archive.archive_uuid +
                                           " (root \"" + *registering + "\") is creating revision " +
                                           std::to_string(outcome.revision) +
                                           "; nothing was changed (finish it with `register --resume` first, or "
                                           "abandon it with `archive abandon-registration " +
                                           archive.archive_uuid + "`)");
        }
        if (cancelled_waiting) {
            return fail(Kind::Incomplete, "relocation was interrupted; nothing was changed");
        }
        return fail(Kind::Repository, "cannot record the relocation: " + recorded.error().message);
    }
    return outcome;
}

} // namespace va
