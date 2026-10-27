#include "core/manifest/manifest_reader.h"

#include "common/json_error.h"
#include "core/hash/hashers.h"

#include <algorithm>

namespace va {
namespace {

ManifestFailure io_failure(const IoError& error)
{
    return {ManifestFailure::Kind::Io, error.message, error.kind};
}

ManifestFailure malformed(std::string message)
{
    return {ManifestFailure::Kind::Malformed, std::move(message)};
}

// The entries were read from other bytes than the body validated (R-62).
ManifestFailure changed_while_read()
{
    return {ManifestFailure::Kind::BodyMismatch, "the manifest changed while it was read"};
}

} // namespace

std::expected<std::size_t, ManifestFailure> ManifestReader::fill()
{
    const auto count = source_->read(offset_, buffer_);
    if (!count) {
        return std::unexpected(io_failure(count.error()));
    }
    if (body_read_ && *count > 0) {
        const auto piece = std::span<const std::byte>(buffer_.data(), *count);
        body_read_->hash.update(piece);
        body_read_->length += *count;
        body_read_->lines += static_cast<std::uint64_t>(std::count(piece.begin(), piece.end(), std::byte{'\n'}));
    }
    offset_ += *count;
    buffer_start_ = 0;
    buffer_end_ = *count;
    return *count;
}

std::expected<bool, ManifestFailure> ManifestReader::read_line(std::string& line)
{
    line.clear();
    for (;;) {
        if (buffer_start_ == buffer_end_) {
            const auto count = fill();
            if (!count) {
                return std::unexpected(count.error());
            }
            if (*count == 0) {
                if (!line.empty()) {
                    return std::unexpected(malformed("manifest does not end with a newline"));
                }
                return false;
            }
        }
        const auto* begin = reinterpret_cast<const char*>(buffer_.data()) + buffer_start_;
        const auto* end = reinterpret_cast<const char*>(buffer_.data()) + buffer_end_;
        const auto* newline = std::find(begin, end, '\n');
        line.append(begin, newline);
        buffer_start_ += static_cast<std::size_t>(newline - begin);
        if (newline != end) {
            ++buffer_start_;
            return true;
        }
    }
}

std::expected<ManifestReader, ManifestFailure> ManifestReader::open(
    IFileSystemAccess& files, const std::filesystem::path& path, std::optional<std::string_view> recorded_header_blake3)
{
    ManifestReader reader;
    reader.buffer_.resize(1 << 20);
    auto source = files.open(path, ReadOptions{false, true});
    if (!source) {
        return std::unexpected(io_failure(source.error()));
    }
    reader.source_ = std::move(*source);

    auto has_header = reader.read_line(reader.header_line_);
    // Decision 0028: the bytes of line 1 before their structure. Whatever a
    // line 1 that is not the one recorded holds, the manifest has changed; a
    // read that failed is no answer either way.
    if (recorded_header_blake3 && (has_header || has_header.error().kind != ManifestFailure::Kind::Io) &&
        (!has_header || !*has_header || blake3_hex(reader.header_line_) != *recorded_header_blake3)) {
        return std::unexpected(
            ManifestFailure{ManifestFailure::Kind::HeaderMismatch, "the manifest differs from the repository record"});
    }
    if (!has_header) {
        return std::unexpected(has_header.error());
    }
    if (!*has_header) {
        return std::unexpected(malformed("empty manifest"));
    }
    try {
        reader.header_ = header_from_json(nlohmann::json::parse(reader.header_line_));
    } catch (const std::exception& error) {
        return std::unexpected(malformed("invalid manifest header: " + json_error_text(error)));
    }
    reader.header_blake3_ = blake3_hex(reader.header_line_);

    // R-62: hash everything after line 1 before trusting any entry.
    const std::uint64_t body_start = reader.header_line_.size() + 1;
    Blake3Hasher body_hash;
    std::uint64_t length = 0;
    std::uint64_t lines = 0;
    for (std::uint64_t offset = body_start;;) {
        const auto count = reader.source_->read(offset, reader.buffer_);
        if (!count) {
            return std::unexpected(io_failure(count.error()));
        }
        if (*count == 0) {
            break;
        }
        const auto piece = std::span(reader.buffer_.data(), *count);
        body_hash.update(piece);
        lines += static_cast<std::uint64_t>(
            std::count(piece.begin(), piece.end(), std::byte{'\n'}));
        length += *count;
        offset += *count;
    }
    if (length != reader.header_.body_length || lines != reader.header_.body_lines ||
        body_hash.finish_hex() != reader.header_.body_blake3) {
        return std::unexpected(ManifestFailure{ManifestFailure::Kind::BodyMismatch,
            "manifest body does not match its header"});
    }

    reader.offset_ = body_start;
    reader.buffer_start_ = reader.buffer_end_ = 0;
    // The entries are read again from here: what they come from is hashed too.
    reader.body_read_ = std::make_unique<BodyRead>();
    return reader;
}

std::expected<std::string, ManifestFailure> ManifestReader::read_header_line(
    IFileSystemAccess& files, const std::filesystem::path& path)
{
    ManifestReader reader;
    // A header is a few KiB: the first read usually holds all of it.
    reader.buffer_.resize(8 << 10);
    auto source = files.open(path, ReadOptions{false, true});
    if (!source) {
        return std::unexpected(io_failure(source.error()));
    }
    reader.source_ = std::move(*source);
    std::string line;
    const auto has_line = reader.read_line(line);
    if (!has_line) {
        return std::unexpected(has_line.error());
    }
    if (!*has_line) {
        return std::unexpected(malformed("empty manifest"));
    }
    return line;
}

std::expected<bool, ManifestFailure> ManifestReader::read_as_validated()
{
    while (body_read_) {
        const auto count = fill();
        if (!count) {
            return std::unexpected(count.error());
        }
        if (*count == 0) {
            body_read_differs_ = body_read_->length != header_.body_length ||
                                 body_read_->lines != header_.body_lines ||
                                 body_read_->hash.finish_hex() != header_.body_blake3;
            body_read_.reset();
        }
    }
    return !body_read_differs_;
}

std::unexpected<ManifestFailure> ManifestReader::failed(ManifestFailure failure)
{
    if (failure.kind == ManifestFailure::Kind::Malformed) {
        // Decision 0028: a line that does not parse, comes out of canonical
        // order or ends the file without its newline is a malformed manifest
        // only in the body validated; in a read that gave other bytes, the
        // manifest changed while it was read. Reading on to the end of the
        // body tells which. A read that fails there is no answer either way,
        // and is the failure.
        const auto validated = read_as_validated();
        if (!validated) {
            failure = validated.error();
        } else if (!*validated) {
            failure = changed_while_read();
        }
    }
    failure_ = failure;
    return std::unexpected(std::move(failure));
}

std::expected<std::optional<Entry>, ManifestFailure> ManifestReader::next()
{
    if (failure_) {
        return std::unexpected(*failure_);
    }
    std::string line;
    for (;;) {
        auto has_line = read_line(line);
        if (!has_line) {
            return failed(has_line.error());
        }
        if (!*has_line) {
            // The end of the body: the entries count only when what they were
            // read from is the body that open() validated (R-62).
            const auto validated = read_as_validated();
            if (!validated) {
                return failed(validated.error());
            }
            if (!*validated) {
                return failed(changed_while_read());
            }
            return std::optional<Entry>{};
        }
        Entry entry;
        try {
            const auto json = nlohmann::json::parse(line);
            if (json.at("t").get<std::string>() == "c") {
                continue; // V1.1 chunk lines; V1 verification reads whole files
            }
            entry = entry_from_json(json);
        } catch (const std::exception& error) {
            return failed(malformed("invalid manifest entry: " + json_error_text(error)));
        }
        if (previous_ && !canonical_less(*previous_, entry)) {
            return failed(malformed("manifest entries out of canonical order"));
        }
        previous_ = entry;
        return std::optional<Entry>(std::move(entry));
    }
}

} // namespace va
