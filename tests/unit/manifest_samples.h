#pragma once
// Sample manifests for tests (decision 0031): file content that differs in
// every chunk, file entries with the hashes registration gives them, and
// manifests assembled by hand from their lines, with a header that describes
// the body exactly, so that a test can break one rule at a time.
#include "core/hash/hashers.h"
#include "core/manifest/manifest.h"

#include <string>
#include <utility>
#include <vector>

namespace test {

inline std::string patterned(std::size_t size, unsigned seed)
{
    std::string content(size, '\0');
    for (std::size_t offset = 0; offset < size; ++offset) {
        content[offset] = static_cast<char>((offset * 131 + offset / 997 + seed * 7) & 0xFF);
    }
    return content;
}

// The chunk hashes of `content` straight from their definition: chunk n is
// its bytes from n * chunk_size; content no larger than one chunk has none
// of its own (R-31).
inline std::vector<std::string> chunk_hashes(const std::string& content, std::uint64_t chunk_size)
{
    std::vector<std::string> chunks;
    if (chunk_size == 0 || content.size() <= chunk_size) {
        return chunks;
    }
    for (std::size_t offset = 0; offset < content.size(); offset += chunk_size) {
        chunks.push_back(va::blake3_hex(content.substr(offset, chunk_size)));
    }
    return chunks;
}

// A file entry for `content` as registration records it with `chunk_size`.
inline va::Entry sample_file(std::string p, const std::string& content, std::uint64_t chunk_size)
{
    va::Entry entry;
    entry.path.p = std::move(p);
    entry.size = content.size();
    entry.mtime_ns = 1757912400123456700;
    entry.b3 = va::blake3_hex(content);
    entry.sha256 = va::sha256_hex(content);
    entry.chunks = chunk_hashes(content, chunk_size);
    return entry;
}

// A manifest's header and body lines (without their '\n').
struct ManifestParts {
    va::ManifestHeader header;
    std::vector<std::string> lines;
};

inline ManifestParts split_manifest(const std::string& bytes)
{
    ManifestParts parts;
    const auto newline = bytes.find('\n');
    parts.header = va::header_from_json(nlohmann::json::parse(bytes.substr(0, newline)));
    for (auto start = newline + 1; start < bytes.size();) {
        const auto end = bytes.find('\n', start);
        parts.lines.push_back(bytes.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

// The manifest of `parts`, with the header's body fields computed from the
// lines: nothing else in the header is changed.
inline std::string assemble_manifest(ManifestParts parts)
{
    std::string body;
    for (const auto& line : parts.lines) {
        body += line + '\n';
    }
    parts.header.body_blake3 = va::blake3_hex(body);
    parts.header.body_length = body.size();
    parts.header.body_lines = parts.lines.size();
    return va::header_to_json(parts.header) + '\n' + body;
}

} // namespace test
