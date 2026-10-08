#include "core/engine/rebuild_index.h"

#include "common/utf8.h"
#include "common/uuid.h"
#include "core/manifest/manifest_reader.h"

#include <algorithm>
#include <map>

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
        const auto files = repository_files.list(layout.manifests() / directory.native_name);
        if (!files) {
            report.skipped.push_back(archive_uuid + ": " + files.error().message);
            continue;
        }
        for (const auto& file : *files) {
            const auto name = path_to_utf8(file.native_name);
            const auto revision = revision_from_name(name);
            if (!revision) {
                continue;
            }
            const auto path = layout.manifests() / directory.native_name / file.native_name;
            auto reader = ManifestReader::open(repository_files, path);
            if (!reader) {
                report.skipped.push_back(archive_uuid + "/" + name + ": " + reader.error().message);
                continue;
            }
            if (reader->header().archive_uuid != archive_uuid || reader->header().revision != *revision) {
                report.skipped.push_back(archive_uuid + "/" + name + ": header does not match its file name");
                continue;
            }
            FoundRevision item;
            item.header = reader->header();
            item.record = {archive_uuid, *revision, "manifests/" + archive_uuid + "/" + name,
                reader->header_line(), reader->header_blake3(), reader->header().body_blake3, "",
                reader->header().created};
            for (;;) {
                auto next = reader->next();
                if (!next || !*next) {
                    break;
                }
                item.entries.push_back(std::move(**next));
            }
            found[archive_uuid].push_back(std::move(item));
        }
    }

    if (auto begun = store.begin(); !begun) {
        return std::unexpected(begun.error());
    }
    const auto rebuilt = [&]() -> std::expected<void, IoError> {
        if (auto cleared = store.clear_index(); !cleared) {
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
