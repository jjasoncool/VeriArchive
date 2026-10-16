#include "test_support.h"

#include "fakes.h"

#include "common/json_error.h"
#include "common/utf8.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"
#include "core/scan/scanner.h"
#include "platform/file_system.h"
#include "platform/file_writer.h"

#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

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
    // Decision 0023: written and flushed, but the destination appears only
    // with the commit, which the engines make in the transaction that records
    // the revision. The body file is gone already.
    CHECK(!std::filesystem::exists(destination));
    CHECK(written && written->file && written->file->commit());
    CHECK(std::filesystem::exists(destination));
    std::size_t files_left = 0;
    for ([[maybe_unused]] const auto& left : std::filesystem::directory_iterator(destination.parent_path())) {
        ++files_left;
    }
    CHECK(files_left == 1);

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

// Passes everything to the platform writer, and runs `after_body` once, right
// after a body file (".body...") is committed.
class BodyHookWriter final : public va::IFileWriter {
public:
    BodyHookWriter(va::IFileWriter& inner, std::function<void()> after_body)
        : inner_(inner), after_body_(std::move(after_body))
    {
    }
    std::expected<std::unique_ptr<va::IOutputFile>, va::IoError> create(
        const std::filesystem::path& destination) override
    {
        auto file = inner_.create(destination);
        if (!file || !va::path_to_utf8(destination.filename()).contains(".body")) {
            return file;
        }
        return std::make_unique<Body>(std::move(*file), after_body_);
    }
    std::expected<void, va::IoError> remove(const std::filesystem::path& file) override
    {
        return inner_.remove(file);
    }
    std::expected<void, va::IoError> create_directories(const std::filesystem::path& directory) override
    {
        return inner_.create_directories(directory);
    }

private:
    class Body final : public va::IOutputFile {
    public:
        Body(std::unique_ptr<va::IOutputFile> inner, std::function<void()>& after) : inner_(std::move(inner)), after_(after)
        {
        }
        std::expected<void, va::IoError> write(std::span<const std::byte> data) override
        {
            return inner_->write(data);
        }
        std::expected<void, va::IoError> flush() override { return inner_->flush(); }
        std::expected<void, va::IoError> commit() override
        {
            auto committed = inner_->commit();
            if (const auto after = std::exchange(after_, nullptr)) {
                after();
            }
            return committed;
        }

    private:
        std::unique_ptr<va::IOutputFile> inner_;
        std::function<void()>& after_;
    };

    va::IFileWriter& inner_;
    std::function<void()> after_body_;
};

