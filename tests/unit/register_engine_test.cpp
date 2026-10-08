#include "test_support.h"

#include "engine_fixture.h"

#include "core/hash/hashers.h"
#include "core/manifest/manifest_reader.h"

namespace {

// Registration-only view of the shared engine fixture.
struct Fixture : test::EngineFixture {
    std::expected<va::RegisterOutcome, va::RegisterFailure> run(va::RegisterRequest request)
    {
        return register_root(request);
    }
};

va::RegisterRequest at(const std::filesystem::path& root)
{
    va::RegisterRequest request;
    request.root = root;
    return request;
}

void first_registration_writes_manifest_and_index()
{
    Fixture fixture;
    fixture.archive.add_file("photos/2026/a.jpg", "jpeg bytes");
    fixture.archive.add_file("notes.txt", "hello");
    fixture.archive.add_directory("empty");

    const auto outcome = fixture.run(at("/vol"));
    CHECK(outcome.has_value());
    if (!outcome) {
        std::cerr << outcome.error().message << '\n';
        return;
    }
    CHECK(outcome->label == "ARCH-0001" && outcome->revision == 1);
    CHECK(outcome->counts.files == 2 && outcome->counts.dirs == 1 && outcome->counts.bytes == 15);
    CHECK(outcome->unreadable.empty());

    auto reader = va::ManifestReader::open(*fixture.repository.files, outcome->manifest);
    CHECK(reader.has_value());
    if (reader) {
        CHECK(reader->header().identity.filesystem_uuid == "1111222233334444");
        CHECK(reader->header().root_subpath.empty());
        const auto first = reader->next();
        CHECK(first && *first && (**first).path.p == "empty");
        const auto second = reader->next();
        CHECK(second && *second && (**second).b3 == va::blake3_hex("hello"));
    }
    const auto archive = fixture.store.find_archive(outcome->archive_uuid);
    CHECK(archive && *archive && (*archive)->status == "REGISTERED" && (*archive)->current_revision == 1);
}

// R-97: a registered volume is refused unless the operator says what to do.
void registered_root_needs_a_flag()
{
    Fixture fixture;
    fixture.archive.add_file("a.txt", "a");
    const auto first = fixture.run(at("/vol"));
    CHECK(first.has_value());

    auto again = fixture.run(at("/vol"));
    CHECK(!again && again.error().kind == va::RegisterFailure::Kind::Refused);

    auto request = at("/vol");
    request.new_revision = true;
    fixture.archive.add_file("b.txt", "b");
    const auto second = fixture.run(request);
    CHECK(second && first && second->archive_uuid == first->archive_uuid);
    CHECK(second && second->revision == 2 && second->label == "ARCH-0001");
    if (second) {
        auto reader = va::ManifestReader::open(*fixture.repository.files, second->manifest);
        CHECK(reader && reader->header().supersedes == 1u);
    }

    request = at("/vol");
    request.force_new_archive = true;
    const auto separate = fixture.run(request);
    CHECK(separate && first && separate->archive_uuid != first->archive_uuid);
}

// Decision 0001: roots on one volume may not contain each other.
void overlapping_roots_are_refused()
{
    Fixture fixture;
    fixture.archive.add_file("Backups/2026/a.txt", "a");
    fixture.archive.add_file("Backups/2025/b.txt", "b");
    CHECK(fixture.run(at("/vol/Backups/2026")).has_value());
    CHECK(fixture.run(at("/vol/Backups/2025")).has_value());

    const auto parent = fixture.run(at("/vol/Backups"));
    CHECK(!parent && parent.error().kind == va::RegisterFailure::Kind::Refused);
    const auto other_case = fixture.run(at("/vol/backups/2026"));
    CHECK(!other_case && other_case.error().kind == va::RegisterFailure::Kind::Refused);
}

// Decision 0010: unreadable files are recorded and listed, not skipped.
void unreadable_files_are_recorded()
{
    Fixture fixture;
    fixture.archive.add_file("good.txt", "fine");
    fixture.archive.add_file("bad.bin", std::string(5000, 'x')).read_error_offset = 100;

    const auto outcome = fixture.run(at("/vol"));
    CHECK(outcome && outcome->unreadable.size() == 1);
    if (outcome && outcome->unreadable.size() == 1) {
        CHECK(outcome->unreadable.front().path.p == "bad.bin");
        CHECK(outcome->unreadable.front().unreadable == "media");
        CHECK(outcome->unreadable.front().b3.empty());
    }
}

// Decision 0003: a retired archive takes no new revision.
void retired_archive_takes_no_revision()
{
    Fixture fixture;
    fixture.archive.add_file("a.txt", "a");
    const auto first = fixture.run(at("/vol"));
    if (!first) {
        CHECK(false);
        return;
    }
    auto archive = fixture.store.find_archive(first->archive_uuid);
    if (archive && *archive) {
        (*archive)->status = "RETIRED";
        CHECK(fixture.store.update_archive(**archive));
    }
    auto request = at("/vol");
    request.new_revision = true;
    const auto refused = fixture.run(request);
    CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Refused);
}

// R-104: an interrupted registration resumes without re-hashing finished files.
void interrupted_registration_resumes()
{
    Fixture fixture;
    for (int index = 1; index <= 6; ++index) {
        fixture.archive.add_file("f" + std::to_string(index), std::string(1000, static_cast<char>('a' + index)));
    }
    fixture.archive.node("f4").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.run(at("/vol"));
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);

    const auto archives = fixture.store.archives();
    CHECK(archives && archives->size() == 1 && archives->front().status == "REGISTRATION_INCOMPLETE");
    const auto cached = archives && !archives->empty()
                            ? fixture.store.file_rows(archives->front().archive_uuid, 1)
                            : std::expected<std::vector<va::Entry>, va::IoError>{};
    CHECK(cached && cached->size() >= 3); // f1..f3 were checkpointed

    // A plain register of the same root is refused while the interrupted one exists.
    fixture.archive.node("f4").open_error.reset();
    const auto again = fixture.run(at("/vol"));
    CHECK(!again && again.error().kind == va::RegisterFailure::Kind::Refused);

    fixture.archive.bytes_read = 0;
    auto request = at("/vol");
    request.resume = true;
    const auto resumed = fixture.run(request);
    CHECK(resumed && resumed->revision == 1 && resumed->counts.files == 6);
    // Only files without a checkpointed hash are read again.
    CHECK(cached && fixture.archive.bytes_read == 6000 - 1000 * cached->size());
    const auto archive = fixture.store.find_archive(resumed ? resumed->archive_uuid : "");
    CHECK(archive && *archive && (*archive)->status == "REGISTERED");

    // Verification of the resumed registration passes.
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED");
}

void unidentifiable_volume_fails()
{
    Fixture fixture;
    fixture.media.error = va::IoError{va::IoErrorKind::NotSupported, 0, "no volume"};
    const auto outcome = fixture.run(at("/vol"));
    CHECK(!outcome && outcome.error().kind == va::RegisterFailure::Kind::Media);
}

} // namespace

void register_engine_tests()
{
    first_registration_writes_manifest_and_index();
    registered_root_needs_a_flag();
    overlapping_roots_are_refused();
    unreadable_files_are_recorded();
    retired_archive_takes_no_revision();
    interrupted_registration_resumes();
    unidentifiable_volume_fails();
}
