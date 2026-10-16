#include "test_support.h"

#include "fakes.h"
#include "manifest_samples.h"

#include "common/hex.h"
#include "common/json_error.h"
#include "common/utf8.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"
#include "core/scan/scanner.h"
#include "platform/file_system.h"
#include "platform/file_writer.h"

#include <algorithm>
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

    va::ManifestWriter manifest(*writer, *files, destination, 0);
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

// Decision 0005: a copy's manifest names the archive it is a copy of in the
// header key copy_of, directly after supersedes (decision 0015). The golden
// case: the second revision of ARCH-0008, a copy of the golden manifest's
// archive with the same entries, on its own sample volume (sample data).
// CLAUDE.md rule 10: any change to these bytes needs a decision record.
void golden_manifest_with_copy_of()
{
    const auto root = temporary_directory("va-manifest-copy-of-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    const auto destination = va::manifest_path(root, "c0ffee00-0005-4000-8000-000000000008", 2);
    std::filesystem::create_directories(destination.parent_path());

    auto header = golden_header();
    header.archive_uuid = "c0ffee00-0005-4000-8000-000000000008";
    header.label = "ARCH-0008";
    header.copy_of = "28ff645e-a220-4aac-ae0b-6c328bf1a2b3";
    header.identity.filesystem_uuid = "c0ffee0000000008";
    header.identity.filesystem_uuid_short = "00000008";
    header.identity.volume_label = "SAMPLE-COPY";
    header.identity.devices.front().model = "SAMPLE-DEVICE";
    header.identity.devices.front().serial = "SAMPLE0008";
    header.root_hint = "K:\\";
    va::ManifestWriter manifest(*writer, *files, destination, 0);
    for (const auto& entry : golden_entries()) {
        CHECK(manifest.add(entry));
    }
    const auto written = manifest.finish(header);
    CHECK(written && written->file && written->file->commit());

    const auto golden_path = std::filesystem::path(VERIARCHIVE_TEST_DATA_DIR) / "golden_copy_of_r0002.vam";
    const auto actual = read_file(destination);
    const auto expected = read_file(golden_path);
    CHECK(actual == expected);
    if (actual != expected) {
        std::ofstream(root.parent_path() / "va-golden-copy-of-actual.vam", std::ios::binary) << actual;
        std::cerr << "golden mismatch; actual bytes written to "
                  << (root.parent_path() / "va-golden-copy-of-actual.vam").string() << '\n';
    }
    CHECK(actual.find(R"("revision":2,"supersedes":1,"copy_of":"28ff645e-a220-4aac-ae0b-6c328bf1a2b3","created":)") !=
          std::string::npos);

    // The reader gives copy_of back.
    const auto reader = va::ManifestReader::open(*files, destination);
    CHECK(reader && reader->header().copy_of == std::optional<va::Uuid>("28ff645e-a220-4aac-ae0b-6c328bf1a2b3"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// Passes everything to the platform writer, and runs `after` once, right
// after a side file whose name holds `marker` (".body", ".chunks") is
// committed.
class SideFileHookWriter final : public va::IFileWriter {
public:
    SideFileHookWriter(va::IFileWriter& inner, std::string marker, std::function<void()> after)
        : inner_(inner), marker_(std::move(marker)), after_(std::move(after))
    {
    }
    std::expected<std::unique_ptr<va::IOutputFile>, va::IoError> create(
        const std::filesystem::path& destination) override
    {
        auto file = inner_.create(destination);
        if (!file || !va::path_to_utf8(destination.filename()).contains(marker_)) {
            return file;
        }
        return std::make_unique<SideFile>(std::move(*file), after_);
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
    class SideFile final : public va::IOutputFile {
    public:
        SideFile(std::unique_ptr<va::IOutputFile> inner, std::function<void()>& after)
            : inner_(std::move(inner)), after_(after)
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
    std::string marker_;
    std::function<void()> after_;
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
    va::ManifestWriter second(*writer, *files, destination, 0);
    CHECK(second.add(file("a.txt", "two")));
    bool second_named = false;
    SideFileHookWriter hooked(*writer, ".body", [&] {
        auto written = second.finish(golden_header());
        second_named = written && written->file->commit();
    });
    va::ManifestWriter first(hooked, *files, destination, 0);
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
    va::ManifestWriter manifest(*writer, *files, root / "r0001.vam", 0);
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

// Passes everything to `inner`, except that each file it opens fails its
// second read at the end of the file (sample data: a CRC error), as a device
// that fails while the rest of a manifest is read again; the first is the
// read with which open() finds the end of the body it validates.
class SecondEndFails final : public va::IFileSystemAccess {
public:
    explicit SecondEndFails(va::IFileSystemAccess& inner) : inner_(inner) {}

    std::expected<std::vector<va::DirEntry>, va::IoError> list(const std::filesystem::path& directory) override
    {
        return inner_.list(directory);
    }
    std::expected<std::unique_ptr<va::IByteSource>, va::IoError> open(
        const std::filesystem::path& file, va::ReadOptions options) override
    {
        auto source = inner_.open(file, options);
        if (!source) {
            return source;
        }
        return std::make_unique<Source>(std::move(*source));
    }
    std::expected<std::string, va::IoError> read_link(const std::filesystem::path& link) override
    {
        return inner_.read_link(link);
    }
    std::expected<void, va::IoError> drop_cache(const std::filesystem::path& file) override
    {
        return inner_.drop_cache(file);
    }

private:
    class Source final : public va::IByteSource {
    public:
        explicit Source(std::unique_ptr<va::IByteSource> inner) : inner_(std::move(inner)) {}
        std::expected<std::size_t, va::IoError> read(std::uint64_t offset, std::span<std::byte> output) override
        {
            if (offset >= inner_->size() && ++ends_read_ == 2) {
                return std::unexpected(va::IoError{va::IoErrorKind::MediaError, 23, "CRC error (sample)"});
            }
            return inner_->read(offset, output);
        }
        std::uint64_t size() const override { return inner_->size(); }
        std::size_t alignment() const override { return inner_->alignment(); }

    private:
        std::unique_ptr<va::IByteSource> inner_;
        int ends_read_ = 0;
    };

    va::IFileSystemAccess& inner_;
};

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
    va::ManifestWriter manifest(*writer, *files, path, 0);
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
              end.error().message == "the manifest changed while it was read");
        const auto again = reader->next();
        CHECK(!again && again.error().kind == va::ManifestFailure::Kind::BodyMismatch);
    }
    CHECK(differing.differed());

    // Other bytes that make a line that does not parse (a quote made an
    // apostrophe), or entries out of canonical order (b.txt read as a.txt
    // again): read on to the end of the body, they are not the body
    // validated, so the reader fails with BodyMismatch, not as a malformed
    // manifest, and so again when asked again (decision 0028).
    const std::pair<std::string, std::string> read_otherwise[] = {
        {"\"b3\":\"" + b3, "\"b3\":'" + b3},
        {"\"p\":\"b.txt\"", "\"p\":\"a.txt\""},
    };
    for (const auto& [from, to] : read_otherwise) {
        test::SecondReadDiffers other_line(*files, "r0001.vam", 1, bytes.find('\n') + 1, from, to);
        auto opened = va::ManifestReader::open(other_line, path);
        CHECK(opened.has_value());
        if (!opened) {
            continue;
        }
        std::optional<va::ManifestFailure> failure;
        for (;;) {
            const auto next = opened->next();
            if (!next) {
                failure = next.error();
                break;
            }
            if (!*next) {
                break;
            }
        }
        CHECK(other_line.differed());
        CHECK(failure && failure->kind == va::ManifestFailure::Kind::BodyMismatch &&
              failure->message == "the manifest changed while it was read");
        const auto again = opened->next();
        CHECK(!again && again.error().kind == va::ManifestFailure::Kind::BodyMismatch);
    }
    // The rest of the body cannot be read after such a line: that says
    // nothing either way, and the failure to read it is the failure.
    {
        test::SecondReadDiffers other_line(*files, "r0001.vam", 1, bytes.find('\n') + 1, "\"b3\":\"" + b3,
            "\"b3\":'" + b3);
        SecondEndFails failing(other_line);
        auto opened = va::ManifestReader::open(failing, path);
        CHECK(opened.has_value());
        if (opened) {
            const auto first = opened->next();
            CHECK(other_line.differed());
            CHECK(!first && first.error().kind == va::ManifestFailure::Kind::Io &&
                  first.error().io_kind == va::IoErrorKind::MediaError &&
                  first.error().message == "CRC error (sample)");
        }
    }

    // A body that is the one validated and still has a line that does not
    // parse (sample data: the apostrophe written into the file, and the
    // header's body hash made to match it) is a malformed manifest, also
    // when asked again.
    {
        const auto newline = bytes.find('\n');
        auto header = bytes.substr(0, newline + 1);
        const auto body = bytes.substr(newline + 1);
        auto broken = body;
        broken.replace(broken.find("\"b3\":\"" + b3), 6, "\"b3\":'");
        header.replace(header.find(va::blake3_hex(body)), 64, va::blake3_hex(broken));
        const auto malformed_path = root / "r0002.vam";
        std::ofstream(malformed_path, std::ios::binary) << header << broken;
        auto opened = va::ManifestReader::open(*files, malformed_path);
        CHECK(opened.has_value());
        if (opened) {
            const auto first = opened->next();
            CHECK(!first && first.error().kind == va::ManifestFailure::Kind::Malformed &&
                  first.error().message.starts_with("invalid manifest entry: "));
            const auto again = opened->next();
            CHECK(!again && again.error().kind == va::ManifestFailure::Kind::Malformed);
        }
    }

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
// body gone with it), and an empty file; a file that cannot be opened, or
// read once opened, stays an I/O failure. Without the hash (an import's
// source, rebuild-index), a line 1 that does not parse is a malformed
// manifest, as before.
void line_one_is_compared_before_it_is_parsed()
{
    using Kind = va::ManifestFailure::Kind;
    const auto root = temporary_directory("va-manifest-recorded-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    const auto path = root / "r0001.vam";
    va::ManifestWriter manifest(*writer, *files, path, 0);
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
    // A file that opens and then cannot be read (sample data: a CRC error from
    // its first byte on, as a bad sector gives) is no answer either way: it
    // stays an I/O failure, also given the BLAKE3 recorded.
    test::FakeFileSystem bad_sector(root);
    bad_sector.add_file("r0003.vam", bytes).read_error_offset = 0;
    for (const auto hash : {std::optional<std::string_view>(recorded), std::optional<std::string_view>()}) {
        const auto unreadable = va::ManifestReader::open(bad_sector, root / "r0003.vam", hash);
        CHECK(!unreadable && unreadable.error().kind == Kind::Io &&
              unreadable.error().io_kind == va::IoErrorKind::MediaError);
    }

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// ---- Chunk lines and the Merkle root (decision 0031) ----

constexpr std::uint64_t test_chunk_size = 64 * 1024;

// The fixed input of the golden manifest with chunk lines, with a 64 KiB
// chunk size: files smaller than a chunk, of exactly one chunk and of
// several, the last chunk shorter or not; two files that share their p
// (R-41); a large stream and a large unreadable file, which have no chunk
// lines; a directory and a link.
std::vector<va::Entry> golden_chunked_entries()
{
    using test::patterned;
    using test::sample_file;
    std::vector<va::Entry> entries;
    auto hello = sample_file("a.txt", "hello", test_chunk_size);
    hello.mode = 420;
    entries.push_back(hello);
    entries.push_back(sample_file("big.bin", patterned(2 * test_chunk_size + 100, 1), test_chunk_size));
    auto stream = sample_file("big.bin", patterned(test_chunk_size + 4464, 2), test_chunk_size);
    stream.type = 's';
    stream.stream = "Zone.Identifier";
    stream.chunks.clear();
    entries.push_back(stream);
    va::Entry empty;
    empty.type = 'd';
    empty.path.p = "empty";
    entries.push_back(empty);
    entries.push_back(sample_file("exact.bin", patterned(test_chunk_size, 3), test_chunk_size));
    auto link = sample_file("link", "a.txt", test_chunk_size);
    link.type = 'l';
    entries.push_back(link);
    va::Entry locked;
    locked.path.p = "locked.bin";
    locked.size = 3 * test_chunk_size;
    locked.mtime_ns = 1757912400000000000;
    locked.unreadable = "media";
    entries.push_back(locked);
    auto first = sample_file("x\xEF\xBF\xBD", patterned(test_chunk_size + 1, 4), test_chunk_size);
    first.path.enc = "bytes";
    first.path.raw = "78fe";
    entries.push_back(first);
    auto second = sample_file("x\xEF\xBF\xBD", patterned(3 * test_chunk_size, 5), test_chunk_size);
    second.path.enc = "bytes";
    second.path.raw = "78ff";
    second.attrs = 32;
    entries.push_back(second);
    return entries;
}

va::ManifestHeader golden_chunked_header()
{
    auto header = golden_header();
    header.revision = 3;
    header.supersedes = 2;
    return header;
}

// Writes `entries` with `chunk_size` to `destination` and gives the manifest
// its name.
std::expected<va::WrittenManifest, va::IoError> write_manifest(va::IFileWriter& writer,
    va::IFileSystemAccess& files, const std::filesystem::path& destination, std::uint64_t chunk_size,
    const std::vector<va::Entry>& entries, const va::ManifestHeader& header)
{
    va::ManifestWriter manifest(writer, files, destination, chunk_size);
    for (const auto& entry : entries) {
        if (auto added = manifest.add(entry); !added) {
            return std::unexpected(added.error());
        }
    }
    auto written = manifest.finish(header);
    if (written) {
        if (auto named = written->file->commit(); !named) {
            return std::unexpected(named.error());
        }
    }
    return written;
}

// Reads a manifest to its end, keeping the entries in `entries`: the failure,
// if any.
std::optional<va::ManifestFailure> read_to_end(
    va::IFileSystemAccess& files, const std::filesystem::path& path, std::vector<va::Entry>* entries = nullptr)
{
    auto reader = va::ManifestReader::open(files, path);
    if (!reader) {
        return reader.error();
    }
    for (;;) {
        auto next = reader->next();
        if (!next) {
            return next.error();
        }
        if (!*next) {
            return std::nullopt;
        }
        if (entries) {
            entries->push_back(std::move(**next));
        }
    }
}

std::size_t files_in(const std::filesystem::path& directory)
{
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& file : std::filesystem::directory_iterator(directory)) {
        ++count;
    }
    return count;
}

std::string uppercase(std::string text)
{
    for (auto& digit : text) {
        if (digit >= 'a' && digit <= 'f') {
            digit = static_cast<char>(digit - 'a' + 'A');
        }
    }
    return text;
}

// CLAUDE.md rule 10: the golden manifest with chunk lines, byte for byte on
// every OS. Its chunk hashes, body hash and Merkle root were also checked
// outside the code base, with a BLAKE3 written in Python from its
// specification.
void golden_chunked_manifest_bytes()
{
    const auto root = temporary_directory("va-manifest-chunked-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    const auto destination = va::manifest_path(root, "28ff645e-a220-4aac-ae0b-6c328bf1a2b3", 3);
    std::filesystem::create_directories(destination.parent_path());
    const auto entries = golden_chunked_entries();
    const auto written =
        write_manifest(*writer, *files, destination, test_chunk_size, entries, golden_chunked_header());
    CHECK(written.has_value());
    if (!written) {
        return;
    }
    CHECK(files_in(destination.parent_path()) == 1); // both side files are gone (decision 0023)

    const auto golden_path = std::filesystem::path(VERIARCHIVE_TEST_DATA_DIR) / "golden_chunks_r0003.vam";
    const auto actual = read_file(destination);
    const auto expected = read_file(golden_path);
    CHECK(actual == expected);
    if (actual != expected) {
        std::ofstream(root.parent_path() / "va-golden-chunks-actual.vam", std::ios::binary) << actual;
        std::cerr << "golden mismatch; actual bytes written to "
                  << va::path_to_utf8(root.parent_path() / "va-golden-chunks-actual.vam") << '\n';
    }

    // The leaves of §B6: a.txt; big.bin's chunks; its stream, whole; exact.bin,
    // one chunk, whole; the link; both x files' chunks. Not the directory, not
    // the unreadable file.
    std::vector<std::string> leaves{entries[0].b3};
    leaves.insert(leaves.end(), entries[1].chunks.begin(), entries[1].chunks.end());
    leaves.insert(leaves.end(), {entries[2].b3, entries[4].b3, entries[5].b3});
    leaves.insert(leaves.end(), entries[7].chunks.begin(), entries[7].chunks.end());
    leaves.insert(leaves.end(), entries[8].chunks.begin(), entries[8].chunks.end());
    va::MerkleTreeBuilder tree;
    for (const auto& leaf : leaves) {
        CHECK(tree.add_hex(leaf));
    }
    const auto& header = written->header;
    CHECK(leaves.size() == 12 && tree.leaves() == 12);
    CHECK(header.chunk_size == test_chunk_size && header.merkle_root == tree.root_hex());
    CHECK(header.counts.chunks == 8 && header.body_lines == 17);
    CHECK(header.counts.files == 6 && header.counts.dirs == 1 && header.counts.links == 1 &&
          header.counts.streams == 1 && header.counts.bytes == 528858);

    // The chunk lines follow all entries, in entry order and then idx, with p
    // alone even where two files share it.
    const auto parts = test::split_manifest(actual);
    std::vector<std::string> chunk_lines;
    for (const auto& entry : entries) {
        for (std::uint64_t idx = 0; idx < entry.chunks.size(); ++idx) {
            auto chunk = va::chunk_of(entry.path.p, *entry.size, idx, test_chunk_size);
            chunk.b3 = entry.chunks[idx];
            chunk_lines.push_back(va::chunk_line_to_json(chunk));
        }
    }
    CHECK(parts.lines.size() == 17 && chunk_lines.size() == 8);
    CHECK(parts.lines.size() == 17 &&
          std::equal(chunk_lines.begin(), chunk_lines.end(), parts.lines.begin() + 9, parts.lines.end()));
    CHECK(parts.lines.size() == 17 &&
          parts.lines[11] == "{\"t\":\"c\",\"p\":\"big.bin\",\"idx\":2,\"offset\":131072,\"length\":100,\"b3\":\"" +
                                 entries[1].chunks[2] + "\"}");

    // Round trip: every entry, with the chunk hashes of the files that have
    // chunk lines.
    std::vector<va::Entry> read;
    CHECK(!read_to_end(*files, destination, &read));
    CHECK(read.size() == entries.size());
    for (std::size_t index = 0; index < read.size() && index < entries.size(); ++index) {
        CHECK(va::entry_to_json(read[index]) == va::entry_to_json(entries[index]));
        CHECK(read[index].chunks == entries[index].chunks);
    }

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// Decision 0023 for the chunk lines: two writers of one revision at once
// keep their own. Here the second writer finishes, and gives its manifest the
// name, right after the first has given its chunk lines' side file its name.
void writers_of_one_revision_keep_their_own_chunk_lines()
{
    const auto root = temporary_directory("va-manifest-two-chunked-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    const auto destination = root / "r0001.vam";
    const auto one = test::sample_file("big.bin", test::patterned(2 * test_chunk_size + 1, 1), test_chunk_size);
    const auto two = test::sample_file("big.bin", test::patterned(2 * test_chunk_size + 1, 2), test_chunk_size);
    va::ManifestWriter second(*writer, *files, destination, test_chunk_size);
    CHECK(second.add(two));
    bool second_named = false;
    SideFileHookWriter hooked(*writer, ".chunks", [&] {
        auto written = second.finish(golden_header());
        second_named = written && written->file->commit();
    });
    va::ManifestWriter first(hooked, *files, destination, test_chunk_size);
    CHECK(first.add(one));
    auto written = first.finish(golden_header());
    CHECK(second_named);
    CHECK(written && written->file->commit());

    std::vector<va::Entry> read;
    CHECK(!read_to_end(*files, destination, &read));
    CHECK(read.size() == 1 && read.front().chunks == one.chunks);
    CHECK(files_in(root) == 1);
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// Passes everything to `inner` but the manifest itself (".vam"), which it
// cannot create; first it notes what is in the manifest's folder.
class FailingManifestWriter final : public va::IFileWriter {
public:
    explicit FailingManifestWriter(va::IFileWriter& inner) : inner_(inner) {}

    std::vector<std::string> seen; // the folder when the manifest was to be created

    std::expected<std::unique_ptr<va::IOutputFile>, va::IoError> create(
        const std::filesystem::path& destination) override
    {
        if (destination.extension() != ".vam") {
            return inner_.create(destination);
        }
        for (const auto& file : std::filesystem::directory_iterator(destination.parent_path())) {
            seen.push_back(va::path_to_utf8(file.path().filename()));
        }
        std::sort(seen.begin(), seen.end());
        return std::unexpected(va::IoError{va::IoErrorKind::Other, 0, "disk full"});
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
    va::IFileWriter& inner_;
};

// The chunk lines' side file follows the body's rules (decision 0023): a
// name of the writer's own, next to the body's, and gone when finish()
// returns, also when the manifest cannot be written.
void side_files_go_whatever_the_outcome()
{
    const auto root = temporary_directory("va-manifest-side-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    FailingManifestWriter failing(*writer);
    {
        va::ManifestWriter manifest(failing, *files, root / "r0001.vam", test_chunk_size);
        for (const auto& entry : golden_chunked_entries()) {
            CHECK(manifest.add(entry));
        }
        const auto written = manifest.finish(golden_chunked_header());
        CHECK(!written && written.error().message == "disk full");
        CHECK(files_in(root) == 0);
    }
    CHECK(failing.seen.size() == 2);
    if (failing.seen.size() == 2) {
        const auto& body = failing.seen[0];
        const auto& chunks = failing.seen[1];
        CHECK(body.starts_with("r0001.vam.body-") && chunks.starts_with("r0001.vam.chunks-"));
        CHECK(body.substr(15) == chunks.substr(17) && body.size() == 15 + 36);
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// Decision 0031: a manifest with chunk lines is read strictly. Each case
// breaks one rule of a valid manifest whose header still describes its body:
// broken structure is Malformed, content hashes that do not give the Merkle
// root a MerkleMismatch, which the engines report as ManifestHashMismatch.
class StrictReading {
public:
    StrictReading()
        : root_(temporary_directory("va-manifest-strict-")),
          writer_(va::platform::make_file_writer()),
          files_(va::platform::make_file_system_access())
    {
        CHECK(write_manifest(*writer_, *files_, root_ / "valid.vam", test_chunk_size, golden_chunked_entries(),
            golden_chunked_header()));
        valid_ = test::split_manifest(read_file(root_ / "valid.vam"));
    }
    ~StrictReading()
    {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    StrictReading(const StrictReading&) = delete;
    StrictReading& operator=(const StrictReading&) = delete;

    // The valid manifest, to change. Lines 0-8 are the entries, 9-11
    // big.bin's chunk lines, 12-13 the first x's, 14-16 the second x's.
    const test::ManifestParts& valid() const { return valid_; }

    std::optional<va::ManifestFailure> failure(const test::ManifestParts& parts) const
    {
        const auto path = root_ / "case.vam";
        std::ofstream(path, std::ios::binary | std::ios::trunc) << test::assemble_manifest(parts);
        return read_to_end(*files_, path);
    }

    bool malformed(const test::ManifestParts& parts, const std::string& message) const
    {
        const auto found = failure(parts);
        const bool right = found && found->kind == va::ManifestFailure::Kind::Malformed &&
                           found->message.starts_with(message);
        if (!right) {
            std::cerr << "  expected Malformed: " << message << "\n  found: " << (found ? found->message : "nothing")
                      << '\n';
        }
        return right;
    }

    bool merkle_mismatch(const test::ManifestParts& parts) const
    {
        const auto found = failure(parts);
        return found && found->kind == va::ManifestFailure::Kind::MerkleMismatch && found->hash_mismatch() &&
               found->message == "manifest content hashes do not match its merkle_root";
    }

    // The valid manifest with the chunk line at `line` changed by `change`.
    template <typename Change>
    test::ManifestParts chunk_changed(std::size_t line, Change change) const
    {
        auto parts = valid_;
        auto chunk = va::chunk_line_from_json(nlohmann::json::parse(parts.lines[line]));
        change(chunk);
        parts.lines[line] = va::chunk_line_to_json(chunk);
        return parts;
    }

    // The valid manifest with the entry line at `line` changed by `change`.
    template <typename Change>
    test::ManifestParts entry_changed(std::size_t line, Change change) const
    {
        auto parts = valid_;
        auto entry = va::entry_from_json(nlohmann::json::parse(parts.lines[line]));
        change(entry);
        parts.lines[line] = va::entry_to_json(entry);
        return parts;
    }

    // The valid manifest changed by `change`.
    template <typename Change>
    test::ManifestParts changed(Change change) const
    {
        auto parts = valid_;
        change(parts);
        return parts;
    }

private:
    std::filesystem::path root_;
    std::unique_ptr<va::IFileWriter> writer_;
    std::unique_ptr<va::IFileSystemAccess> files_;
    test::ManifestParts valid_;
};

void chunked_manifests_are_read_strictly()
{
    using Parts = test::ManifestParts;
    const StrictReading reading;
    CHECK(!reading.failure(reading.valid()));
    const std::string x = "x\xEF\xBF\xBD";

    // The header's chunk fields.
    CHECK(reading.malformed(reading.changed([](Parts& parts) { parts.header.chunk_size = 1000; }),
        "invalid manifest header: chunk_size 1000 is not 0 or a multiple of 65536 up to 1073741824"));
    CHECK(reading.malformed(reading.changed([](Parts& parts) { parts.header.chunk_size = 3 * 32 * 1024; }),
        "invalid manifest header: chunk_size 98304 is not"));
    CHECK(reading.malformed(
        reading.changed([](Parts& parts) { parts.header.chunk_size = va::max_chunk_size + va::chunk_size_unit; }),
        "invalid manifest header: chunk_size 1073807360 is not"));
    const std::string no_merkle_root =
        "invalid manifest header: merkle_root is missing or not 64 lowercase hex digits";
    CHECK(reading.malformed(reading.changed([](Parts& parts) { parts.header.merkle_root.clear(); }), no_merkle_root));
    CHECK(reading.malformed(
        reading.changed([](Parts& parts) { parts.header.merkle_root = uppercase(parts.header.merkle_root); }),
        no_merkle_root));
    CHECK(reading.malformed(reading.changed([](Parts& parts) { parts.header.counts.chunks = 18; }),
        "invalid manifest header: counts.chunks is larger than body.lines"));

    // The chunk lines are the last counts.chunks lines of the body.
    CHECK(reading.malformed(reading.changed([](Parts& parts) {
        parts.lines.insert(parts.lines.begin() + 1, parts.lines[9]);
    }),
        "chunk line among the manifest entries"));
    CHECK(reading.malformed(reading.changed([](Parts& parts) { --parts.header.counts.chunks; }),
        "chunk 0 of big.bin is missing or does not match the file"));
    CHECK(reading.malformed(reading.changed([](Parts& parts) { ++parts.header.counts.chunks; }),
        "manifest entry among the chunk lines"));

    // Each file owns the next chunk lines, where its size and the chunk size
    // put them.
    const std::string big_1 = "chunk 1 of big.bin is missing or does not match the file";
    CHECK(reading.malformed(reading.chunk_changed(10, [](va::ChunkLine& chunk) { chunk.p = "big.bim"; }), big_1));
    CHECK(reading.malformed(reading.chunk_changed(10, [](va::ChunkLine& chunk) { chunk.idx = 2; }), big_1));
    CHECK(reading.malformed(reading.chunk_changed(10, [](va::ChunkLine& chunk) { ++chunk.offset; }), big_1));
    CHECK(reading.malformed(reading.chunk_changed(11, [](va::ChunkLine& chunk) { ++chunk.length; }),
        "chunk 2 of big.bin is missing or does not match the file"));
    CHECK(reading.malformed(reading.changed([](Parts& parts) {
        parts.lines.erase(parts.lines.begin() + 11);
        --parts.header.counts.chunks;
    }),
        "chunk 2 of big.bin is missing or does not match the file"));
    CHECK(reading.malformed(reading.changed([](Parts& parts) {
        parts.lines.pop_back();
        --parts.header.counts.chunks;
    }),
        "chunk 2 of " + x + " is missing or does not match the file"));
    CHECK(reading.malformed(reading.changed([](Parts& parts) {
        parts.lines.push_back(parts.lines.back());
        ++parts.header.counts.chunks;
    }),
        "chunk 2 of " + x + " belongs to no file"));
    // Two files that share their p take their chunk lines by position.
    CHECK(reading.malformed(reading.changed([](Parts& parts) {
        std::rotate(parts.lines.begin() + 12, parts.lines.begin() + 14, parts.lines.end());
    }),
        "chunk 1 of " + x + " is missing or does not match the file"));
    // A file of exactly one chunk and an unreadable file have none.
    const auto exact_b3 = golden_chunked_entries()[4].b3;
    CHECK(reading.malformed(reading.changed([&](Parts& parts) {
        parts.lines.insert(parts.lines.begin() + 12,
            va::chunk_line_to_json({"exact.bin", 0, 0, test_chunk_size, exact_b3}));
        ++parts.header.counts.chunks;
    }),
        "chunk 0 of " + x + " is missing or does not match the file"));
    CHECK(reading.malformed(reading.changed([&](Parts& parts) {
        parts.lines.push_back(va::chunk_line_to_json({"locked.bin", 0, 0, test_chunk_size, exact_b3}));
        ++parts.header.counts.chunks;
    }),
        "chunk 0 of locked.bin belongs to no file"));

    // Chunk lines in canonical form (R-64), each with a hash.
    const std::string not_canonical = "invalid chunk line: not canonical JSON";
    CHECK(reading.malformed(
        reading.changed([](Parts& parts) { parts.lines[9].insert(parts.lines[9].size() - 1, " "); }),
        not_canonical));
    CHECK(reading.malformed(
        reading.changed([](Parts& parts) { parts.lines[9].insert(parts.lines[9].size() - 1, ",\"x\":1"); }),
        not_canonical));
    CHECK(reading.malformed(reading.changed([](Parts& parts) {
        const auto b3 = nlohmann::json::parse(parts.lines[9]).at("b3").get<std::string>();
        parts.lines[9] = "{\"p\":\"big.bin\",\"t\":\"c\",\"idx\":0,\"offset\":0,\"length\":65536,\"b3\":\"" + b3 + "\"}";
    }),
        not_canonical));
    CHECK(reading.malformed(reading.changed([](Parts& parts) {
        const auto b3 = nlohmann::json::parse(parts.lines[9]).at("b3").get<std::string>();
        parts.lines[9] = "{\"t\":\"c\",\"p\":\"big.bin\",\"idx\":0.0,\"offset\":0,\"length\":65536,\"b3\":\"" + b3 + "\"}";
    }),
        not_canonical));
    CHECK(reading.malformed(reading.changed([](Parts& parts) { parts.lines[9] = "{\"t\":\"c\",\"p\":\"big.bin\"}"; }),
        "invalid chunk line: key 'idx' not found"));
    CHECK(reading.malformed(reading.changed([](Parts& parts) { parts.lines[9] = "{"; }),
        "invalid chunk line: parse error"));
    CHECK(reading.malformed(reading.chunk_changed(9, [](va::ChunkLine& chunk) { chunk.b3 = uppercase(chunk.b3); }),
        "invalid chunk line: b3 is not 64 lowercase hex digits"));
    CHECK(reading.malformed(reading.entry_changed(0, [](va::Entry& entry) { entry.b3 = uppercase(entry.b3); }),
        "invalid manifest entry: b3 is not 64 lowercase hex digits"));

    // Content hashes that do not give the Merkle root.
    CHECK(reading.merkle_mismatch(
        reading.chunk_changed(13, [](va::ChunkLine& chunk) { chunk.b3 = va::blake3_hex("other"); })));
    CHECK(reading.merkle_mismatch(
        reading.entry_changed(0, [](va::Entry& entry) { entry.b3 = va::blake3_hex("other"); })));
    CHECK(reading.merkle_mismatch(
        reading.changed([](Parts& parts) { parts.header.merkle_root = va::blake3_hex("other"); })));
}

// Two files that share their p and their size have chunk lines that differ
// only in their hashes: the Merkle root tells whose they are.
void merkle_root_binds_chunk_lines_to_their_file()
{
    const auto root = temporary_directory("va-manifest-twins-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    auto first = test::sample_file("x\xEF\xBF\xBD", test::patterned(2 * test_chunk_size, 6), test_chunk_size);
    first.path.enc = "bytes";
    first.path.raw = "78fe";
    auto second = test::sample_file("x\xEF\xBF\xBD", test::patterned(2 * test_chunk_size, 7), test_chunk_size);
    second.path.enc = "bytes";
    second.path.raw = "78ff";
    const auto path = root / "r0001.vam";
    CHECK(write_manifest(*writer, *files, path, test_chunk_size, {first, second}, golden_header()));
    CHECK(!read_to_end(*files, path));

    auto parts = test::split_manifest(read_file(path));
    CHECK(parts.lines.size() == 6);
    if (parts.lines.size() == 6) {
        std::rotate(parts.lines.begin() + 2, parts.lines.begin() + 4, parts.lines.end());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << test::assemble_manifest(parts);
        const auto swapped = read_to_end(*files, path);
        CHECK(swapped && swapped->kind == va::ManifestFailure::Kind::MerkleMismatch);
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// A manifest with a chunk size need not have chunk lines, or leaves: no file
// larger than a chunk, nothing with a content hash, nothing at all.
void chunked_manifests_without_chunk_lines()
{
    const auto root = temporary_directory("va-manifest-unchunked-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    va::Entry directory;
    directory.type = 'd';
    directory.path.p = "d";
    const auto small = test::sample_file("small.txt", "small", test_chunk_size);
    va::MerkleTreeBuilder one_leaf;
    one_leaf.add_hex(small.b3);
    const std::pair<std::vector<va::Entry>, std::string> cases[] = {
        {{directory, small}, one_leaf.root_hex()},
        {{directory}, va::to_hex(va::merkle_empty_root())},
        {{}, va::to_hex(va::merkle_empty_root())},
    };
    for (const auto& [entries, merkle_root] : cases) {
        const auto path = root / "r0001.vam";
        const auto written = write_manifest(*writer, *files, path, test_chunk_size, entries, golden_header());
        CHECK(written && written->header.counts.chunks == 0 && written->header.merkle_root == merkle_root);
        std::vector<va::Entry> read;
        CHECK(!read_to_end(*files, path, &read));
        CHECK(read.size() == entries.size());
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// A manifest without a chunk size, as every V1 manifest, is read as before:
// chunk lines are skipped wherever they are, and merkle_root is ignored.
void manifests_without_a_chunk_size_are_read_as_before()
{
    const auto root = temporary_directory("va-manifest-v1-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    const auto path = root / "r0002.vam";
    const auto written = write_manifest(*writer, *files, path, 0, golden_entries(), golden_header());
    CHECK(written && written->header.chunk_size == 0 && written->header.merkle_root.empty());
    CHECK(!read_file(path).contains("merkle_root"));

    auto parts = test::split_manifest(read_file(path));
    parts.lines.insert(parts.lines.begin() + 1, va::chunk_line_to_json({"a.txt", 0, 0, 5, va::blake3_hex("hello")}));
    parts.lines.push_back("{\"t\":\"c\"}");
    parts.header.merkle_root = va::blake3_hex("anything");
    std::ofstream(path, std::ios::binary | std::ios::trunc) << test::assemble_manifest(parts);
    std::vector<va::Entry> read;
    CHECK(!read_to_end(*files, path, &read));
    const auto entries = golden_entries();
    CHECK(read.size() == entries.size());
    for (std::size_t index = 0; index < read.size() && index < entries.size(); ++index) {
        CHECK(va::entry_to_json(read[index]) == va::entry_to_json(entries[index]));
        CHECK(read[index].chunks.empty());
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// The writer takes chunk hashes for exactly the files that have chunk lines,
// and only a chunk size that the reader accepts.
void writer_takes_chunk_hashes_only_where_they_belong()
{
    const auto root = temporary_directory("va-manifest-guards-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    const auto refused = [&](std::uint64_t chunk_size, const va::Entry& entry) {
        va::ManifestWriter manifest(*writer, *files, root / "r0001.vam", chunk_size);
        const auto added = manifest.add(entry);
        return added ? std::string{} : added.error().message;
    };
    const auto big = test::sample_file("big.bin", test::patterned(2 * test_chunk_size + 1, 1), test_chunk_size);
    CHECK(big.chunks.size() == 3 && refused(test_chunk_size, big).empty());
    auto short_of_one = big;
    short_of_one.chunks.pop_back();
    CHECK(refused(test_chunk_size, short_of_one) == "chunk hashes of big.bin: 2 given, 3 expected");
    CHECK(refused(0, big) == "chunk hashes of big.bin: 3 given, 0 expected");
    auto exact = test::sample_file("exact.bin", test::patterned(test_chunk_size, 2), test_chunk_size);
    exact.chunks = {exact.b3};
    CHECK(refused(test_chunk_size, exact) == "chunk hashes of exact.bin: 1 given, 0 expected");
    auto unreadable = big;
    unreadable.b3.clear();
    unreadable.sha256.clear();
    unreadable.unreadable = "media";
    CHECK(refused(test_chunk_size, unreadable) == "chunk hashes of big.bin: 3 given, 0 expected");
    auto stream = big;
    stream.type = 's';
    stream.stream = "s";
    CHECK(refused(test_chunk_size, stream) == "chunk hashes of big.bin: 3 given, 0 expected");
    auto upper_chunk = big;
    upper_chunk.chunks[1] = uppercase(upper_chunk.chunks[1]);
    CHECK(refused(test_chunk_size, upper_chunk) == "b3 is not 64 lowercase hex digits");
    auto upper_file = test::sample_file("a.txt", "hello", test_chunk_size);
    upper_file.b3 = uppercase(upper_file.b3);
    CHECK(refused(test_chunk_size, upper_file) == "b3 is not 64 lowercase hex digits");
    CHECK(refused(0, upper_file).empty()); // without a chunk size, as before
    const std::string invalid = "chunk_size 1000 is not 0 or a multiple of 65536 up to 1073741824";
    CHECK(refused(1000, test::sample_file("a.txt", "hello", 1000)) == invalid);
    {
        va::ManifestWriter empty(*writer, *files, root / "r0001.vam", 1000);
        const auto finished = empty.finish(golden_header());
        CHECK(!finished && finished.error().message == invalid);
    }
    CHECK(va::valid_chunk_size(0) && va::valid_chunk_size(va::chunk_size_unit) &&
          va::valid_chunk_size(va::default_chunk_size) && va::valid_chunk_size(va::max_chunk_size));
    CHECK(!va::valid_chunk_size(va::chunk_size_unit / 2) &&
          !va::valid_chunk_size(va::max_chunk_size + va::chunk_size_unit));
    CHECK(va::default_chunk_size == 67108864);
    CHECK(files_in(root) == 0);
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// The leaves each kind of entry gives the Merkle tree (§B6): its chunks for
// a file larger than the chunk size; its b3 for a smaller file, a link or a
// stream, whatever its size; none for a directory, even one that carries a
// hash, or for an entry without a content hash.
void merkle_leaves_of_each_entry()
{
    const auto leaves = [](const va::Entry& entry) {
        va::MerkleTreeBuilder tree;
        CHECK(va::add_merkle_leaves(tree, entry, test_chunk_size));
        return tree.leaves();
    };
    const auto big = test::sample_file("big.bin", test::patterned(2 * test_chunk_size + 1, 1), test_chunk_size);
    CHECK(leaves(big) == 3);
    va::MerkleTreeBuilder chunks;
    for (const auto& chunk : big.chunks) {
        chunks.add_hex(chunk);
    }
    va::MerkleTreeBuilder from_entry;
    CHECK(va::add_merkle_leaves(from_entry, big, test_chunk_size) && from_entry.root() == chunks.root());

    const auto exact = test::sample_file("exact.bin", test::patterned(test_chunk_size, 2), test_chunk_size);
    CHECK(exact.chunks.empty() && leaves(exact) == 1);
    auto stream = big;
    stream.type = 's';
    stream.stream = "s";
    stream.chunks.clear();
    CHECK(leaves(stream) == 1);
    auto link = test::sample_file("link", "target", test_chunk_size);
    link.type = 'l';
    CHECK(leaves(link) == 1);
    auto unreadable = big;
    unreadable.b3.clear();
    unreadable.sha256.clear();
    unreadable.chunks.clear();
    unreadable.unreadable = "permission";
    CHECK(leaves(unreadable) == 0);
    auto directory = test::sample_file("dir", "", test_chunk_size);
    directory.type = 'd';
    CHECK(!directory.b3.empty() && leaves(directory) == 0);
}

// After a failure, the reader gives the same failure again rather than go on
// with the entries after it.
void a_failed_reader_stays_failed()
{
    const StrictReading reading;
    const auto root = temporary_directory("va-manifest-failed-");
    const auto files = va::platform::make_file_system_access();
    const auto path = root / "r0003.vam";
    std::ofstream(path, std::ios::binary)
        << test::assemble_manifest(reading.chunk_changed(10, [](va::ChunkLine& chunk) { chunk.p = "big.bim"; }));
    auto reader = va::ManifestReader::open(*files, path);
    CHECK(reader.has_value());
    if (reader) {
        auto first = reader->next(); // a.txt
        CHECK(first && *first);
        const auto failed = reader->next(); // big.bin, whose chunk 1 is out of place
        CHECK(!failed && failed.error().kind == va::ManifestFailure::Kind::Malformed);
        const auto again = reader->next();
        CHECK(!again && !failed && again.error().message == failed.error().message);
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// R-62 with chunk lines: each position's read is held to its stretch of the
// body as open() validated it, so other bytes in an entry (the first
// position's read of the body's start, the second read there) or in a chunk
// line (the second position's, the first read of the chunk lines) fail the
// end of the body with BodyMismatch, before the Merkle root is compared, and
// a chunk line that the second position read otherwise and that no longer
// parses is no malformed manifest either (sample data).
void chunked_entries_are_read_from_the_body_validated()
{
    const auto root = temporary_directory("va-manifest-chunked-second-read-");
    const auto writer = va::platform::make_file_writer();
    const auto files = va::platform::make_file_system_access();
    const auto path = root / "r0001.vam";
    const std::vector<va::Entry> entries{
        test::sample_file("big.bin", test::patterned(2 * test_chunk_size + 7, 1), test_chunk_size),
        test::sample_file("small.txt", "small (sample data)", test_chunk_size)};
    CHECK(write_manifest(*writer, *files, path, test_chunk_size, entries, golden_header()));
    const auto bytes = read_file(path);
    const std::uint64_t body_start = bytes.find('\n') + 1;
    const std::uint64_t chunks_start = bytes.find("{\"t\":\"c\"");
    CHECK(chunks_start != std::string::npos && chunks_start > body_start);

    const auto other = [](std::string hash) {
        hash[0] = hash[0] == '0' ? '1' : '0';
        return hash;
    };
    struct Case {
        std::uint64_t offset; // where a read gives other bytes
        int read;             // which read there
        std::string from;     // what it gives other bytes for
        std::string to;
    };
    const Case cases[] = {{body_start, 2, entries[1].b3, other(entries[1].b3)},
        {chunks_start, 1, entries[0].chunks[1], other(entries[0].chunks[1])},
        {chunks_start, 1, "\"idx\":1", "\"idy\":1"}};
    for (const auto& [offset, read, from, to] : cases) {
        test::SecondReadDiffers differing(*files, "r0001.vam", 1, offset, from, to, read);
        auto reader = va::ManifestReader::open(differing, path);
        CHECK(reader.has_value());
        if (!reader) {
            continue;
        }
        std::optional<va::ManifestFailure> failure;
        for (auto next = reader->next();; next = reader->next()) {
            if (!next) {
                failure = next.error();
                break;
            }
            if (!*next) {
                break;
            }
        }
        CHECK(differing.differed());
        CHECK(failure && failure->kind == va::ManifestFailure::Kind::BodyMismatch &&
              failure->message == "the manifest changed while it was read");
    }

    std::vector<va::Entry> read;
    CHECK(!read_to_end(*files, path, &read));
    CHECK(read.size() == entries.size() && !read.empty() && read[0].chunks == entries[0].chunks);
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

} // namespace

void manifest_tests()
{
    golden_manifest_bytes();
    golden_manifest_with_copy_of();
    golden_chunked_manifest_bytes();
    writers_of_one_revision_keep_their_own_body();
    writers_of_one_revision_keep_their_own_chunk_lines();
    side_files_go_whatever_the_outcome();
    out_of_order_entries_are_rejected();
    malformed_manifests_say_why();
    entries_are_read_from_the_body_validated();
    line_one_is_compared_before_it_is_parsed();
    chunked_manifests_are_read_strictly();
    merkle_root_binds_chunk_lines_to_their_file();
    chunked_manifests_without_chunk_lines();
    manifests_without_a_chunk_size_are_read_as_before();
    writer_takes_chunk_hashes_only_where_they_belong();
    merkle_leaves_of_each_entry();
    a_failed_reader_stays_failed();
    chunked_entries_are_read_from_the_body_validated();
}
