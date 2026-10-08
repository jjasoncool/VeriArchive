#include "core/engine/transfer.h"

#include "common/canonical_json.h"
#include "common/utf8.h"
#include "core/engine/recorded_manifest.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"
#include "core/write_file.h"

#include <algorithm>

namespace va {
namespace {

std::unexpected<ImportFailure> fail(std::string failure_class, std::string message)
{
    return std::unexpected(ImportFailure{std::move(failure_class), std::move(message)});
}

} // namespace

std::string identity_file_json(const Uuid& archive_uuid, const std::string& label,
    std::uint32_t revision, const std::string& root_subpath, const std::string& created)
{
    return canonical_json::ObjectWriter{}
               .add_string("archive_uuid", archive_uuid)
               .add_string("label", label)
               .add_unsigned("revision", revision)
               .add_string("root_subpath", root_subpath)
               .add_string("created", created)
               .str() +
           '\n';
}

std::expected<void, IoError> copy_file(IFileSystemAccess& files, IFileWriter& writer,
    const std::filesystem::path& source, const std::filesystem::path& destination)
{
    auto input = files.open(source, ReadOptions{false, true});
    if (!input) {
        return std::unexpected(input.error());
    }
    auto output = writer.create(destination);
    if (!output) {
        return std::unexpected(output.error());
    }
    std::vector<std::byte> buffer(1 << 20);
    for (std::uint64_t offset = 0;;) {
        const auto count = (*input)->read(offset, buffer);
        if (!count) {
            return std::unexpected(count.error());
        }
        if (*count == 0) {
            break;
        }
        if (auto written = (*output)->write(std::span(buffer.data(), *count)); !written) {
            return written;
        }
        offset += *count;
    }
    return (*output)->commit();
}

std::expected<ExportResult, std::string> export_archive(ArchiveStore& store,
    IFileSystemAccess& repository_files, IFileWriter& writer, const RepositoryLayout& layout,
    const Uuid& archive_uuid, const std::filesystem::path& out_directory)
{
    const auto archive = store.find_archive(archive_uuid);
    if (!archive || !*archive) {
        return std::unexpected("no archive " + archive_uuid);
    }
    const auto revision = (*archive)->current_revision;
    const auto manifest = load_recorded_manifest(store, repository_files, layout, archive_uuid, revision);
    if (!manifest) {
        return std::unexpected(manifest.error().failure_class + ": " + manifest.error().message);
    }

    std::error_code error;
    std::filesystem::create_directories(out_directory, error);
    ExportResult result;
    const auto source = manifest_path(layout.manifests(), archive_uuid, revision);
    result.manifest = out_directory / source.filename();
    if (auto copied = copy_file(repository_files, writer, source, result.manifest); !copied) {
        return std::unexpected(copied.error().message);
    }

    result.identity = out_directory / "identity.json";
    const auto identity = identity_file_json(archive_uuid, (*archive)->label, revision,
        manifest->header.root_subpath, (*archive)->created);
    if (auto written = write_file_atomically(writer, result.identity, identity); !written) {
        return std::unexpected(written.error().message);
    }
    return result;
}

std::expected<ImportResult, ImportFailure> import_manifest(ArchiveStore& store,
    IFileSystemAccess& files, IFileWriter& writer, const RepositoryLayout& layout,
    const std::filesystem::path& manifest, bool force_rollback)
{
    auto reader = ManifestReader::open(files, manifest);
    if (!reader) {
        return fail(reader.error().kind == ManifestFailure::Kind::BodyMismatch ? "ManifestHashMismatch"
                                                                              : "RepositoryError",
            reader.error().message);
    }
    const auto header = reader->header();
    ImportResult result{header.archive_uuid, header.label, header.revision, false, false};

    const auto known = store.revisions(header.archive_uuid);
    if (!known) {
        return fail("RepositoryError", known.error().message);
    }
    const auto destination = manifest_path(layout.manifests(), header.archive_uuid, header.revision);

    // Already known: the bytes must be identical (R-87).
    const auto same = std::find_if(known->begin(), known->end(),
        [&](const RevisionRecord& record) { return record.revision == header.revision; });
    if (same != known->end()) {
        const auto incoming = read_whole_file(files, manifest);
        const auto stored = read_whole_file(files, destination);
        if (!incoming || !stored) {
            return fail("RepositoryError", "cannot compare with the stored manifest");
        }
        if (*incoming != *stored) {
            return fail("ManifestHashMismatch",
                "revision " + std::to_string(header.revision) + " is already known with different bytes");
        }
        result.already_present = true;
        return result;
    }

    // R-15: never go back to an older revision without an explicit override.
    std::uint32_t highest = 0;
    for (const auto& record : *known) {
        highest = std::max(highest, record.revision);
    }
    if (header.revision < highest && !force_rollback) {
        return fail("ManifestRevisionRollback",
            "revision " + std::to_string(header.revision) + " is older than the known revision " +
                std::to_string(highest) + "; pass --force-rollback to import it anyway");
    }

    std::vector<Entry> entries;
    for (;;) {
        auto next = reader->next();
        if (!next) {
            return fail("RepositoryError", next.error().message);
        }
        if (!*next) {
            break;
        }
        entries.push_back(std::move(**next));
    }

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (auto copied = copy_file(files, writer, manifest, destination); !copied) {
        return fail("RepositoryError", copied.error().message);
    }

    const auto recorded = [&]() -> std::expected<void, IoError> {
        if (auto begun = store.begin(); !begun) {
            return begun;
        }
        const auto existing = store.find_archive(header.archive_uuid);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        if (!*existing) {
            result.new_archive = true;
            ArchiveRecord archive{header.archive_uuid, header.label, header.created, header.revision,
                "REGISTERED", header.root_subpath, ""};
            if (auto inserted = store.insert_archive(archive); !inserted) {
                return inserted;
            }
        } else if (header.revision > (*existing)->current_revision) {
            auto archive = **existing;
            archive.current_revision = header.revision;
            archive.root_subpath = header.root_subpath;
            if (auto updated = store.update_archive(archive); !updated) {
                return updated;
            }
        }
        RevisionRecord revision{header.archive_uuid, header.revision,
            "manifests/" + header.archive_uuid + "/" + path_to_utf8(destination.filename()),
            reader->header_line(), reader->header_blake3(), header.body_blake3, path_to_utf8(manifest),
            header.created};
        if (auto inserted = store.insert_revision(revision); !inserted) {
            return inserted;
        }
        if (auto indexed = store.insert_file_rows(header.archive_uuid, header.revision, entries); !indexed) {
            return indexed;
        }
        return store.commit();
    }();
    if (!recorded) {
        store.rollback();
        return fail("RepositoryError", recorded.error().message);
    }
    return result;
}

} // namespace va
