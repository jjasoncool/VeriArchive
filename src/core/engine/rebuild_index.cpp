#include "core/engine/rebuild_index.h"

#include "common/utf8.h"
#include "common/uuid.h"
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
            auto reader = ManifestReader::open(repository_files, path);
            if (!reader) {
                report.skipped.push_back(cannot_read + reader.error().message);
                continue;
            }
            if (reader->header().archive_uuid != archive_uuid || reader->header().revision != *revision) {
                report.skipped.push_back(cannot_read + "header does not match its file name");
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
                report.skipped.push_back(cannot_read + *entry_error);
                continue;
            }
            found[archive_uuid].push_back(std::move(item));
        }
    }

    if (auto begun = store.begin(); !begun) {
        return std::unexpected(begun.error());
    }
    const auto rebuilt = [&]() -> std::expected<void, IoError> {
        // The saved progress of the unfinished registrations stays (decision
        // 0026): the rows under the revision each plan creates, which are no
        // index and which no manifest holds. Unless a manifest read here
        // defines that revision: then the plan is done with (decision 0023),
        // and its rows are that manifest's index, rebuilt below. Read in this
        // transaction, so no plan is made or removed in between.
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
            const auto manifests = found.find(archive_uuid);
            const bool defined = manifests != found.end() &&
                                 std::ranges::any_of(manifests->second, [&](const FoundRevision& manifest) {
                                     return manifest.record.revision == created;
                                 });
            if (!defined) {
                saved_progress.emplace(archive_uuid, created);
                ++report.saved_progress_kept;
            }
        }
        if (auto cleared = store.clear_index(saved_progress); !cleared) {
            return cleared;
        }
        for (auto& [archive_uuid, revisions] : found) {
            std::sort(revisions.begin(), revisions.end(),
                [](const FoundRevision& a, const FoundRevision& b) { return a.record.revision < b.record.revision; });
            const auto& latest = revisions.back();
            auto existing = store.find_archive(archive_uuid);
            if (!existing) {
                return std::unexpected(existing.error());
            }
            ArchiveRecord archive;
            if (*existing) {
                archive = **existing; // keeps label edits, RETIRED status and notes
            } else {
                archive = {archive_uuid, latest.header.label, revisions.front().header.created, 0,
                    "REGISTERED", "", ""};
            }
            archive.current_revision = latest.record.revision;
            archive.root_subpath = latest.header.root_subpath;
            if (auto saved = *existing ? store.update_archive(archive) : store.insert_archive(archive); !saved) {
                return saved;
            }
            for (const auto& revision : revisions) {
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
