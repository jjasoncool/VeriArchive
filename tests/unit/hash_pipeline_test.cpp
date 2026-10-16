#include "test_support.h"

#include "fakes.h"

#include "core/hash/hash_pipeline.h"
#include "core/hash/hashers.h"

#include <chrono>
#include <map>
#include <thread>

namespace {

class RecordingProgress final : public va::IProgressSink {
public:
    void on_progress(const va::Progress& progress) override { reports.push_back(progress); }
    std::vector<va::Progress> reports;
};

// Reads like `inner`, but each read takes 10 ms, as from a slow device.
class SlowFileSystem final : public va::IFileSystemAccess {
public:
    explicit SlowFileSystem(va::IFileSystemAccess& inner) : inner_(inner) {}

    std::expected<std::vector<va::DirEntry>, va::IoError> list(const std::filesystem::path& directory) override
    {
        return inner_.list(directory);
    }
    std::expected<std::unique_ptr<va::IByteSource>, va::IoError> open(
        const std::filesystem::path& file, va::ReadOptions options) override
    {
        auto source = inner_.open(file, options);
        if (!source) {
            return std::unexpected(source.error());
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
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return inner_->read(offset, output);
        }
        std::uint64_t size() const override { return inner_->size(); }
        std::size_t alignment() const override { return inner_->alignment(); }

    private:
        std::unique_ptr<va::IByteSource> inner_;
    };
    va::IFileSystemAccess& inner_;
};

// Reads like `inner`, but gives fewer bytes than asked for, in a repeating
// pattern of odd sizes: a buffered read may do that before the end of a file.
class ShortReadFileSystem final : public va::IFileSystemAccess {
public:
    explicit ShortReadFileSystem(va::IFileSystemAccess& inner) : inner_(inner) {}

