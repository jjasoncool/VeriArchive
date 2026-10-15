#include "test_support.h"

#include "fakes.h"

#include "core/hash/hash_pipeline.h"
#include "core/hash/hashers.h"

#include <map>

namespace {

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

} // namespace

void hash_pipeline_tests()
{
    every_file_is_hashed_once();
    read_failures_are_reported_per_file();
    stopping_and_cancelling();
}
