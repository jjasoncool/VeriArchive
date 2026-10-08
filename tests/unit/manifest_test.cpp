#include "test_support.h"

#include "common/json_error.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"
#include "core/scan/scanner.h"
#include "platform/file_system.h"
#include "platform/file_writer.h"

#include <chrono>
#include <fstream>
#include <iterator>

namespace {

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

va::Entry file(std::string p, std::string content)
{
    va::Entry entry;
    entry.path.p = std::move(p);
    entry.size = content.size();
    entry.mtime_ns = 1757912400123456700;
    entry.b3 = va::blake3_hex(content);
    entry.sha256 = va::sha256_hex(content);
    return entry;
}

// The fixed input of the golden manifest: one entry of every kind.
std::vector<va::Entry> golden_entries()
{
    std::vector<va::Entry> entries;
    auto hello = file("a.txt", "hello");
    hello.mode = 420;
    entries.push_back(hello);

    va::Entry empty;
    empty.type = 'd';
    empty.path.p = "empty";
    entries.push_back(empty);

    va::Entry link = file("link", "dir");
    link.type = 'l';
    entries.push_back(link);

    va::Entry locked;
    locked.path.p = "locked.bin";
    locked.size = 10;
    locked.mtime_ns = 1757912400000000000;
    locked.unreadable = "permission";
    entries.push_back(locked);

    auto raw = file("x\xEF\xBF\xBD", "raw name");
    raw.path.enc = "bytes";
    raw.path.raw = "78ff";
    raw.attrs = 32;
    raw.warn = {"case-collision"};
    entries.push_back(raw);
    return entries;
}

va::ManifestHeader golden_header()
{
    va::ManifestHeader header;
    header.archive_uuid = "28ff645e-a220-4aac-ae0b-6c328bf1a2b3";
    header.label = "ARCH-0007";
    header.revision = 2;
    header.supersedes = 1;
    header.created = "2026-09-15T05:00:00Z";
    header.app_version = "0.1.0";
    header.os = "windows";
    header.host = "ws-01";
    header.identity.filesystem_type = "ntfs";
    header.identity.filesystem_uuid = "a1b2c3d4e5f60718";
    header.identity.filesystem_uuid_short = "e5f60718";
    header.identity.volume_label = "ARCHIVE-07";
    header.identity.capacity_bytes = 8001563222016;
    va::PhysicalDeviceInfo device;
    device.model = "WDC WD80EFAX";
    device.serial = "WD-XXXX";
    device.capacity_bytes = 8001563222016;
    device.bus = "usb";
    device.device_ref = "\\\\.\\PhysicalDrive2"; // runtime only: must not appear
    header.identity.devices.push_back(device);
    header.root_hint = "E:\\";
    header.exclusions = va::default_exclusions();
    return header;
}

std::filesystem::path temporary_directory(const char* name)
{
    const auto path = std::filesystem::temp_directory_path() /
        (std::string(name) + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    return path;
}

void golden_manifest_bytes()
{
    const auto root = temporary_directory("va-manifest-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    const auto destination = va::manifest_path(root, "28ff645e-a220-4aac-ae0b-6c328bf1a2b3", 2);
    std::filesystem::create_directories(destination.parent_path());
    CHECK(destination.filename() == "r0002.vam");

    va::ManifestWriter manifest(*writer, *files, destination);
    for (const auto& entry : golden_entries()) {
        CHECK(manifest.add(entry));
    }
    const auto written = manifest.finish(golden_header());
    CHECK(written.has_value());
    CHECK(std::filesystem::exists(destination));
    CHECK(!std::filesystem::exists(destination.string() + ".body"));

    // CLAUDE.md rule 10: any change to these bytes needs a decision record.
    const auto golden_path = std::filesystem::path(VERIARCHIVE_TEST_DATA_DIR) / "golden_r0002.vam";
    const auto actual = read_file(destination);
    const auto expected = read_file(golden_path);
    CHECK(actual == expected);
    if (actual != expected) {
        std::ofstream(root.parent_path() / "va-golden-actual.vam", std::ios::binary) << actual;
        std::cerr << "golden mismatch; actual bytes written to "
                  << (root.parent_path() / "va-golden-actual.vam").string() << '\n';
    }

    // Independent check of the header's body fields.
    const auto newline = actual.find('\n');
    CHECK(written && written->header_line == actual.substr(0, newline));
    CHECK(written && written->header.body_blake3 == va::blake3_hex(actual.substr(newline + 1)));
    CHECK(written && written->header.body_length == actual.size() - newline - 1);
    CHECK(written && written->header.counts.files == 3 && written->header.counts.bytes == 13);

    // Round trip through the reader.
    auto reader = va::ManifestReader::open(*files, destination);
    CHECK(reader.has_value());
    if (reader) {
        CHECK(reader->header().archive_uuid == "28ff645e-a220-4aac-ae0b-6c328bf1a2b3");
        CHECK(reader->header().supersedes == 1u);
        CHECK(reader->header().identity.devices.size() == 1);
        CHECK(reader->header_blake3() == va::blake3_hex(reader->header_line()));
        std::vector<std::string> lines;
        while (auto next = reader->next()) {
            if (!*next) {
                break;
            }
            lines.push_back(va::entry_to_json(**next));
        }
        std::vector<std::string> original;
        for (const auto& entry : golden_entries()) {
            original.push_back(va::entry_to_json(entry));
        }
        CHECK(lines == original);
    }

    // R-62: one changed body byte is detected before any entry is trusted.
    auto tampered = actual;
    tampered[newline + 10] ^= 0x01;
    const auto tampered_path = root / "tampered.vam";
    std::ofstream(tampered_path, std::ios::binary) << tampered;
    const auto rejected = va::ManifestReader::open(*files, tampered_path);
    CHECK(!rejected && rejected.error().kind == va::ManifestFailure::Kind::BodyMismatch);

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void out_of_order_entries_are_rejected()
{
    const auto root = temporary_directory("va-manifest-order-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    va::ManifestWriter manifest(*writer, *files, root / "r0001.vam");
    CHECK(manifest.add(file("b", "b")));
    CHECK(!manifest.add(file("a", "a")));
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// A malformed line is explained with the JSON library's own sentence, without
// its "[json.exception.<kind>.<id>] " tag, which means nothing to people.
void malformed_manifests_say_why()
{
    const auto root = temporary_directory("va-manifest-malformed-");
    const auto files = va::platform::make_file_system_access();
    const auto failure = [&](const std::string& bytes) {
        const auto path = root / "r0001.vam";
        std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
        const auto reader = va::ManifestReader::open(*files, path);
        return reader ? std::string{} : reader.error().message;
    };
    CHECK(failure("{\"format\":\"vam\",\"version\":2}\n") ==
          "invalid manifest header: not a version 1 .vam manifest");
    CHECK(failure("{\"version\":1}\n") == "invalid manifest header: key 'format' not found");
    CHECK(failure("{\"format\":1}\n") == "invalid manifest header: type must be string, but is number");
    CHECK(failure("[]\n") == "invalid manifest header: cannot use at() with array");
    CHECK(failure("{\n").starts_with("invalid manifest header: parse error at line 1, column 2: "));

    try {
        va::entry_from_json(nlohmann::json{{"t", "x"}, {"p", "a"}});
        CHECK(false);
    } catch (const nlohmann::json::exception& error) {
        CHECK(va::json_error_text(error) == "unknown entry type x");
    }

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

} // namespace

void manifest_tests()
{
    golden_manifest_bytes();
    out_of_order_entries_are_rejected();
    malformed_manifests_say_why();
}
