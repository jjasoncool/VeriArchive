#include "core/engine/verify_engine.h"

#include "common/time.h"
#include "common/utf8.h"
#include "common/uuid.h"
#include "core/engine/item_rules.h"
#include "core/hash/hash_pipeline.h"
#include "core/identity/volume_json.h"
#include "core/manifest/manifest_reader.h"
#include "core/scan/scanner.h"

#include <algorithm>
#include <set>

namespace va {
namespace {

std::unexpected<VerifyStop> stop(VerifyStop::Kind kind, std::string message, MatchOutcome identity = {})
{
    return std::unexpected(VerifyStop{kind, std::move(message), std::move(identity)});
}

// How an item status affects the run result (§A6).
enum class Severity { None, Warning, Failure };

struct Located {
    std::filesystem::path root;
    VolumeInfo volume;
    std::string root_subpath;
};

std::optional<std::filesystem::path> native_subpath(const std::string& root_subpath)
{
    if (root_subpath.empty()) {
        return std::filesystem::path{};
    }
    const auto native = archive_path_to_native(ArchivePath{root_subpath, "", ""});
    return native ? std::optional(*native) : std::nullopt;
}

std::expected<Located, VerifyStop> locate(VerifyServices& services, const VerifyRequest& request,
    const std::vector<ArchiveCandidate>& pool)
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
        const auto root = relative->empty() ? volume.mount_point : volume.mount_point / *relative;
        if (services.archive_files.list(root)) {
            return Located{root, volume, candidate.root_subpath};
        }
    }
    return stop(VerifyStop::Kind::NotFound,
        "archive " + candidate.archive_uuid + " is not on any attached volume");
}

// Ancestor directories of an ArchivePath string: "a/b/c" -> "a", "a/b".
template <typename Visitor>
void for_each_ancestor(const std::string& path, Visitor visit)
{
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        visit(path.substr(0, slash));
    }
}

template <typename Container>
std::optional<std::string> ancestor_in(const std::string& path, const Container& paths)
{
    std::optional<std::string> found;
    for_each_ancestor(path, [&](const std::string& ancestor) {
        if (!found && paths.contains(ancestor)) {
            found = ancestor;
        }
    });
    return found;
}

std::string unreadable_status(const std::string& reason)
{
    return "Unreadable(" + reason + ")";
}

class Comparison {
public:
    Comparison(VerifyOutcome& outcome, bool strict_metadata, std::string filesystem_type,
        bool same_os)
        : outcome_(outcome), strict_metadata_(strict_metadata),
          filesystem_type_(std::move(filesystem_type)), same_os_(same_os)
    {
    }

    bool content_failure = false;
    bool warning = false;

    void add(const Entry& entry, const std::string& status, const std::string& detail, Severity severity)
    {
        ++outcome_.counts[status];
        if (severity == Severity::Failure) {
            content_failure = true;
        } else if (severity == Severity::Warning) {
            warning = true;
        }
        if (status != "Verified") {
            outcome_.items.push_back({outcome_.run_id, entry.path, entry.stream, status, detail});
        }
    }

    // Metadata differences are warnings unless --strict-metadata (R-51).
    void verified_or_metadata(const Entry& recorded, const Entry& current)
    {
        std::string detail;
        if (recorded.mtime_ns && current.mtime_ns &&
            !mtime_matches(filesystem_type_, *recorded.mtime_ns, *current.mtime_ns)) {
            detail = "mtime";
        }
        if (strict_metadata_) {
            if (recorded.mode && current.mode && *recorded.mode != *current.mode) {
                detail += detail.empty() ? "mode" : ",mode";
            }
            if (recorded.attrs && current.attrs && *recorded.attrs != *current.attrs) {
                detail += detail.empty() ? "attrs" : ",attrs";
            }
        }
        if (detail.empty()) {
            add(recorded, "Verified", "", Severity::None);
        } else {
            add(recorded, "MetadataMismatch", detail,
                strict_metadata_ ? Severity::Failure : Severity::Warning);
        }
    }