// Decision 0023: two writers of the same revision at once (two attempts of one
// registration) each read back the body they wrote. Here the second writer
// finishes, and gives its manifest the name, while the first is between its
// body and its manifest.
void writers_of_one_revision_keep_their_own_body()
{
    const auto root = temporary_directory("va-manifest-two-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    const auto destination = root / "r0001.vam";
    va::ManifestWriter second(*writer, *files, destination);
    CHECK(second.add(file("a.txt", "two")));
    bool second_named = false;
    BodyHookWriter hooked(*writer, [&] {
        auto written = second.finish(golden_header());
        second_named = written && written->file->commit();
    });
    va::ManifestWriter first(hooked, *files, destination);
    CHECK(first.add(file("a.txt", "one")));
    auto written = first.finish(golden_header());
    CHECK(second_named);
    CHECK(written && written->file->commit());

    auto reader = va::ManifestReader::open(*files, destination); // the body matches the header
    CHECK(reader.has_value());
    if (reader) {
        const auto entry = reader->next();
        CHECK(entry && *entry && (**entry).b3 == va::blake3_hex("one"));
    }
    std::size_t names = 0;
    for ([[maybe_unused]] const auto& left : std::filesystem::directory_iterator(root)) {
        ++names;
    }
    CHECK(names == 1);
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

// R-62: the entries come from a second read of the file, after open()
// validated the body. When that read gives other bytes (sample data: one hex
// digit of the first entry's BLAKE3, as removable media read again might),
// the entries read are not the body validated: the end of the body fails with
// BodyMismatch, also when asked again. Read as the file is, the end of the
// body is the end.
void entries_are_read_from_the_body_validated()
{
    const auto root = temporary_directory("va-manifest-second-read-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    const auto path = root / "r0001.vam";
    va::ManifestWriter manifest(*writer, *files, path);
    CHECK(manifest.add(file("a.txt", "one")));
    CHECK(manifest.add(file("b.txt", "two")));
    auto written = manifest.finish(golden_header());
    CHECK(written && written->file->commit());
    const auto bytes = read_file(path);
    const auto b3 = va::blake3_hex("one");
    auto other = b3;
    other[0] = other[0] == '0' ? '1' : '0';

    test::SecondReadDiffers differing(*files, "r0001.vam", 1, bytes.find('\n') + 1, "\"b3\":\"" + b3,
        "\"b3\":\"" + other);
    auto reader = va::ManifestReader::open(differing, path);
    CHECK(reader.has_value());
    if (reader) {
        const auto first = reader->next();
        CHECK(first && *first && (**first).b3 == other); // read, but not yet trusted
        const auto second = reader->next();
        CHECK(second && *second && (**second).path.p == "b.txt");
        const auto end = reader->next();
        CHECK(!end && end.error().kind == va::ManifestFailure::Kind::BodyMismatch &&
              end.error().message == "the manifest changed while it was read (ManifestHashMismatch)");
        const auto again = reader->next();
        CHECK(!again && again.error().kind == va::ManifestFailure::Kind::BodyMismatch);
    }
    CHECK(differing.differed());

    auto unchanged = va::ManifestReader::open(*files, path);
    CHECK(unchanged.has_value());
    if (unchanged) {
        std::size_t entries = 0;
        for (auto next = unchanged->next(); next && *next; next = unchanged->next()) {
            ++entries;
        }
        const auto end = unchanged->next();
        CHECK(entries == 2 && end && !*end);
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// Decision 0028: given the BLAKE3 that a repository recorded for line 1, the
// reader compares line 1 with it before it parses it. The line recorded
// opens; a line 1 that differs fails with HeaderMismatch, also one that no
// longer parses, one cut off, the line recorded without its newline (the
// body gone with it), and an empty file; a file that cannot be opened stays
// an I/O failure. Without the hash (an import's source, rebuild-index), a
// line 1 that does not parse is a malformed manifest, as before.
void line_one_is_compared_before_it_is_parsed()
{
    using Kind = va::ManifestFailure::Kind;
    const auto root = temporary_directory("va-manifest-recorded-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    const auto path = root / "r0001.vam";
    va::ManifestWriter manifest(*writer, *files, path);
    CHECK(manifest.add(file("a.txt", "one")));
    auto written = manifest.finish(golden_header());
    CHECK(written && written->file->commit());
    const auto bytes = read_file(path);
    const auto newline = bytes.find('\n');
    const auto recorded = va::blake3_hex(bytes.substr(0, newline));
    CHECK(va::ManifestReader::open(*files, path, recorded).has_value());

    const auto failure = [&](const std::string& changed,
                             std::optional<std::string_view> hash) -> std::optional<va::ManifestFailure> {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << changed;
        const auto reader = va::ManifestReader::open(*files, path, hash);
        return reader ? std::nullopt : std::optional(reader.error());
    };
    const auto kind = [&](const std::string& changed, std::optional<std::string_view> hash) {
        const auto failed = failure(changed, hash);
        return failed ? std::optional(failed->kind) : std::nullopt;
    };
    auto unparseable = bytes;
    unparseable[0] = '[';
    const auto mismatch = failure(unparseable, recorded);
    CHECK(mismatch && mismatch->kind == Kind::HeaderMismatch &&
          mismatch->message == "the manifest differs from the repository record");
    CHECK(kind(unparseable, std::nullopt) == Kind::Malformed);
    CHECK(kind(bytes.substr(0, newline / 2), recorded) == Kind::HeaderMismatch);
    CHECK(kind(bytes.substr(0, newline), recorded) == Kind::HeaderMismatch);
    CHECK(kind("", recorded) == Kind::HeaderMismatch);
    auto relabelled = bytes;
    relabelled[relabelled.find("ARCHIVE-07")] = 'B';  // still a header, just another one
    CHECK(kind(relabelled, recorded) == Kind::HeaderMismatch);
    CHECK(!kind(relabelled, std::nullopt));
    const auto gone = va::ManifestReader::open(*files, root / "r0002.vam", recorded);
    CHECK(!gone && gone.error().kind == Kind::Io);

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

} // namespace

void manifest_tests()
{
    golden_manifest_bytes();
    writers_of_one_revision_keep_their_own_body();
    out_of_order_entries_are_rejected();
    malformed_manifests_say_why();
    entries_are_read_from_the_body_validated();
    line_one_is_compared_before_it_is_parsed();
}
