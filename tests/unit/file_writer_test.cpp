#include "test_support.h"

#include "core/write_file.h"
#include "platform/file_writer.h"

#include <chrono>
#include <fstream>
#include <iterator>

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

    std::error_code error;
    std::filesystem::remove_all(root, error);
}
