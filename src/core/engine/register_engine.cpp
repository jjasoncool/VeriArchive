#include "core/engine/register_engine.h"

#include "common/time.h"
#include "common/utf8.h"
#include "common/unicode.h"
#include "common/uuid.h"
#include "core/hash/hash_pipeline.h"
#include "core/identity/volume_json.h"
#include "core/manifest/manifest_writer.h"
#include "core/scan/scanner.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <regex>

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

// The unfinished registration of `root`, if there is one.
std::expected<std::optional<Plan>, IoError> find_pending(ArchiveStore& store, const std::filesystem::path& root)
{
    const auto pending = store.settings_with_prefix("registration:");
    if (!pending) {
        return std::unexpected(pending.error());
    }
    const auto wanted = unicode_fold(path_to_utf8(root.lexically_normal()));
    for (const auto& [key, value] : *pending) {
        const auto json = nlohmann::json::parse(value, nullptr, false);
        if (!json.is_object() || !json.contains("root") ||
            unicode_fold(path_to_utf8(path_from_utf8(json["root"].get<std::string>()).lexically_normal())) != wanted) {
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
    const std::vector<ArchiveCandidate>& candidates, const MediaEvidence& evidence,
    const std::string& root_subpath, const RegisterRequest& request)
{
    std::optional<ArchiveCandidate> target;
    for (const auto& candidate : candidates) {
        const auto match = match_identity(evidence, candidate.root_subpath, std::span(&candidate, 1));
        const bool same_volume = match.result == MatchResult::Match ||
                                 match.result == MatchResult::MatchWeak ||
                                 match.result == MatchResult::Migrated;
        const bool previous_carrier = match.result == MatchResult::PreviousCarrier;
        if ((!same_volume && !previous_carrier) || !roots_overlap(candidate.root_subpath, root_subpath)) {
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
        return fail(Kind::Refused, "labels have the form ARCH-NNNN");
    }
    if (request.copy_of && !is_canonical_uuid(*request.copy_of)) {
        return fail(Kind::Refused, "--copy-of needs an archive uuid");
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

    // Decide which archive and revision this registration creates.
    Plan plan;
    std::optional<ArchiveRecord> existing; // archive receiving a new revision
    std::map<std::string, Entry> cached;   // R-104: hashes from the interrupted attempt
    if (request.resume) {
        if (!*pending) {
            return fail(Kind::Refused, "there is no interrupted registration of this root to resume");
        }
        plan = **pending;
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
        if (*pending) {
            return fail(Kind::Refused, "an interrupted registration of this root exists (archive " +
                                           (*pending)->archive_uuid + "); finish it with --resume");
        }
        const auto candidates = load_candidates(store);
        if (!candidates) {
            return fail(Kind::Repository, candidates.error().message);
        }
        const auto checked = check_existing_archives(*candidates, evidence, *root_subpath, request);
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
                const auto previous = store.revision(existing->archive_uuid, existing->current_revision);
                if (previous && *previous) {
                    if (const auto header = recorded_header(**previous)) {
                        plan.copy_of = header->copy_of;
                    }
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
        // Recorded before any work, so an interruption leaves a resumable registration.
        const auto recorded = [&]() -> std::expected<void, IoError> {
            if (auto begun = store.begin(); !begun) {
                return begun;
            }
            if (plan.new_archive) {
                ArchiveRecord archive{plan.archive_uuid, plan.label, plan.created, 0,
                    "REGISTRATION_INCOMPLETE", *root_subpath, ""};
                if (auto inserted = store.insert_archive(archive); !inserted) {
                    return inserted;
                }
            }
            if (auto saved = store.set_setting(pending_key(plan.archive_uuid), plan_json(plan, request.root).dump());
                !saved) {
                return saved;
            }
            return store.commit();
        }();
        if (!recorded) {
            store.rollback();
            return fail(Kind::Repository, recorded.error().message);
        }
    }

    // Scan, then hash every readable file not already hashed before an interruption.
    const auto interrupted = [&](const std::string& why) {
        return fail(Kind::Incomplete, why + "; continue with `register <root_path> --resume`");
    };
    ScanOptions scan_options;
    auto scanned = scan_tree(services.archive_files, request.root, scan_options, cancel, progress);
    if (!scanned) {
        const auto kind = scanned.error().kind;
        if (kind == IoErrorKind::Cancelled || kind == IoErrorKind::DeviceRemoved) {
            return interrupted("registration was interrupted while scanning");
        }
        return fail(Kind::Media, "cannot scan the root: " + scanned.error().message);
    }

    std::vector<HashJob> jobs;
    for (std::size_t index = 0; index < scanned->entries.size(); ++index) {
        auto& entry = scanned->entries[index].entry;
        if (entry.type != 'f' || !entry.unreadable.empty()) {
            continue;
        }
        const auto hit = cached.find(entry_key(entry));
        if (hit != cached.end() && !hit->second.b3.empty() && hit->second.size == entry.size &&
            hit->second.mtime_ns == entry.mtime_ns) {
            entry.b3 = hit->second.b3; // unchanged since the interrupted attempt hashed it
            entry.sha256 = hit->second.sha256;
            continue;
        }
        jobs.push_back({index, scanned->entries[index].native});
    }

    std::vector<Entry> checkpoint;
    auto last_checkpoint = std::chrono::steady_clock::now();
    const auto flush_checkpoint = [&] {
        if (!checkpoint.empty() && store.begin()) {
            (void)store.upsert_file_rows(plan.archive_uuid, plan.revision, checkpoint);
            (void)store.commit();
        }
        checkpoint.clear();
        last_checkpoint = std::chrono::steady_clock::now();
    };
    HashPipelineOptions hash_options;
    hash_options.readers = default_reader_count(rotational_of(*volume, request.rotational));
    bool device_removed = false;
    const auto hashed = hash_files(services.archive_files, jobs, hash_options, cancel, progress,
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
            if (checkpoint.size() >= 1000 ||
                std::chrono::steady_clock::now() - last_checkpoint > std::chrono::seconds(60)) {
                flush_checkpoint();
            }
            return true;
        });
    flush_checkpoint();
    if (!hashed || device_removed) {
        return interrupted(device_removed ? "the device disappeared during registration"
                                          : "registration was cancelled");
    }

    // Manifest first: it is the truth. The database index follows.
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
    const auto written = manifest.finish(header);
    if (!written) {
        return fail(Kind::Repository, "cannot write the manifest: " + written.error().message);
    }
    outcome.counts = written->header.counts;
    outcome.skipped_mount_points = scanned->skipped_mount_points;

    const auto record = [&]() -> std::expected<void, IoError> {
        if (auto begun = store.begin(); !begun) {
            return begun;
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
        return store.commit();
    }();
    if (!record) {
        store.rollback();
        return fail(Kind::Repository, "cannot record the archive: " + record.error().message);
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