    std::expected<std::vector<va::DirEntry>, va::IoError> list(const std::filesystem::path& directory) override
    {
        return inner_.list(directory);
    }
    std::expected<std::unique_ptr<va::IByteSource>, va::IoError> open(
        const std::filesystem::path& file, va::ReadOptions options) override
    {
        auto source = inner_.open(file, options);
        if (!source) {
            return std::unexpected(source.error());
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
            static constexpr std::size_t pattern[] = {1, 4093, 65537, 7, 30000, 65535};
            const auto most = pattern[reads_++ % std::size(pattern)];
            return inner_->read(offset, output.first(std::min(output.size(), most)));
        }
        std::uint64_t size() const override { return inner_->size(); }
        std::size_t alignment() const override { return inner_->alignment(); }

    private:
        std::unique_ptr<va::IByteSource> inner_;
        std::size_t reads_ = 0;
    };
    va::IFileSystemAccess& inner_;
};

std::string content_of(std::size_t index)
{
    std::string content(index * 9973 % 300000, '\0');
    for (std::size_t offset = 0; offset < content.size(); ++offset) {
        content[offset] = static_cast<char>(offset * 7 + index);
    }
    return content;
}

va::HashPipelineOptions small_buffers(std::size_t readers)
{
    va::HashPipelineOptions options;
    options.readers = readers;
    options.buffer_size = 4096;   // many buffers per file
    options.buffers_per_reader = 2;
    return options;
}

void every_file_is_hashed_once()
{
    test::FakeFileSystem files("/archive");
    std::vector<va::HashJob> jobs;
    for (std::size_t index = 0; index < 40; ++index) {
        const auto name = "file" + std::to_string(index);
        files.add_file(name, content_of(index));
        jobs.push_back({index, files.root() / name});
    }

    va::CancelToken cancel;
    test::NullProgress progress;
    std::map<std::size_t, va::HashResult> results;
    const auto status = va::hash_files(files, jobs, small_buffers(3), cancel, progress,
        [&](va::HashResult&& result) {
            CHECK(!results.contains(result.id));
            results[result.id] = std::move(result);
            return true;
        });

    CHECK(status.has_value());
    CHECK(results.size() == jobs.size());
    for (const auto& [id, result] : results) {
        const auto expected = content_of(id);
        CHECK(!result.error);
        CHECK(result.size == expected.size());
        CHECK(result.b3 == va::blake3_hex(expected));
        CHECK(result.sha256 == va::sha256_hex(expected));
    }
}

void read_failures_are_reported_per_file()
{
    test::FakeFileSystem files("/archive");
    files.add_file("good", "fine");
    files.add_file("bad-sector", std::string(100000, 'x')).read_error_offset = 50000;
    files.add_file("denied", "secret").open_error =
        va::IoError{va::IoErrorKind::PermissionDenied, 5, "denied"};
    const std::vector<va::HashJob> jobs{
        {0, files.root() / "good"}, {1, files.root() / "bad-sector"}, {2, files.root() / "denied"}};

    va::CancelToken cancel;
    test::NullProgress progress;
    std::map<std::size_t, va::HashResult> results;
    CHECK(va::hash_files(files, jobs, small_buffers(2), cancel, progress, [&](va::HashResult&& result) {
        results[result.id] = std::move(result);
        return true;
    }));
    CHECK(results.size() == 3);
    CHECK(!results[0].error && results[0].b3 == va::blake3_hex("fine"));
    CHECK(results[1].error && results[1].error->kind == va::IoErrorKind::MediaError);
    CHECK(results[2].error && results[2].error->kind == va::IoErrorKind::PermissionDenied);
}

// A file whose opening fails with a Cancelled error that the pipeline did not
// cause (a file system that reports one; neither platform does, decision 0034)
// is that file's result like any other failure: it is delivered, and the bytes
// it did not read leave the total to hash. A run ends without the result of a
// file only by stopping. Sample data.
void a_cancelled_error_the_pipeline_did_not_cause_is_delivered()
{
    test::FakeFileSystem files("/archive");
    files.add_file("SAMPLE-good", "fine");
    files.add_file("SAMPLE-aborted", "never read").open_error =
        va::IoError{va::IoErrorKind::Cancelled, 995, "aborted (sample)"};
    const std::vector<va::HashJob> jobs{
        {0, files.root() / "SAMPLE-good", 4}, {1, files.root() / "SAMPLE-aborted", 10}};

    va::CancelToken cancel;
    RecordingProgress progress;
    std::map<std::size_t, va::HashResult> results;
    CHECK(va::hash_files(files, jobs, small_buffers(2), cancel, progress, [&](va::HashResult&& result) {
        results[result.id] = std::move(result);
        return true;
    }));
    CHECK(results.size() == 2);
    CHECK(results[1].error && results[1].error->native_code == 995);
    CHECK(!progress.reports.empty());
    const auto& last = progress.reports.back();
    CHECK(last.items_done == 2 && last.bytes_done == 4 && last.bytes_total == 4);
}

void stopping_and_cancelling()
{
    test::FakeFileSystem files("/archive");
    std::vector<va::HashJob> jobs;
    for (std::size_t index = 0; index < 30; ++index) {
        const auto name = "f" + std::to_string(index);
        files.add_file(name, content_of(index + 1));
        jobs.push_back({index, files.root() / name});
    }
    test::NullProgress progress;

    va::CancelToken running;
    std::size_t delivered = 0;
    auto status = va::hash_files(files, jobs, small_buffers(2), running, progress,
        [&](va::HashResult&&) { return ++delivered < 5; });
    CHECK(!status && status.error().kind == va::IoErrorKind::Cancelled);
    CHECK(delivered == 5); // nothing after the consumer said stop, however fast the readers were

    va::CancelToken cancelled;
    cancelled.cancel();
    delivered = 0;
    status = va::hash_files(files, jobs, small_buffers(2), cancelled, progress,
        [&](va::HashResult&&) { return ++delivered > 0; });
    CHECK(!status && delivered == 0);

    CHECK(va::default_reader_count(true) == 1);
    CHECK(va::default_reader_count(std::nullopt) == 1);
    CHECK(va::default_reader_count(false) >= 1 && va::default_reader_count(false) <= 8);
}

// Progress counts bytes against the sum of the jobs' sizes, and that total
// follows what was really read: a file shorter than expected, or one that
// could not be read to its end, leaves only the bytes read. At the end the
// bytes done equal the total, so the share done ends at 100 %.
void progress_counts_bytes_against_the_sizes()
{
    test::FakeFileSystem files("/archive");
    files.add_file("a", std::string(10000, 'a'));
    files.add_file("b", std::string(20000, 'b'));
    files.add_file("bad-sector", std::string(100000, 'x')).read_error_offset = 50000;
    files.add_file("shrunk", std::string(1000, 's'));
    const std::vector<va::HashJob> jobs{{0, files.root() / "a", 10000}, {1, files.root() / "b", 20000},
        {2, files.root() / "bad-sector", 100000}, {3, files.root() / "shrunk", 5000}};

    va::CancelToken cancel;
    RecordingProgress progress;
    std::uint64_t read = 0;
    CHECK(va::hash_files(files, jobs, small_buffers(2), cancel, progress, [&](va::HashResult&& result) {
        read += result.size;
        return true;
    }));
    CHECK(!progress.reports.empty());
    for (const auto& report : progress.reports) {
        CHECK(report.items_total == 4);
        CHECK(report.bytes_done <= report.bytes_total);
        CHECK(report.bytes_total <= 135000);
    }
    const auto& last = progress.reports.back();
    CHECK(last.items_done == 4);
    CHECK(read > 10000 + 20000 + 1000 && read < 10000 + 20000 + 50000 + 1000); // the bad sector stopped one
    CHECK(last.bytes_done == read);
    CHECK(last.bytes_total == read);

    // Without sizes the total is unknown, and progress counts files only.
    const std::vector<va::HashJob> unsized{{0, files.root() / "a"}, {1, files.root() / "b"}};
    RecordingProgress items_only;
    CHECK(va::hash_files(files, unsized, small_buffers(1), cancel, items_only, [](va::HashResult&&) { return true; }));
    CHECK(!items_only.reports.empty() && items_only.reports.back().bytes_total == 0);
    CHECK(items_only.reports.back().bytes_done == 30000);
    CHECK(items_only.reports.back().items_done == 2);
}

// A cancel leaves the total honest: a finished file counts the bytes read, any
// other (stopped halfway, or not started) its expected size, as its bytes were
// not reached. So the bytes done stay below the total after a cancel, and the
// share done does not claim the work was finished.
void cancelled_progress_keeps_the_sizes_not_reached()
{
    test::FakeFileSystem fake("/archive");
    std::vector<va::HashJob> jobs;
    for (std::size_t index = 0; index < 10; ++index) {
        const auto name = "f" + std::to_string(index);
        fake.add_file(name, std::string(4096 * 10, 'c')); // 10 reads of 10 ms each
        jobs.push_back({index, fake.root() / name, 4096 * 10});
    }
    jobs[0].size = 50000; // the first file shrank since the scan
    SlowFileSystem files(fake);

    va::CancelToken cancel;
    RecordingProgress progress;
    std::map<std::size_t, std::uint64_t> delivered; // id -> bytes read
    const auto status = va::hash_files(files, jobs, small_buffers(1), cancel, progress,
        [&](va::HashResult&& result) {
            delivered[result.id] = result.size;
            cancel.cancel(); // the operator cancels after the first file
            return true;
        });
    CHECK(!status && status.error().kind == va::IoErrorKind::Cancelled);
    CHECK(delivered.contains(0) && delivered.size() < jobs.size());
    std::uint64_t expected_total = 0;
    std::uint64_t read = 0;
    for (const auto& job : jobs) {
        const auto found = delivered.find(job.id);
        expected_total += found != delivered.end() ? found->second : job.size;
        read += found != delivered.end() ? found->second : 0;
    }
    CHECK(!progress.reports.empty());
    if (progress.reports.empty()) {
        return;
    }
    for (const auto& report : progress.reports) {
        CHECK(report.bytes_done <= report.bytes_total);
    }
    const auto& last = progress.reports.back();
    CHECK(last.items_done == delivered.size());
    CHECK(last.bytes_done >= read);           // and what the stopped file had read
    CHECK(last.bytes_done < last.bytes_total);
    CHECK(last.bytes_total == expected_total);
}

// A large file shows its bytes while it is read, not only when it is done:
// on an HDD one file can take many minutes.
void progress_moves_within_a_large_file()
{
    test::FakeFileSystem fake("/archive");
    fake.add_file("large", std::string(4096 * 100, 'L')); // 100 reads of 10 ms each
    SlowFileSystem files(fake);
    const std::vector<va::HashJob> jobs{{0, fake.root() / "large", 4096 * 100}};

    va::CancelToken cancel;
    RecordingProgress progress;
    CHECK(va::hash_files(files, jobs, small_buffers(1), cancel, progress, [](va::HashResult&&) { return true; }));
    std::size_t during = 0;
    for (const auto& report : progress.reports) {
        during += report.items_done == 0 && report.bytes_done > 0 && report.bytes_done < 4096 * 100;
    }
    CHECK(during >= 2);
    CHECK(progress.reports.back().bytes_done == 4096 * 100);
}

// R-110, R-111 (decision 0034): whether a volume's devices are rotational,
// which sets the readers: --io wins; then the config's io.rotational_override
// for a device's serial, compared as identity matching compares serials (case,
// spaces and underscores aside); then what the platform detected. A device
// without a serial has nothing to look up. Sample serials.
void the_rotational_override_goes_by_serial()
{
    va::VolumeInfo volume;
    va::PhysicalDeviceInfo bridge;
    bridge.serial = "SAMPLE-BRIDGE 01";
    bridge.rotational = false; // the bridge says SSD
    volume.devices.push_back(bridge);
    const std::map<std::string, bool> none;
    const std::map<std::string, bool> hdd{{"sample-bridge_01", true}};
    const std::map<std::string, bool> ssd{{"SAMPLE-BRIDGE 01", false}};
    const std::map<std::string, bool> another{{"SAMPLE-OTHER", true}};
    CHECK(va::volume_rotational(volume, std::nullopt, none) == false);
    CHECK(va::volume_rotational(volume, std::nullopt, hdd) == true);
    CHECK(va::volume_rotational(volume, std::nullopt, another) == false);
    CHECK(va::volume_rotational(volume, false, hdd) == false); // --io ssd
    CHECK(va::volume_rotational(volume, true, ssd) == true);   // --io hdd
    volume.devices.front().rotational.reset(); // the bridge says nothing
    CHECK(!va::volume_rotational(volume, std::nullopt, none));
    CHECK(va::volume_rotational(volume, std::nullopt, ssd) == false);
    // Of two devices (a volume over two disks), rotational when either is.
    va::PhysicalDeviceInfo second;
    second.serial = "SAMPLE-SECOND";
    second.rotational = false;
    volume.devices.push_back(second);
    CHECK(va::volume_rotational(volume, std::nullopt, hdd) == true);
    CHECK(va::volume_rotational(volume, std::nullopt, ssd) == false);
    volume.devices.front().serial.clear();
    CHECK(va::volume_rotational(volume, std::nullopt, hdd) == false);
    CHECK(va::default_reader_count(true) == 1 && va::default_reader_count(std::nullopt) == 1);
}

constexpr std::uint64_t test_chunk_size = 64 * 1024;

// Bytes that differ in every chunk, so a hash of the wrong bytes shows.
std::string patterned(std::size_t size, std::size_t seed)
{
    std::string content(size, '\0');
    for (std::size_t offset = 0; offset < size; ++offset) {
        content[offset] = static_cast<char>((offset * 131 + offset / 997 + seed) & 0xFF);
    }
    return content;
}

// The chunk hashes of `content` straight from their definition: chunk n is
// the bytes from n * chunk_size; a file no larger than one chunk has none
// (R-31).
std::vector<std::string> chunks_of(const std::string& content)
{
    std::vector<std::string> chunks;
    if (content.size() <= test_chunk_size) {
        return chunks;
    }
    for (std::size_t offset = 0; offset < content.size(); offset += test_chunk_size) {
        chunks.push_back(va::blake3_hex(content.substr(offset, test_chunk_size)));
    }
    return chunks;
}

// Sizes around the chunk boundaries, one file each.
const std::vector<std::size_t>& boundary_sizes()
{
    static const std::vector<std::size_t> sizes{0, 1, test_chunk_size - 1, test_chunk_size,
        test_chunk_size + 1, 2 * test_chunk_size, 2 * test_chunk_size + 1, 3 * test_chunk_size + 12345};
    return sizes;
}

// Hashes the boundary files through `files` and checks every result against
// the definition: the chunks and, unchanged, the whole file's hashes.
void check_boundary_files(test::FakeFileSystem& fake, va::IFileSystemAccess& files,
    std::size_t buffer_size, std::size_t readers)
{
    std::vector<va::HashJob> jobs;
    std::vector<std::string> contents;
    for (std::size_t index = 0; index < boundary_sizes().size(); ++index) {
        const auto name = "chunked" + std::to_string(index);
        contents.push_back(patterned(boundary_sizes()[index], index));
        fake.add_file(name, contents.back());
        jobs.push_back({index, fake.root() / name, contents.back().size()});
    }
    auto options = small_buffers(readers);
    options.buffer_size = buffer_size;
    options.chunk_size = test_chunk_size;

    va::CancelToken cancel;
    test::NullProgress progress;
    std::map<std::size_t, va::HashResult> results;
    CHECK(va::hash_files(files, jobs, options, cancel, progress, [&](va::HashResult&& result) {
        results[result.id] = std::move(result);
        return true;
    }));
    CHECK(results.size() == jobs.size());
    for (const auto& [id, result] : results) {
        const auto& content = contents[id];
        const bool right = !result.error && result.size == content.size() &&
                           result.b3 == va::blake3_hex(content) && result.sha256 == va::sha256_hex(content) &&
                           result.chunks == chunks_of(content);
        CHECK(right);
        if (!right) {
            std::cerr << "  file of " << content.size() << " bytes, buffers of " << buffer_size << '\n';
        }
    }
}

// Chunks end at multiples of the chunk size from the start of the file, not
// where a buffer ends: buffers that divide the chunk size, that do not, and
// that hold more than one chunk give the same hashes.
void chunks_are_cut_at_file_offsets()
{
    for (const std::size_t buffer_size : {std::size_t{4096}, std::size_t{10000}, std::size_t{200000}}) {
        test::FakeFileSystem files("/archive");
        check_boundary_files(files, files, buffer_size, 2);
    }
}

// Nor where a read ends: reads that return fewer bytes than asked for (but
// more than nothing) move no chunk boundary.
void short_reads_do_not_move_chunk_boundaries()
{
    test::FakeFileSystem fake("/archive");
    ShortReadFileSystem files(fake);
    check_boundary_files(fake, files, 100000, 1);
}

// Without a chunk size (V1), and for a file that could not be read to its
// end, there are no chunk hashes.
void no_chunk_hashes_without_a_chunk_size_or_a_whole_file()
{
    test::FakeFileSystem files("/archive");
    files.add_file("large", patterned(3 * test_chunk_size, 1));
    files.add_file("bad-sector", patterned(3 * test_chunk_size, 2)).read_error_offset = 2 * test_chunk_size + 5;
    const std::vector<va::HashJob> jobs{{0, files.root() / "large"}, {1, files.root() / "bad-sector"}};

    va::CancelToken cancel;
    test::NullProgress progress;
    std::map<std::size_t, va::HashResult> results;
    const auto collect = [&](va::HashResult&& result) {
        results[result.id] = std::move(result);
        return true;
    };
    CHECK(va::hash_files(files, jobs, small_buffers(1), cancel, progress, collect));
    CHECK(results.size() == 2 && !results[0].error && results[0].chunks.empty());

    auto options = small_buffers(1);
    options.chunk_size = test_chunk_size;
    results.clear();
    CHECK(va::hash_files(files, jobs, options, cancel, progress, collect));
    CHECK(results.size() == 2 && results[0].chunks.size() == 3);
    CHECK(results[1].error && results[1].error->kind == va::IoErrorKind::MediaError);
    CHECK(results[1].chunks.empty() && results[1].b3.empty());
}

} // namespace

void hash_pipeline_tests()
{
    the_rotational_override_goes_by_serial();
    every_file_is_hashed_once();
    read_failures_are_reported_per_file();
    a_cancelled_error_the_pipeline_did_not_cause_is_delivered();
    stopping_and_cancelling();
    progress_counts_bytes_against_the_sizes();
    cancelled_progress_keeps_the_sizes_not_reached();
    progress_moves_within_a_large_file();
    chunks_are_cut_at_file_offsets();
    short_reads_do_not_move_chunk_boundaries();
    no_chunk_hashes_without_a_chunk_size_or_a_whole_file();
}
