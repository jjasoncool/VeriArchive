#include "test_support.h"

#include "core/engine/sha256sums.h"

namespace {

va::Entry file(std::string p, std::string sha256)
{
    va::Entry entry;
    entry.path.p = std::move(p);
    entry.sha256 = std::move(sha256);
    return entry;
}

} // namespace

// R-67: output that GNU `sha256sum -c` accepts.
void sha256sums_tests()
{
    const std::string hash(64, 'a');
    std::vector<va::Entry> entries{
        file("dir/report.pdf", hash),
        file("back\\slash", hash),
        file("new\nline", hash),
    };
    va::Entry directory;
    directory.type = 'd';
    directory.path.p = "empty";
    entries.push_back(directory);
    entries.push_back(file("unreadable.bin", ""));
    auto raw = file("x\xEF\xBF\xBD", hash);
    raw.path.enc = "bytes";
    raw.path.raw = "78ff";
    entries.push_back(raw);
    auto windows_raw = raw;
    windows_raw.path.enc = "utf16le";
    entries.push_back(windows_raw);

    const auto sums = va::sha256sums(entries);
    const std::string expected = hash + "  dir/report.pdf\n" +
                                 "\\" + hash + "  back\\\\slash\n" +
                                 "\\" + hash + "  new\\nline\n" +
                                 hash + "  x\xFF\n";
    CHECK(sums.text == expected);
    CHECK(sums.skipped.size() == 2);
}
