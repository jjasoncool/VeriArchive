#include "core/manifest/manifest.h"

#include "common/canonical_json.h"
#include "common/hex.h"

#include <algorithm>
#include <format>

namespace va {
namespace {

using canonical_json::ObjectWriter;

std::string devices_to_json(const std::vector<PhysicalDeviceInfo>& devices)
{
    std::string text = "[";
    for (std::size_t index = 0; index < devices.size(); ++index) {
        if (index > 0) {
            text += ',';
        }
        const auto& device = devices[index];
        // Runtime-only fields (device_ref, rotational) are not identity (§B4.3).
        text += ObjectWriter{}
                    .add_string("model", device.model)
                    .add_string("serial", device.serial)
                    .add_string("wwn", device.wwn)
                    .add_unsigned("capacity_bytes", device.capacity_bytes)
                    .add_string("bus", device.bus)
                    .str();
    }
    return text + ']';
}

std::string identity_to_json(const ManifestHeader& header)
{
    const auto& volume = header.identity;
    return ObjectWriter{}
        .add_string("filesystem_type", volume.filesystem_type)
        .add_string("filesystem_uuid", volume.filesystem_uuid)
        .add_string("filesystem_uuid_short", volume.filesystem_uuid_short)
        .add_string("volume_label", volume.volume_label)
        .add_unsigned("capacity_bytes", volume.capacity_bytes)
        .add_string("root_subpath", header.root_subpath)
        .add_raw("devices", devices_to_json(volume.devices))
        .str();
}

std::string text_or(const nlohmann::json& json, const char* key)
{
    return json.contains(key) ? json.at(key).get<std::string>() : std::string{};
}

} // namespace

bool valid_chunk_size(std::uint64_t chunk_size)
{
    return chunk_size % chunk_size_unit == 0 && chunk_size <= max_chunk_size;
}

std::string invalid_chunk_size(std::uint64_t chunk_size)
{
    return std::format("chunk_size {} is not 0 or a multiple of {} up to {}", chunk_size, chunk_size_unit,
        max_chunk_size);
}

bool has_chunk_lines(const Entry& entry, std::uint64_t chunk_size)
{
    return chunk_size > 0 && entry.type == 'f' && !entry.b3.empty() && entry.size && *entry.size > chunk_size;
}

std::uint64_t chunk_line_count(std::uint64_t size, std::uint64_t chunk_size)
{
    return size / chunk_size + (size % chunk_size != 0 ? 1 : 0);
}

ChunkLine chunk_of(const std::string& p, std::uint64_t size, std::uint64_t idx, std::uint64_t chunk_size)
{
    const auto offset = idx * chunk_size;
    return {p, idx, offset, std::min(chunk_size, size - offset), {}};
}

bool add_merkle_leaves(MerkleTreeBuilder& tree, const Entry& entry, std::uint64_t chunk_size)
{
    // Also a b3 that gives no leaf: a file's with chunk lines, a directory's.
    if (!entry.b3.empty() && !is_hash_hex(entry.b3)) {
        return false;
    }
    if (has_chunk_lines(entry, chunk_size)) {
        if (!std::all_of(entry.chunks.begin(), entry.chunks.end(),
                [](const std::string& chunk) { return is_hash_hex(chunk); })) {
            return false;
        }
        for (const auto& chunk : entry.chunks) {
            tree.add_hex(chunk);
        }
        return true;
    }
    if (entry.type == 'd' || entry.b3.empty()) {
        return true; // no content hash, no leaf
    }
    return tree.add_hex(entry.b3);
}

// Key order is fixed by §B4.3 (copy_of: decision 0005).
std::string header_to_json(const ManifestHeader& header)
{
    ObjectWriter object;
    object.add_string("format", "vam")
        .add_integer("version", 1)
        .add_string("archive_uuid", header.archive_uuid)
        .add_string("label", header.label)
        .add_unsigned("revision", header.revision);
    if (header.supersedes) {
        object.add_unsigned("supersedes", *header.supersedes);
    }
    if (header.copy_of) {
        object.add_string("copy_of", *header.copy_of);
    }
    object.add_string("created", header.created)
        .add_raw("created_by", ObjectWriter{}
                                   .add_string("app", "veriarchive")
                                   .add_string("app_version", header.app_version)
                                   .add_string("os", header.os)
                                   .add_string("host", header.host)
                                   .str())
        .add_raw("identity", identity_to_json(header))
        .add_string("root_hint", header.root_hint)
        .add_raw("hash", ObjectWriter{}
                             .add_strings("file", {"blake3", "sha256"})
                             .add_string("chunk", "blake3")
                             .add_unsigned("chunk_size", header.chunk_size)
                             .str())
        .add_integer("exclusions_version", header.exclusions_version)
        .add_strings("exclusions", header.exclusions)
        .add_raw("counts", ObjectWriter{}
                               .add_unsigned("files", header.counts.files)
                               .add_unsigned("dirs", header.counts.dirs)
                               .add_unsigned("links", header.counts.links)
                               .add_unsigned("streams", header.counts.streams)
                               .add_unsigned("chunks", header.counts.chunks)
                               .add_unsigned("bytes", header.counts.bytes)
                               .str());
    // Omitted when the chunk size is 0 (§B4.3), so V1 headers keep their bytes.
    if (!header.merkle_root.empty()) {
        object.add_string("merkle_root", header.merkle_root);
    }
    object.add_raw("body", ObjectWriter{}
                               .add_string("blake3", header.body_blake3)
                               .add_unsigned("length", header.body_length)
                               .add_unsigned("lines", header.body_lines)
                               .str());
    return object.str();
}

// Key order is fixed by §B4.4 (unreadable: decision 0010). Empty optional
// values are omitted.
std::string entry_to_json(const Entry& entry)
{
    ObjectWriter object;
    object.add_string("t", std::string(1, entry.type)).add_string("p", entry.path.p);
    if (entry.size) {
        object.add_unsigned("size", *entry.size);
    }
    if (entry.mtime_ns) {
        object.add_integer("mtime_ns", *entry.mtime_ns);
    }
    if (!entry.b3.empty()) {
        object.add_string("b3", entry.b3);
    }
    if (!entry.sha256.empty()) {
        object.add_string("sha256", entry.sha256);
    }
    if (!entry.unreadable.empty()) {
        object.add_string("unreadable", entry.unreadable);
    }
    if (entry.mode) {
        object.add_unsigned("mode", *entry.mode);
    }
    if (entry.attrs) {
        object.add_unsigned("attrs", *entry.attrs);
    }
    if (!entry.path.enc.empty()) {
        object.add_string("enc", entry.path.enc).add_string("raw", entry.path.raw);
    }
    if (!entry.warn.empty()) {
        object.add_strings("warn", entry.warn);
    }
    if (!entry.stream.empty()) {
        object.add_string("stream", entry.stream);
    }
    return object.str();
}

// Key order is fixed by §B4.4. A chunk line names its file by p alone; the
// reader matches it to its file by position (decision 0031).
std::string chunk_line_to_json(const ChunkLine& chunk)
{
    return ObjectWriter{}
        .add_string("t", "c")
        .add_string("p", chunk.p)
        .add_unsigned("idx", chunk.idx)
        .add_unsigned("offset", chunk.offset)
        .add_unsigned("length", chunk.length)
        .add_string("b3", chunk.b3)
        .str();
}

ManifestHeader header_from_json(const nlohmann::json& json)
{
    if (json.at("format").get<std::string>() != "vam" || json.at("version").get<int>() != 1) {
        throw nlohmann::json::other_error::create(501, "not a version 1 .vam manifest", &json);
    }
    ManifestHeader header;
    header.archive_uuid = json.at("archive_uuid").get<std::string>();
    header.label = text_or(json, "label");
    header.revision = json.at("revision").get<std::uint32_t>();
    if (json.contains("supersedes")) {
        header.supersedes = json.at("supersedes").get<std::uint32_t>();
    }
    if (json.contains("copy_of")) {
        header.copy_of = json.at("copy_of").get<std::string>();
    }
    header.created = text_or(json, "created");
    const auto& created_by = json.at("created_by");
    header.app_version = text_or(created_by, "app_version");
    header.os = text_or(created_by, "os");
    header.host = text_or(created_by, "host");

    const auto& identity = json.at("identity");
    header.identity.filesystem_type = text_or(identity, "filesystem_type");
    header.identity.filesystem_uuid = text_or(identity, "filesystem_uuid");
    header.identity.filesystem_uuid_short = text_or(identity, "filesystem_uuid_short");
    header.identity.volume_label = text_or(identity, "volume_label");
    header.identity.capacity_bytes = identity.value("capacity_bytes", std::uint64_t{0});
    header.root_subpath = text_or(identity, "root_subpath");
    for (const auto& item : identity.at("devices")) {
        PhysicalDeviceInfo device;
        device.model = text_or(item, "model");
        device.serial = text_or(item, "serial");
        device.wwn = text_or(item, "wwn");
        device.capacity_bytes = item.value("capacity_bytes", std::uint64_t{0});
        device.bus = text_or(item, "bus");
        header.identity.devices.push_back(std::move(device));
    }

    header.root_hint = text_or(json, "root_hint");
    header.chunk_size = json.at("hash").value("chunk_size", std::uint64_t{0});
    header.exclusions_version = json.at("exclusions_version").get<int>();
    header.exclusions = json.at("exclusions").get<std::vector<std::string>>();
    const auto& counts = json.at("counts");
    header.counts.files = counts.value("files", std::uint64_t{0});
    header.counts.dirs = counts.value("dirs", std::uint64_t{0});
    header.counts.links = counts.value("links", std::uint64_t{0});
    header.counts.streams = counts.value("streams", std::uint64_t{0});
    header.counts.chunks = counts.value("chunks", std::uint64_t{0});
    header.counts.bytes = counts.value("bytes", std::uint64_t{0});
    header.merkle_root = text_or(json, "merkle_root");
    const auto& body = json.at("body");
    header.body_blake3 = body.at("blake3").get<std::string>();
    header.body_length = body.at("length").get<std::uint64_t>();
    header.body_lines = body.at("lines").get<std::uint64_t>();
    return header;
}

Entry entry_from_json(const nlohmann::json& json)
{
    Entry entry;
    const auto type = json.at("t").get<std::string>();
    if (type.size() != 1 || std::string_view("fdls").find(type[0]) == std::string_view::npos) {
        throw nlohmann::json::other_error::create(501, "unknown entry type " + type, &json);
    }
    entry.type = type[0];
    entry.path.p = json.at("p").get<std::string>();
    if (json.contains("size")) {
        entry.size = json.at("size").get<std::uint64_t>();
    }
    if (json.contains("mtime_ns")) {
        entry.mtime_ns = json.at("mtime_ns").get<std::int64_t>();
    }
    entry.b3 = text_or(json, "b3");
    entry.sha256 = text_or(json, "sha256");
    entry.unreadable = text_or(json, "unreadable");
    if (json.contains("mode")) {
        entry.mode = json.at("mode").get<std::uint32_t>();
    }
    if (json.contains("attrs")) {
        entry.attrs = json.at("attrs").get<std::uint32_t>();
    }
    entry.path.enc = text_or(json, "enc");
    entry.path.raw = text_or(json, "raw");
    if (json.contains("warn")) {
        entry.warn = json.at("warn").get<std::vector<std::string>>();
    }
    entry.stream = text_or(json, "stream");
    return entry;
}

ChunkLine chunk_line_from_json(const nlohmann::json& json)
{
    if (json.at("t").get<std::string>() != "c") {
        throw nlohmann::json::other_error::create(501, "not a chunk line", &json);
    }
    ChunkLine chunk;
    chunk.p = json.at("p").get<std::string>();
    chunk.idx = json.at("idx").get<std::uint64_t>();
    chunk.offset = json.at("offset").get<std::uint64_t>();
    chunk.length = json.at("length").get<std::uint64_t>();
    chunk.b3 = json.at("b3").get<std::string>();
    if (!is_hash_hex(chunk.b3)) {
        throw nlohmann::json::other_error::create(501, "b3 is not 64 lowercase hex digits", &json);
    }
    return chunk;
}

} // namespace va
