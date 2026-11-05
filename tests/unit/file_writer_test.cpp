#include "test_support.h"

#include "core/write_file.h"
#include "platform/file_writer.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <string_view>

namespace {

std::string read_all(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::size_t count_entries(const std::filesystem::path& directory)
{
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(directory)) {
        ++count;
    }
    return count;
}

} // namespace

void file_writer_tests()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("va-writer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto target = root / "repository.json";
    const auto writer = va::platform::make_file_writer();

    CHECK(va::write_file_atomically(*writer, target, "one"));
    CHECK(read_all(target) == "one");

    // An abandoned write leaves the old contents and no temporary file behind.
    {
        auto file = writer->create(target);
        CHECK(file.has_value());
        if (file) {
            const std::string text = "two";
            CHECK((*file)->write(std::as_bytes(std::span(text.data(), text.size()))));
        }
    }
    CHECK(read_all(target) == "one");
    CHECK(count_entries(root) == 1);

    // A committed write replaces the destination.
    CHECK(va::write_file_atomically(*writer, target, "three"));
    CHECK(read_all(target) == "three");
    CHECK(count_entries(root) == 1);

    // Decision 0023: flush() makes the bytes durable without giving them the
    // destination's name; the commit after it renames, also with bytes
    // written after the flush.
    const auto bytes = [](std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); };
    {
        auto file = writer->create(target);
        CHECK(file.has_value());
        if (file) {
            CHECK((*file)->write(bytes("fou")));
            CHECK((*file)->flush());
            CHECK(read_all(target) == "three");
            CHECK(count_entries(root) == 2); // the temporary file beside it
            CHECK((*file)->write(bytes("r")));
            CHECK((*file)->commit());
        }
    }
    CHECK(read_all(target) == "four");
    CHECK(count_entries(root) == 1);
    // A flushed file that is never committed leaves nothing behind.
    {
        auto file = writer->create(target);
        CHECK(file && (*file)->write(bytes("five")) && (*file)->flush());
    }
    CHECK(read_all(target) == "four");
    CHECK(count_entries(root) == 1);

    CHECK(writer->remove(target));
    CHECK(!std::filesystem::exists(target));
    const auto missing = writer->remove(target);
    CHECK(!missing && missing.error().kind == va::IoErrorKind::NotFound);

    // On Windows a folder given in the \\?\ form, as it may be typed for
    // export --out, is the same folder: the file is written there and
    // removed from there.
    if (std::filesystem::path::preferred_separator == '\\') {
        auto verbatim = std::filesystem::path(R"(\\?\)");
        verbatim += root;
        CHECK(va::write_file_atomically(*writer, verbatim / "sums.txt", "five"));
        CHECK(read_all(root / "sums.txt") == "five");
        CHECK(count_entries(root) == 1);
        CHECK(writer->remove(verbatim / "sums.txt"));
        CHECK(!std::filesystem::exists(root / "sums.txt"));
    }

    // Decision 0048: commit_new() names the file as commit() does, but only
    // where nothing of that name is there when it is named, as `report`
    // writes its FILE. Sample data: SAMPLE bytes in a folder of the temporary
    // folder above, which the test makes and removes.
    const auto reports = root / "SAMPLE-reports";
    std::filesystem::create_directories(reports);
    const auto first = reports / "SAMPLE report 1.html";
    {
        auto file = writer->create(first);
        CHECK(file && (*file)->write(bytes("SAMPLE one")) && (*file)->commit_new());
    }
    CHECK(read_all(first) == "SAMPLE one");
    CHECK(count_entries(reports) == 1);
    // A file that another program makes under that name after create() is
    // left as it is, and the temporary file goes at once, before the output
    // file itself goes.
    const auto second = reports / "SAMPLE report 2.html";
    {
        auto file = writer->create(second);
        CHECK(file && (*file)->write(bytes("SAMPLE report")));
        std::ofstream(second, std::ios::binary) << "SAMPLE made meanwhile";
        if (file) {
            const auto refused = (*file)->commit_new();
            CHECK(!refused && refused.error().kind == va::IoErrorKind::AlreadyExists);
        }
        CHECK(read_all(second) == "SAMPLE made meanwhile");
        CHECK(count_entries(reports) == 2);
    }
    CHECK(count_entries(reports) == 2);
    // After a flush, the commit only renames (decision 0023), and refuses as
    // well: here a folder of that name.
    const auto folder = reports / "SAMPLE folder.html";
    std::filesystem::create_directories(folder);
    {
        auto file = writer->create(folder);
        CHECK(file && (*file)->write(bytes("SAMPLE flushed")) && (*file)->flush());
        if (file) {
            const auto refused = (*file)->commit_new();
            CHECK(!refused && refused.error().kind == va::IoErrorKind::AlreadyExists);
        }
        CHECK(count_entries(reports) == 3); // the temporary file is gone at once
    }
    CHECK(std::filesystem::is_directory(folder) && count_entries(folder) == 0 && count_entries(reports) == 3);
    // write_new_file_atomically: the same, in one call.
    const auto refused = va::write_new_file_atomically(*writer, first, "SAMPLE two");
    CHECK(!refused && refused.error().kind == va::IoErrorKind::AlreadyExists);
    CHECK(read_all(first) == "SAMPLE one");
    const auto third = reports / "SAMPLE report 3.html";
    CHECK(va::write_new_file_atomically(*writer, third, "SAMPLE three"));
    CHECK(read_all(third) == "SAMPLE three");
    CHECK(count_entries(reports) == 4);

    std::error_code error;
    std::filesystem::remove_all(root, error);
}
