#include "core/engine/verify_engine.h"

#include "common/shell_argument.h"
#include "common/time.h"
#include "common/utf8.h"
#include "common/uuid.h"
#include "core/engine/comparison.h"
#include "core/engine/heartbeat.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/relocate_engine.h"
#include "core/engine/volume_watch.h"
#include "core/hash/hash_pipeline.h"
#include "core/identity/volume_json.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"
#include "core/write_file.h"

#include <algorithm>
#include <format>

namespace va {
namespace {

std::unexpected<VerifyStop> stop(VerifyStop::Kind kind, std::string message, MatchOutcome identity = {})
{
    return std::unexpected(VerifyStop{kind, std::move(message), std::move(identity)});
}

struct Located {
    std::filesystem::path root;
    VolumeInfo volume;
    std::string root_subpath;
};

// The run that a verification resumes (R-104), as its record says.
struct ResumePoint {
    Uuid archive_uuid;
    std::uint32_t revision = 0;
    std::string root;  // where the run read (decision 0022); empty in a record of an older version
};

std::optional<std::filesystem::path> native_subpath(const std::string& root_subpath)
{
    if (root_subpath.empty()) {
        return std::filesystem::path{};
    }
    const auto native = archive_path_to_native(ArchivePath{root_subpath, "", ""});
    return native ? std::optional(*native) : std::nullopt;
}

// Decision 0022, point 7: an archive named by its uuid that more than one
// attached volume holds, as a device and a sector clone of it do. Matching
// cannot tell them apart. A run resumed by the archive's uuid would join two
// volumes into one result where it continued on another volume than the one
// it read (its checkpoints say what matched there); a verification that is
// not a resume would read the volume listed first, not one the operator
// chose, and record its result as the archive's. So both stop, as AMBIGUOUS
// does, naming the roots (and the root a resumed run read), for the operator
// to name the root, or to detach the other volumes.
VerifyStop on_several_volumes(const Uuid& archive_uuid, const std::vector<Located>& holding,
    const std::optional<std::string>& run_id, const std::optional<ResumePoint>& resume)
{
    std::string roots;
    std::vector<std::filesystem::path> found;
    for (const auto& place : holding) {
        roots += (roots.empty() ? "\"" : ", \"") + path_to_utf8(place.root) + "\"";
        found.push_back(place.root);
    }
    MatchOutcome identity;
    identity.result = MatchResult::Ambiguous;
    identity.archives = {archive_uuid};
    const auto head = "AMBIGUOUS: archive " + archive_uuid + " is on more than one attached volume (cloned media): " +
                      roots + "; ";
    if (!resume || !run_id) {
        return VerifyStop{VerifyStop::Kind::Decision,
            head + "verify it at the root on one of them with `verify --full <root_path>`, or detach the other volumes "
                   "and verify it again",
            std::move(identity), std::move(found)};
    }
    const auto how = "with `verify --full <root_path> --resume " + *run_id +
                     "`, or detach the other volumes and resume it again";
    return VerifyStop{VerifyStop::Kind::Decision,
        resume->root.empty()
            ? head + "resume run " + *run_id + " at the root on the volume it read " + how
            : head + "run " + *run_id + " read \"" + resume->root + "\": resume it at the root on the volume it read " +
                  how,
        std::move(identity), std::move(found)};
}

// Where the verification reads. By root, there. By archive uuid, on the
// attached volume that holds the archive: its identity matches, and its root
// folder is there. Every attached volume is looked at, and where more than
// one holds the archive, the verification stops (on_several_volumes).
std::expected<Located, VerifyStop> locate(VerifyServices& services, const VerifyRequest& request,
    const std::vector<ArchiveCandidate>& pool, const std::optional<ResumePoint>& resume)
{
    if (request.root) {
        const auto volume = services.media.volume_for_path(*request.root);
        if (!volume) {
            return stop(VerifyStop::Kind::NotFound,
                "cannot identify the volume of the root: " + volume.error().message);
        }
        const auto subpath = root_subpath_of(*request.root, *volume);
        if (!subpath) {
            return stop(VerifyStop::Kind::NotFound, subpath.error().message);
        }
        return Located{*request.root, *volume, *subpath};
    }

    // By archive uuid: look for its carrier among the attached volumes.
    const auto volumes = services.media.volumes();
    if (!volumes) {
        return stop(VerifyStop::Kind::NotFound, "cannot list volumes: " + volumes.error().message);
    }
    const auto& candidate = pool.front();
    std::vector<Located> holding;
    // The archive's folder on a volume that matches it, where that folder is
    // not there: the volume is attached, the folder was moved or renamed.
    std::optional<std::filesystem::path> folder_gone;
    for (const auto& volume : *volumes) {
        const MediaEvidence evidence{current_timestamp(), volume, std::nullopt};
        const auto match = match_identity(evidence, candidate.root_subpath, std::span(&candidate, 1));
        if (match.result != MatchResult::Match && match.result != MatchResult::MatchWeak) {
            continue;
        }
        const auto relative = native_subpath(candidate.root_subpath);
        if (!relative) {
            continue;
        }
        // The root is a folder as the user interfaces make one: the mount
        // point of a volume mounted in a folder ends with a separator on
        // Windows ("C:\mnt\vol\"), which a root at that volume's root drops,
        // while "X:\" and "/" keep theirs.
        const auto root = without_trailing_separators(
            relative->empty() ? volume.mount_point : volume.mount_point / *relative);
        if (services.archive_files.list(root)) {
            holding.push_back(Located{root, volume, candidate.root_subpath});
        } else if (!folder_gone) {
            folder_gone = root;
        }
    }
    if (holding.empty() && folder_gone) {
        return stop(VerifyStop::Kind::NotFound, "the folder " + path_to_utf8(*folder_gone) + " of archive " +
                                                    candidate.archive_uuid +
                                                    " was not found: its volume is attached, but the folder is "
                                                    "not there");
    }
    if (holding.empty()) {
        return stop(VerifyStop::Kind::NotFound,
            "archive " + candidate.archive_uuid + " is not on any attached volume");
    }
    if (holding.size() > 1) {
        return std::unexpected(on_several_volumes(candidate.archive_uuid, holding, request.resume_run, resume));
    }
    return holding.front();
}

// A run in progress is described in settings ("run:<id>") so an interrupted
// run can be resumed even when the process was killed before recording it.
std::string run_setting(const std::string& run_id)
{
    return "run:" + run_id;
}

// An unfinished relocation made for an older revision (decision 0030): the
// revision it verifies, and the later one its archive has now.
struct MadeForOlder {
    std::uint32_t verifies = 0;
    std::uint32_t archive_revision = 0;
};

// Whether the archive's unfinished relocation was made for an older revision
// than the archive has now (ArchiveStore::later_revision), as its record says
// the revision it verifies; nothing for a record that says none.
std::expected<std::optional<MadeForOlder>, IoError> made_for_older(ArchiveStore& store, const Uuid& archive_uuid)
{
    const auto stored = store.setting(relocation_key(archive_uuid));
    if (!stored) {
        return std::unexpected(stored.error());
    }
    const auto record = nlohmann::json::parse(*stored, nullptr, false);
    const auto revision = record.is_object() ? record.find("revision") : record.end();
    if (!record.is_object() || revision == record.end() || !revision->is_number_unsigned()) {
        return std::optional<MadeForOlder>{};
    }
    const auto verifies = revision->get<std::uint32_t>();
    const auto later = store.later_revision(archive_uuid, verifies);
    if (!later) {
        return std::unexpected(later.error());
    }
    if (!*later) {
        return std::optional<MadeForOlder>{};
    }
    return std::optional<MadeForOlder>(MadeForOlder{verifies, **later});
}

// The new root of the archive's unfinished relocation, as its record names
// it, written as one argument of the command that resumes it
// (shell_argument); a record that names none (none is written so) gives the
// placeholder of the usage.
std::expected<std::string, IoError> relocation_root_argument(ArchiveStore& store, const Uuid& archive_uuid)
{
    const auto stored = store.setting(relocation_key(archive_uuid));
    if (!stored) {
        return std::unexpected(stored.error());
    }
    const auto record = nlohmann::json::parse(*stored, nullptr, false);
    const auto root = record.is_object() ? record.find("root") : record.end();
    if (!record.is_object() || root == record.end() || !root->is_string() || root->get<std::string>().empty()) {
        return std::string("<root_path>");
    }
    return shell_argument(root->get<std::string>());
}

// What identity matching noted of the media, in plain words for the warning
// that says it (decision 0017, amended 2026-10-27); `identity.details` in
// --json keeps the codes.
std::string identity_detail_words(const std::string& detail)
{
    if (detail == "volume-label-differs") {
        return "the volume label differs from the one registered";
    }
    if (detail == "capacity-differs") {
        return "the capacity differs from the one registered";
    }
    if (detail == "filesystem-type-differs") {
        return "the filesystem type differs from the one registered";
    }
    if (detail == "model-differs") {
        return "the device model differs from the one registered";
    }
    if (detail == "device-replaced") {
        return "the device was replaced";
    }
    if (detail == "filesystem-id-unavailable") {
        return "the filesystem id cannot be read";
    }
    if (detail == "retired") {
        return "the archive is retired"; // the archive, not its identity
    }
    if (detail == "identity-file-copied") {
        return "the identity file was copied from other media";
    }
    return detail;
}

std::expected<ResumePoint, VerifyStop> find_resume_point(ArchiveStore& store, const std::string& run_id)
{
    const auto setting = store.setting(run_setting(run_id));
    if (!setting) {
        return stop(VerifyStop::Kind::Repository, setting.error().message);
    }
    if (setting->empty()) {
        // An interrupted attempt of a relocation is recorded INCOMPLETE under
        // a run of its own, with no "run:<id>" record: the relocation's own
        // record and checkpoints carry it on, and `archive relocate --resume`
        // continues it while the relocation is unfinished (decision 0026).
        const auto run = store.verify_run(run_id);
        if (!run) {
            return stop(VerifyStop::Kind::Repository, run.error().message);
        }
        const auto part = *run ? relocation_part(**run) : std::nullopt;
        if (part && part->interrupted_attempt) {
            const auto& archive_uuid = (*run)->archive_uuid;
            const auto unfinished = store.relocation_recorded(archive_uuid, part->relocation);
            if (!unfinished) {
                return stop(VerifyStop::Kind::Repository, unfinished.error().message);
            }
            if (*unfinished) {
                // Made for an older revision (decision 0030), it can no
                // longer finish: its resume would be refused.
                const auto older = made_for_older(store, archive_uuid);
                if (!older) {
                    return stop(VerifyStop::Kind::Repository, older.error().message);
                }
                if (*older) {
                    return stop(VerifyStop::Kind::NotFound,
                        "run " + run_id + " is an interrupted attempt of the unfinished relocation of archive " +
                            archive_uuid + ", which verifies revision " + std::to_string((*older)->verifies) +
                            ", but the archive now has a later revision (" +
                            std::to_string((*older)->archive_revision) +
                            "); the relocation can no longer finish: abandon it with `archive abandon-relocation " +
                            archive_uuid + "`, then relocate again");
                }
                const auto root = relocation_root_argument(store, archive_uuid);
                if (!root) {
                    return stop(VerifyStop::Kind::Repository, root.error().message);
                }
                return stop(VerifyStop::Kind::NotFound,
                    "run " + run_id + " is an interrupted attempt of the unfinished relocation of archive " +
                        archive_uuid + "; continue it with `archive relocate " + archive_uuid + " " + *root +
                        " --resume`");
            }
            return stop(VerifyStop::Kind::NotFound, "run " + run_id +
                                                        " is an interrupted attempt of a relocation of archive " +
                                                        archive_uuid + " that has ended; nothing is left to resume");
        }
        return stop(VerifyStop::Kind::NotFound, "run " + run_id + " is not an interrupted run");
    }
    const auto json = nlohmann::json::parse(*setting, nullptr, false);
    if (!json.is_object() || !json.contains("archive_uuid") || !json.contains("revision")) {
        return stop(VerifyStop::Kind::Repository, "run " + run_id + " has an invalid resume record");
    }
    const auto root = json.find("root");
    return ResumePoint{json["archive_uuid"].get<std::string>(), json["revision"].get<std::uint32_t>(),
        root != json.end() && root->is_string() ? root->get<std::string>() : std::string{}};
}

// Decision 0028: the archive is known, and the manifest of its current
// revision cannot be used. The verification FAILED before anything was
// looked for on the media: it reads nothing and records no media evidence,
// and its run joins the history's hash chain like any run (R-18, R-90), its
// counts saying that it read no media (read_no_media). A run it was to resume
// stays as it was, for a resume once the manifest is restored, and stays
// listed with the unfinished operations: such a run supersedes none.
std::expected<VerifyOutcome, VerifyStop> failed_on_its_manifest(VerifyServices& services,
    const ArchiveRecord& archive, const ManifestLoadFailure& failure, const Timestamp& started)
{
    VerifyOutcome outcome;
    outcome.run_id = generate_uuid();
    outcome.archive_uuid = archive.archive_uuid;
    outcome.label = archive.label;
    outcome.revision = archive.current_revision;
    outcome.result = "FAILED";
    outcome.media_read = false;
    outcome.failure_classes.push_back(failure.failure_class);
    outcome.warnings.push_back("manifest: " + failure.message);

    VerifyRunRecord run;
    run.run_id = outcome.run_id;
    run.archive_uuid = outcome.archive_uuid;
    run.revision = outcome.revision;
    run.mode = "FULL";
    run.started = started;
    run.finished = current_timestamp();
    run.result = outcome.result;
    run.coverage_bytes = 0;
    run.counts_json = nlohmann::json{{"items", outcome.counts}, {"media_read", false}}.dump();
    run.failure_classes = failure.failure_class;
    run.host = services.host.host;
    run.app_version = services.host.app_version;
    const auto recorded = services.store.write_transaction([&]() -> std::expected<void, IoError> {
        if (auto appended = services.store.append_verify_run(run, services.repository_uuid); !appended) {
            return std::unexpected(appended.error());
        }
        return {};
    });
    if (!recorded) {
        return stop(VerifyStop::Kind::Repository,
            "verification finished as FAILED but could not be recorded: " + recorded.error().message);
    }
    return outcome;
}

// Decision 0028: by root, an archive whose latest manifest cannot be used
// may be the archive at the root, as only that manifest records its carrier
// and its root; matching without it could take the root for another
// archive's, or for no archive's. Unless the root is plainly another
// archive's, the verification stops, saying which archive, why, and how to
// go on: a verification of that archive by its uuid records the failure,
// then the steps of the plain message, its manifest put back with `import`
// (which records it where the index has no record of it) and, where nothing
// holds it, the last resort, or the index rebuilt where only it is wrong.
std::string root_may_belong_to(const CandidatesFailure& failure)
{
    const auto& unusable = *failure.unusable;
    return "this root may belong to archive " + failure.label + " (" + unusable.archive_uuid +
           "), whose manifest cannot be used: " + what_is_wrong_with(unusable) +
           "; verify that archive by its uuid (`verify --full " + unusable.archive_uuid +
           "`) to record the failure, then " + how_to_fix(unusable, "its manifest");
}

// R-89: a manifest copy on the media is compared with the repository copy. A
// different copy of the same revision is a warning; a valid copy of a *newer*
// revision means the repository may have been rolled back, which fails the run.
void check_media_copies(VerifyServices& services, const std::filesystem::path& root, VerifyOutcome& outcome)
{
    const auto directory = root / ".veriarchive";
    const auto listing = services.archive_files.list(directory);
    if (!listing) {
        return;
    }
    for (const auto& file : *listing) {
        const auto name = path_to_utf8(file.native_name);
        if (name.size() < 9 || name.front() != 'r' || !name.ends_with(".vam")) {
            continue;
        }
        std::uint32_t revision = 0;
        try {
            revision = static_cast<std::uint32_t>(std::stoul(name.substr(1, name.size() - 5)));
        } catch (const std::exception&) {
            continue;
        }
        if (revision == outcome.revision) {
            const auto on_media = read_whole_file(services.archive_files, directory / file.native_name);
            const auto in_repository = read_whole_file(services.repository_files,
                manifest_path(services.layout.manifests(), outcome.archive_uuid, revision));
            if (on_media && in_repository && *on_media != *in_repository) {
                outcome.warnings.push_back("media copy " + name + " differs from the repository copy");
            }
        } else if (revision > outcome.revision) {
            const auto reader = ManifestReader::open(services.archive_files, directory / file.native_name);
            if (reader && reader->header().archive_uuid == outcome.archive_uuid) {
                outcome.failure_classes.push_back("ManifestRevisionRollback");
                outcome.warnings.push_back("the media holds revision " + std::to_string(revision) +
                                           ", newer than the repository's " + std::to_string(outcome.revision) +
                                           ": the repository may have been rolled back");
            }
        }
    }
}

} // namespace

std::expected<VerifyOutcome, VerifyStop> verify_full(
    VerifyServices& services,
    const VerifyRequest& request,
    CancelToken& cancel,
    IProgressSink& progress)
{
    auto& store = services.store;
    std::optional<ResumePoint> resume;
    if (request.resume_run) {
        auto point = find_resume_point(store, *request.resume_run);
        if (!point) {
            return std::unexpected(point.error());
        }
        resume = *point;
    }

    // Naming an archive (or resuming its run) is the operator's selection (§B10
    // AMBIGUOUS): it is the one candidate, and no other archive's manifest is
    // read. A root alone is matched against every archive's latest carrier.
    const auto selected = resume ? std::optional(resume->archive_uuid) : request.archive_uuid;
    // Decision 0028: a named archive is known before anything is looked for
    // on the media, and so is the manifest of its current revision. One that
    // cannot be used fails the verification at once, which is recorded; one
    // that can is read again with its entries once the media is found.
    if (selected) {
        const auto checking_since = current_timestamp();
        const auto archive = store.find_archive(*selected);
        if (!archive) {
            return stop(VerifyStop::Kind::Repository, archive.error().message);
        }
        if (*archive && (*archive)->current_revision > 0) {
            const auto checked = check_recorded_manifest(store, services.repository_files, services.layout,
                (*archive)->archive_uuid, (*archive)->current_revision);
            if (!checked && checked.error().unusable) {
                return failed_on_its_manifest(services, **archive, checked.error(), checking_since);
            }
        }
    }
    auto pool = load_candidate_pool(store, services.repository_files, services.layout, selected);
    if (!pool) {
        return stop(VerifyStop::Kind::Repository, pool.error().message);
    }
    // A named archive whose manifest passed the check above and then cannot
    // be used stops with the plain message alone: it is no archive that may
    // be at a root.
    if (selected && !pool->unusable.empty()) {
        return stop(VerifyStop::Kind::Repository, pool->unusable.front().message);
    }
    auto& candidates = pool->candidates;
    if (selected && candidates.empty()) {
        return stop(VerifyStop::Kind::NotFound, "no registered archive " + *selected);
    }

    const auto located = locate(services, request, candidates, resume);
    if (!located) {
        return std::unexpected(located.error());
    }
    MediaEvidence evidence{current_timestamp(), located->volume,
        read_identity_claim(services.archive_files, located->root)};
    // Decision 0028: by root, an archive whose latest manifest cannot be used
    // may be the archive at the root, unless the root is plainly another
    // archive's: a MATCH of one whose manifest can be used, at its root (the
    // filesystem id and a device that its manifest records, and no identity
    // file naming another archive). Roots of archives never overlap on one
    // volume (decision 0001), so that archive's root cannot be there.
    if (!pool->unusable.empty() && !plainly_another_archives_root(evidence, located->root_subpath, candidates)) {
        return stop(VerifyStop::Kind::Repository, root_may_belong_to(pool->unusable.front()));
    }
    // Earlier revisions' carriers are read only when no latest carrier
    // matched: then they tell an earlier carrier (decision 0004) from media
    // that is no archive's.
    const auto matched =
        match_recorded(store, services.repository_files, services.layout, evidence, located->root_subpath, candidates);
    if (!matched) {
        return stop(VerifyStop::Kind::Repository, matched.error().message);
    }
    const auto& identity = *matched;

    const auto record_evidence = [&](const Uuid& archive_uuid, const std::string& run_id) {
        return store.insert_evidence({archive_uuid, run_id, evidence.seen_at,
            volume_to_json_text(evidence.volume), std::string(to_string(identity.result)), ""});
    };
    switch (identity.result) {
    case MatchResult::Unknown:
        (void)record_evidence("", "");
        return stop(VerifyStop::Kind::Decision,
            "UNKNOWN: this is not a registered archive; use `register` to create one", identity);
    case MatchResult::Ambiguous:
        for (const auto& archive_uuid : identity.archives) {
            (void)record_evidence(archive_uuid, "");
        }
        // Selected by its uuid, an archive whose volume and a clone of it are
        // attached together stops as well, finding it on both (decision 0022,
        // point 7): only the volume of the one to verify may stay attached.
        return stop(VerifyStop::Kind::Decision,
            "AMBIGUOUS: the media matches several archives (cloned media); select one with "
            "`verify --full <archive_uuid>`; where the volumes of those archives are attached together, detach all "
            "but the volume of the one to verify, then verify it",
            identity);
    case MatchResult::Migrated:
        (void)record_evidence(identity.archives.front(), "");
        return stop(VerifyStop::Kind::Decision,
            "MIGRATED: the filesystem matches archive " + identity.archives.front() +
                " but the device differs; confirm with `archive relocate " +
                identity.archives.front() + " <root_path>`",
            identity);
    case MatchResult::PreviousCarrier:
        (void)record_evidence(identity.archives.front(), "");
        return stop(VerifyStop::Kind::Decision,
            "PREVIOUS_CARRIER: this media held an earlier revision of archive " +
                identity.archives.front() + " before it was relocated; it is not verified",
            identity);
    default:
        break;
    }

    const auto archive = store.find_archive(identity.archives.front());
    if (!archive || !*archive) {
        return stop(VerifyStop::Kind::Repository, "the matched archive is missing from the database");
    }
    // R-104: resume only while the identity matches and the revision is unchanged.
    if (resume && (identity.result == MatchResult::Mismatch || (*archive)->current_revision != resume->revision)) {
        return stop(VerifyStop::Kind::Decision,
            "run " + *request.resume_run + " cannot be resumed: the media identity or the archive "
            "revision changed since it started; run a new verification");
    }

    VerifyOutcome outcome;
    outcome.run_id = generate_uuid();
    outcome.resumed_from = request.resume_run.value_or("");
    outcome.archive_uuid = (*archive)->archive_uuid;
    outcome.label = (*archive)->label;
    outcome.revision = (*archive)->current_revision;
    outcome.root = located->root;
    outcome.identity = identity;
    // Decision 0006: the program that read the media, where it is no kernel
    // driver of its format (ntfs-3g), is noted; identity and result never
    // see it.
    if (!located->volume.driver.empty()) {
        outcome.notes.push_back("driver: " + located->volume.driver);
    }
    const auto started = current_timestamp();

    // Make this run resumable from now on, carrying over a resumed run's
    // checkpoints. The record also says where the run reads and, while it
    // works, that it does ("updated"; decision 0022).
    std::set<std::string> content_verified;
    nlohmann::json run_record{{"archive_uuid", outcome.archive_uuid}, {"revision", outcome.revision},
        {"started", started}, {"root", path_to_utf8(located->root)}, {"updated", started}};
    (void)store.set_setting(run_setting(outcome.run_id), run_record.dump());
    Heartbeat heartbeat(store, run_setting(outcome.run_id), std::move(run_record), progress);
    if (resume) {
        if (auto previous = store.progress(*request.resume_run)) {
            content_verified = std::move(*previous);
        }
        const std::vector<std::string> keys(content_verified.begin(), content_verified.end());
        // All or nothing. When it fails, the run still skips what the run it
        // resumes verified (the keys are in memory), and that run stays as it
        // was, resumable.
        (void)store.write_transaction([&]() -> std::expected<void, IoError> {
            if (auto added = store.add_progress(outcome.run_id, keys); !added) {
                return added;
            }
            if (auto cleared = store.clear_progress(*request.resume_run); !cleared) {
                return cleared;
            }
            return store.remove_setting(run_setting(*request.resume_run));
        });
    }

    const bool no_filesystem_id = std::ranges::find(identity.details, "filesystem-id-unavailable") !=
                                  identity.details.end();
    for (const auto& detail : identity.details) {
        if (detail == "filesystem-id-unavailable" && identity.result == MatchResult::MatchWeak) {
            continue; // said by the MATCH_WEAK line below
        }
        outcome.warnings.push_back("identity: " + identity_detail_words(detail));
    }
    if (identity.result == MatchResult::MatchWeak) {
        outcome.warnings.push_back(no_filesystem_id
                ? "identity: the filesystem id cannot be read; the volume was recognised by its device (MATCH_WEAK)"
                : "identity: no hardware serial to compare (MATCH_WEAK)");
    }
    if (identity.result == MatchResult::Mismatch) {
        outcome.failure_classes.push_back("MediaIdentityMismatch");
    }

    // The manifest bytes must be the ones the repository recorded (R-14).
    std::optional<RecordedManifest> manifest;
    if (outcome.failure_classes.empty()) {
        auto loaded = load_recorded_manifest(
            store, services.repository_files, services.layout, outcome.archive_uuid, outcome.revision);
        if (!loaded) {
            outcome.failure_classes.push_back(loaded.error().failure_class);
            outcome.warnings.push_back("manifest: " + loaded.error().message);
        } else {
            manifest = std::move(*loaded);
            outcome.registered_bytes = manifest->header.counts.bytes;
            check_media_copies(services, located->root, outcome);
            // What there is to read: the files (and streams) whose content
            // has hashes, as the checkpoints count them. Those unreadable at
            // registration have none and are never checkpointed (decision 0010).
            std::uint64_t files_total = 0;
            std::uint64_t bytes_total = 0;
            for (const auto& entry : manifest->entries) {
                if ((entry.type == 'f' || entry.type == 's') && !entry.b3.empty()) {
                    ++files_total;
                    bytes_total += entry.size.value_or(0);
                }
            }
            heartbeat.set("files_total", files_total);
            heartbeat.set("bytes_total", bytes_total);
            heartbeat.beat();
        }
    }

    ComparisonResult comparison;
    if (manifest) {
        ComparisonInput input;
        input.root = located->root;
        input.filesystem_type = located->volume.filesystem_type;
        // R-111: --io, else the config's override by serial, else detection.
        input.rotational = volume_rotational(located->volume, request.rotational, request.rotational_override);
        input.host_os = services.host.os;
        input.header = &manifest->header;
        input.recorded = &manifest->entries;
        input.strict_metadata = request.strict_metadata;
        input.content_verified = std::move(content_verified);
        // A checkpoint that cannot be saved is rolled back, so its open
        // transaction cannot swallow the next checkpoints and the run's
        // record; its keys are tried again with the next checkpoint. The run
        // goes on: checkpoints only spare re-reading after an interruption.
        std::vector<std::string> unsaved;
        input.checkpoint = [&](const std::vector<std::string>& keys) {
            unsaved.insert(unsaved.end(), keys.begin(), keys.end());
            if (store.write_transaction([&] { return store.add_progress(outcome.run_id, unsaved); })) {
                unsaved.clear();
            }
            return true;
        };
        // Decision 0034: a read error is the entry's only while the volume is
        // still attached at the root and its device still answers.
        VolumeWatch watch(services.media, located->root, located->volume);
        input.watch = &watch;
        comparison = compare_with_manifest(services.archive_files, input, cancel, heartbeat);
        if (!comparison.scan_error.empty()) {
            outcome.failure_classes.push_back("FilesystemAccessError");
            outcome.warnings.push_back("scan: " + comparison.scan_error);
        }
        if (comparison.device_stop == DeviceStop::WentAway) {
            outcome.warnings.push_back("the device that holds the archive went away while it was read (unplugged, "
                                       "or no longer answering); attach it again, then resume this run");
        } else if (comparison.device_stop == DeviceStop::NotAnswering) {
            outcome.warnings.push_back(std::format(
                "the device that holds the archive stopped answering while it was read: {} entries in a row could "
                "not be read; unplug it and attach it again, then resume this run; if it stops again at the same "
                "place, the device may be damaged",
                io_errors_that_stop));
        }
        // §A6, R-42: portability warnings that the manifest records make the
        // run PASSED_WITH_WARNINGS at most, in one plain line.
        if (records_name_collisions(manifest->entries)) {
            outcome.warnings.push_back(name_collision_warning);
        }
        outcome.counts = comparison.counts;
        outcome.items = comparison.items;
        outcome.coverage_bytes = comparison.coverage_bytes;
        for (auto& item : outcome.items) {
            item.run_id = outcome.run_id;
        }
    }

    // Run result (§A6): INCOMPLETE makes no claim; run-level failures fail.
    if (comparison.incomplete) {
        outcome.result = "INCOMPLETE";
    } else if (!outcome.failure_classes.empty() || comparison.content_failure) {
        outcome.result = "FAILED";
    } else if (comparison.warning || !outcome.warnings.empty()) {
        outcome.result = "PASSED_WITH_WARNINGS";
    } else {
        outcome.result = "PASSED";
    }

    // Record evidence, the chained run row and its items in one transaction.
    nlohmann::json counts{{"items", outcome.counts}};
    if (!outcome.resumed_from.empty()) {
        counts["resumed_from"] = outcome.resumed_from;
    }
    VerifyRunRecord run;
    run.run_id = outcome.run_id;
    run.archive_uuid = outcome.archive_uuid;
    run.revision = outcome.revision;
    run.mode = "FULL";
    run.started = started;
    run.finished = current_timestamp();
    run.result = outcome.result;
    run.coverage_bytes = outcome.coverage_bytes;
    run.counts_json = counts.dump();
    for (const auto& failure : outcome.failure_classes) {
        run.failure_classes += (run.failure_classes.empty() ? "" : ",") + failure;
    }
    run.host = services.host.host;
    run.app_version = services.host.app_version;

    const auto recorded_run = [&]() -> std::expected<void, IoError> {
        if (auto begun = store.begin(); !begun) {
            return begun;
        }
        if (auto inserted = record_evidence(outcome.archive_uuid, outcome.run_id); !inserted) {
            return inserted;
        }
        if (auto appended = store.append_verify_run(run, services.repository_uuid); !appended) {
            return std::unexpected(appended.error());
        }
        if (auto items = store.insert_verify_items(outcome.items); !items) {
            return items;
        }
        // A finished run needs no checkpoints; an INCOMPLETE one keeps them for --resume.
        if (outcome.result != "INCOMPLETE") {
            if (auto cleared = store.clear_progress(outcome.run_id); !cleared) {
                return cleared;
            }
            if (auto removed = store.remove_setting(run_setting(outcome.run_id)); !removed) {
                return removed;
            }
        }
        return store.commit();
    }();
    if (!recorded_run) {
        store.rollback();
        return stop(VerifyStop::Kind::Repository,
            "verification finished as " + outcome.result + " but could not be recorded: " +
                recorded_run.error().message);
    }
    return outcome;
}

} // namespace va
