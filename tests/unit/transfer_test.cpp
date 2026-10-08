#include "test_support.h"

#include "engine_fixture.h"

#include "core/engine/transfer.h"

#include <fstream>

namespace {

std::filesystem::path out_directory(const test::EngineFixture& fixture, const char* name)
{
    return fixture.repository.context.repo.root.parent_path() / name;
}

} // namespace

// M4: export -> import round trip, duplicate detection, rollback protection.
void transfer_tests()
{
    test::EngineFixture source;
    source.archive.add_file("a.txt", "a");
    const auto first = source.register_root(source.archive.root());
    CHECK(first.has_value());
    if (!first) {
        return;
    }
    const auto& layout = source.repository.context.repo;
    auto& files = *source.repository.files;
    auto& writer = *source.repository.writer;

    const auto r1 = va::export_archive(source.store, files, writer, layout, first->archive_uuid,
        out_directory(source, "export-r1"));
    CHECK(r1.has_value());
    CHECK(r1 && std::filesystem::exists(r1->identity));

    va::RegisterRequest rebaseline;
    rebaseline.root = source.archive.root();
    rebaseline.new_revision = true;
    source.archive.add_file("b.txt", "b");
    CHECK(source.register_root(rebaseline).has_value());
    const auto r2 = va::export_archive(source.store, files, writer, layout, first->archive_uuid,
        out_directory(source, "export-r2"));
    CHECK(r2.has_value());
    if (!r1 || !r2) {
        return;
    }

    // Import into a second repository.
    test::TestRepository target;
    auto store = target.store();
    const auto& target_layout = target.context.repo;
    auto imported = va::import_manifest(store, *target.files, *target.writer, target_layout, r2->manifest, false);
    CHECK(imported && imported->new_archive && imported->revision == 2 && imported->label == "ARCH-0001");
    const auto archive = store.find_archive(first->archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);

    imported = va::import_manifest(store, *target.files, *target.writer, target_layout, r2->manifest, false);
    CHECK(imported && imported->already_present);

    // R-15: an older revision is refused unless forced.
    auto rollback = va::import_manifest(store, *target.files, *target.writer, target_layout, r1->manifest, false);
    CHECK(!rollback && rollback.error().failure_class == "ManifestRevisionRollback");
    rollback = va::import_manifest(store, *target.files, *target.writer, target_layout, r1->manifest, true);
    CHECK(rollback && rollback->revision == 1);
    const auto kept = store.find_archive(first->archive_uuid);
    CHECK(kept && *kept && (*kept)->current_revision == 2);

    // A changed copy of a known revision is a hash mismatch.
    std::string bytes;
    {
        std::ifstream input(r2->manifest, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    bytes[bytes.size() - 4] ^= 0x01;
    const auto tampered = out_directory(source, "tampered.vam");
    std::ofstream(tampered, std::ios::binary) << bytes;
    const auto rejected = va::import_manifest(store, *target.files, *target.writer, target_layout, tampered, false);
    CHECK(!rejected && rejected.error().failure_class == "ManifestHashMismatch");
}
