#include "test_support.h"

#include "engine_fixture.h"
#include "manifest_samples.h"

#include "core/engine/compare_archives.h"

#include <fstream>

namespace {

// Two archives on the fake volume: roots "a" (the reference) and "b" (its copy).
struct TwoArchives : test::EngineFixture {
    va::Uuid a;
    va::Uuid b;

    // Registers both roots after the test has filled them.
    bool register_both()
    {
        const auto first = register_root(archive.root() / "a");
        const auto second = register_root(archive.root() / "b");
        CHECK(first.has_value());
        CHECK(second.has_value());
        if (!first || !second) {
            return false;
        }
        a = first->archive_uuid;
        b = second->archive_uuid;
        return true;
    }

    // The same file in both roots.
    void add_to_both(const std::string& relative, const std::string& content)
    {
        archive.add_file("a/" + relative, content);
        archive.add_file("b/" + relative, content);
    }

    std::expected<va::ArchiveComparison, va::CompareFailure> compare(const va::Uuid& left, const va::Uuid& right)
    {
        return va::compare_archives(store, *repository.files, repository.context.repo, left, right, cancel, progress);
    }

    va::ArchiveComparison compare_ok(const va::Uuid& left, const va::Uuid& right)
    {
        auto comparison = compare(left, right);
        CHECK(comparison.has_value());
        if (!comparison) {
            std::cerr << "compare failed: " << comparison.error().failure_class << ": "
                      << comparison.error().message << '\n';
            std::abort();
        }
        return std::move(*comparison);
    }
};

// "path status detail" per difference, in the order reported.
std::vector<std::string> lines_of(const va::ArchiveComparison& comparison)
{
    std::vector<std::string> lines;
    for (const auto& difference : comparison.differences) {
        lines.push_back(difference.path + " " + difference.status + " " + difference.detail);
    }
    return lines;
}

void identical_copy_is_identical()
{
    TwoArchives fixture;
    fixture.add_to_both("docs/report.pdf", "report bytes");
    fixture.add_to_both("photos/a.jpg", std::string(20000, 'a'));
    fixture.add_to_both("notes.txt", "hello");
    fixture.archive.add_directory("a/empty");
    fixture.archive.add_directory("b/empty");
    fixture.archive.node("b/notes.txt").mtime_ns += 5'000'000'000; // metadata is not compared
    if (!fixture.register_both()) {
        return;
    }

    const auto comparison = fixture.compare_ok(fixture.a, fixture.b);
    CHECK(comparison.result == "IDENTICAL");
    CHECK(comparison.differences.empty());
    CHECK(comparison.counts.empty());
    CHECK(comparison.entries_a == 4);
    CHECK(comparison.entries_b == 4);
    CHECK(comparison.same == 4);
    CHECK(comparison.archive_a == fixture.a);
    CHECK(comparison.archive_b == fixture.b);
    CHECK(comparison.label_a == "ARCH-0001");
    CHECK(comparison.label_b == "ARCH-0002");
    CHECK(comparison.revision_a == 1);
    CHECK(comparison.revision_b == 1);
    CHECK(!comparison.registered_a.empty());
    CHECK(!comparison.registered_b.empty());
}

// One of each difference, reported in canonical order whatever the kind.
void differences_in_canonical_order()
{
    TwoArchives fixture;
    fixture.add_to_both("keep.txt", "same");
    fixture.archive.add_file("a/mod.txt", "hello");
    fixture.archive.add_file("b/mod.txt", "hello!");   // Modified
    fixture.archive.add_file("a/gone.txt", "x");        // Missing
    fixture.archive.add_file("b/added.txt", "new");     // New
    fixture.archive.add_file("a/was_file", "f");
    fixture.archive.add_directory("b/was_file");       // file -> empty directory
    fixture.archive.add_directory("a/empty");          // empty directory Missing
    if (!fixture.register_both()) {
        return;
    }

    const auto comparison = fixture.compare_ok(fixture.a, fixture.b);
    CHECK(comparison.result == "DIFFERENT");
    CHECK((lines_of(comparison) == std::vector<std::string>{
        "added.txt New ",
        "empty Missing ",
        "gone.txt Missing ",
        "mod.txt Modified size 5 -> 6",
        "was_file TypeChanged f -> d",
    }));
    CHECK((comparison.counts == std::map<std::string, std::uint64_t>{
        {"Missing", 2}, {"Modified", 1}, {"New", 1}, {"TypeChanged", 1}}));
    CHECK(comparison.same == 1);
    CHECK(comparison.entries_a == 5);
    CHECK(comparison.entries_b == 4);
}

// A directory with contents is not in the manifest itself, only its entries
// are; it is still recognised as a directory.
void directories_with_contents()
{
    TwoArchives fixture;
    fixture.add_to_both("k.txt", "k");
    fixture.archive.add_file("a/x", "file");
    fixture.archive.add_file("b/x/child.txt", "child");     // file -> directory with contents
    fixture.archive.add_file("a/d/inner.txt", "inner");
    fixture.archive.add_file("b/d", "now a file");          // directory with contents -> file
    fixture.archive.add_directory("a/e");
    fixture.archive.add_file("b/e/f.txt", "f");             // empty directory gained a file
    fixture.archive.add_file("a/g/h.txt", "h");
    fixture.archive.add_directory("b/g");                   // directory lost its file
    if (!fixture.register_both()) {
        return;
    }

    const auto comparison = fixture.compare_ok(fixture.a, fixture.b);
    CHECK(comparison.result == "DIFFERENT");
    CHECK((lines_of(comparison) == std::vector<std::string>{
        "d TypeChanged d -> f",
        "d/inner.txt Missing ",
        "e/f.txt New ",
        "g/h.txt Missing ",
        "x TypeChanged f -> d",
        "x/child.txt New ",
    }));
    CHECK(comparison.same == 3); // k.txt, e and g are in both
}

void modified_details()
{
    TwoArchives fixture;
    fixture.archive.add_file("a/same-size.txt", "abc");
    fixture.archive.add_file("b/same-size.txt", "abd");
    fixture.archive.add_link("a/link", "target-one");
    fixture.archive.add_link("b/link", "target-two");
    fixture.archive.add_link("a/same-link", "target");
    fixture.archive.add_link("b/same-link", "target");
    if (!fixture.register_both()) {
        return;
    }

    const auto comparison = fixture.compare_ok(fixture.a, fixture.b);
    CHECK((lines_of(comparison) == std::vector<std::string>{
        "link Modified link target differs",
        "same-size.txt Modified content differs",
    }));
    CHECK(comparison.same == 1);
}

// Decision 0008: an absolute link target read on another OS may have been
// translated by the driver, so it cannot prove a difference.
void absolute_link_across_systems_warns()
{
    TwoArchives fixture;
    fixture.archive.add_link("a/abs", "C:/data/x");
    fixture.archive.add_link("b/abs", "/mnt/data/x");
    fixture.host.os = "windows";
    const auto first = fixture.register_root(fixture.archive.root() / "a");
    fixture.host.os = "linux";
    const auto second = fixture.register_root(fixture.archive.root() / "b");
    CHECK(first && second);
    if (!first || !second) {
        return;
    }

    auto comparison = fixture.compare_ok(first->archive_uuid, second->archive_uuid);
    CHECK(comparison.result == "IDENTICAL_WITH_WARNINGS");
    CHECK((lines_of(comparison) == std::vector<std::string>{"abs MetadataMismatch link-target-translated"}));
}

// Decision 0010: an entry without hashes cannot be compared, in either direction.
void unreadable_at_registration_is_unbaselined()
{
    TwoArchives fixture;
    fixture.add_to_both("good.txt", "good");
    fixture.archive.add_file("a/bad.bin", std::string(5000, 'b')).read_error_offset = 10;
    fixture.archive.add_file("b/bad.bin", std::string(5000, 'b'));
    fixture.archive.add_file("a/locked/inside.txt", "i");
    fixture.archive.node("a/locked").list_error = va::IoError{va::IoErrorKind::PermissionDenied, 5, "denied"};
    fixture.archive.add_file("b/locked/inside.txt", "i");
    if (!fixture.register_both()) {
        return;
    }

    auto comparison = fixture.compare_ok(fixture.a, fixture.b);
    CHECK(comparison.result == "IDENTICAL_WITH_WARNINGS");
    CHECK((lines_of(comparison) == std::vector<std::string>{
        "bad.bin Unbaselined unreadable in A (media)",
        "locked Unbaselined unreadable in A (permission)",
        "locked/inside.txt Unbaselined inside a directory unreadable in A",
    }));
    CHECK(comparison.counts.at("Unbaselined") == 3);
    CHECK(comparison.same == 1);

    comparison = fixture.compare_ok(fixture.b, fixture.a);
    CHECK(comparison.result == "IDENTICAL_WITH_WARNINGS");
    CHECK((lines_of(comparison) == std::vector<std::string>{
        "bad.bin Unbaselined unreadable in B (media)",
        "locked Unbaselined unreadable in B (permission)",
        "locked/inside.txt Unbaselined inside a directory unreadable in B",
    }));

    // Warnings do not hide a real difference.
    fixture.archive.add_file("b/extra.txt", "extra");
    va::RegisterRequest request;
    request.root = fixture.archive.root() / "b";
    request.new_revision = true;
    CHECK(fixture.register_root(request).has_value());
    comparison = fixture.compare_ok(fixture.a, fixture.b);
    CHECK(comparison.result == "DIFFERENT");
    CHECK(comparison.revision_b == 2);
}

// An archive compared with itself; known-unreadable entries stay unproven.
void archive_compared_with_itself()
{
    TwoArchives fixture;
    fixture.add_to_both("one.txt", "1");
    fixture.add_to_both("two/three.txt", "3");
    fixture.archive.add_file("b/bad.bin", std::string(5000, 'b')).read_error_offset = 10;
    if (!fixture.register_both()) {
        return;
    }

    auto comparison = fixture.compare_ok(fixture.a, fixture.a);
    CHECK(comparison.result == "IDENTICAL");
    CHECK(comparison.archive_a == comparison.archive_b);
    CHECK(comparison.same == 2 && comparison.entries_a == 2 && comparison.entries_b == 2);

    comparison = fixture.compare_ok(fixture.b, fixture.b);
    CHECK(comparison.result == "IDENTICAL_WITH_WARNINGS");
    CHECK((lines_of(comparison) == std::vector<std::string>{"bad.bin Unbaselined unreadable in both archives"}));
}

// R-14: a manifest whose bytes are not the recorded ones is refused.
void tampered_manifest_is_refused()
{
    TwoArchives fixture;
    fixture.add_to_both("notes.txt", "hello");
    if (!fixture.register_both()) {
        return;
    }
    const auto revisions = fixture.store.revisions(fixture.b);
    if (!revisions || revisions->empty()) {
        CHECK(false);
        return;
    }
    const auto path = fixture.repository.context.repo.root / revisions->front().manifest_relpath;
    std::string bytes;
    {
        std::ifstream input(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    bytes[bytes.size() - 5] ^= 0x01;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;

    const auto comparison = fixture.compare(fixture.a, fixture.b);
    CHECK(!comparison);
    CHECK(!comparison && comparison.error().failure_class == "ManifestHashMismatch");
    CHECK(!comparison && comparison.error().message.starts_with("cannot read the manifest of ARCH-0002 revision 1: "));
}

void unknown_archive_is_refused()
{
    TwoArchives fixture;
    fixture.add_to_both("notes.txt", "hello");
    if (!fixture.register_both()) {
        return;
    }
    const va::Uuid unknown = "00000000-0000-4000-8000-000000000000";
    auto comparison = fixture.compare(fixture.a, unknown);
    CHECK(!comparison && comparison.error().failure_class == "RepositoryError");
    CHECK(!comparison && comparison.error().message == "no archive " + unknown);
    comparison = fixture.compare(unknown, fixture.a);
    CHECK(!comparison && comparison.error().message == "no archive " + unknown);
}

class RecordingProgress final : public va::IProgressSink {
public:
    void on_progress(const va::Progress& progress) override { reports.push_back(progress); }
    std::vector<va::Progress> reports;
};

void progress_and_cancel()
{
    TwoArchives fixture;
    fixture.add_to_both("one.txt", "1");
    fixture.archive.add_file("a/two.txt", "2");
    if (!fixture.register_both()) {
        return;
    }

    RecordingProgress progress;
    const auto comparison =
        va::compare_archives(fixture.store, *fixture.repository.files, fixture.repository.context.repo,
            fixture.a, fixture.b, fixture.cancel, progress);
    CHECK(comparison.has_value());
    CHECK(!progress.reports.empty());
    if (!progress.reports.empty()) {
        CHECK(progress.reports.back().items_total == 3);
        CHECK(progress.reports.back().items_done == 3);
    }

    fixture.cancel.cancel();
    const auto cancelled = fixture.compare(fixture.a, fixture.b);
    CHECK(!cancelled && cancelled.error().failure_class == "Cancelled");
    fixture.cancel.reset();
}

// Decision 0032: a comparison compares whole-file hashes, never chunks. The
// original registered without chunk lines (as V1 registered it) and its copy
// registered with them, of the same content, are identical; a change in one
// chunk of the copy is a difference (sample data).
void a_chunked_and_an_unchunked_copy_are_identical()
{
    constexpr std::uint64_t chunk_size = 64 * 1024;
    TwoArchives fixture;
    fixture.add_to_both("video/big.bin", test::patterned(3 * chunk_size + 5, 7));
    fixture.add_to_both("notes.txt", "notes (sample data)");
    va::RegisterRequest original;
    original.root = fixture.archive.root() / "a";
    original.chunk_size = 0;
    va::RegisterRequest copy;
    copy.root = fixture.archive.root() / "b";
    copy.chunk_size = chunk_size;
    const auto a = fixture.register_root(original);
    const auto b = fixture.register_root(copy);
    CHECK(a && a->counts.chunks == 0 && b && b->counts.chunks == 4);
    if (!a || !b) {
        return;
    }
    const auto comparison = fixture.compare_ok(a->archive_uuid, b->archive_uuid);
    CHECK(comparison.result == "IDENTICAL" && comparison.same == 2 && comparison.differences.empty());

    auto changed = test::patterned(3 * chunk_size + 5, 7);
    changed[2 * chunk_size + 1] ^= 0x01;
    fixture.archive.node("b/video/big.bin").content = changed;
    auto again = copy;
    again.new_revision = true;
    CHECK(fixture.register_root(again).has_value());
    const auto changed_comparison = fixture.compare_ok(a->archive_uuid, b->archive_uuid);
    CHECK(changed_comparison.result == "DIFFERENT" &&
          (lines_of(changed_comparison) == std::vector<std::string>{"video/big.bin Modified content differs"}));
}

} // namespace

void compare_archives_tests()
{
    identical_copy_is_identical();
    differences_in_canonical_order();
    directories_with_contents();
    modified_details();
    absolute_link_across_systems_warns();
    unreadable_at_registration_is_unbaselined();
    archive_compared_with_itself();
    tampered_manifest_is_refused();
    unknown_archive_is_refused();
    progress_and_cancel();
    a_chunked_and_an_unchunked_copy_are_identical();
}