    // Decision 0008: on the registering OS a link target must match exactly;
    // across OSes an absolute target may be translated by the driver.
    void compare_link(const Entry& recorded, const Entry& current)
    {
        if (recorded.b3 == current.b3 && recorded.sha256 == current.sha256) {
            verified_or_metadata(recorded, current);
            return;
        }
        const auto absolute = [](const Entry& entry) {
            return std::find(entry.warn.begin(), entry.warn.end(), "link-absolute") != entry.warn.end();
        };
        if (!same_os_ && (absolute(recorded) || absolute(current))) {
            add(recorded, "MetadataMismatch", "link-target-translated",
                strict_metadata_ ? Severity::Failure : Severity::Warning);
            return;
        }
        add(recorded, "Modified", "link target differs", Severity::Failure);
    }

private:
    VerifyOutcome& outcome_;
    bool strict_metadata_;
    std::string filesystem_type_;
    bool same_os_;
};

std::optional<Uuid> read_identity_claim(IFileSystemAccess& files, const std::filesystem::path& root)
{
    auto source = files.open(root / ".veriarchive" / "identity.json", ReadOptions{false, true});
    if (!source) {
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(std::min<std::uint64_t>((*source)->size(), 65536)), '\0');
    const auto count = (*source)->read(0, std::as_writable_bytes(std::span(text.data(), text.size())));
    if (!count) {
        return std::nullopt;
    }
    text.resize(*count);
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (!json.is_object() || !json.contains("archive_uuid") || !json["archive_uuid"].is_string()) {
        return std::nullopt;
    }
    return json["archive_uuid"].get<std::string>();
}

} // namespace

