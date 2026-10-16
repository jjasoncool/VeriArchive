#include "core/engine/candidates.h"

#include "common/archive_path.h"
#include "common/unicode.h"
#include "common/utf8.h"
#include "core/engine/recorded_manifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <span>
#include <string_view>

namespace va {

std::expected<std::vector<ArchiveCandidate>, CandidatesFailure> load_candidates(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const std::optional<Uuid>& only)
{
    auto pool = load_candidate_pool(store, repository_files, layout, only);
    if (!pool) {
        return std::unexpected(std::move(pool.error()));
    }
    if (!pool->unusable.empty()) {
        return std::unexpected(std::move(pool->unusable.front()));
    }
    return std::move(pool->candidates);
}

std::expected<CandidatePool, CandidatesFailure> load_candidate_pool(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const std::optional<Uuid>& only)
{
    const auto archives = store.archives();
    if (!archives) {
        return std::unexpected(CandidatesFailure{archives.error().message, std::nullopt, {}});
    }
    CandidatePool pool;
    for (const auto& archive : *archives) {
        if (only && archive.archive_uuid != *only) {
            continue;
        }
        if (archive.current_revision == 0) {
            continue; // registration never finished
        }
        const auto latest = store.revision(archive.archive_uuid, archive.current_revision);
        if (!latest) {
            return std::unexpected(CandidatesFailure{latest.error().message, std::nullopt, {}});
        }
        // The archive's revision of which the index has no record (as where
        // `repo rebuild-index` could not read its manifest) cannot be used
        // either: its carrier and root are unknown (decision 0028).
        const auto header = *latest ? load_recorded_header(repository_files, layout, **latest)
                                    : std::expected<ManifestHeader, UnusableManifest>(std::unexpect,
                                          manifest_not_indexed(repository_files, layout, archive.archive_uuid,
                                              archive.current_revision, false));
        if (!header) {
            auto unusable = header.error();
            unusable.retired = archive.status == "RETIRED";
            // Decision 0028, point 9: a retired archive whose latest manifest
            // is lost, as the operator declares where its device is gone as
            // well, is no possible owner of the root: matching could never
            // recognise its media without that manifest, and it would stop
            // every registration of a new folder or volume and every
            // relocation for good. An index that a rebuild repairs still
            // stops them, as for any archive: its manifest is not lost.
            if (lost_by_a_retired_archive(unusable)) {
                continue;
            }
            pool.unusable.push_back(CandidatesFailure{plain_message(unusable), unusable, archive.label});
            continue;
        }
        ArchiveCandidate candidate;
        candidate.archive_uuid = archive.archive_uuid;
        candidate.retired = archive.status == "RETIRED";
        candidate.identity = header->identity;
        candidate.root_subpath = header->root_subpath;
        pool.candidates.push_back(std::move(candidate));
    }
    return pool;
}

std::expected<MatchOutcome, IoError> match_recorded(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const MediaEvidence& evidence, std::string_view root_subpath,
    std::span<ArchiveCandidate> candidates)
{
    const auto outcome = match_identity(evidence, root_subpath, candidates);
    if (!outcome.previous_carriers_consulted) {
        return outcome;
    }
    // Decision 0004: the carriers of every revision but the latest, each as
    // its manifest names it.
    for (auto& candidate : candidates) {
        const auto archive = store.find_archive(candidate.archive_uuid);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        const auto revisions = store.revisions(candidate.archive_uuid);
        if (!revisions) {
            return std::unexpected(revisions.error());
        }
        candidate.previous_carriers.clear();
        for (const auto& revision : *revisions) {
            if (*archive && revision.revision == (*archive)->current_revision) {
                continue;
            }
            const auto header = load_recorded_header(repository_files, layout, revision);
            if (!header) {
                // An earlier revision's: it has a later revision already.
                auto unusable = header.error();
                unusable.latest = false;
                return std::unexpected(IoError{IoErrorKind::Other, 0, plain_message(unusable)});
            }
            candidate.previous_carriers.push_back({header->identity, header->root_subpath});
        }
    }
    return match_identity(evidence, root_subpath, candidates);
}

bool plainly_another_archives_root(
    const MediaEvidence& evidence, std::string_view root_subpath, std::span<const ArchiveCandidate> candidates)
{
    return match_identity(evidence, root_subpath, candidates).result == MatchResult::Match;
}

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

std::expected<std::string, IoError> root_subpath_of(
    const std::filesystem::path& root, const VolumeInfo& volume)
{
    const auto relative = root.lexically_normal().lexically_relative(volume.mount_point.lexically_normal());
    if (relative.empty() || relative.native().starts_with(std::filesystem::path("..").native())) {
        return std::unexpected(IoError{IoErrorKind::Other, 0, "the root is not on the reported volume"});
    }
    if (relative == ".") {
        return std::string{};
    }
    const auto archive_path = archive_path_from_native(relative);
    if (!archive_path) {
        return std::unexpected(archive_path.error());
    }
    return archive_path->p;
}

namespace {

// Whether `path` is a volume's name by its GUID, "\\?\Volume{...}" ("Volume"
// in any case), without the separator that makes it the volume's root folder
// (decision 0029, point 3). Told from its text: the C++ library reads such a
// path as one below "\" of the current drive.
bool names_a_volume_by_its_guid(const std::filesystem::path& path)
{
    const auto text = path_to_utf8(path);
    constexpr std::string_view prefix = R"(\\?\)";
    constexpr std::string_view volume = "volume{";
    if (!text.starts_with(prefix) || !text.ends_with('}') || text.size() <= prefix.size() + volume.size()) {
        return false;
    }
    const auto name = std::string_view(text).substr(prefix.size());
    for (std::size_t index = 0; index < volume.size(); ++index) {
        const char character = name[index];
        const char lower = character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        if (lower != volume[index]) {
            return false;
        }
    }
    return name.find_first_of(R"(\/)") == std::string_view::npos;
}

} // namespace

std::filesystem::path without_trailing_separators(std::filesystem::path root)
{
    // A path that ends with a separator has an empty file name; one without a
    // relative part is a root ("D:\", "/"). Each parent_path() drops one
    // element, and the separators before it. A volume's GUID path is a root
    // too, the volume's root folder, as "D:\" is: it keeps one separator,
    // its own, without which it names the volume itself, which Windows does
    // not take for a folder.
    while (!root.has_filename() && root.has_relative_path()) {
        auto parent = root.parent_path();
        if (names_a_volume_by_its_guid(parent)) {
            return parent / "";
        }
        root = std::move(parent);
    }
    return root;
}

bool roots_overlap(std::string_view left, std::string_view right)
{
    const auto a = unicode_fold(left);
    const auto b = unicode_fold(right);
    return a.empty() || b.empty() || a == b || a.starts_with(b + "/") || b.starts_with(a + "/");
}

} // namespace va
