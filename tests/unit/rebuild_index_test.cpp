#include "test_support.h"

#include "engine_fixture.h"

#include "core/engine/rebuild_index.h"
#include "core/hash/hashers.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace {

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool contains(const std::vector<std::string>& lines, const std::string& line)
{
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

// R-80: the index can be rebuilt from manifests alone; history is untouched.
void rebuild_keeps_database_facts()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("one/a.txt", "a");
    fixture.archive.add_file("two/b.txt", "b");
    const auto first = fixture.register_root(fixture.archive.root() / "one");
    const auto second = fixture.register_root(fixture.archive.root() / "two");
    CHECK(first && second);
    if (!first || !second) {
        return;
    }
    CHECK(fixture.verify(fixture.archive.root() / "one").has_value());

    auto archive = fixture.store.find_archive(second->archive_uuid);
    if (archive && *archive) {
        (*archive)->status = "RETIRED";
        CHECK(fixture.store.update_archive(**archive));
    }
    fixture.repository.execute("DELETE FROM files; DELETE FROM archive_revisions;");

    const auto report = va::rebuild_index(fixture.store, *fixture.repository.files, fixture.repository.context.repo);
    CHECK(report && report->archives == 2 && report->revisions == 2 && report->skipped.empty());
    const auto revisions = fixture.store.revisions(first->archive_uuid);
    CHECK(revisions && revisions->size() == 1);
    const auto retired = fixture.store.find_archive(second->archive_uuid);
    CHECK(retired && *retired && (*retired)->status == "RETIRED"); // database-only facts are kept

    // Verification works against the rebuilt index, and history survived.
    const auto verified = fixture.verify(fixture.archive.root() / "one");
    CHECK(verified && verified->result == "PASSED");
    const auto runs = fixture.store.verify_runs(first->archive_uuid);
    CHECK(runs && runs->size() == 2);

    // A damaged manifest is reported, not indexed.
    const auto path = fixture.repository.context.repo.root / (*revisions)[0].manifest_relpath;
    std::string bytes;
    {
        std::ifstream input(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    bytes[bytes.size() - 3] ^= 0x01;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    const auto damaged = va::rebuild_index(fixture.store, *fixture.repository.files, fixture.repository.context.repo);
    CHECK(damaged && damaged->skipped.size() == 1 && damaged->revisions == 1);
    if (damaged && damaged->skipped.size() == 1) {
        CHECK(damaged->skipped.front() == "cannot read manifest " + first->archive_uuid +
            "/r0001.vam: manifest body does not match its header (ManifestHashMismatch)");
    }
}

// Each skipped line starts with its own words and ends with the cause after
// ": ", so the interface language catalog can match it (decision 0017).
void skipped_manifests_say_why()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a.txt", "a");
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    const auto& layout = fixture.repository.context.repo;
    const auto revisions = fixture.store.revisions(registered->archive_uuid);
    CHECK(revisions && revisions->size() == 1);
    if (!revisions || revisions->size() != 1) {
        return;
    }
    const auto manifest = read_file(layout.root / revisions->front().manifest_relpath);

    // The same manifest with one entry type changed and the body hash in the
    // header fixed up, so the body passes and only the entry is wrong.
    const auto newline = manifest.find('\n');
    auto header = manifest.substr(0, newline + 1);
    auto body = manifest.substr(newline + 1);
    const auto body_hash = header.find(va::blake3_hex(body));
    const auto entry_type = body.find("\"t\":\"f\"");
    CHECK(body_hash != std::string::npos && entry_type != std::string::npos);
    if (body_hash == std::string::npos || entry_type == std::string::npos) {
        return;
    }
    body[entry_type + 5] = 'x';
    header.replace(body_hash, 64, va::blake3_hex(body));

    // manifests/ as rebuild-index sees it, with one fault per manifest.
    const std::string unlisted = "aaaaaaaa-0000-4000-8000-000000000001";
    const std::string broken = "bbbbbbbb-0000-4000-8000-000000000002";
    const auto& own = registered->archive_uuid; // the bad entry needs a matching header
    test::FakeFileSystem manifests(layout.manifests());
    manifests.add_directory(unlisted).list_error = va::IoError{va::IoErrorKind::PermissionDenied, 5, "denied"};
    manifests.add_file(broken + "/r0001.vam", "");
    manifests.add_file(broken + "/r0002.vam", "{\"format\":\"vam\",\"version\":2}\n");
    manifests.add_file(broken + "/r0003.vam", manifest); // another archive's manifest
    manifests.add_file(own + "/r0001.vam", header + body);

    const auto report = va::rebuild_index(fixture.store, manifests, layout);
    CHECK(report && report->skipped.size() == 5 && report->revisions == 0);
    if (!report) {
        return;
    }
    const auto& skipped = report->skipped;
    CHECK(contains(skipped, "cannot list the manifests of archive " + unlisted + ": denied"));
    CHECK(contains(skipped, "cannot read manifest " + broken + "/r0001.vam: empty manifest"));
    // No "[json.exception.other_error.501] " tag from the JSON library.
    CHECK(contains(skipped,
        "cannot read manifest " + broken + "/r0002.vam: invalid manifest header: not a version 1 .vam manifest"));
    CHECK(contains(skipped,
        "cannot read manifest " + broken + "/r0003.vam: header does not match its file name"));
    // Not indexed with only the entries before the bad one.
    CHECK(contains(skipped,
        "cannot read manifest " + own + "/r0001.vam: invalid manifest entry: unknown entry type x"));
}

} // namespace

void rebuild_index_tests()
{
    rebuild_keeps_database_facts();
    skipped_manifests_say_why();
}
