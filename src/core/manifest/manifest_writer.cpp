#include "core/manifest/manifest_writer.h"

#include "common/uuid.h"

#include <format>
#include <functional>

namespace va {
namespace {

std::span<const std::byte> as_bytes(std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

IoError writer_error(std::string message)
{
    return IoError{IoErrorKind::Other, 0, std::move(message)};
}

// Reads a side file this writer wrote and gives its bytes to `sink`; returns
// how many there were.
std::expected<std::uint64_t, IoError> read_side_file(IFileSystemAccess& files, const std::filesystem::path& path,
    const std::function<std::expected<void, IoError>(std::span<const std::byte>)>& sink)
{
    auto source = files.open(path, ReadOptions{false, true});
    if (!source) {
        return std::unexpected(source.error());
    }
    std::vector<std::byte> buffer(1 << 20);
    std::uint64_t offset = 0;
    for (;;) {
        const auto count = (*source)->read(offset, buffer);
        if (!count) {
            return std::unexpected(count.error());
        }
        if (*count == 0) {
            return offset;
        }
        if (auto taken = sink(std::span(buffer.data(), *count)); !taken) {
            return std::unexpected(taken.error());
        }
        offset += *count;
    }
}

} // namespace

std::filesystem::path manifest_path(
    const std::filesystem::path& manifests_root, const Uuid& archive_uuid, std::uint32_t revision)
{
    return manifests_root / archive_uuid / std::format("r{:04}.vam", revision);
}

ManifestWriter::ManifestWriter(IFileWriter& writer, IFileSystemAccess& reader, std::filesystem::path destination,
    std::uint64_t chunk_size)
    : writer_(writer), reader_(reader), destination_(std::move(destination)), chunk_size_(chunk_size)
{
    // Names of this writer's own: attempts that write the same revision at
    // once never read each other's side files (decision 0023).
    const auto own = generate_uuid();
    body_path_ = destination_;
    body_path_ += ".body-" + own;
    chunks_path_ = destination_;
    chunks_path_ += ".chunks-" + own;
}

std::expected<void, IoError> ManifestWriter::open_body()
{
    if (body_) {
        return {};
    }
    auto body = writer_.create(body_path_);
    if (!body) {
        return std::unexpected(body.error());
    }
    body_ = std::move(*body);
    return {};
}

std::expected<void, IoError> ManifestWriter::add(const Entry& entry)
{
    if (!valid_chunk_size(chunk_size_)) {
        return std::unexpected(writer_error(invalid_chunk_size(chunk_size_)));
    }
    if (previous_ && !canonical_less(*previous_, entry)) {
        return std::unexpected(writer_error("manifest entries out of canonical order at " + entry.path.p));
    }
    // Chunk hashes for exactly the files that have chunk lines, all of them.
    const bool chunked = has_chunk_lines(entry, chunk_size_);
    const auto expected_chunks = chunked ? chunk_line_count(*entry.size, chunk_size_) : 0;
    if (entry.chunks.size() != expected_chunks) {
        return std::unexpected(writer_error(
            std::format("chunk hashes of {}: {} given, {} expected", entry.path.p, entry.chunks.size(),
                expected_chunks)));
    }
    if (chunk_size_ > 0 && !add_merkle_leaves(merkle_, entry, chunk_size_)) {
        return std::unexpected(writer_error("b3 is not 64 lowercase hex digits"));
    }
    previous_ = entry;
    if (auto opened = open_body(); !opened) {
        return opened;
    }

    const auto line = entry_to_json(entry) + '\n';
    body_hash_.update(as_bytes(line));
    entries_length_ += line.size();
    ++entry_lines_;
    switch (entry.type) {
    case 'f':
        ++counts_.files;
        break;
    case 'd':
        ++counts_.dirs;
        break;
    case 'l':
        ++counts_.links;
        break;
    case 's':
        ++counts_.streams;
        break;
    default:
        break;
    }
    if ((entry.type == 'f' || entry.type == 's') && !entry.b3.empty() && entry.size) {
        counts_.bytes += *entry.size;
    }
    if (auto written = body_->write(as_bytes(line)); !written) {
        return written;
    }
    return chunked ? add_chunk_lines(entry) : std::expected<void, IoError>{};
}

// The chunk lines follow all entries in the body (§B4.4), so they wait in a
// side file of their own until finish().
std::expected<void, IoError> ManifestWriter::add_chunk_lines(const Entry& file)
{
    if (!chunks_) {
        auto created = writer_.create(chunks_path_);
        if (!created) {
            return std::unexpected(created.error());
        }
        chunks_ = std::move(*created);
    }
    std::string lines;
    for (std::uint64_t idx = 0; idx < file.chunks.size(); ++idx) {
        auto chunk = chunk_of(file.path.p, *file.size, idx, chunk_size_);
        chunk.b3 = file.chunks[idx];
        lines += chunk_line_to_json(chunk) + '\n';
    }
    chunks_length_ += lines.size();
    counts_.chunks += file.chunks.size();
    return chunks_->write(as_bytes(lines));
}

std::expected<WrittenManifest, IoError> ManifestWriter::finish(ManifestHeader header)
{
    if (!valid_chunk_size(chunk_size_)) {
        return std::unexpected(writer_error(invalid_chunk_size(chunk_size_)));
    }
    if (auto opened = open_body(); !opened) {
        return std::unexpected(opened.error());
    }
    // The side files are committed so they can be read back and copied after
    // the header; they are removed when finish() returns, also after a
    // failure.
    const bool chunked = chunks_ != nullptr;
    struct RemoveSideFiles {
        IFileWriter& writer;
        const std::filesystem::path& body;
        const std::filesystem::path* chunks;
        ~RemoveSideFiles()
        {
            (void)writer.remove(body);
            if (chunks) {
                (void)writer.remove(*chunks);
            }
        }
    };
    auto body_committed = body_->commit();
    body_.reset();
    auto chunks_committed = chunked ? chunks_->commit() : std::expected<void, IoError>{};
    chunks_.reset();
    RemoveSideFiles remove_side_files{writer_, body_path_, chunked ? &chunks_path_ : nullptr};
    if (!body_committed) {
        return std::unexpected(body_committed.error());
    }
    if (!chunks_committed) {
        return std::unexpected(chunks_committed.error());
    }

    // The body hash covers the entry lines, hashed as they came, and then the
    // chunk lines.
    if (chunked) {
        const auto hashed = read_side_file(reader_, chunks_path_, [&](std::span<const std::byte> bytes) {
            body_hash_.update(bytes);
            return std::expected<void, IoError>{};
        });
        if (!hashed) {
            return std::unexpected(hashed.error());
        }
        if (*hashed != chunks_length_) {
            return std::unexpected(writer_error("manifest body changed while writing"));
        }
    }

    header.chunk_size = chunk_size_;
    header.counts = counts_;
    header.merkle_root = chunk_size_ > 0 ? merkle_.root_hex() : std::string{};
    header.body_blake3 = body_hash_.finish_hex();
    header.body_length = entries_length_ + chunks_length_;
    header.body_lines = entry_lines_ + counts_.chunks;

    WrittenManifest written;
    written.header_line = header_to_json(header);
    written.header_blake3 = blake3_hex(written.header_line);
    written.header = std::move(header);

    auto output = writer_.create(destination_);
    if (!output) {
        return std::unexpected(output.error());
    }
    if (auto result = (*output)->write(as_bytes(written.header_line + '\n')); !result) {
        return std::unexpected(result.error());
    }
    const auto copy = [&](std::span<const std::byte> bytes) { return (*output)->write(bytes); };
    const auto entries = read_side_file(reader_, body_path_, copy);
    if (!entries) {
        return std::unexpected(entries.error());
    }
    const auto chunk_lines =
        chunked ? read_side_file(reader_, chunks_path_, copy) : std::expected<std::uint64_t, IoError>{0};
    if (!chunk_lines) {
        return std::unexpected(chunk_lines.error());
    }
    if (*entries != entries_length_ || *chunk_lines != chunks_length_) {
        return std::unexpected(writer_error("manifest body changed while writing"));
    }
    // Durable, and not at the destination yet: the caller commits it in the
    // transaction that records the revision (decision 0023).
    if (auto flushed = (*output)->flush(); !flushed) {
        return std::unexpected(flushed.error());
    }
    written.file = std::move(*output);
    return written;
}

} // namespace va
