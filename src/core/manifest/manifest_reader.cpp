#include "core/manifest/manifest_reader.h"

#include "common/hex.h"
#include "common/json_error.h"
#include "core/hash/hashers.h"

#include <algorithm>
#include <format>
#include <tuple>

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

std::uint64_t newlines_in(std::span<const std::byte> piece)
{
    return static_cast<std::uint64_t>(std::count(piece.begin(), piece.end(), std::byte{'\n'}));
}

// What the header says about chunk lines (decision 0031). Without a chunk size
// there is nothing to check: such a manifest is read as before.
std::expected<void, ManifestFailure> check_chunk_fields(const ManifestHeader& header)
{
    if (header.chunk_size == 0) {
        return {};
    }
    if (!valid_chunk_size(header.chunk_size)) {
        return std::unexpected(malformed("invalid manifest header: " + invalid_chunk_size(header.chunk_size)));
    }
    if (!is_hash_hex(header.merkle_root)) {
        return std::unexpected(
            malformed("invalid manifest header: merkle_root is missing or not 64 lowercase hex digits"));
    }
    if (header.counts.chunks > header.body_lines) {
        return std::unexpected(malformed("invalid manifest header: counts.chunks is larger than body.lines"));
    }
    return {};
}

// One line of the chunk lines: a chunk line in canonical form (R-64), or the
// reason it is not one.
std::expected<ChunkLine, ManifestFailure> parse_chunk_line(const std::string& line)
{
    try {
        const auto json = nlohmann::json::parse(line);
        if (json.at("t").get<std::string>() != "c") {
            return std::unexpected(malformed("manifest entry among the chunk lines"));
        }
        auto chunk = chunk_line_from_json(json);
        if (chunk_line_to_json(chunk) != line) {
            return std::unexpected(malformed("invalid chunk line: not canonical JSON"));
        }
        return chunk;
    } catch (const std::exception& error) {
        return std::unexpected(malformed("invalid chunk line: " + json_error_text(error)));
    }
}

} // namespace

std::expected<std::size_t, ManifestFailure> ManifestReader::fill(LineCursor& cursor)
{
    const auto want = std::min<std::uint64_t>(cursor.buffer.size(), cursor.limit - cursor.offset);
    const auto count = want == 0 ? std::expected<std::size_t, IoError>{0}
                                 : source_->read(cursor.offset,
                                       std::span(cursor.buffer.data(), static_cast<std::size_t>(want)));
    if (!count) {
        return std::unexpected(io_failure(count.error()));
    }
    if (cursor.read && *count > 0) {
        const auto piece = std::span<const std::byte>(cursor.buffer.data(), *count);
        cursor.read->hash.update(piece);
        cursor.read->length += *count;
        cursor.read->lines += newlines_in(piece);
    }
    cursor.offset += *count;
    cursor.start = 0;
    cursor.end = *count;
    return *count;
}

