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

} // namespace

std::expected<RebuildReport, IoError> rebuild_index(
    ArchiveStore& store, IFileSystemAccess& repository_files, const RepositoryLayout& layout)
{
    RebuildReport report;
    std::map<Uuid, std::vector<FoundRevision>> found;
    // The revisions that a manifest in the archive's folder names by its file
    // name, which this rebuild could not read (decision 0028).
    std::map<Uuid, std::set<std::uint32_t>> unread;

    const auto archives = repository_files.list(layout.manifests());
    if (!archives) {
        return std::unexpected(archives.error());
    }
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
            const auto revision = revision_from_name(name);
            if (!revision) {
                continue;
            }
            const auto path = layout.manifests() / directory.native_name / file.native_name;
            const auto cannot_read = "cannot read manifest " + archive_uuid + "/" + name + ": ";
            const auto skip = [&](const std::string& cause) {
                report.skipped.push_back(cannot_read + cause);
                // Revisions start at 1 (§B4.3): "r0000.vam" names none.
                if (*revision > 0) {
                    unread[archive_uuid].insert(*revision);
                }
            };
            auto reader = ManifestReader::open(repository_files, path);
            if (!reader) {
                skip(reader.error().message);
                continue;
            }
            if (reader->header().archive_uuid != archive_uuid || reader->header().revision != *revision) {
                skip("header does not match its file name");
                continue;
            }
            FoundRevision item;
            item.header = reader->header();
            item.record = {archive_uuid, *revision, "manifests/" + archive_uuid + "/" + name,
                reader->header_line(), reader->header_blake3(), reader->header().body_blake3, "",
                reader->header().created};
            // A manifest whose entries cannot all be read is skipped, never
            // indexed with only the entries before the bad line.
            std::optional<std::string> entry_error;
            for (;;) {
                auto next = reader->next();
                if (!next) {
                    entry_error = next.error().message;
                    break;
                }
                if (!*next) {
                    break;
                }
                item.entries.push_back(std::move(**next));
            }
            if (entry_error) {
                skip(*entry_error);
                continue;
            }
            found[archive_uuid].push_back(std::move(item));
        }
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
        // Decision 0028: a revision whose manifest this rebuild could not
        // read, as it is missing or was skipped, keeps its record and its
        // index as they were. No manifest can rebuild them, and the record is
        // what the manifest is checked against once it is restored (R-14):
        // dropped, the index would no longer know the revision, and would
        // take an earlier one for the archive's. Only a record that names the
        // manifest at its revision's own place (R-63): one that names another
        // place is the index differing, and is rebuilt from the manifests.
        // Read in this transaction, as the plans below.
        const auto archives = store.archives();
        if (!archives) {
            return std::unexpected(archives.error());
        }
        std::map<Uuid, ArchiveRecord> existing;
        std::set<std::pair<Uuid, std::uint32_t>> kept;
        for (const auto& archive : *archives) {
            existing.emplace(archive.archive_uuid, archive);
            const auto records = store.revisions(archive.archive_uuid);
            if (!records) {
                return std::unexpected(records.error());
            }
            for (const auto& record : *records) {
                if (record.manifest_relpath == manifest_relpath_of(record.archive_uuid, record.revision) &&
                    !read_here(record.archive_uuid, record.revision)) {
                    kept.emplace(record.archive_uuid, record.revision);
                }
            }
        }
        report.revisions_kept = kept.size();
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
        // Each archive is at the latest revision that a manifest in its folder
        // names by its file name, whether this rebuild could read it or not,
        // or that a record kept names (decision 0028): never at an earlier
        // revision for want of the manifest of the latest. An archive that
        // has no record is made one only from a manifest read here; one of
        // which nothing is left keeps its record as it is.
        std::set<Uuid> archive_uuids;
        for (const auto& [archive_uuid, revisions] : found) {
            archive_uuids.insert(archive_uuid);
        }
        for (const auto& [archive_uuid, revision] : kept) {
            archive_uuids.insert(archive_uuid);
        }
        for (const auto& [archive_uuid, revisions] : unread) {
            archive_uuids.insert(archive_uuid);
        }
        for (const auto& archive_uuid : archive_uuids) {
            auto read = found.find(archive_uuid);
            std::vector<FoundRevision>* revisions = read == found.end() ? nullptr : &read->second;
            std::uint32_t latest = 0;
            if (revisions) {
                std::sort(revisions->begin(), revisions->end(), [](const FoundRevision& a, const FoundRevision& b) {
                    return a.record.revision < b.record.revision;
                });
                latest = revisions->back().record.revision;
            }
            for (const auto& [kept_uuid, revision] : kept) {
                if (kept_uuid == archive_uuid) {
                    latest = std::max(latest, revision);
                }
            }
            if (const auto names = unread.find(archive_uuid); names != unread.end()) {
                latest = std::max(latest, *names->second.rbegin());
            }
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
            archive.current_revision = latest;
            // The root as the manifest of the archive's revision records it,
            // where this rebuild read that manifest.
            if (revisions && revisions->back().record.revision == latest) {
                archive.root_subpath = revisions->back().header.root_subpath;
            }
            if (auto saved = record != existing.end() ? store.update_archive(archive) : store.insert_archive(archive);
                !saved) {
                return saved;
            }
            if (!revisions) {
                continue;
            }
            for (const auto& revision : *revisions) {
                if (auto inserted = store.insert_revision(revision.record); !inserted) {
                    return inserted;
                }
                if (auto indexed = store.insert_file_rows(archive_uuid, revision.record.revision, revision.entries);
                    !indexed) {
                    return indexed;
                }
                ++report.revisions;
                report.entries += revision.entries.size();
            }
            ++report.archives;
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