std::expected<VerifyOutcome, VerifyStop> verify_full(
    VerifyServices& services,
    const VerifyRequest& request,
    CancelToken& cancel,
    IProgressSink& progress)
{
    auto& store = services.store;
    auto candidates = load_candidates(store);
    if (!candidates) {
        return stop(VerifyStop::Kind::Repository, candidates.error().message);
    }
    // Naming an archive is the operator's selection (§B10 AMBIGUOUS).
    if (request.archive_uuid) {
        std::erase_if(*candidates, [&](const ArchiveCandidate& candidate) {
            return candidate.archive_uuid != *request.archive_uuid;
        });
        if (candidates->empty()) {
            return stop(VerifyStop::Kind::NotFound, "no registered archive " + *request.archive_uuid);
        }
    }

    const auto located = locate(services, request, *candidates);
    if (!located) {
        return std::unexpected(located.error());
    }
    MediaEvidence evidence{current_timestamp(), located->volume,
        read_identity_claim(services.archive_files, located->root)};
    const auto identity = match_identity(evidence, located->root_subpath, *candidates);

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
        return stop(VerifyStop::Kind::Decision,
            "AMBIGUOUS: the media matches several archives (cloned media); select one with "
            "`verify --full <archive_uuid>`",
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
                identity.archives.front() + " before it was relocated; it is not verified (decision 0004)",
            identity);
    default:
        break;
    }

    const auto archive = store.find_archive(identity.archives.front());
    if (!archive || !*archive) {
        return stop(VerifyStop::Kind::Repository, "the matched archive is missing from the database");
    }

    VerifyOutcome outcome;
    outcome.run_id = generate_uuid();
    outcome.archive_uuid = (*archive)->archive_uuid;
    outcome.label = (*archive)->label;
    outcome.revision = (*archive)->current_revision;
    outcome.root = located->root;
    outcome.identity = identity;
    const auto started = current_timestamp();
    bool incomplete = false;
    bool content_failure = false; // an item status that fails the run
    bool item_warning = false;    // an item status that caps the run at warnings

    for (const auto& detail : identity.details) {
        outcome.warnings.push_back("identity: " + detail);
    }
    if (identity.result == MatchResult::MatchWeak) {
        outcome.warnings.push_back("identity: no hardware serial to compare (MATCH_WEAK)");
    }
    if (identity.result == MatchResult::Mismatch) {
        outcome.failure_classes.push_back("MediaIdentityMismatch");
    }

    // The manifest bytes must be the ones the repository recorded (R-14).
    std::vector<Entry> recorded;
    ManifestHeader header;
    if (outcome.failure_classes.empty()) {
        const auto revision = store.revision(outcome.archive_uuid, outcome.revision);
        const auto all_revisions = store.revisions(outcome.archive_uuid);
        if (!revision || !*revision || !all_revisions) {
            outcome.failure_classes.push_back("RepositoryError");
        } else {
            const auto path = services.layout.root / path_from_utf8((*revision)->manifest_relpath);
            auto reader = ManifestReader::open(services.repository_files, path);
            if (!reader) {
                outcome.failure_classes.push_back(
                    reader.error().kind == ManifestFailure::Kind::BodyMismatch ? "ManifestHashMismatch"
                                                                               : "RepositoryError");
                outcome.warnings.push_back("manifest: " + reader.error().message);
            } else if (reader->header_blake3() != (*revision)->header_blake3 ||
                       reader->header().body_blake3 != (*revision)->body_blake3 ||
                       reader->header().archive_uuid != outcome.archive_uuid ||
                       reader->header().revision != outcome.revision) {
                outcome.failure_classes.push_back("ManifestHashMismatch");
            } else {
                const auto highest = std::max_element(all_revisions->begin(), all_revisions->end(),
                    [](const RevisionRecord& a, const RevisionRecord& b) { return a.revision < b.revision; });
                if (highest != all_revisions->end() && reader->header().revision < highest->revision) {
                    outcome.failure_classes.push_back("ManifestRevisionRollback"); // R-15
                }
                header = reader->header();
                outcome.registered_bytes = header.counts.bytes;
                for (;;) {
                    auto next = reader->next();
                    if (!next) {
                        outcome.failure_classes.push_back("RepositoryError");
                        break;
                    }
                    if (!*next) {
                        break;
                    }
                    recorded.push_back(std::move(**next));
                }
            }
        }
    }

    if (outcome.failure_classes.empty()) {
        ScanOptions scan_options;
        scan_options.exclusions = header.exclusions;
        auto scanned = scan_tree(services.archive_files, located->root, scan_options, cancel, progress);
        if (!scanned) {
            const auto kind = scanned.error().kind;
            if (kind == IoErrorKind::Cancelled || kind == IoErrorKind::DeviceRemoved) {
                incomplete = true;
            } else {
                outcome.failure_classes.push_back("FilesystemAccessError");
                outcome.warnings.push_back("scan: " + scanned.error().message);
            }
        } else {
            Comparison comparison(outcome, request.strict_metadata, located->volume.filesystem_type,
                header.os == services.host.os);

            // Directory facts needed to explain missing and new entries.
            std::set<std::string> known_unreadable_dirs;  // unreadable at registration
            std::set<std::string> recorded_dirs;          // every directory implied by the manifest
            for (const auto& entry : recorded) {
                if (entry.type == 'd' && !entry.unreadable.empty()) {
                    known_unreadable_dirs.insert(entry.path.p);
                }
                for_each_ancestor(entry.path.p, [&](const std::string& dir) { recorded_dirs.insert(dir); });
            }
            std::map<std::string, std::string> unreadable_dirs_now;
            std::set<std::string> current_dirs;
            for (const auto& scanned_entry : scanned->entries) {
                const auto& entry = scanned_entry.entry;
                if (entry.type == 'd' && !entry.unreadable.empty()) {
                    unreadable_dirs_now[entry.path.p] = entry.unreadable;
                }
                for_each_ancestor(entry.path.p, [&](const std::string& dir) { current_dirs.insert(dir); });
            }
            std::set<std::string> mount_points_now;
            for (const auto& mount_point : scanned->skipped_mount_points) {
                mount_points_now.insert(mount_point.p);
            }

            const auto missing = [&](const Entry& entry) {
                if (entry.type == 'd' && current_dirs.contains(entry.path.p)) {
                    comparison.add(entry, "Verified", "", Severity::None); // now holds new entries
                } else if (ancestor_in(entry.path.p, mount_points_now)) {
                    comparison.add(entry, "SkippedMountPoint", "a mount point now covers this path",
                        Severity::Failure);
                } else if (const auto dir = ancestor_in(entry.path.p, unreadable_dirs_now)) {
                    comparison.add(entry, unreadable_status(unreadable_dirs_now.at(*dir)),
                        "directory " + *dir + " cannot be listed", Severity::Failure);
                } else if ((services.host.os == "windows" && !representable_on_windows(entry.path.p)) ||
                           (!entry.path.enc.empty() && !archive_path_to_native(entry.path))) {
                    comparison.add(entry, "Unrepresentable", "name cannot exist on " + services.host.os,
                        Severity::Warning); // R-44
                } else {
                    comparison.add(entry, "Missing", "", Severity::Failure);
                }
            };
            const auto added = [&](const Entry& entry) {
                if (entry.type == 'd' && recorded_dirs.contains(entry.path.p)) {
                    return; // existed implicitly; its recorded contents are reported individually
                }
                if (ancestor_in(entry.path.p, known_unreadable_dirs)) {
                    comparison.add(entry, "Unbaselined", "inside a directory unreadable at registration",
                        Severity::Warning);
                } else {
                    comparison.add(entry, "New", "", Severity::Failure);
                }
            };

            // Merge the two sorted lists (R-43 order) and collect files to hash.
            std::vector<std::pair<const Entry*, const ScannedEntry*>> to_hash;
            std::size_t m = 0;
            std::size_t s = 0;
            const auto& current = scanned->entries;
            while (m < recorded.size() || s < current.size()) {
                if (s == current.size() || (m < recorded.size() && canonical_less(recorded[m], current[s].entry))) {
                    missing(recorded[m++]);
                    continue;
                }
                if (m == recorded.size() || canonical_less(current[s].entry, recorded[m])) {
                    added(current[s++].entry);
                    continue;
                }
                const auto& was = recorded[m++];
                const auto& now = current[s++];
                const auto& is = now.entry;
                if (was.type != is.type) {
                    comparison.add(was, "TypeChanged", std::string(1, was.type) + " -> " + is.type,
                        Severity::Failure); // R-50
                    continue;
                }
                const bool known = !was.unreadable.empty();
                if (!is.unreadable.empty()) {
                    comparison.add(was, unreadable_status(is.unreadable),
                        known ? "known-since-registration" : "",
                        known ? Severity::Warning : Severity::Failure); // decision 0010
                    continue;
                }
                if (known && was.type != 'f') {
                    comparison.add(was, "Unbaselined", "readable now, unreadable at registration",
                        Severity::Warning);
                    continue;
                }
                if (was.type == 'd') {
                    comparison.add(was, "Verified", "", Severity::None);
                } else if (was.type == 'l') {
                    comparison.compare_link(was, is);
                } else {
                    to_hash.emplace_back(&was, &now);
                }
            }

            // Re-read and re-hash every recorded file (R-100, R-101).
            std::vector<HashJob> jobs;
            jobs.reserve(to_hash.size());
            for (std::size_t index = 0; index < to_hash.size(); ++index) {
                jobs.push_back({index, to_hash[index].second->native});
            }
            HashPipelineOptions hash_options;
            std::optional<bool> rotational = request.rotational;
            if (!rotational) {
                for (const auto& device : located->volume.devices) {
                    if (device.rotational) {
                        rotational = rotational.value_or(false) || *device.rotational;
                    }
                }
            }
            hash_options.readers = default_reader_count(rotational);
            const auto hashed = hash_files(services.archive_files, jobs, hash_options, cancel, progress,
                [&](HashResult&& result) {
                    const auto& was = *to_hash[result.id].first;
                    const auto& is = to_hash[result.id].second->entry;
                    const bool known = !was.unreadable.empty(); // decision 0010
                    if (result.error) {
                        if (result.error->kind == IoErrorKind::DeviceRemoved) {
                            incomplete = true;
                            return false;
                        }
                        comparison.add(was, unreadable_status(unreadable_reason(*result.error)),
                            known ? "known-since-registration" : result.error->message,
                            known ? Severity::Warning : Severity::Failure);
                        return true;
                    }
                    if (known) {
                        comparison.add(was, "Unbaselined", "readable now, unreadable at registration",
                            Severity::Warning);
                        return true;
                    }
                    outcome.coverage_bytes += result.size;
                    if (result.b3 != was.b3 || result.sha256 != was.sha256 || !was.size ||
                        result.size != *was.size) {
                        comparison.add(was, "Modified",
                            was.size && result.size != *was.size ? "size differs" : "content differs",
                            Severity::Failure);
                    } else {
                        comparison.verified_or_metadata(was, is);
                    }
                    return true;
                });
            if (!hashed) {
                incomplete = true;
            }
            content_failure = comparison.content_failure;
            item_warning = comparison.warning;
        }
    }

    // Run result (§A6): INCOMPLETE makes no claim; run-level failures fail.
    if (incomplete) {
        outcome.result = "INCOMPLETE";
    } else if (!outcome.failure_classes.empty() || content_failure) {
        outcome.result = "FAILED";
    } else if (item_warning || !outcome.warnings.empty()) {
        outcome.result = "PASSED_WITH_WARNINGS";
    } else {
        outcome.result = "PASSED";
    }

    // Record evidence, the chained run row and its items in one transaction.
    nlohmann::json counts = nlohmann::json::object();
    for (const auto& [status, count] : outcome.counts) {
        counts[status] = count;
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
