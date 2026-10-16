#include "test_support.h"

#include "common/utf8.h"
#include "platform/file_system.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>

namespace {

// Reads a whole file the way the hash pipeline will: aligned buffer, aligned
// offsets, until a short read.
std::string read_all(va::IByteSource& source)
{
    const std::size_t alignment = source.alignment();
    const std::size_t chunk = 64 * 1024;
    std::vector<std::byte> storage(chunk + alignment);
    void* pointer = storage.data();
    std::size_t space = storage.size();
    auto* aligned = static_cast<std::byte*>(std::align(alignment, chunk, pointer, space));

    std::string result;
    std::uint64_t offset = 0;
    for (;;) {
        const auto count = source.read(offset, std::span(aligned, chunk));
        if (!count) {
            return "<error>";
        }
        result.append(reinterpret_cast<const char*>(aligned), *count);
        offset += *count;
        if (*count < chunk) {
            return result;
        }
    }
}

} // namespace

void fs_access_tests()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("va-fs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "sub");
    std::filesystem::create_directories(root / "emptydir");
    std::string content(100000, '\0');
    for (std::size_t index = 0; index < content.size(); ++index) {
        content[index] = static_cast<char>((index * 131) ^ (index >> 7));
    }
    std::ofstream(root / "a.txt", std::ios::binary) << content;
    std::ofstream(root / "empty.bin", std::ios::binary).close();
    std::ofstream(root / "sub" / "b.txt", std::ios::binary) << "b";

    const auto access = va::platform::make_file_system_access();
    const auto listed = access->list(root);
    CHECK(listed.has_value());
    if (listed) {
        std::map<std::string, va::DirEntry> by_name;
        for (const auto& entry : *listed) {
            by_name.emplace(va::path_to_utf8(entry.native_name), entry);
        }
        CHECK(by_name.size() == 4);
        CHECK(by_name.contains("a.txt") && by_name.at("a.txt").type == va::EntryType::File);
        CHECK(by_name.contains("a.txt") && by_name.at("a.txt").size == content.size());
        CHECK(by_name.contains("sub") && by_name.at("sub").type == va::EntryType::Directory);
        CHECK(by_name.contains("emptydir") && !by_name.at("emptydir").is_mount_point);
        CHECK(by_name.contains("a.txt") && by_name.at("a.txt").mtime_ns > 0);
    }
    // The directory given with a separator at its end, as Windows reports a
    // volume mounted in a folder, lists the same entries.
    const auto with_separator = access->list(root / "");
    CHECK(with_separator && listed && with_separator->size() == listed->size());

    // Uncached read (R-100) returns exactly the file's bytes, including the
    // final partial sector.
    auto source = access->open(root / "a.txt", va::ReadOptions{});
    CHECK(source.has_value());
    if (source) {
        CHECK((*source)->size() == content.size());
        CHECK(read_all(**source) == content);
    }
    auto empty = access->open(root / "empty.bin", va::ReadOptions{});
    CHECK(empty && read_all(**empty).empty());

    const auto missing = access->open(root / "missing", va::ReadOptions{});
    CHECK(!missing && missing.error().kind == va::IoErrorKind::NotFound);

    // A link to a directory is listed as a link and never followed; its target
    // uses '/' separators (decision 0008).
    bool created = false;
    if (std::filesystem::path::preferred_separator == '\\') {
        const auto command = "mklink /J \"" + va::path_to_utf8(root / "link") + "\" \"" +
                             va::path_to_utf8(root / "sub") + "\" >NUL";
        created = std::system(command.c_str()) == 0;
    } else {
        std::error_code error;
        std::filesystem::create_directory_symlink("sub", root / "link", error);
        created = !error;
    }
    CHECK(created);
    if (created) {
        const auto relisted = access->list(root);
        const auto link = relisted ? std::find_if(relisted->begin(), relisted->end(),
                                         [](const va::DirEntry& entry) {
                                             return va::path_to_utf8(entry.native_name) == "link";
                                         })
                                   : std::vector<va::DirEntry>::const_iterator{};
        CHECK(relisted && link != relisted->end() && link->type == va::EntryType::Link);
        const auto target = access->read_link(root / "link");
        CHECK(target && target->ends_with("sub") && target->find('\\') == std::string::npos);
    }

    std::error_code error;
    std::filesystem::remove_all(root, error);
}
