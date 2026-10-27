#include "core/engine/rebuild_index.h"

#include "common/utf8.h"
#include "common/uuid.h"
#include "core/engine/recorded_manifest.h"
#include "core/manifest/manifest_reader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace va {
namespace {

struct FoundRevision {
    RevisionRecord record;
    ManifestHeader header;
    std::vector<Entry> entries;
};

// "r0007.vam" -> 7
std::optional<std::uint32_t> revision_from_name(const std::string& name)
{
    if (name.size() < 9 || name.front() != 'r' || !name.ends_with(".vam")) {
        return std::nullopt;
    }
    try {
        return static_cast<std::uint32_t>(std::stoul(name.substr(1, name.size() - 5)));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// A manifest that the rebuild reads: its archive, its file and the revision
// its file name names.
struct ManifestFile {
    Uuid archive_uuid;
    std::filesystem::path path;
    std::string name;              // "r0002.vam"
    std::uint32_t revision = 0;
    std::uint64_t size = 0;        // as listed, for the progress
};

IoError cancelled_rebuild()
{
    return {IoErrorKind::Cancelled, 0, "the rebuild of the index was cancelled; the index was left as it was"};
}

} // namespace

std::expected<RebuildReport, IoError> rebuild_index(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, CancelToken& cancel, IProgressSink& progress)
{
    RebuildReport report;
    std::map<Uuid, std::vector<FoundRevision>> found;
    // The revisions that a manifest in the archive's folder names by its file
    // name, which this rebuild could not read, and why (decision 0028).
    std::map<Uuid, std::map<std::uint32_t, std::string>> unread;

    const auto archives = repository_files.list(layout.manifests());
    if (!archives) {
        return std::unexpected(archives.error());
    }
    // Every manifest first, so the progress knows how many there are and
    // their bytes (R-94).
    std::vector<ManifestFile> manifests;
    for (const auto& directory : *archives) {
        const auto archive_uuid = path_to_utf8(directory.native_name);
        if (directory.type != EntryType::Directory || !is_canonical_uuid(archive_uuid)) {
            continue;
        }
        // Each skipped line starts with its own words and ends with the cause
        // after ": ", so the interface language catalog (decision 0017) can
        // recognise the sentence and translate the cause on its own.
        const auto files = repository_files.list(layout.manifests() / directory.native_name);
        if (!files) {
            report.skipped.push_back(
                "cannot list the manifests of archive " + archive_uuid + ": " + files.error().message);
            continue;
        }
        for (const auto& file : *files) {
            const auto name = path_to_utf8(file.native_name);
            if (const auto revision = revision_from_name(name)) {
                manifests.push_back({archive_uuid, layout.manifests() / directory.native_name / file.native_name, name,
                    *revision, file.size});
            }
        }
    }
    Progress state{0, 0, 0, manifests.size(), ""};
    for (const auto& manifest : manifests) {
        state.bytes_total += manifest.size;
    }
    progress.on_progress(state);

    for (const auto& manifest : manifests) {
        // Asked before each manifest: a cancel that came with a report reads
        // no further manifest (R-94).
        if (cancel.cancelled()) {
            return std::unexpected(cancelled_rebuild());
        }
        const auto& archive_uuid = manifest.archive_uuid;
        const auto& name = manifest.name;
        const auto revision = manifest.revision;
        state.current_item = "manifests/" + archive_uuid + "/" + name;
        const auto cannot_read = "cannot read manifest " + archive_uuid + "/" + name + ": ";
        const auto skip = [&](const std::string& cause) {
            report.skipped.push_back(cannot_read + cause);
            // Revisions start at 1 (§B4.3): "r0000.vam" names none.
            if (revision > 0) {
                unread[archive_uuid][revision] = cause;
            }
        };
        // Reads the manifest; false where a cancel came while it read the
        // entries, which are then not taken.
        const auto read = [&]() -> bool {
            auto reader = ManifestReader::open(repository_files, manifest.path);
            if (!reader) {
                skip(reader.error().message);
                return true;
            }
            if (reader->header().archive_uuid != archive_uuid || reader->header().revision != revision) {
                skip("header does not match its file name");
                return true;
            }
            FoundRevision item;
            item.header = reader->header();
            item.record = {archive_uuid, revision, "manifests/" + archive_uuid + "/" + name, reader->header_line(),
                reader->header_blake3(), reader->header().body_blake3, "", reader->header().created};
            // A manifest whose entries cannot all be read is skipped, never
            // indexed with only the entries before the bad line.
            std::optional<std::string> entry_error;
            for (;;) {
                if (cancel.cancelled()) {
                    return false;
                }
                auto next = reader->next();
                if (!next) {
                    entry_error = next.error().message;
                    break;
                }
                if (!*next) {
                    break;
                }
                item.entries.push_back(std::move(**next));
                // A report every 1000 entries too: a manifest of millions
                // of entries takes a while, and a cancel stops in it.
                if (item.entries.size() % 1000 == 0) {
                    progress.on_progress(state);
                }
            }
            if (entry_error) {
                skip(*entry_error);
                return true;
            }
            found[archive_uuid].push_back(std::move(item));
            return true;
        };
        if (!read()) {
            return std::unexpected(cancelled_rebuild());
        }
        ++state.items_done;
        state.bytes_done += manifest.size;
        progress.on_progress(state);
    }

    if (auto begun = store.begin(); !begun) {
        return std::unexpected(begun.error());
    }
    const auto rebuilt = [&]() -> std::expected<void, IoError> {
        const auto read_here = [&](const Uuid& archive_uuid, std::uint32_t revision) {
            const auto manifests = found.find(archive_uuid);
            return manifests != found.end() &&
                   std::ranges::any_of(manifests->second,
                       [&](const FoundRevision& manifest) { return manifest.record.revision == revision; });
        };
        // Read in this transaction, as the plans below: the archives' records,
        // and the records of the revisions whose manifest this rebuild could
        // not read, as it is missing or was skipped (decision 0028).
        const auto archives = store.archives();
        if (!archives) {
            return std::unexpected(archives.error());
        }
        std::map<Uuid, ArchiveRecord> existing;
        std::map<Uuid, std::vector<RevisionRecord>> unread_records;
        for (const auto& archive : *archives) {
            existing.emplace(archive.archive_uuid, archive);
            const auto records = store.revisions(archive.archive_uuid);
            if (!records) {
                return std::unexpected(records.error());
            }
            for (const auto& record : *records) {
                if (!read_here(record.archive_uuid, record.revision)) {
                    unread_records[record.archive_uuid].push_back(record);
                }
            }
        }
        // A record that names the manifest at its revision's own place (R-63).
        // One that names another place is the index differing, which the
        // manifests rebuild (decision 0028, point 2).
        const auto at_its_place = [](const RevisionRecord& record) {
            return record.manifest_relpath == manifest_relpath_of(record.archive_uuid, record.revision);
        };
        // Each archive is at the latest revision that a manifest in its folder
        // names by its file name, whether this rebuild could read it or not,
        // or that a record of a revision whose manifest it could not read
        // names (decision 0028): never at an earlier revision for want of the
        // manifest of the latest. An archive that has no record is made one
        // only from a manifest read here; one of which nothing is left keeps
        // its record as it is.
        std::map<Uuid, std::uint32_t> latest;
        for (auto& [archive_uuid, revisions] : found) {
            std::sort(revisions.begin(), revisions.end(), [](const FoundRevision& a, const FoundRevision& b) {
                return a.record.revision < b.record.revision;
            });
            latest[archive_uuid] = revisions.back().record.revision;
        }
        for (const auto& [archive_uuid, names] : unread) {
            latest[archive_uuid] = std::max(latest[archive_uuid], names.rbegin()->first);
        }
        for (const auto& [archive_uuid, records] : unread_records) {
            for (const auto& record : records) {
                if (at_its_place(record)) {
                    latest[archive_uuid] = std::max(latest[archive_uuid], record.revision);
                }
            }
        }
        // Nor below the latest revision that a verification of the archive
        // which read its media names: that verification compared the media
        // with the manifest of that revision, which the repository had
        // recorded (R-14). The verification history (R-18) is no index, and
        // no rebuild changes it, so it still names the revision where an
        // older version's rebuild set the archive back for want of that
        // manifest. A verification that read no media (read_no_media) names
        // the revision that the archive's record said, which proves nothing
        // of it. An archive of which nothing else is left keeps its record's
        // revision where the history names no later one.
        for (const auto& [archive_uuid, archive] : existing) {
            const auto runs = store.verify_runs(archive_uuid);
            if (!runs) {
                return std::unexpected(runs.error());
            }
            std::uint32_t verified = 0;
            for (const auto& run : *runs) {
                if (!read_no_media(run.counts_json)) {
                    verified = std::max(verified, run.revision);
                }
            }
            if (verified == 0) {
                continue;
            }
            if (const auto named = latest.find(archive_uuid); named != latest.end()) {
                named->second = std::max(named->second, verified);
            } else if (verified > archive.current_revision) {
                latest[archive_uuid] = verified;
            }
        }
        // What the report says of a revision whose manifest this rebuild
        // could not read: the archive, the revision, its manifest's place, and
        // why the file there could not be read, unless it is missing.
        const auto unread_revision = [&](const Uuid& archive_uuid, std::uint32_t revision, std::string manifest) {
            UnreadRevision item{archive_uuid, "", revision, std::move(manifest), std::nullopt};
            if (const auto archive = existing.find(archive_uuid); archive != existing.end()) {
                item.label = archive->second.label;
            }
            if (const auto names = unread.find(archive_uuid); names != unread.end()) {
                if (const auto cause = names->second.find(revision); cause != names->second.end()) {
                    item.cause = cause->second;
                }
            }
            return item;
        };
        // The archive's revision whose manifest this rebuild could not read
        // keeps its record and its index as they were: no manifest can
        // rebuild them, and the record is what the manifest is checked
        // against once it is restored (R-14); dropped, the index would no
        // longer know the revision, and would take an earlier one for the
        // archive's. The record of an earlier revision whose manifest this
        // rebuild could not read goes with its index, as every record that no
        // manifest rebuilds: kept, its manifest that cannot be used would stop
        // every verification and registration whose matching reads the
        // earlier carriers (decision 0004), for good where no backup holds it.
        std::set<std::pair<Uuid, std::uint32_t>> kept;
        for (const auto& [archive_uuid, records] : unread_records) {
            for (const auto& record : records) {
                auto item = unread_revision(archive_uuid, record.revision, record.manifest_relpath);
                if (at_its_place(record) && record.revision == latest[archive_uuid]) {
                    kept.emplace(archive_uuid, record.revision);
                    report.revisions_kept.push_back(std::move(item));
                } else {
                    report.revisions_left_out.push_back(std::move(item));
                }
            }
        }
        // The archive's revision that a file names, which this rebuild could
        // not read, and of which no record is left: the index has no record of
        // its manifest (decision 0028, point 2).
        for (const auto& [archive_uuid, revision] : latest) {
            if (existing.contains(archive_uuid) && !read_here(archive_uuid, revision) &&
                !kept.contains({archive_uuid, revision})) {
                report.revisions_not_indexed.push_back(
                    unread_revision(archive_uuid, revision, manifest_relpath_of(archive_uuid, revision)));
            }
        }
        // The saved progress of the unfinished registrations stays (decision
        // 0026): the rows under the revision each plan creates, which are no
        // index and which no manifest holds. Unless a manifest read here
        // defines that revision, or a record kept: then the plan is done with
        // (decision 0023), and its rows are that revision's index. Read in
        // this transaction, so no plan is made or removed in between.
        const auto plans = store.settings_with_prefix("registration:");
        if (!plans) {
            return std::unexpected(plans.error());
        }
        std::set<std::pair<Uuid, std::uint32_t>> saved_progress;
        for (const auto& [key, value] : *plans) {
            const auto plan = nlohmann::json::parse(value, nullptr, false);
            const auto revision = plan.is_object() ? plan.find("revision") : plan.end();
            if (!plan.is_object() || revision == plan.end() || !revision->is_number_unsigned()) {
                continue; // no plan this version wrote
            }
            const auto archive_uuid = key.substr(std::string_view("registration:").size());
            const auto created = revision->get<std::uint32_t>();
            if (!read_here(archive_uuid, created) && !kept.contains({archive_uuid, created})) {
                saved_progress.emplace(archive_uuid, created);
                ++report.saved_progress_kept;
            }
        }
        if (auto cleared = store.clear_index(kept, saved_progress); !cleared) {
            return cleared;
        }
        for (const auto& [archive_uuid, revision] : latest) {
            // Rolled back below: a cancelled rebuild changes nothing (R-94).
            if (cancel.cancelled()) {
                return std::unexpected(cancelled_rebuild());
            }
            auto read = found.find(archive_uuid);
            std::vector<FoundRevision>* revisions = read == found.end() ? nullptr : &read->second;
            const auto record = existing.find(archive_uuid);
            if (record == existing.end() && !revisions) {
                continue;
            }
            ArchiveRecord archive;
            if (record != existing.end()) {
                archive = record->second; // keeps label edits, RETIRED status and notes
            } else {
                const auto& newest = revisions->back().header;
                archive = {archive_uuid, newest.label, revisions->front().header.created, 0, "REGISTERED",
                    newest.root_subpath, ""};
            }
            archive.current_revision = revision;
            // The root as the manifest of the archive's revision records it,
            // where this rebuild read that manifest.
            if (revisions && revisions->back().record.revision == revision) {
                archive.root_subpath = revisions->back().header.root_subpath;
            }
            if (auto saved = record != existing.end() ? store.update_archive(archive) : store.insert_archive(archive);
                !saved) {
                return saved;
            }
            if (!revisions) {
                continue;
            }
            for (const auto& found_revision : *revisions) {
                if (auto inserted = store.insert_revision(found_revision.record); !inserted) {
                    return inserted;
                }
                if (auto indexed =
                        store.insert_file_rows(archive_uuid, found_revision.record.revision, found_revision.entries);
                    !indexed) {
                    return indexed;
                }
                ++report.revisions;
                report.entries += found_revision.entries.size();
            }
            ++report.archives;
        }
        // The lists read line 1 of an archive's manifest alone, which says
        // nothing of a body or an entry that this rebuild could not read. So
        // each archive whose revision's manifest it skipped is marked, and
        // the lists read that manifest as this rebuild did, whole, until a
        // rebuild reads it (decision 0028). The marks of the rebuild before
        // go, also those of a manifest restored since.
        const auto marks = store.settings_with_prefix(skipped_manifest_prefix);
        if (!marks) {
            return std::unexpected(marks.error());
        }
        for (const auto& [key, value] : *marks) {
            if (auto removed = store.remove_setting(key); !removed) {
                return removed;
            }
        }
        for (const auto* revisions : {&report.revisions_kept, &report.revisions_not_indexed}) {
            for (const auto& skipped : *revisions) {
                if (!skipped.cause) {
                    continue; // missing: line 1 alone says so
                }
                if (auto marked = store.set_setting(skipped_manifest_key(skipped.archive_uuid),
                        nlohmann::json{{"revision", skipped.revision}}.dump());
                    !marked) {
                    return marked;
                }
            }
        }
        // The index of a database made again is filled now: its mark goes
        // with this transaction, and every command can use it (decision
        // 0036). A rebuild cancelled or failed before leaves the mark.
        if (auto unmarked = store.remove_setting(replaced_database_key); !unmarked) {
            return unmarked;
        }
        return store.commit();
    }();
    if (!rebuilt) {
        store.rollback();
        return std::unexpected(rebuilt.error());
    }
    return report;
}

} // namespace va
