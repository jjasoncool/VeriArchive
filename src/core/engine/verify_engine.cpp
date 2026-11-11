#include "core/engine/verify_engine.h"

#include "common/shell_argument.h"
#include "common/time.h"
#include "common/utf8.h"
#include "common/uuid.h"
#include "core/engine/add_chunk_hashes.h"
#include "core/engine/comparison.h"
#include "core/engine/heartbeat.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/registration_attempt.h"
#include "core/engine/relocate_engine.h"
#include "core/engine/volume_watch.h"
#include "core/hash/hash_pipeline.h"
#include "core/identity/volume_json.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"
#include "core/write_file.h"

#include <algorithm>
#include <format>
#include <map>

namespace va {
namespace {

std::unexpected<VerifyStop> stop(VerifyStop::Kind kind, std::string message, MatchOutcome identity = {})
{
    return std::unexpected(VerifyStop{kind, std::move(message), std::move(identity), {}, std::nullopt});
}

// Refused while `adding`, the archive's verification that adds chunk hashes,
// is unfinished (decision 0033), in the command line's words, with that
// verification for the GUI's.
std::unexpected<VerifyStop> refused_by(const AddingVerification& adding)
{
    return std::unexpected(VerifyStop{
        VerifyStop::Kind::Refused, adding_verification_refusal(adding), {}, {}, refusing_verification(adding)});
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
    std::uint64_t adding = 0;  // the chunk size at which the run adds chunk hashes (decision 0033)
    std::optional<RecordedConfirmation> confirmation;  // the run went on past a changed enclosure (decision 0047)
    std::vector<SmartRead> smart;  // the SMART reads its first attempt recorded (decision 0052)
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
    // Decision 0047: the archive's folder on a volume whose filesystem is the
    // archive's, on another device (MIGRATED), as behind another enclosure.
    // Looked at only where no volume matches the archive as recorded:
    // identity matching then gives MIGRATED there, and the verification goes
    // on only with a carrier confirmation, and otherwise stops as every
    // MIGRATED does (exit 12), naming relocation.
    std::vector<Located> migrated;
    // The archive's folder on a volume that matches it, where that folder is
    // not there: the volume is attached, the folder was moved or renamed.
    std::optional<std::filesystem::path> folder_gone;
    for (const auto& volume : *volumes) {
        const MediaEvidence evidence{current_timestamp(), volume, std::nullopt};
        const auto match = match_identity(evidence, candidate.root_subpath, std::span(&candidate, 1));
        const bool other_device = match.result == MatchResult::Migrated;
        if (match.result != MatchResult::Match && match.result != MatchResult::MatchWeak && !other_device) {
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
        if (other_device) {
            if (services.archive_files.list(root)) {
                migrated.push_back(Located{root, volume, candidate.root_subpath});
            }
        } else if (services.archive_files.list(root)) {
            holding.push_back(Located{root, volume, candidate.root_subpath});
        } else if (!folder_gone) {
            folder_gone = root;
        }
    }
    if (holding.empty() && !folder_gone) {
        holding = std::move(migrated);
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
        return "the volume label differs from the one recorded";
    }
    if (detail == "capacity-differs") {
        return "the capacity differs from the one recorded";
    }
    if (detail == "filesystem-type-differs") {
        return "the filesystem type differs from the one recorded";
    }
    if (detail == "model-differs") {
        return "the device model differs from the one recorded";
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
        // A run that added chunk hashes was taken over by another resume, or
        // ended: while a verification that adds them to its archive is
        // unfinished, that one is the one to finish or abandon (decision
        // 0033).
        if (*run && adding_chunk_size(nlohmann::json::parse((*run)->counts_json, nullptr, false)) > 0) {
            const auto adding = unfinished_adding_verification(store, (*run)->archive_uuid);
            if (!adding) {
                return stop(VerifyStop::Kind::Repository, adding.error().message);
            }
            if (*adding) {
                return refused_by(**adding);
            }
        }
        return stop(VerifyStop::Kind::NotFound, "run " + run_id + " is not an interrupted run");
    }
    const auto json = nlohmann::json::parse(*setting, nullptr, false);
    if (!json.is_object() || !json.contains("archive_uuid") || !json.contains("revision")) {
        return stop(VerifyStop::Kind::Repository, "run " + run_id + " has an invalid resume record");
    }
    const auto root = json.find("root");
    return ResumePoint{json["archive_uuid"].get<std::string>(), json["revision"].get<std::uint32_t>(),
        root != json.end() && root->is_string() ? root->get<std::string>() : std::string{}, adding_chunk_size(json),
        carrier_confirmed_in_record(json), json.contains("smart") ? smart_reads_from_json(json["smart"]) : std::vector<SmartRead>{}};
}

// Why a run that adds chunk hashes records no revision with them, though it
// read everything (decision 0033): one is recorded only when the run did not
// fail and every entry of revision `revision` matched as recorded: Verified,
// or recorded unreadable at registration and carried as it is, whether it
// can be read now or not (decision 0010: Unreadable since registration, or
// Unbaselined); nothing new found but in a folder unreadable at registration.
std::optional<std::string> why_no_revision(const VerifyOutcome& outcome, std::uint32_t revision)
{
    const auto stays = "; revision " + std::to_string(revision) + " stays without chunk hashes";
    if (outcome.result == "FAILED") {
        return "the verification did not pass" + stays;
    }
    for (const auto& item : outcome.items) {
        const bool known_unreadable =
            item.status.starts_with("Unreadable(") && item.detail == "known-since-registration";
        if (!known_unreadable && item.status != "Unbaselined") {
            return "not every entry matches revision " + std::to_string(revision) + " as recorded" + stays +
                   " (`register --new-revision` records the archive as it is now, with chunk hashes)";
        }
    }
    return std::nullopt;
}

// Why a run that went on past a changed enclosure, and passed, records no
// revision with the new carrier (decision 0047): a relocation requires every
// entry to match (decision 0004), and a name that cannot exist on this system
// would drop out of the new revision; nothing when one can be recorded. A
// MetadataMismatch is recorded with the new metadata, and an entry unreadable
// since registration is carried as it is (decision 0010), as a relocation
// records them. The cause alone, as VerifyOutcome::carrier_not_recorded holds
// it.
std::optional<std::string> why_no_carrier_revision(const VerifyOutcome& outcome)
{
    for (const auto& item : outcome.items) {
        if (item.status == "Unrepresentable") {
            return "some names cannot exist on this system and would drop out of a new revision (verify the archive "
                   "on a system where they can exist)";
        }
    }
    return std::nullopt;
}

// The media evidence note of a run that went on past a changed enclosure
// (decision 0047), naming how it was confirmed.
std::string confirmation_note(const CarrierConfirmation& confirmation)
{
    if (confirmation.how == CarrierConfirmation::How::DeviceSerial) {
        return "enclosure changed, same device by its serial";
    }
    return confirmation.device ? "enclosure changed, confirmed by the operator after reading the device's serial"
                               : "enclosure changed, confirmed by the operator without the serial";
}

// Decision 0047: whether a verification whose carrier identity matching found
// MIGRATED goes on, with the confirmation it is offered (a resumed run's, as
// its record carries it, or the request's): only where the match is a changed
// enclosure (enclosure_changed) of an archive that takes new revisions, while
// no other operation that creates its next revision is unfinished (decision
// 0033), for the enclosures the confirmation names, as recorded and as seen
// now, on a volume that is no other archive's carrier as recorded at an
// overlapping root (decision 0001, as a relocation's new root), in a
// verification that adds no chunk hashes; a device_serial confirmation only
// with the device's serial of the archive's confirmed device. Nothing: it
// stops as every MIGRATED does (exit 12); a refusal: it stops so, saying why.
std::expected<std::optional<RecordedConfirmation>, VerifyStop> going_on_past_the_enclosure(VerifyServices& services,
    const std::optional<CarrierConfirmation>& offered, const std::optional<ResumePoint>& resume,
    const std::optional<std::string>& resume_run, std::uint64_t adding, const ArchiveCandidate& recorded,
    const VolumeInfo& now, const MatchOutcome& identity)
{
    if (!offered || !enclosure_changed(recorded.identity, now)) {
        return std::optional<RecordedConfirmation>{};
    }
    auto& store = services.store;
    const auto& uuid = recorded.archive_uuid;
    const auto refused = [&](std::string message) {
        return std::unexpected(VerifyStop{VerifyStop::Kind::Refused, std::move(message), identity, {}, std::nullopt});
    };
    const auto archive = store.find_archive(uuid);
    if (!archive) {
        return std::unexpected(VerifyStop{VerifyStop::Kind::Repository, archive.error().message, identity, {},
            std::nullopt});
    }
    if (!*archive) {
        return std::unexpected(VerifyStop{VerifyStop::Kind::Repository,
            "the matched archive is missing from the database", identity, {}, std::nullopt});
    }
    if ((*archive)->status == "RETIRED") {
        return refused("a retired archive takes no new revisions");
    }
    if (adding > 0) {
        return refused("chunk hashes are not added by the verification that accepts another enclosure; verify the "
                       "archive first, then add chunk hashes");
    }
    // One operation that creates the archive's next revision at a time
    // (decision 0033), as a relocation begins: not while a registration, a
    // relocation or a verification that adds chunk hashes is unfinished,
    // whose plan, record or checkpoints the record of revision N+1 would
    // otherwise end. Without the identity: the window then names its own ways
    // on (app::stop_message_in_window), not its MIGRATED advice.
    auto another = another_operation_creates_the_revision(store, **archive);
    if (!another) {
        return std::unexpected(VerifyStop{VerifyStop::Kind::Repository, another.error().message, identity, {},
            std::nullopt});
    }
    if (*another) {
        return std::unexpected(VerifyStop{VerifyStop::Kind::Refused, std::move((*another)->message), {}, {},
            std::move((*another)->refusing)});
    }
    const auto enclosure_recorded = enclosure_serial(recorded.identity);
    const auto enclosure_now = enclosure_serial(now);
    if (!serials_equal(offered->enclosure_recorded, enclosure_recorded) ||
        !serials_equal(offered->enclosure_now, enclosure_now)) {
        if (resume && resume_run) {
            return std::unexpected(VerifyStop{VerifyStop::Kind::Decision,
                "run " + *resume_run + " cannot be resumed: the media identity or the archive revision changed "
                "since it started; run a new verification",
                identity, {}, std::nullopt});
        }
        return refused("the confirmation names the enclosure " + offered->enclosure_now + " (recorded: " +
                       offered->enclosure_recorded + "), but archive " + uuid + " is seen through the enclosure " +
                       enclosure_now + " (recorded: " + enclosure_recorded + "); confirm it again");
    }
    // Decision 0001, as a relocation's new root may not overlap another
    // archive on its volume: a verification by uuid reads no other archive's
    // manifest, so it never saw that this volume is, exactly as recorded
    // (MATCH, MATCH_WEAK), another archive's carrier at an overlapping root,
    // such as a sector-by-sector clone registered as another archive (a copy
    // of this one) behind the enclosure seen now. Recording it as this
    // archive's carrier would make two archives record one volume, and the
    // other's verification find two. One seen through a third enclosure is
    // MIGRATED for both twins: that is the question the operator answers.
    const auto others = load_candidates(store, services.repository_files, services.layout);
    if (!others) {
        return std::unexpected(VerifyStop{VerifyStop::Kind::Repository, others.error().message, identity, {},
            std::nullopt});
    }
    const MediaEvidence seen{current_timestamp(), now, std::nullopt};
    for (const auto& other : *others) {
        if (other.archive_uuid == uuid) {
            continue;
        }
        const auto match = match_identity(seen, other.root_subpath, std::span(&other, 1));
        if ((match.result == MatchResult::Match || match.result == MatchResult::MatchWeak) &&
            roots_overlap(other.root_subpath, recorded.root_subpath)) {
            return refused("this volume is the carrier of archive " + other.archive_uuid +
                           " as recorded, so it is not taken for the device of archive " + uuid + "; verify archive " +
                           other.archive_uuid + ", or attach the device of archive " + uuid +
                           " through another enclosure");
        }
    }
    if (offered->how == CarrierConfirmation::How::DeviceSerial && !resume) {
        if (!offered->device) {
            return refused("a confirmation by the device's serial names no device");
        }
        const auto confirmed = read_confirmed_device(store, uuid);
        if (!confirmed) {
            return std::unexpected(VerifyStop{VerifyStop::Kind::Repository, confirmed.error().message, identity,
                {}, std::nullopt});
        }
        if (!*confirmed) {
            return refused("archive " + uuid + " has no confirmed device to compare the serial " +
                           offered->device->serial + " with; only the operator can confirm it");
        }
        if (!same_device_identity(*offered->device, (*confirmed)->device)) {
            return refused("the device's own serial " + offered->device->serial + " is not the serial " +
                           (*confirmed)->device.serial + " of the device of archive " + uuid +
                           ": it is another device");
        }
    }
    RecordedConfirmation going_on;
    going_on.confirmation = *offered;
    going_on.confirmation.enclosure_recorded = enclosure_recorded;
    going_on.confirmation.enclosure_now = enclosure_now;
    going_on.from_revision = resume && resume->confirmation ? resume->confirmation->from_revision
                                                            : (*archive)->current_revision;
    return std::optional<RecordedConfirmation>(std::move(going_on));
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
    // Decision 0033: the chunk size at which this run adds chunk hashes; 0
    // when it adds none. A resumed run adds them as the run it continues did.
    std::uint64_t adding = 0;
    if (resume) {
        adding = resume->adding;
        if (request.add_chunk_hashes && adding == 0) {
            return stop(VerifyStop::Kind::Refused,
                "run " + *request.resume_run +
                    " adds no chunk hashes; resume it without --add-chunk-hashes, or start a new verification with "
                    "it");
        }
    } else if (request.add_chunk_hashes) {
        if (request.chunk_size == 0) {
            return stop(VerifyStop::Kind::Refused, "chunk hashes cannot be added at chunk size 0");
        }
        if (!valid_chunk_size(request.chunk_size)) {
            return stop(VerifyStop::Kind::Refused, invalid_chunk_size(request.chunk_size));
        }
        adding = request.chunk_size;
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

    const auto record_evidence = [&](const Uuid& archive_uuid, const std::string& run_id, const std::string& note) {
        return store.insert_evidence({archive_uuid, run_id, evidence.seen_at,
            volume_to_json_text(evidence.volume), std::string(to_string(identity.result)), note});
    };
    // Decision 0047: the confirmation offered past a changed enclosure: a
    // resumed run's, as its record carries it, and no other; otherwise the
    // request's. And the one this run goes on with, where its carrier is
    // MIGRATED behind another enclosure.
    // A confirmation names enclosures, not an archive: it is taken only for
    // the archive named by its uuid, never for whichever archive a root
    // alone matches.
    std::optional<CarrierConfirmation> offered;
    if (resume) {
        if (resume->confirmation) {
            offered = resume->confirmation->confirmation;
        }
    } else if (request.archive_uuid) {
        offered = request.carrier_confirmation;
    }
    std::optional<RecordedConfirmation> confirmed;
    switch (identity.result) {
    case MatchResult::Unknown: {
        (void)record_evidence("", "", "");
        // An empty folder is most often a place where a volume is mounted
        // while its device is not attached, as a mount point on Linux that
        // the system mounts when the device is there (decision 0029, point
        // 7): it says so, as for a root that is not there, never offering
        // `register` for an empty folder (exit 10).
        if (const auto listed = services.archive_files.list(located->root); listed && listed->empty()) {
            return stop(VerifyStop::Kind::NotFound,
                "the folder " + path_to_utf8(located->root) +
                    " is empty, and no archive is registered there; if a volume is mounted at this folder, check "
                    "that the device that holds it is attached");
        }
        return stop(VerifyStop::Kind::Decision,
            "UNKNOWN: this is not a registered archive; use `register` to create one", identity);
    }
    case MatchResult::Ambiguous:
        for (const auto& archive_uuid : identity.archives) {
            (void)record_evidence(archive_uuid, "", "");
        }
        // Selected by its uuid, an archive whose volume and a clone of it are
        // attached together stops as well, finding it on both (decision 0022,
        // point 7): only the volume of the one to verify may stay attached.
        return stop(VerifyStop::Kind::Decision,
            "AMBIGUOUS: the media matches several archives (cloned media); select one with "
            "`verify --full <archive_uuid>`; where the volumes of those archives are attached together, detach all "
            "but the volume of the one to verify, then verify it",
            identity);
    case MatchResult::Migrated: {
        // Decision 0047: behind another enclosure, with a confirmation that
        // it holds the archive's device, the verification goes on; every
        // other MIGRATED stops, naming relocation.
        const auto recorded = std::ranges::find(candidates, identity.archives.front(), &ArchiveCandidate::archive_uuid);
        if (recorded != candidates.end()) {
            auto going_on = going_on_past_the_enclosure(
                services, offered, resume, request.resume_run, adding, *recorded, located->volume, identity);
            if (!going_on) {
                (void)record_evidence(identity.archives.front(), "", "");
                return std::unexpected(std::move(going_on.error()));
            }
            confirmed = std::move(*going_on);
        }
        if (confirmed) {
            break;
        }
        (void)record_evidence(identity.archives.front(), "", "");
        return stop(VerifyStop::Kind::Decision,
            "MIGRATED: the filesystem matches archive " + identity.archives.front() +
                " but the device differs; confirm with `archive relocate " +
                identity.archives.front() + " <root_path>`",
            identity);
    }
    case MatchResult::PreviousCarrier:
        (void)record_evidence(identity.archives.front(), "", "");
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
    // R-104: resume only while the identity matches and the revision is
    // unchanged; a run that went on past a changed enclosure only behind that
    // enclosure (decision 0047).
    if (resume && (identity.result == MatchResult::Mismatch || (*archive)->current_revision != resume->revision ||
                      (resume->confirmation && !confirmed))) {
        return stop(VerifyStop::Kind::Decision,
            "run " + *request.resume_run + " cannot be resumed: the media identity or the archive "
            "revision changed since it started; run a new verification");
    }
    // Decision 0033: chunk hashes are added to an archive whose current
    // revision has none, as its manifest says (R-14), and that takes new
    // revisions. Refused before anything is read or written.
    if (adding > 0 && !resume) {
        const auto refused =
            why_chunk_hashes_cannot_be_added(store, services.repository_files, services.layout, **archive);
        if (!refused) {
            return stop(VerifyStop::Kind::Repository, refused.error().message);
        }
        if (*refused) {
            return stop(VerifyStop::Kind::Refused, **refused);
        }
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
    outcome.adding_chunk_size = adding;
    outcome.carrier_confirmed = confirmed;
    const auto started = current_timestamp();

    // Make this run resumable from now on, carrying over a resumed run's
    // checkpoints. The record also says where the run reads and, while it
    // works, that it does ("updated"; decision 0022); and whether it adds
    // chunk hashes, at which chunk size, and which process runs it
    // (decisions 0033, 0024).
    std::set<std::string> content_verified;
    nlohmann::json run_record{{"archive_uuid", outcome.archive_uuid}, {"revision", outcome.revision},
        {"started", started}, {"root", path_to_utf8(located->root)}, {"updated", started}};
    // Decision 0047: the confirmation it goes on with past a changed
    // enclosure, which its resume goes on with.
    if (confirmed) {
        run_record[carrier_confirmed_member] =
            carrier_confirmation_json(confirmed->confirmation, confirmed->from_revision);
    }
    // Decision 0052: the SMART of the volume's devices as the window read it
    // when the verification was started, which its resume records too.
    outcome.smart = resume ? resume->smart : reads_of_volume(request.smart_reads, located->volume.devices);
    if (!outcome.smart.empty()) {
        run_record["smart"] = smart_reads_json(outcome.smart);
    }
    // The chunk hashes read, by entry key, of every file larger than the
    // chunk size whose content matched; with those that the checkpoints of a
    // resumed run kept (decision 0033).
    std::map<std::string, std::vector<std::string>> chunks_read;
    bool abandoned = false; // the run's record is gone: the operator abandoned it (decision 0033)
    if (adding > 0) {
        run_record[adding_member] = adding;
        if (const auto self = services.processes.current()) {
            record_attempt(run_record, AttemptProcess{*self, services.host.host});
        }
        // One operation that creates the archive's next revision at a time
        // (decision 0033): the run begins, or takes over the run it resumes
        // with its checkpoints and kept chunk hashes, in one write transaction
        // that looks first, before anything of the media is read. So two
        // started at once cannot both go on.
        std::optional<std::string> refused;
        // The other operation that creates the archive's next revision, where
        // it is what refuses this run.
        std::optional<RefusingOperation> refusing;
        const auto claimed = [&]() -> std::expected<void, IoError> {
            if (auto begun = store.begin(); !begun) {
                return begun;
            }
            if (resume) {
                const auto& before = *request.resume_run;
                const auto stored = store.setting(run_setting(before));
                if (!stored) {
                    return std::unexpected(stored.error());
                }
                if (stored->empty()) {
                    // Taken over by another resume meanwhile, or finished.
                    const auto other = unfinished_adding_verification(store, outcome.archive_uuid);
                    if (!other) {
                        return std::unexpected(other.error());
                    }
                    refused = *other ? adding_verification_refusal(**other)
                                     : "run " + before + " is not an interrupted run";
                    if (*other) {
                        refusing = refusing_verification(**other);
                    }
                    store.rollback();
                    return {};
                }
                // Never under an attempt whose process still runs on this
                // computer (decision 0024); a run recorded INCOMPLETE stopped.
                const auto recorded = store.verify_run(before);
                if (!recorded) {
                    return std::unexpected(recorded.error());
                }
                if (!*recorded || (*recorded)->result != "INCOMPLETE") {
                    const auto judged = judge_attempt(
                        nlohmann::json::parse(*stored, nullptr, false), services.processes, services.host.host);
                    if (judged.state == AttemptState::Running) {
                        const auto& process = judged.attempt->process;
                        refused = "this verification is still running in process " + std::to_string(process.id) +
                                  " (" + (process.program.empty() ? std::string("?") : process.program) +
                                  ") on this computer; resume it once that process has ended";
                        store.rollback();
                        return {};
                    }
                }
                auto previous = store.progress(before);
                if (!previous) {
                    return std::unexpected(previous.error());
                }
                content_verified = std::move(*previous);
                auto kept = store.progress_chunks(before);
                if (!kept) {
                    return std::unexpected(kept.error());
                }
                chunks_read = std::move(*kept);
                const std::vector<std::string> keys(content_verified.begin(), content_verified.end());
                if (auto added = store.add_progress(outcome.run_id, keys); !added) {
                    return added;
                }
                if (auto added = store.add_progress_chunks(outcome.run_id, chunks_read); !added) {
                    return added;
                }
                if (auto cleared = store.clear_progress(before); !cleared) {
                    return cleared;
                }
                if (auto removed = store.remove_setting(run_setting(before)); !removed) {
                    return removed;
                }
            } else {
                const auto now = store.find_archive(outcome.archive_uuid);
                if (!now) {
                    return std::unexpected(now.error());
                }
                if (*now) {
                    auto another = another_operation_creates_the_revision(store, **now);
                    if (!another) {
                        return std::unexpected(another.error());
                    }
                    if (*another) {
                        refused = std::move((*another)->message);
                        refusing = std::move((*another)->refusing);
                        store.rollback();
                        return {};
                    }
                    if (auto removed = remove_adding_verifications_of_earlier_revisions(store, **now); !removed) {
                        return removed;
                    }
                }
            }
            if (auto saved = store.set_setting(run_setting(outcome.run_id), run_record.dump()); !saved) {
                return saved;
            }
            return store.commit();
        }();
        if (!claimed) {
            store.rollback();
            return stop(VerifyStop::Kind::Repository, "cannot start the verification: " + claimed.error().message);
        }
        if (refused) {
            return std::unexpected(
                VerifyStop{VerifyStop::Kind::Refused, std::move(*refused), {}, {}, std::move(refusing)});
        }
    } else {
        (void)store.set_setting(run_setting(outcome.run_id), run_record.dump());
        if (resume) {
            if (auto previous = store.progress(*request.resume_run)) {
                content_verified = std::move(*previous);
            }
            const std::vector<std::string> keys(content_verified.begin(), content_verified.end());
            // All or nothing. When it fails, the run still skips what the run
            // it resumes verified (the keys are in memory), and that run stays
            // as it was, resumable.
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
    }
    // Decision 0044: what each device of the volume reports of its health,
    // asked before anything of the archive is read, kept with the run's media
    // evidence and in its counts, and told at once.
    observe_device_health(services.media, evidence.volume);
    outcome.devices = evidence.volume.devices;
    if (request.devices_observed) {
        request.devices_observed(outcome.devices);
    }
    Heartbeat heartbeat(store, run_setting(outcome.run_id), std::move(run_record), progress);
    // However a run that adds chunk hashes returns without recording itself
    // (its revision's manifest or its record could not be written), its
    // record says last that it stopped ("stopped"), as a registration's plan
    // does (decisions 0022, 0024): judged by the process it names, it would
    // otherwise read as running for as long as that process goes on, as the
    // GUI's does, and its resume and abandoning would be refused. The write is
    // in a destructor, which runs on every way out of this function, so no
    // return below can forget it. Once the run is recorded the mark is off: a
    // run recorded INCOMPLETE has stopped by its row in the history, and a
    // finished one has no record.
    struct StopMark {
        Heartbeat& heartbeat;
        bool on = false;
        ~StopMark()
        {
            if (on) {
                heartbeat.stop();
            }
        }
    } stop_mark{heartbeat, adding > 0};

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
        // A resumed run that adds chunk hashes reads again a checkpointed
        // file that has chunk lines only where the checkpoints did not keep
        // all of its chunk hashes (decision 0033).
        if (adding > 0) {
            for (const auto& entry : manifest->entries) {
                if (!has_chunk_lines(entry, adding)) {
                    continue;
                }
                const auto kept = chunks_read.find(entry_key(entry));
                if (kept == chunks_read.end() ||
                    kept->second.size() != chunk_line_count(*entry.size, adding)) {
                    content_verified.erase(entry_key(entry));
                }
            }
        }
        ComparisonInput input;
        input.root = located->root;
        input.filesystem_type = located->volume.filesystem_type;
        // R-111: --io, else the config's override by serial, else detection.
        input.rotational = volume_rotational(located->volume, request.rotational, request.rotational_override);
        input.host_os = services.host.os;
        input.header = &manifest->header;
        input.recorded = &manifest->entries;
        input.strict_metadata = request.strict_metadata;
        // By chunk as well, in the same read (decision 0032): a file whose
        // content differs names the chunks that differ.
        input.compare_chunks = true;
        // Or, for a revision without chunk lines, the chunk hashes to add,
        // from the same read (decision 0033).
        input.adding_chunk_size = adding;
        input.chunks_read = [&](const Entry& recorded, std::vector<std::string> chunks) {
            chunks_read[entry_key(recorded)] = std::move(chunks);
        };
        input.content_verified = std::move(content_verified);
        // A checkpoint that cannot be saved is rolled back, so its open
        // transaction cannot swallow the next checkpoints and the run's
        // record; its keys are tried again with the next checkpoint. The run
        // goes on: checkpoints only spare re-reading after an interruption.
        // Adding chunk hashes, a checkpoint keeps the chunk hashes of the
        // files it saves, in its transaction (decision 0033), only while the
        // run's record is there: once the operator has abandoned the run
        // (where this computer could not see that it runs), it saves nothing
        // more, and records the verification alone.
        std::vector<std::string> unsaved;
        input.checkpoint = [&](const std::vector<std::string>& keys) {
            unsaved.insert(unsaved.end(), keys.begin(), keys.end());
            const auto saved = store.write_transaction([&]() -> std::expected<void, IoError> {
                if (adding > 0) {
                    const auto ours = store.setting(run_setting(outcome.run_id));
                    if (!ours) {
                        return std::unexpected(ours.error());
                    }
                    if (ours->empty()) {
                        abandoned = true;
                        return {};
                    }
                }
                if (auto added = store.add_progress(outcome.run_id, unsaved); !added || adding == 0) {
                    return added;
                }
                std::map<std::string, std::vector<std::string>> kept;
                for (const auto& key : unsaved) {
                    if (const auto read = chunks_read.find(key); read != chunks_read.end()) {
                        kept.insert(*read);
                    }
                }
                return store.add_progress_chunks(outcome.run_id, kept);
            });
            if (saved || abandoned) {
                unsaved.clear();
            }
            return true;
        };
        // Decision 0034: a read error is the entry's only while the volume is
        // still attached at the root and its device still answers.
        VolumeWatch watch(services.media, located->root, located->volume);
        input.watch = &watch;
        input.clock = request.read_clock;
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
        // Files whose chunk hashes or file hash cannot be relied on (decision 0032).
        outcome.warnings.insert(outcome.warnings.end(), comparison.warnings.begin(), comparison.warnings.end());
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

    // Decision 0033: the revision with chunk hashes added, when the run read
    // every entry and each matched as revision N records it. Its entry lines
    // are N's as they are, which the writer holds to N's body; the manifest
    // is written and flushed without a lock, and gets its name in the record
    // below (decision 0023). A failure to write it keeps the run resumable,
    // as a record that cannot be written does: its resume reads only what it
    // did not match, and records again.
    const auto previous_revision = outcome.revision;
    const auto creates = previous_revision + 1;
    const auto resume_hint = "; resume the run with `verify --full " + outcome.archive_uuid + " --resume " +
                             outcome.run_id + "` to record it";
    std::optional<WrittenManifest> added;
    std::vector<Entry> added_entries;
    if (adding > 0 && outcome.result != "INCOMPLETE") {
        if (auto why = why_no_revision(outcome, previous_revision)) {
            outcome.not_added = std::move(*why);
        } else if (manifest) {
            // Writing the manifest of a large archive takes a while without
            // progress reports: it starts from a fresh "updated".
            heartbeat.beat();
            auto entries = entries_with_chunk_hashes(manifest->entries, chunks_read, adding);
            if (!entries) {
                return stop(VerifyStop::Kind::Repository,
                    "verification finished as " + outcome.result +
                        " but its revision with chunk hashes could not be written: " + entries.error().message +
                        resume_hint);
            }
            const auto header = header_with_chunk_hashes(manifest->header, current_timestamp(), services.host,
                located->volume, located->root_subpath, path_to_utf8(located->root));
            const auto path = manifest_path(services.layout.manifests(), outcome.archive_uuid, creates);
            std::error_code directory_error;
            std::filesystem::create_directories(path.parent_path(), directory_error);
            ManifestWriter writer(services.writer, services.repository_files, path, adding);
            std::expected<void, IoError> written_entries;
            for (const auto& entry : *entries) {
                if (written_entries = writer.add(entry); !written_entries) {
                    break;
                }
            }
            auto written = written_entries ? writer.finish(header)
                                           : std::expected<WrittenManifest, IoError>(
                                                 std::unexpected(written_entries.error()));
            if (!written) {
                return stop(VerifyStop::Kind::Repository,
                    "verification finished as " + outcome.result +
                        " but its revision with chunk hashes could not be written: " + written.error().message +
                        resume_hint);
            }
            if (written->entry_lines_blake3 != manifest->header.body_blake3 ||
                written->entry_lines_length != manifest->header.body_length) {
                outcome.not_added = "the entry lines of revision " + std::to_string(previous_revision) +
                                    " cannot be written again as they are; revision " +
                                    std::to_string(previous_revision) +
                                    " stays without chunk hashes (`register --new-revision` records them)";
            } else {
                added = std::move(*written);
                added_entries = std::move(*entries);
            }
        }
    }
    // The chunk size of the revision recorded with the run: the one chunk
    // hashes are added at, or rN's for a new carrier.
    auto added_chunk_size = adding;
    // Decision 0047: the new carrier behind another enclosure, recorded as
    // revision N+1 when the run passed and every entry matched as a
    // relocation requires (decision 0004): written as a relocation writes it
    // (write_new_carrier_revision), from the comparison's entries (rN's hashes
    // with the carrier's names and metadata), with the carrier read as its
    // identity, so that later verifications match it. A FAILED run records
    // nothing of the carrier, and says so; an INCOMPLETE one waits for its
    // resume.
    if (confirmed && outcome.result == "FAILED") {
        outcome.carrier_not_recorded = "the verification did not pass";
    }
    if (confirmed && manifest && (outcome.result == "PASSED" || outcome.result == "PASSED_WITH_WARNINGS")) {
        if (auto why = why_no_carrier_revision(outcome)) {
            outcome.carrier_not_recorded = std::move(*why);
        } else {
            heartbeat.beat();
            auto written = write_new_carrier_revision(services.writer, services.repository_files,
                manifest_path(services.layout.manifests(), outcome.archive_uuid, creates), manifest->header,
                comparison.current_entries, services.host, located->volume, located->root_subpath,
                path_to_utf8(located->root));
            if (!written) {
                return stop(VerifyStop::Kind::Repository,
                    "verification finished as " + outcome.result +
                        " but its revision with the new enclosure could not be written: " + written.error().message +
                        resume_hint);
            }
            added = std::move(*written);
            added_entries = std::move(comparison.current_entries);
            added_chunk_size = manifest->header.chunk_size;
        }
    }

    // Record evidence, the chained run row and its items in one transaction.
    // With a revision with chunk hashes added, the transaction records it too
    // (decision 0033), as a relocation records its revision (decisions 0023,
    // 0026): it waits for the write lock as such a record does; the archive
    // must still be at revision N, the one compared, or another operation has
    // recorded a revision meanwhile; a registration that has saved files of
    // N+1 keeps the revision (decision 0023). Either way the run records the
    // verification alone, as a FULL verify does. The manifest is renamed to
    // its name last, so a recorded manifest is never replaced.
    nlohmann::json counts{{"items", outcome.counts}};
    if (!outcome.resumed_from.empty()) {
        counts["resumed_from"] = outcome.resumed_from;
    }
    if (const auto health = device_health_json(outcome.devices); !health.empty()) {
        counts["device_health"] = health;
    }
    // Decision 0047: the confirmation it went on with past a changed
    // enclosure, hash-chained with the run whatever its result, so that a
    // wrong "same device" is audited too.
    if (confirmed) {
        counts[carrier_confirmed_member] = carrier_confirmation_json(confirmed->confirmation, confirmed->from_revision);
    }
    // Decision 0052: the SMART read as it was started, hash-chained with the
    // run whatever its result.
    if (!outcome.smart.empty()) {
        counts["smart"] = smart_reads_json(outcome.smart);
    }
    // Decision 0044: how fast this run read each hundredth of the content it
    // read; and, once the verification reached a result, how its read (with
    // the runs it resumed) compares with the last FULL verification of the
    // archive on this device whose speed was as usual.
    if (!comparison.read_parts.empty()) {
        ReadSpeed own;
        own.content_bytes = comparison.content_bytes;
        set_device(own, located->volume);
        own.parts = comparison.read_parts;
        counts["read_speed"] = read_speed_json(own);
        if (outcome.result != "INCOMPLETE") {
            const auto runs = store.verify_runs(outcome.archive_uuid);
            const auto history = runs ? *runs : std::vector<VerifyRunRecord>{};
            auto whole = whole_verification(own, outcome.resumed_from, history);
            if (const auto baseline = speed_baseline(whole, resumed_runs(outcome.resumed_from, history), history)) {
                if (auto compared = compare_read_speed(whole, baseline->speed)) {
                    compared->baseline_run = baseline->run_id;
                    compared->baseline_finished = baseline->finished;
                    counts["read_compared"] = speed_comparison_json(*compared);
                    outcome.read_compared = std::move(*compared);
                }
            }
            outcome.read_speed = std::move(whole);
        }
    }
    VerifyRunRecord run;
    run.run_id = outcome.run_id;
    run.archive_uuid = outcome.archive_uuid;
    run.mode = "FULL";
    run.started = started;
    run.result = outcome.result;
    run.coverage_bytes = outcome.coverage_bytes;
    for (const auto& failure : outcome.failure_classes) {
        run.failure_classes += (run.failure_classes.empty() ? "" : ",") + failure;
    }
    run.host = services.host.host;
    run.app_version = services.host.app_version;

    std::string not_recorded; // why the revision with chunk hashes was not recorded after all
    std::optional<ArchiveRecord> current; // the archive's record as the transaction reads it
    const auto recorded_run = [&]() -> std::expected<void, IoError> {
        not_recorded.clear();
        if (added) {
            if (auto begun = store.begin_waiting(record_lock_wait, cancel); !begun) {
                return begun;
            }
            auto now = store.find_archive(outcome.archive_uuid);
            if (!now) {
                return std::unexpected(now.error());
            }
            current = std::move(*now);
            const auto registering = store.has_file_rows(outcome.archive_uuid, creates);
            if (!registering) {
                return std::unexpected(registering.error());
            }
            if (!current || current->current_revision != previous_revision) {
                not_recorded = "archive " + outcome.archive_uuid + " is at revision " +
                               std::to_string(current ? current->current_revision : 0) +
                               " now, recorded by another operation while this verification ran";
            } else if (current->status == "RETIRED") {
                not_recorded = "archive " + outcome.archive_uuid +
                               " was retired while this verification ran; a retired archive takes no new revisions";
            } else if (*registering) {
                const auto plan = store.registration_plan(outcome.archive_uuid, creates);
                not_recorded = "an unfinished registration of archive " + outcome.archive_uuid + " (root \"" +
                               (plan && *plan ? **plan : std::string{}) + "\") is creating revision " +
                               std::to_string(creates) + "; finish it with `register --resume`, or abandon it with "
                                                         "`archive abandon-registration " +
                               outcome.archive_uuid + "`";
            }
        } else if (auto begun = store.begin(); !begun) {
            return begun;
        }
        // A run that adds chunk hashes and whose record is gone was
        // abandoned while it ran: its checkpoints go even when it was
        // interrupted, as no resume can take it over.
        if (adding > 0) {
            const auto ours = store.setting(run_setting(outcome.run_id));
            if (!ours) {
                return std::unexpected(ours.error());
            }
            if (ours->empty()) {
                abandoned = true;
                if (added && not_recorded.empty()) {
                    not_recorded = "the verification was abandoned while it ran";
                }
            }
        }
        const bool records_revision = added && not_recorded.empty();
        auto row = run;
        auto row_counts = counts;
        // The history says which runs added chunk hashes, at which chunk size.
        if (adding > 0) {
            row_counts[adding_member] = adding;
        }
        row.revision = records_revision ? creates : previous_revision;
        if (records_revision && adding > 0) {
            row_counts["chunk_hashes_added_from_revision"] = previous_revision;
        }
        row.counts_json = row_counts.dump();
        row.finished = current_timestamp();
        if (auto inserted = record_evidence(outcome.archive_uuid, outcome.run_id,
                confirmed ? confirmation_note(confirmed->confirmation) : std::string{});
            !inserted) {
            return inserted;
        }
        if (auto appended = store.append_verify_run(row, services.repository_uuid); !appended) {
            return std::unexpected(appended.error());
        }
        if (auto items = store.insert_verify_items(outcome.items); !items) {
            return items;
        }
        if (records_revision) {
            // A registration or a relocation that creates the same revision
            // and has saved nothing for it can never finish once it is
            // recorded: its plan or record goes (decisions 0023, 0026).
            if (auto removed = store.remove_registration_plan(outcome.archive_uuid, creates); !removed) {
                return removed;
            }
            if (auto removed = store.remove_relocation_creating(outcome.archive_uuid, creates); !removed) {
                return removed;
            }
            auto record = *current;
            record.current_revision = creates;
            record.root_subpath = located->root_subpath;
            if (auto updated = store.update_archive(record); !updated) {
                return updated;
            }
            const auto path = manifest_path(services.layout.manifests(), outcome.archive_uuid, creates);
            auto relative = path_to_utf8(path.lexically_relative(services.layout.root));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            RevisionRecord revision{outcome.archive_uuid, creates, relative, added->header_line,
                added->header_blake3, added->header.body_blake3, "", added->header.created,
                merkle_root_of(added->header)};
            if (auto inserted = store.insert_revision(revision); !inserted) {
                return inserted;
            }
            if (auto indexed =
                    store.insert_file_rows(outcome.archive_uuid, creates, added_entries, added_chunk_size);
                !indexed) {
                return indexed;
            }
            // Decision 0047: the device whose serial confirmed the new
            // carrier, or that the operator confirmed after reading its
            // serial, is the archive's confirmed device from now on, cached.
            if (confirmed && confirmed->confirmation.device) {
                const ConfirmedDevice device{*confirmed->confirmation.device, row.finished, outcome.run_id};
                if (auto cached = store.set_setting(
                        device_identity_key(outcome.archive_uuid), confirmed_device_json(device).dump());
                    !cached) {
                    return cached;
                }
            }
        }
        // A finished run needs no checkpoints; an INCOMPLETE one keeps them for --resume.
        if (outcome.result != "INCOMPLETE" || abandoned) {
            if (auto cleared = store.clear_progress(outcome.run_id); !cleared) {
                return cleared;
            }
            if (auto removed = store.remove_setting(run_setting(outcome.run_id)); !removed) {
                return removed;
            }
        }
        if (records_revision) {
            if (auto named = added->file->commit(); !named) {
                return named;
            }
        }
        return store.commit();
    }();
    if (!recorded_run) {
        store.rollback();
        return stop(VerifyStop::Kind::Repository,
            "verification finished as " + outcome.result + " but could not be recorded: " +
                recorded_run.error().message + (added ? resume_hint : std::string{}));
    }
    stop_mark.on = false;
    if (added && confirmed) {
        if (not_recorded.empty()) {
            outcome.revision = creates;
            outcome.carrier_revision = creates;
        } else {
            outcome.carrier_not_recorded = not_recorded;
        }
    } else if (added && not_recorded.empty()) {
        outcome.revision = creates;
        outcome.added_revision = creates;
        outcome.added_chunks = added->header.counts.chunks;
        outcome.added_merkle_root = added->header.merkle_root;
    } else if (added) {
        outcome.not_added = not_recorded;
    }
    return outcome;
}

} // namespace va
