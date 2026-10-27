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
    options.buffer_size = 4096;   // many chunks per file
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

} // namespace

void hash_pipeline_tests()
{
    the_rotational_override_goes_by_serial();
    every_file_is_hashed_once();
    read_failures_are_reported_per_file();
    stopping_and_cancelling();
    progress_counts_bytes_against_the_sizes();
    cancelled_progress_keeps_the_sizes_not_reached();
    progress_moves_within_a_large_file();
}
