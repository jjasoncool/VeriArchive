#include "core/engine/transfer.h"

#include "common/canonical_json.h"
#include "common/utf8.h"
#include "core/engine/recorded_manifest.h"
#include "core/hash/hashers.h"
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

std::unexpected<ImportFailure> refuse(std::string message)
{
    return std::unexpected(ImportFailure{"", std::move(message), ImportFailure::Kind::Refused});
}

// A read of the source that gives other bytes than the one validated: its
// entries, or the copy (decision 0030).
constexpr const char* changed_while_imported = "the manifest changed while it was imported; nothing was imported";

// R-15: the highest revision of the archive that the repository knows.
std::uint32_t highest_revision(const std::vector<RevisionRecord>& known)
{
    std::uint32_t highest = 0;
    for (const auto& record : known) {
        highest = std::max(highest, record.revision);
    }
    return highest;
}

std::string rollback_message(std::uint32_t revision, std::uint32_t highest)
{
    return "revision " + std::to_string(revision) + " is older than the known revision " +
           std::to_string(highest) + "; pass --force-rollback to import it anyway";
}

// The manifest's bytes under a temporary name beside `destination`, flushed to
// stable storage and not at its name yet: the caller names it in the
// transaction that records its revision (decision 0030). They are the bytes
// that were validated: line 1 is the header line read before, and the rest has
// the header's body BLAKE3 and length. A file that changed since, or a second
// read of removable media that differs, is not staged.
std::expected<std::unique_ptr<IOutputFile>, ImportFailure> stage_manifest(IFileSystemAccess& files,
    IFileWriter& writer, const std::filesystem::path& source, const std::filesystem::path& destination,
    const std::string& header_line, const ManifestHeader& header)
{
    auto input = files.open(source, ReadOptions{false, true});
    if (!input) {
        return fail("RepositoryError", input.error().message);
    }
    auto output = writer.create(destination);
    if (!output) {
        return fail("RepositoryError", output.error().message);
    }
    const std::uint64_t body_start = header_line.size() + 1;
    std::string first_line; // line 1 and its '\n', as copied
    Blake3Hasher body_hash;
    std::uint64_t body_length = 0;
    std::vector<std::byte> buffer(1 << 20);
    for (std::uint64_t offset = 0;;) {
        const auto count = (*input)->read(offset, buffer);
        if (!count) {
            return fail("RepositoryError", count.error().message);
        }
        if (*count == 0) {
            break;
        }
        auto piece = std::span<const std::byte>(buffer.data(), *count);
        if (offset < body_start) {
            const auto head = static_cast<std::size_t>(std::min<std::uint64_t>(piece.size(), body_start - offset));
            first_line.append(reinterpret_cast<const char*>(piece.data()), head);
            piece = piece.subspan(head);
        }
        body_hash.update(piece);
        body_length += piece.size();
        if (auto written = (*output)->write(std::span(buffer.data(), *count)); !written) {
            return fail("RepositoryError", written.error().message);
        }
        offset += *count;
    }
    if (first_line != header_line + '\n' || body_length != header.body_length ||
        body_hash.finish_hex() != header.body_blake3) {
        return fail("ManifestHashMismatch", changed_while_imported);
    }
    if (auto flushed = (*output)->flush(); !flushed) {
        return fail("RepositoryError", flushed.error().message);
    }
    return std::move(*output);
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
    const auto& uuid = header.archive_uuid;
    ImportResult result{uuid, header.label, header.revision, false, false};

    const auto known = store.revisions(uuid);
    if (!known) {
        return fail("RepositoryError", known.error().message);
    }
    const auto destination = manifest_path(layout.manifests(), uuid, header.revision);

    // Already known: the bytes must be identical (R-87). A recorded manifest
    // is never replaced (decision 0030), so it is read without a lock.
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
    // Checked again in the record's transaction; here before anything is copied.
    if (const auto highest = highest_revision(*known); header.revision < highest && !force_rollback) {
        return fail("ManifestRevisionRollback", rollback_message(header.revision, highest));
    }

    // The index is made of these entries, read from the source a second time:
    // they are used only when what they were read from is the body validated,
    // which the reader checks at the end of the body, as the copy below is
    // checked (decision 0030).
    std::vector<Entry> entries;
    for (;;) {
        auto next = reader->next();
        if (!next) {
            if (next.error().kind == ManifestFailure::Kind::BodyMismatch) {
                return fail("ManifestHashMismatch", changed_while_imported);
            }
            return fail("RepositoryError", next.error().message);
        }
        if (!*next) {
            break;
        }
        entries.push_back(std::move(**next));
    }

    // Copied and flushed without a lock, which takes a while for a large
    // manifest; not at its name yet.
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    auto staged = stage_manifest(files, writer, manifest, destination, reader->header_line(), header);
    if (!staged) {
        return std::unexpected(staged.error());
    }

    // One step, as a registration's and a relocation's record (decisions 0023,
    // 0030): in one write transaction the revision is found not recorded, the
    // records are written, and the manifest is renamed to its name last. No
    // other operation can record the revision or rename a manifest there
    // meanwhile, so a recorded manifest is never replaced, and the revision is
    // recorded with the bytes this import validated under its name.
    bool lost = false;                      // another operation recorded the revision first
    std::optional<std::uint32_t> newer;     // a higher revision was recorded meanwhile (R-15)
    std::optional<std::string> registering; // the root of a registration that has saved files of the revision
    const auto recorded = [&]() -> std::expected<void, IoError> {
        // Another operation's record of a large archive holds the write lock
        // for seconds: this one waits for it, and then finds the revision
        // recorded rather than the database locked. An import cannot be
        // cancelled while it waits (Ctrl+C ends the process).
        const CancelToken not_cancelled;
        if (auto begun = store.begin_waiting(record_lock_wait, not_cancelled); !begun) {
            // Locked for longer still: whoever held the lock may have
            // recorded the revision.
            if (const auto taken = store.revision_recorded(uuid, header.revision); taken && *taken) {
                lost = true;
            }
            return begun;
        }
        const auto taken = store.revision_recorded(uuid, header.revision);
        if (!taken) {
            return std::unexpected(taken.error());
        }
        if (*taken) {
            lost = true;
            return std::unexpected(IoError{IoErrorKind::Other, 0, "the revision is recorded"});
        }
        // R-15 as it holds now: a higher revision may have been recorded
        // since the check before the copy.
        if (!force_rollback) {
            const auto now_known = store.revisions(uuid);
            if (!now_known) {
                return std::unexpected(now_known.error());
            }
            if (const auto highest = highest_revision(*now_known); header.revision < highest) {
                newer = highest;
                return std::unexpected(IoError{IoErrorKind::Other, 0, "a newer revision is recorded"});
            }
        }
        // An unfinished registration creating this revision checkpoints the
        // files it hashed under it (R-104). Recorded over them, they would
        // fail the index's insert on their keys and lose the hashing done: the
        // import leaves the revision to the registration, as a relocation
        // does (decision 0023). Rows that no plan names are no registration's
        // saved progress (decision 0026): left by a crash, they go.
        const auto checkpointed = store.has_file_rows(uuid, header.revision);
        if (!checkpointed) {
            return std::unexpected(checkpointed.error());
        }
        if (*checkpointed) {
            const auto plan = store.registration_plan(uuid, header.revision);
            if (!plan) {
                return std::unexpected(plan.error());
            }
            if (*plan) {
                registering = **plan;
                return std::unexpected(IoError{IoErrorKind::Other, 0, "a registration creates the revision"});
            }
            if (auto cleared = store.delete_file_rows(uuid, header.revision); !cleared) {
                return cleared;
            }
        }
        // A registration that has saved nothing of this revision, and a
        // relocation that creates it, can never finish once it is recorded:
        // the registration's plan, and the relocation's record and saved
        // progress, go with this record, as with a registration's or a
        // relocation's own (decisions 0023, 0026).
        if (auto removed = store.remove_registration_plan(uuid, header.revision); !removed) {
            return removed;
        }
        if (auto removed = store.remove_relocation_creating(uuid, header.revision); !removed) {
            return removed;
        }
        const auto existing = store.find_archive(uuid);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        if (!*existing) {
            result.new_archive = true;
            ArchiveRecord archive{uuid, header.label, header.created, header.revision, "REGISTERED",
                header.root_subpath, ""};
            if (auto inserted = store.insert_archive(archive); !inserted) {
                return inserted;
            }
        } else if (header.revision > (*existing)->current_revision) {
            auto archive = **existing;
            archive.current_revision = header.revision;
            archive.root_subpath = header.root_subpath;
            // An archive whose first registration was unfinished has a
            // revision now.
            if (archive.status == "REGISTRATION_INCOMPLETE") {
                archive.status = "REGISTERED";
            }
            if (auto updated = store.update_archive(archive); !updated) {
                return updated;
            }
        }
        RevisionRecord revision{uuid, header.revision,
            "manifests/" + uuid + "/" + path_to_utf8(destination.filename()), reader->header_line(),
            reader->header_blake3(), header.body_blake3, path_to_utf8(manifest), header.created};
        if (auto inserted = store.insert_revision(revision); !inserted) {
            return inserted;
        }
        if (auto indexed = store.insert_file_rows(uuid, header.revision, entries); !indexed) {
            return indexed;
        }
        // Named last. V1.1 stages the signature file (rNNNN.vam.sig, R-63)
        // the same way and names it here too, so neither ever replaces a
        // recorded revision's file.
        if (auto named = (*staged)->commit(); !named) {
            return named;
        }
        return store.commit();
    }();
    if (!recorded) {
        store.rollback(); // the staged copy goes with `staged`
        if (lost) {
            return refuse("revision " + std::to_string(header.revision) + " of archive " + uuid +
                          " was recorded by another operation while this import ran; nothing was imported "
                          "(import it again to compare it with the recorded manifest)");
        }
        if (newer) {
            return fail("ManifestRevisionRollback", rollback_message(header.revision, *newer));
        }
        if (registering) {
            return refuse("an unfinished registration of archive " + uuid + " (root \"" + *registering +
                          "\") is creating revision " + std::to_string(header.revision) +
                          "; nothing was imported (finish that registration with `register --resume`, or abandon "
                          "it with `archive abandon-registration " +
                          uuid + "` and import again)");
        }
        return fail("RepositoryError", recorded.error().message);
    }
    return result;
}

} // namespace va
