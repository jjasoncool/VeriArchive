#include "test_support.h"

#include "engine_fixture.h"

#include "core/engine/rebuild_index.h"

#include <fstream>

// R-80: the index can be rebuilt from manifests alone; history is untouched.
void rebuild_index_tests()
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
}
