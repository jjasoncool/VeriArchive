#include "test_support.h"

#include "fakes.h"

#include "core/hash/hashers.h"
#include "core/scan/scanner.h"

#include <algorithm>

namespace {

std::vector<std::string> paths(const va::ScanResult& result)
{
    std::vector<std::string> names;
    for (const auto& scanned : result.entries) {
        names.push_back(std::string(1, scanned.entry.type) + ":" + scanned.entry.path.p);
    }
    return names;
}

const va::Entry* find(const va::ScanResult& result, const std::string& p)
{
    const auto found = std::find_if(result.entries.begin(), result.entries.end(),
        [&](const va::ScannedEntry& scanned) { return scanned.entry.path.p == p; });
    return found == result.entries.end() ? nullptr : &found->entry;
}

va::ScanResult scan(test::FakeFileSystem& files)
{
    va::CancelToken cancel;
    test::NullProgress progress;
    auto result = va::scan_tree(files, files.root(), va::ScanOptions{}, cancel, progress);
    CHECK(result.has_value());
    return result ? std::move(*result) : va::ScanResult{};
}

// R-43 order, R-49 empty directories, links never followed.
void tree_is_recorded_in_canonical_order()
{
    test::FakeFileSystem files("/archive");
    files.add_file("b.txt", "b");
    files.add_file("a.txt", "a");
    files.add_file("dir/c.txt", "c");
    files.add_directory("empty");
    files.add_link("link", "dir");

    const auto result = scan(files);
    CHECK(paths(result) ==
          (std::vector<std::string>{"f:a.txt", "f:b.txt", "f:dir/c.txt", "d:empty", "l:link"}));
    const auto* a = find(result, "a.txt");
    CHECK(a && a->size == 1u && a->b3.empty()); // content is hashed later, not by the scanner
    const auto* link = find(result, "link");
    CHECK(link && link->b3 == va::blake3_hex("dir") && link->size == 3u);
}

// R-54..R-56: default exclusions match directories at the archive root only.
void exclusions_apply_at_root_only()
{
    test::FakeFileSystem files("/archive");
    files.add_file("$RECYCLE.BIN/x", "x");
    files.add_file(".Trash-1000/y", "y");
    files.add_file("System Volume Information/z", "z");
    files.add_file("sub/lost+found/w", "w");
    files.add_file(".veriarchive", "a file, not the excluded directory");

    const auto result = scan(files);
    CHECK(paths(result) == (std::vector<std::string>{"f:.veriarchive", "f:sub/lost+found/w"}));
}

// R-30: mount points are reported and not descended.
void mount_points_are_skipped()
{
    test::FakeFileSystem files("/archive");
    files.add_directory("vol/mnt").mount_point = true;
    files.add_file("vol/mnt/inside.txt", "other volume");

    const auto result = scan(files);
    CHECK(result.skipped_mount_points.size() == 1 &&
          result.skipped_mount_points.front().p == "vol/mnt");
    CHECK(paths(result) == (std::vector<std::string>{"d:vol"}));
}

// R-10, decisions 0009, 0010 and 0014: problems become unreadable entries.
void problems_become_unreadable_entries()
{
    test::FakeFileSystem files("/archive");
    files.add_directory("locked").list_error =
        va::IoError{va::IoErrorKind::PermissionDenied, 5, "denied"};
    files.add_file("locked/hidden.txt", "never listed");
    files.add_file("fifo", "").type = va::EntryType::Special;
    files.add_file("online-only.docx", "").attrs = 0x00400000u;

    const auto result = scan(files);
    const auto* locked = find(result, "locked");
    CHECK(locked && locked->type == 'd' && locked->unreadable == "permission");
    CHECK(!find(result, "locked/hidden.txt"));
    const auto* fifo = find(result, "fifo");
    CHECK(fifo && fifo->type == 'f' && fifo->unreadable == "other");
    const auto* placeholder = find(result, "online-only.docx");
    CHECK(placeholder && placeholder->unreadable == "other");
}

// R-42: portability warnings, never failures.
void collisions_are_warned()
{
    test::FakeFileSystem files("/archive");
    files.add_file("Caf\xC3\xA9.txt", "composed");
    files.add_file("Cafe\xCC\x81.txt", "decomposed");
    files.add_file("README", "upper");
    files.add_file("readme", "lower");
    files.add_file("unique", "");

    const auto result = scan(files);
    const auto has = [&](const std::string& p, const std::string& warning) {
        const auto* entry = find(result, p);
        return entry && std::find(entry->warn.begin(), entry->warn.end(), warning) != entry->warn.end();
    };
    CHECK(has("Caf\xC3\xA9.txt", "nfc-collision"));
    CHECK(has("Cafe\xCC\x81.txt", "nfc-collision"));
    CHECK(has("README", "case-collision"));
    CHECK(has("readme", "case-collision"));
    const auto* unique = find(result, "unique");
    CHECK(unique && unique->warn.empty());
}

// Decision 0008: absolute targets are flagged because only their hash is stored.
void absolute_link_targets_are_flagged()
{
    test::FakeFileSystem files("/archive");
    files.add_link("absolute", "C:/Data");
    files.add_link("relative", "../Data");

    const auto result = scan(files);
    const auto* absolute = find(result, "absolute");
    CHECK(absolute && absolute->warn == std::vector<std::string>{"link-absolute"});
    const auto* relative = find(result, "relative");
    CHECK(relative && relative->warn.empty());
}

void cancellation_and_root_failure()
{
    test::FakeFileSystem files("/archive");
    files.add_file("a", "a");
    va::CancelToken cancel;
    cancel.cancel();
    test::NullProgress progress;
    auto result = va::scan_tree(files, files.root(), va::ScanOptions{}, cancel, progress);
    CHECK(!result && result.error().kind == va::IoErrorKind::Cancelled);

    va::CancelToken running;
    result = va::scan_tree(files, "/missing", va::ScanOptions{}, running, progress);
    CHECK(!result && result.error().kind == va::IoErrorKind::NotFound);
}

} // namespace

void scanner_tests()
{
    tree_is_recorded_in_canonical_order();
    exclusions_apply_at_root_only();
    mount_points_are_skipped();
    problems_become_unreadable_entries();
    collisions_are_warned();
    absolute_link_targets_are_flagged();
    cancellation_and_root_failure();
}