std::expected<bool, ManifestFailure> ManifestReader::read_line(LineCursor& cursor, std::string& line)
{
    line.clear();
    for (;;) {
        if (cursor.start == cursor.end) {
            const auto count = fill(cursor);
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
        const auto* begin = reinterpret_cast<const char*>(cursor.buffer.data()) + cursor.start;
        const auto* end = reinterpret_cast<const char*>(cursor.buffer.data()) + cursor.end;
        const auto* newline = std::find(begin, end, '\n');
        line.append(begin, newline);
        cursor.start += static_cast<std::size_t>(newline - begin);
        if (newline != end) {
            ++cursor.start;
            return true;
        }
    }
}

std::expected<ManifestReader, ManifestFailure> ManifestReader::open(
    IFileSystemAccess& files, const std::filesystem::path& path, std::optional<std::string_view> recorded_header_blake3)
{
    ManifestReader reader;
    reader.entries_.buffer.resize(1 << 20);
    auto source = files.open(path, ReadOptions{false, true});
    if (!source) {
        return std::unexpected(io_failure(source.error()));
    }
    reader.source_ = std::move(*source);

    auto has_header = reader.read_line(reader.entries_, reader.header_line_);
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
    // A header that breaks the rules of chunk lines fails the first next(),
    // not open(): the hashes come first (R-14), so that a caller comparing
    // the header with the one it recorded tells a changed header as a changed
    // manifest (ManifestHashMismatch), not as a malformed one.
    if (auto checked = check_chunk_fields(reader.header_); !checked) {
        reader.failed_ = checked.error();
    }

    // R-62: hash everything after line 1 before trusting any entry. On the
    // way, find where the chunk lines start: they are the last counts.chunks
    // lines of the body (§B4.4); the entries and the chunk lines, which the
    // two positions read, are hashed each on their own as well.
    const bool chunked = reader.header_.chunk_size > 0 && !reader.failed_;
    const std::uint64_t body_start = reader.header_line_.size() + 1;
    const std::uint64_t entry_lines = chunked ? reader.header_.body_lines - reader.header_.counts.chunks : 0;
    std::optional<std::uint64_t> chunks_start;
    if (chunked && entry_lines == 0) {
        chunks_start = body_start;
    }
    auto& buffer = reader.entries_.buffer;
    Blake3Hasher body_hash;
    BodyRead entries;
    BodyRead chunk_lines;
    const auto add = [](BodyRead& stretch, std::span<const std::byte> piece) {
        stretch.hash.update(piece);
        stretch.length += piece.size();
        stretch.lines += newlines_in(piece);
    };
    std::uint64_t length = 0;
    std::uint64_t lines = 0;
    for (std::uint64_t offset = body_start;;) {
        const auto count = reader.source_->read(offset, buffer);
        if (!count) {
            return std::unexpected(io_failure(count.error()));
        }
        if (*count == 0) {
            break;
        }
        const auto piece = std::span<const std::byte>(buffer.data(), *count);
        body_hash.update(piece);
        const auto newlines = newlines_in(piece);
        if (chunked && !chunks_start && lines + newlines >= entry_lines) {
            // The last entry line ends in this piece.
            auto remaining = entry_lines - lines;
            for (std::size_t index = 0; index < piece.size(); ++index) {
                if (piece[index] == std::byte{'\n'} && --remaining == 0) {
                    chunks_start = offset + index + 1;
                    break;
                }
            }
        }
        if (chunked) {
            const auto split = !chunks_start ? piece.size()
                               : *chunks_start <= offset ? std::size_t{0}
                                                         : static_cast<std::size_t>(*chunks_start - offset);
            add(entries, piece.first(split));
            add(chunk_lines, piece.subspan(split));
        }
        lines += newlines;
        length += *count;
        offset += *count;
    }
    if (length != reader.header_.body_length || lines != reader.header_.body_lines ||
        body_hash.finish_hex() != reader.header_.body_blake3) {
        return std::unexpected(ManifestFailure{ManifestFailure::Kind::BodyMismatch,
            "manifest body does not match its header"});
    }

    reader.entries_.offset = body_start;
    reader.entries_.start = reader.entries_.end = 0;
    if (chunked) {
        // The body has body.lines lines, so the last entry line was found.
        reader.entries_.limit = *chunks_start;
        reader.entry_lines_ = {entries.length, entries.lines, entries.hash.finish_hex()};
        reader.chunks_.buffer.resize(1 << 20);
        reader.chunks_.offset = *chunks_start;
        reader.chunk_lines_ = {chunk_lines.length, chunk_lines.lines, chunk_lines.hash.finish_hex()};
        reader.chunks_.read = std::make_unique<BodyRead>();
    } else {
        reader.entry_lines_ = {reader.header_.body_length, reader.header_.body_lines, reader.header_.body_blake3};
    }
    // The entries are read again from here: what they come from is hashed too.
    reader.entries_.read = std::make_unique<BodyRead>();
    return reader;
}

std::expected<std::string, ManifestFailure> ManifestReader::read_header_line(
    IFileSystemAccess& files, const std::filesystem::path& path)
{
    ManifestReader reader;
    // A header is a few KiB: the first read usually holds all of it.
    reader.entries_.buffer.resize(8 << 10);
    auto source = files.open(path, ReadOptions{false, true});
    if (!source) {
        return std::unexpected(io_failure(source.error()));
    }
    reader.source_ = std::move(*source);
    std::string line;
    const auto has_line = reader.read_line(reader.entries_, line);
    if (!has_line) {
        return std::unexpected(has_line.error());
    }
    if (!*has_line) {
        return std::unexpected(malformed("empty manifest"));
    }
    return line;
}

std::expected<bool, ManifestFailure> ManifestReader::read_as_validated(LineCursor& cursor, const Stretch& validated)
{
    while (cursor.read) {
        const auto count = fill(cursor);
        if (!count) {
            return std::unexpected(count.error());
        }
        if (*count == 0) {
            cursor.differs = cursor.read->length != validated.length || cursor.read->lines != validated.lines ||
                             cursor.read->hash.finish_hex() != validated.blake3;
            cursor.read.reset();
        }
    }
    return !cursor.differs;
}

std::expected<bool, ManifestFailure> ManifestReader::read_as_validated()
{
    const auto entries = read_as_validated(entries_, entry_lines_);
    if (!entries || !*entries) {
        return entries;
    }
    return read_as_validated(chunks_, chunk_lines_);
}

std::unexpected<ManifestFailure> ManifestReader::failed(ManifestFailure failure)
{
    if (failure.kind == ManifestFailure::Kind::Malformed) {
        // Decision 0028: a line that does not parse, comes out of canonical
        // order or ends the file without its newline is a malformed manifest
        // only in the body validated; in a read that gave other bytes, the
        // manifest changed while it was read. Reading on to the end of the
        // body, with both positions, tells which. A read that fails there is
        // no answer either way, and is the failure.
        const auto validated = read_as_validated();
        if (!validated) {
            failure = validated.error();
        } else if (!*validated) {
            failure = changed_while_read();
        }
    }
    failed_ = failure;
    return std::unexpected(std::move(failure));
}

std::expected<std::optional<Entry>, ManifestFailure> ManifestReader::next()
{
    if (failed_) {
        return std::unexpected(*failed_);
    }
    auto entry = next_entry();
    if (!entry) {
        return failed(entry.error());
    }
    return entry;
}

std::expected<std::optional<Entry>, ManifestFailure> ManifestReader::next_entry()
{
    if (finished_) {
        return std::optional<Entry>{};
    }
    const auto chunk_size = header_.chunk_size;
    std::string line;
    for (;;) {
        auto has_line = read_line(entries_, line);
        if (!has_line) {
            return std::unexpected(has_line.error());
        }
        if (!*has_line) {
            // The end of the entries: they count only when what they were
            // read from is what open() validated (R-62).
            const auto validated = read_as_validated(entries_, entry_lines_);
            if (!validated) {
                return std::unexpected(validated.error());
            }
            if (!*validated) {
                return std::unexpected(changed_while_read());
            }
            if (chunk_size > 0) {
                if (auto ended = check_end(); !ended) {
                    return std::unexpected(ended.error());
                }
            }
            finished_ = true;
            return std::optional<Entry>{};
        }
        Entry entry;
        try {
            const auto json = nlohmann::json::parse(line);
            if (json.at("t").get<std::string>() == "c") {
                if (chunk_size == 0) {
                    continue; // without a chunk size, chunk lines are skipped as before
                }
                return std::unexpected(malformed("chunk line among the manifest entries"));
            }
            entry = entry_from_json(json);
        } catch (const std::exception& error) {
            return std::unexpected(malformed("invalid manifest entry: " + json_error_text(error)));
        }
        if (previous_ && !canonical_less(*previous_, entry)) {
            return std::unexpected(malformed("manifest entries out of canonical order"));
        }
        previous_ = entry;
        if (chunk_size > 0) {
            if (has_chunk_lines(entry, chunk_size)) {
                if (auto attached = attach_chunks(entry); !attached) {
                    return std::unexpected(attached.error());
                }
            }
            if (!add_merkle_leaves(merkle_, entry, chunk_size)) {
                return std::unexpected(malformed("invalid manifest entry: b3 is not 64 lowercase hex digits"));
            }
        }
        return std::optional<Entry>(std::move(entry));
    }
}

// A file with chunk lines owns the next ones, chunk 0 first, each where the
// file's size and the chunk size put it (decision 0031): the chunk lines name
// their file only by p, which two files can share (R-41).
std::expected<void, ManifestFailure> ManifestReader::attach_chunks(Entry& file)
{
    const auto chunk_size = header_.chunk_size;
    const auto count = chunk_line_count(*file.size, chunk_size);
    std::string line;
    for (std::uint64_t idx = 0; idx < count; ++idx) {
        const auto out_of_place = [&] {
            return std::unexpected(
                malformed(std::format("chunk {} of {} is missing or does not match the file", idx, file.path.p)));
        };
        auto has_line = read_line(chunks_, line);
        if (!has_line) {
            return std::unexpected(has_line.error());
        }
        if (!*has_line) {
            return out_of_place();
        }
        auto chunk = parse_chunk_line(line);
        if (!chunk) {
            return std::unexpected(chunk.error());
        }
        const auto expected = chunk_of(file.path.p, *file.size, idx, chunk_size);
        if (std::tie(chunk->p, chunk->idx, chunk->offset, chunk->length) !=
            std::tie(expected.p, expected.idx, expected.offset, expected.length)) {
            return out_of_place();
        }
        file.chunks.push_back(std::move(chunk->b3));
    }
    return {};
}

// After the last entry: every chunk line has found its file, what the second
// position read is the chunk lines that open() validated, and the content
// hashes give the header's Merkle root (§B6).
std::expected<void, ManifestFailure> ManifestReader::check_end()
{
    std::string line;
    auto has_line = read_line(chunks_, line);
    if (!has_line) {
        return std::unexpected(has_line.error());
    }
    if (*has_line) {
        auto chunk = parse_chunk_line(line);
        if (!chunk) {
            return std::unexpected(chunk.error());
        }
        return std::unexpected(malformed(std::format("chunk {} of {} belongs to no file", chunk->idx, chunk->p)));
    }
    const auto validated = read_as_validated(chunks_, chunk_lines_);
    if (!validated) {
        return std::unexpected(validated.error());
    }
    if (!*validated) {
        return std::unexpected(changed_while_read());
    }
    if (merkle_.root_hex() != header_.merkle_root) {
        return std::unexpected(ManifestFailure{ManifestFailure::Kind::MerkleMismatch,
            "manifest content hashes do not match its merkle_root"});
    }
    return {};
}

} // namespace va
