#include "test_support.h"

#include "engine_fixture.h"

#include "core/engine/relocate_engine.h"
#include "core/manifest/manifest_reader.h"

namespace {

// Volume 1 is mounted at /vol, volume 2 at /vol/new (a different disk).
struct RelocationFixture : test::EngineFixture {
    va::Uuid archive_uuid;

    RelocationFixture()
    {
        auto second = test::fake_volume(archive.root() / "new");
        second.filesystem_uuid = "5555666677778888";
        second.filesystem_uuid_short = "77778888";
        second.devices.front().serial = "NEW-DISK";
        media.extra_volumes.push_back(second);

        archive.add_file("old/photos/a.jpg", "jpeg");
        archive.add_file("old/notes.txt", "notes");
        const auto registered = register_root(archive.root() / "old");
        CHECK(registered.has_value());
        if (registered) {
            archive_uuid = registered->archive_uuid;
        }
    }

    // The operator's own copy of the archive onto the new disk.
    void copy_to_new_disk()
    {
        archive.add_file("new/photos/a.jpg", "jpeg").mtime_ns += 3'600'000'000'000; // copy changed mtime
        archive.add_file("new/notes.txt", "notes");
    }

    std::expected<va::RelocateOutcome, va::RelocateFailure> relocate(const std::filesystem::path& root)
    {
        va::RelocateServices services{archive, *repository.files, *repository.writer, media, store,
            repository.context.repo, repository.context.repository_uuid, host};
        va::RelocateRequest request{archive_uuid, root, std::nullopt};
        return va::relocate_archive(services, request, cancel, progress);
    }
};

void relocation_keeps_identity_and_history()
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk();
    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(relocated.has_value());
    if (!relocated) {
        std::cerr << relocated.error().message << '\n';
        return;
    }
    CHECK(relocated->revision == 2 && relocated->result == "PASSED_WITH_WARNINGS"); // mtime warning

    // The new carrier verifies as the same archive; the old one is a previous carrier.
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->archive_uuid == fixture.archive_uuid && verified->result == "PASSED");
    const auto old = fixture.verify(fixture.archive.root() / "old");
    CHECK(!old && old.error().identity.result == va::MatchResult::PreviousCarrier);

    auto reader = va::ManifestReader::open(*fixture.repository.files, relocated->manifest);
    CHECK(reader && reader->header().supersedes == 1u &&
          reader->header().identity.filesystem_uuid == "5555666677778888");
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 2);
}

void differing_copy_is_refused()
{
    RelocationFixture fixture;
    fixture.archive.add_file("new/photos/a.jpg", "JPEG"); // corrupted copy
    fixture.archive.add_file("new/notes.txt", "notes");
    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Differs);
    CHECK(!relocated && relocated.error().items.size() == 1);
    const auto archive = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 1);
}

// Decision 0010: entries unreadable at registration travel unchanged.
void known_unreadable_entries_are_carried()
{
    test::EngineFixture fixture;
    auto second = test::fake_volume(fixture.archive.root() / "new");
    second.filesystem_uuid = "5555666677778888";
    second.devices.front().serial = "NEW-DISK";
    fixture.media.extra_volumes.push_back(second);
    fixture.archive.add_file("old/good.txt", "good");
    fixture.archive.add_file("old/bad.bin", std::string(100, 'x')).read_error_offset = 10;
    const auto registered = fixture.register_root(fixture.archive.root() / "old");
    if (!registered) {
        CHECK(false);
        return;
    }
    fixture.archive.add_file("new/good.txt", "good");
    fixture.archive.add_file("new/bad.bin", "partial copy with zeros");

    va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media, fixture.store, fixture.repository.context.repo,
        fixture.repository.context.repository_uuid, fixture.host};
    const auto relocated = va::relocate_archive(services,
        {registered->archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel,
        fixture.progress);
    CHECK(relocated.has_value());
    if (!relocated) {
        return;
    }
    auto reader = va::ManifestReader::open(*fixture.repository.files, relocated->manifest);
    bool carried = false;
    while (reader) {
        auto next = reader->next();
        if (!next || !*next) {
            break;
        }
        if ((**next).path.p == "bad.bin") {
            carried = (**next).unreadable == "media" && (**next).b3.empty();
        }
    }
    CHECK(carried);
}

} // namespace

void relocate_tests()
{
    relocation_keeps_identity_and_history();
    differing_copy_is_refused();
    known_unreadable_entries_are_carried();
}
