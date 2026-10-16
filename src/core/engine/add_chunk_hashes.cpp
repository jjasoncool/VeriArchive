#include "core/engine/add_chunk_hashes.h"

#include "core/engine/recorded_manifest.h"

#include <format>

namespace va {

std::expected<std::optional<std::string>, IoError> why_chunk_hashes_cannot_be_added(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const ArchiveRecord& archive)
{
    if (archive.status == "RETIRED") {
        return std::optional<std::string>("a retired archive takes no new revisions");
    }
    const auto current = store.revision(archive.archive_uuid, archive.current_revision);
    if (!current) {
        return std::unexpected(current.error());
    }
    if (!*current) {
        return std::unexpected(
            IoError{IoErrorKind::NotFound, 0, "archive " + archive.archive_uuid + " has no revision yet"});
    }
    const auto header = load_recorded_header(repository_files, layout, **current);
    if (!header) {
        return std::unexpected(IoError{IoErrorKind::Other, 0, plain_message(header.error())});
    }
    if (header->chunk_size > 0) {
        return std::optional<std::string>("revision " + std::to_string(header->revision) + " of archive " +
                                          archive.archive_uuid +
                                          " has chunk hashes already; a FULL verify compares them, and there are "
                                          "none to add");
    }
    return std::optional<std::string>{};
}

std::uint64_t adding_chunk_size(const nlohmann::json& record)
{
    if (!record.is_object()) {
        return 0;
    }
    const auto found = record.find(adding_member);
    if (found == record.end() || !found->is_number_unsigned()) {
        return 0;
    }
    const auto chunk_size = found->get<std::uint64_t>();
    return chunk_size > 0 && valid_chunk_size(chunk_size) ? chunk_size : 0;
}

std::expected<std::vector<Entry>, IoError> entries_with_chunk_hashes(const std::vector<Entry>& recorded,
    const std::map<std::string, std::vector<std::string>>& chunks, std::uint64_t chunk_size)
{
    std::vector<Entry> entries;
    entries.reserve(recorded.size());
    for (const auto& entry : recorded) {
        entries.push_back(entry);
        if (!has_chunk_lines(entry, chunk_size)) {
            continue;
        }
        const auto expected = chunk_line_count(*entry.size, chunk_size);
        const auto found = chunks.find(entry_key(entry));
        if (found == chunks.end() || found->second.size() != expected) {
            return std::unexpected(IoError{IoErrorKind::Other, 0,
                std::format("chunk hashes of {}: {} read, {} expected", entry.path.p,
                    found == chunks.end() ? 0 : found->second.size(), expected)});
        }
        entries.back().chunks = found->second;
    }
    return entries;
}

ManifestHeader header_with_chunk_hashes(const ManifestHeader& previous, const Timestamp& created,
    const HostInfo& host, const VolumeInfo& identity, const std::string& root_subpath, const std::string& root_hint)
{
    ManifestHeader header = previous;
    header.revision = previous.revision + 1;
    header.supersedes = previous.revision;
    header.created = created;
    header.app_version = host.app_version;
    header.os = host.os;
    header.host = host.host;
    header.identity = identity;
    header.root_subpath = root_subpath;
    header.root_hint = root_hint;
    return header;
}

} // namespace va
