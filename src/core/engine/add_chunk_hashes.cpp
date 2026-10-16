#include "core/engine/add_chunk_hashes.h"

#include "common/shell_argument.h"
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

namespace {

// Members of a record read without exceptions: another type (a damaged
// record) reads as missing.
std::string text_member(const nlohmann::json& object, const char* name)
{
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::optional<std::uint64_t> unsigned_member(const nlohmann::json& object, const char* name)
{
    const auto found = object.find(name);
    if (found == object.end() || !found->is_number_unsigned()) {
        return std::nullopt;
    }
    return found->get<std::uint64_t>();
}

// The records of the runs that add chunk hashes to the archive, whatever
// revision.
std::expected<std::vector<AddingVerification>, IoError> adding_verifications(
    ArchiveStore& store, const Uuid& archive_uuid)
{
    const auto runs = store.settings_with_prefix("run:");
    if (!runs) {
        return std::unexpected(runs.error());
    }
    std::vector<AddingVerification> found;
    for (const auto& [key, value] : *runs) {
        auto record = nlohmann::json::parse(value, nullptr, false);
        const auto chunk_size = adding_chunk_size(record);
        const auto revision = record.is_object() ? unsigned_member(record, "revision") : std::nullopt;
        if (chunk_size == 0 || !revision || text_member(record, "archive_uuid") != archive_uuid) {
            continue;
        }
        AddingVerification verification;
        verification.run_id = key.substr(std::string_view("run:").size());
        verification.archive_uuid = archive_uuid;
        verification.revision = static_cast<std::uint32_t>(*revision);
        verification.chunk_size = chunk_size;
        verification.root = text_member(record, "root");
        verification.started = text_member(record, "started");
        verification.record = std::move(record);
        found.push_back(std::move(verification));
    }
    return found;
}

} // namespace

std::expected<std::optional<AddingVerification>, IoError> unfinished_adding_verification(
    ArchiveStore& store, const Uuid& archive_uuid)
{
    const auto archive = store.find_archive(archive_uuid);
    if (!archive) {
        return std::unexpected(archive.error());
    }
    if (!*archive) {
        return std::optional<AddingVerification>{};
    }
    auto found = adding_verifications(store, archive_uuid);
    if (!found) {
        return std::unexpected(found.error());
    }
    for (auto& verification : *found) {
        if (verification.revision == (*archive)->current_revision) {
            return std::optional<AddingVerification>(std::move(verification));
        }
    }
    return std::optional<AddingVerification>{};
}

std::string adding_verification_refusal(const AddingVerification& verification)
{
    const auto& uuid = verification.archive_uuid;
    return "an unfinished verification of archive " + uuid + " that adds chunk hashes (run " + verification.run_id +
           ") is creating revision " + std::to_string(verification.revision + 1) +
           "; nothing was changed (finish it with `verify --full " + uuid + " --resume " + verification.run_id +
           "` first, or abandon it with `archive abandon-verification " + uuid + "`)";
}

std::expected<std::optional<NextRevisionRefusal>, IoError> another_operation_creates_the_revision(
    ArchiveStore& store, const ArchiveRecord& archive)
{
    const auto& uuid = archive.archive_uuid;
    using Refusal = std::optional<NextRevisionRefusal>;
    // A registration of the archive (decision 0025), whose revision is not
    // recorded: in the words of a registration through another root path.
    const auto plan_value = store.setting("registration:" + uuid);
    if (!plan_value) {
        return std::unexpected(plan_value.error());
    }
    const auto plan = nlohmann::json::parse(*plan_value, nullptr, false);
    if (const auto creates = plan.is_object() ? unsigned_member(plan, "revision") : std::nullopt) {
        const auto recorded = store.revision_recorded(uuid, static_cast<std::uint32_t>(*creates));
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
        if (!*recorded) {
            return Refusal({"an unfinished registration of archive " + uuid + " (root \"" +
                           text_member(plan, "root") + "\") is creating revision " + std::to_string(*creates) +
                           "; nothing was changed (finish it with `register --resume` first, or abandon it with "
                           "`archive abandon-registration " +
                               uuid + "`)",
                std::nullopt});
        }
    }
    // A relocation of the archive (decision 0026) whose revision is not
    // recorded: in the words of a relocation that meets it.
    const auto relocation_value = store.setting(relocation_key(uuid));
    if (!relocation_value) {
        return std::unexpected(relocation_value.error());
    }
    const auto relocation = nlohmann::json::parse(*relocation_value, nullptr, false);
    if (const auto verifies = relocation.is_object() ? unsigned_member(relocation, "revision") : std::nullopt) {
        const auto recorded = store.revision_recorded(uuid, static_cast<std::uint32_t>(*verifies) + 1);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
        const auto to = text_member(relocation, "root");
        if (!*recorded && !text_member(relocation, "differs").empty()) {
            return Refusal({"the unfinished relocation of archive " + uuid + " (to \"" + to +
                           "\") found that the new carrier does not hold the recorded content and cannot be resumed; "
                           "abandon it with `archive abandon-relocation " +
                               uuid + "`, then relocate again",
                std::nullopt});
        }
        if (!*recorded) {
            return Refusal({"an unfinished relocation of archive " + uuid + " (to \"" + to +
                           "\") exists; resume it with `archive relocate " + uuid + " " + shell_argument(to) +
                               " --resume`, or abandon it with `archive abandon-relocation " + uuid + "`",
                std::nullopt});
        }
    }
    const auto adding = unfinished_adding_verification(store, uuid);
    if (!adding) {
        return std::unexpected(adding.error());
    }
    return *adding ? Refusal({adding_verification_refusal(**adding), **adding}) : Refusal{};
}

std::expected<void, IoError> remove_adding_verifications_of_earlier_revisions(
    ArchiveStore& store, const ArchiveRecord& archive)
{
    const auto found = adding_verifications(store, archive.archive_uuid);
    if (!found) {
        return std::unexpected(found.error());
    }
    for (const auto& verification : *found) {
        if (verification.revision == archive.current_revision) {
            continue;
        }
        if (auto removed = store.remove_setting("run:" + verification.run_id); !removed) {
            return removed;
        }
        if (auto cleared = store.clear_progress(verification.run_id); !cleared) {
            return cleared;
        }
    }
    return {};
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
