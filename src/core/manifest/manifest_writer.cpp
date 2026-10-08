#include "core/manifest/manifest_writer.h"

#include <format>

namespace va {
namespace {

std::span<const std::byte> as_bytes(std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

} // namespace

std::filesystem::path manifest_path(
    const std::filesystem::path& manifests_root, const Uuid& archive_uuid, std::uint32_t revision)
{
    return manifests_root / archive_uuid / std::format("r{:04}.vam", revision);
}

ManifestWriter::ManifestWriter(
    IFileWriter& writer, IFileSystemAccess& reader, std::filesystem::path destination)
    : writer_(writer), reader_(reader), destination_(std::move(destination))
{
    body_path_ = destination_;
    body_path_ += ".body";
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
    if (previous_ && !canonical_less(*previous_, entry)) {
        return std::unexpected(IoError{IoErrorKind::Other, 0,
            "manifest entries out of canonical order at " + entry.path.p});
    }
    previous_ = entry;
    if (auto opened = open_body(); !opened) {
        return opened;
    }

    const auto line = entry_to_json(entry) + '\n';
    body_hash_.update(as_bytes(line));
    body_length_ += line.size();
    ++body_lines_;
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
    return body_->write(as_bytes(line));
}

std::expected<WrittenManifest, IoError> ManifestWriter::finish(ManifestHeader header)
{
    if (auto opened = open_body(); !opened) {
        return std::unexpected(opened.error());
    }
    // The body file is committed (made durable) so it can be read back and
    // copied after the header; it is removed once the manifest exists.
    if (auto committed = body_->commit(); !committed) {
        return std::unexpected(committed.error());
    }
    body_.reset();

    header.counts = counts_;
    header.body_blake3 = body_hash_.finish_hex();
    header.body_length = body_length_;
    header.body_lines = body_lines_;

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

    auto body = reader_.open(body_path_, ReadOptions{false, true});
    if (!body) {
        return std::unexpected(body.error());
    }
    std::vector<std::byte> buffer(1 << 20);
    std::uint64_t offset = 0;
    for (;;) {
        const auto count = (*body)->read(offset, buffer);
        if (!count) {
            return std::unexpected(count.error());
        }
        if (*count == 0) {
            break;
        }
        if (auto result = (*output)->write(std::span(buffer.data(), *count)); !result) {
            return std::unexpected(result.error());
        }
        offset += *count;
    }
    body->reset();
    if (offset != body_length_) {
        return std::unexpected(IoError{IoErrorKind::Other, 0, "manifest body changed while writing"});
    }
    if (auto committed = (*output)->commit(); !committed) {
        return std::unexpected(committed.error());
    }
    (void)writer_.remove(body_path_);
    return written;
}

} // namespace va
