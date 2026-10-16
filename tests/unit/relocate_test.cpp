#include "test_support.h"

#include "engine_fixture.h"

#include "core/engine/abandon_registration.h"
#include "core/engine/relocate_engine.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"

#include <algorithm>
#include <chrono>
#include <semaphore>
#include <set>
#include <thread>
#include <utility>

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

// Decision 0023: two relocations of one archive at once (sample data). The
// second has written and flushed its manifest of revision 2 when the first
// records revision 2; it finds the revision recorded and stops, and the
// recorded manifest keeps the first one's bytes.
void relocations_of_one_archive_record_the_revision_once()
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk();
    const va::RelocateRequest request{fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt};
    std::expected<va::RelocateOutcome, va::RelocateFailure> first = std::unexpected(va::RelocateFailure{});
    bool ran = false;
    test::StagingHookWriter hooked(*fixture.repository.writer, [&] {
        if (std::exchange(ran, true)) {
            return;
        }
        auto store = fixture.repository.store();
        auto host = fixture.host;
        host.host = "host-first";
        va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
            fixture.media, store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, host};
        first = va::relocate_archive(services, request, fixture.cancel, fixture.progress);
    });
    auto host = fixture.host;
    host.host = "host-second";
    va::RelocateServices services{fixture.archive, *fixture.repository.files, hooked, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, host};
    const auto second = va::relocate_archive(services, request, fixture.cancel, fixture.progress);

    CHECK(ran && first && first->revision == 2);
    CHECK(!second && second.error().kind == va::RelocateFailure::Kind::Refused &&
          second.error().message == "revision 2 of archive " + fixture.archive_uuid +
                                        " was recorded by another operation while this relocation ran; "
                                        "nothing was changed");
    const auto recorded = fixture.store.revision(fixture.archive_uuid, 2);
    auto reader = va::ManifestReader::open(*fixture.repository.files,
        va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 2));
    CHECK(recorded && *recorded && reader && reader->header_blake3() == (*recorded)->header_blake3 &&
          reader->header().host == "host-first");
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
    std::size_t names = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(
             fixture.repository.context.repo.manifests() / fixture.archive_uuid)) {
        ++names;
    }
    CHECK(names == 2); // r0001.vam and r0002.vam
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 2);
}

std::set<std::string> manifest_names(RelocationFixture& fixture)
{
    std::set<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(
             fixture.repository.context.repo.manifests() / fixture.archive_uuid, error)) {
        names.insert(va::path_to_utf8(entry.path().filename()));
    }
    return names;
}

// Decision 0023: a relocation records the revision that a registration of
// the archive creates (`register --new-revision`) while the registration has
// hashed nothing it has saved yet (sample data). The registration's plan goes
// with the relocation's record. The registration stops when it finds the
// revision recorded, says that it may be another operation's, and records
// nothing; nothing of its plan is left to block the archive's next
// registration.
void a_relocation_ends_a_registration_that_saved_nothing()
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk();
    fixture.archive.add_file("old/added.txt", "added on the old carrier (sample data)");
    std::expected<va::RelocateOutcome, va::RelocateFailure> relocated = std::unexpected(va::RelocateFailure{});
    std::string plan_seen;
    // While the registration hashes its last file, before its first
    // checkpoint, another process relocates the archive.
    test::HookedFiles hooked(fixture.archive, "a.jpg", [&] {
        auto store = fixture.repository.store();
        plan_seen = store.setting("registration:" + fixture.archive_uuid).value_or("");
        va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
            fixture.media, store, fixture.repository.context.repo, fixture.repository.context.repository_uuid,
            fixture.host};
        relocated = va::relocate_archive(services,
            {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    });
    va::RegisterRequest request;
    request.root = fixture.archive.root() / "old";
    request.new_revision = true;
    va::RegisterServices services{hooked, *fixture.repository.files, *fixture.repository.writer,
        fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    const auto registered = va::register_archive(services, request, fixture.cancel, fixture.progress);

    CHECK(!plan_seen.empty());
    CHECK(relocated && relocated->revision == 2);
    CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Refused &&
          registered.error().message == test::already_recorded_message(fixture.archive_uuid, 2));
    // Revision 2 is the relocation's: the new carrier, and the index of its
    // manifest, without the file only the registration read.
    auto reader = va::ManifestReader::open(*fixture.repository.files,
        va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 2));
    CHECK(reader && reader->header().identity.filesystem_uuid == "5555666677778888");
    const auto rows = fixture.store.file_rows(fixture.archive_uuid, 2);
    CHECK(rows && rows->size() == 2 && std::ranges::none_of(*rows, [](const va::Entry& row) {
        return row.path.p == "added.txt";
    }));
    CHECK(manifest_names(fixture) == (std::set<std::string>{"r0001.vam", "r0002.vam"}));
    const auto plans = fixture.store.settings_with_prefix("registration:");
    CHECK(plans && plans->empty());
    auto resume = request;
    resume.new_revision = false;
    resume.resume = true;
    const auto resumed = fixture.register_root(resume);
    CHECK(!resumed && resumed.error().message == "there is no interrupted registration of this root to resume");

    // The archive takes its next registration, on its new carrier.
    va::RegisterRequest next;
    next.root = fixture.archive.root() / "new";
    next.new_revision = true;
    const auto rebaselined = fixture.register_root(next);
    CHECK(rebaselined && rebaselined->revision == 3);
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->result == "PASSED" && verified->revision == 3);
}

// Decision 0023: a registration creating the revision that a relocation
// records, interrupted before it saved anything it hashed (here in its scan),
// can never be finished: its plan goes with the relocation's record, so it
// is neither listed as resumable nor keeps the root from a registration.
void a_relocation_removes_the_plan_of_its_revision()
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk();
    fixture.archive.node("old/photos").list_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    va::RegisterRequest request;
    request.root = fixture.archive.root() / "old";
    request.new_revision = true;
    const auto interrupted = fixture.register_root(request);
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("old/photos").list_error.reset();
    const auto key = "registration:" + fixture.archive_uuid;
    CHECK(!fixture.store.setting(key).value_or("").empty());
    CHECK(!fixture.store.has_file_rows(fixture.archive_uuid, 2).value_or(true));

    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(relocated && relocated->revision == 2);
    CHECK(fixture.store.setting(key).value_or("x").empty());
    const auto pending = fixture.store.pending_registrations();
    CHECK(pending && pending->empty());
    request.new_revision = false;
    request.resume = true;
    const auto resumed = fixture.register_root(request);
    CHECK(!resumed && resumed.error().message == "there is no interrupted registration of this root to resume");
}

// Decision 0023: a relocation does not take the revision that an unfinished
// registration creates once the registration has saved files it hashed for
// it (checkpoints, R-104): recorded over them, they would join its index or
// fail its insert. It stops (exit 12) and changes nothing; the registration
// finishes, and the relocation then records the revision after it.
void a_relocation_leaves_a_registration_its_saved_files()
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk();
    // `register --new-revision` of the old carrier, interrupted after it hashed notes.txt.
    fixture.archive.node("old/photos/a.jpg").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    va::RegisterRequest request;
    request.root = fixture.archive.root() / "old";
    request.new_revision = true;
    const auto interrupted = fixture.register_root(request);
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("old/photos/a.jpg").open_error.reset();
    CHECK(fixture.store.has_file_rows(fixture.archive_uuid, 2).value_or(false));
    const auto key = "registration:" + fixture.archive_uuid;
    const auto plan = fixture.store.setting(key).value_or("");
    CHECK(!plan.empty());

    const auto refused = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!refused && refused.error().kind == va::RelocateFailure::Kind::Refused &&
          refused.error().message == "an unfinished registration of archive " + fixture.archive_uuid + " (root \"" +
                                         va::path_to_utf8(request.root) +
                                         "\") is creating revision 2; nothing was changed (finish it with "
                                         "`register --resume` first, or abandon it with `archive "
                                         "abandon-registration " +
                                         fixture.archive_uuid + "`)");
    const auto archive = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 1);
    const auto revision = fixture.store.revision(fixture.archive_uuid, 2);
    CHECK(revision && !*revision);
    CHECK(fixture.store.setting(key).value_or("") == plan);
    CHECK(fixture.store.has_file_rows(fixture.archive_uuid, 2).value_or(false));
    CHECK(manifest_names(fixture) == std::set<std::string>{"r0001.vam"});
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->empty());

    request.new_revision = false;
    request.resume = true;
    const auto resumed = fixture.register_root(request);
    CHECK(resumed && resumed->revision == 2);
    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(relocated && relocated->revision == 3);
}

// What happens while a relocation waits for the write lock.
enum class WhileWaiting {
    OtherCommits,         // another operation commits something else
    OtherRecordsIt,       // another operation records the same revision
    RelocationCancelled,  // the operator cancels the relocation (Ctrl+C)
};

// Decision 0023: the record of a relocation waits for the write lock that
// another operation's record holds, also longer than a statement waits for
// it (the record of a large archive holds it for seconds). It then records,
// or stops (exit 12) when that operation recorded the same revision, rather
// than failing because the database is locked; cancelled while it waits, it
// stops as interrupted (exit 4). Nothing is changed unless it records.
// Scaled down: the relocation's statements wait 100 ms for the lock, and the
// other operation holds it 400 ms.
void a_relocation_waits_for_the_write_lock(WhileWaiting what)
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk();
    std::thread holder;
    std::binary_semaphore held{0};
    va::CancelToken cancel;
    // Once the relocation's manifest is written, another process takes the lock.
    test::StagingHookWriter hooked(*fixture.repository.writer, [&] {
        holder = std::thread([&] {
            auto store = fixture.repository.store();
            CHECK(store.begin());
            if (what == WhileWaiting::OtherRecordsIt) {
                CHECK(store.insert_revision({fixture.archive_uuid, 2,
                    "manifests/" + fixture.archive_uuid + "/r0002.vam", "{}", "h", "b", "", "2026-10-15T01:00:00Z"}));
            }
            held.release();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            CHECK(store.commit());
        });
        CHECK(held.try_acquire_for(std::chrono::seconds(60)));
        if (what == WhileWaiting::RelocationCancelled) {
            cancel.cancel();
        }
    });
    auto store = fixture.repository.store(std::chrono::milliseconds(100));
    va::RelocateServices services{fixture.archive, *fixture.repository.files, hooked, fixture.media, store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host};
    const auto relocated = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, cancel, fixture.progress);
    if (holder.joinable()) {
        holder.join();
    }
    bool expected = false;
    switch (what) {
    case WhileWaiting::OtherCommits:
        expected = relocated && relocated->revision == 2;
        break;
    case WhileWaiting::OtherRecordsIt:
        expected = !relocated && relocated.error().kind == va::RelocateFailure::Kind::Refused &&
                   relocated.error().message == "revision 2 of archive " + fixture.archive_uuid +
                                                    " was recorded by another operation while this relocation "
                                                    "ran; nothing was changed";
        break;
    case WhileWaiting::RelocationCancelled:
        expected = !relocated && relocated.error().kind == va::RelocateFailure::Kind::Incomplete &&
                   relocated.error().message == "relocation was interrupted; nothing was changed";
        break;
    }
    CHECK(expected);
    if (!expected && !relocated) {
        std::cerr << "  relocation: " << relocated.error().message << '\n';
    }
    if (what == WhileWaiting::OtherCommits) {
        const auto verified = fixture.verify(fixture.archive.root() / "new");
        CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
        return;
    }
    CHECK(manifest_names(fixture) == std::set<std::string>{"r0001.vam"});
    const auto archive = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 1);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->empty());
}

// Decision 0025: the old carrier failed while `register --new-revision` of
// it was unfinished, after it had saved files for revision 2. The relocation
// of the archive to its new carrier leaves the revision to that registration
// and says how to give it up; once the operator abandons it, the relocation
// records revision 2, and nothing of the registration is left to resume.
void an_abandoned_registration_lets_the_relocation_record()
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk();
    fixture.archive.node("old/photos/a.jpg").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    va::RegisterRequest request;
    request.root = fixture.archive.root() / "old";
    request.new_revision = true;
    const auto interrupted = fixture.register_root(request);
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    CHECK(fixture.store.has_file_rows(fixture.archive_uuid, 2).value_or(false));

    const auto refused = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!refused && refused.error().kind == va::RelocateFailure::Kind::Refused &&
          refused.error().message.ends_with("or abandon it with `archive abandon-registration " +
                                            fixture.archive_uuid + "`)"));

    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto abandoned = va::abandon_registration(services, {fixture.archive_uuid, std::nullopt, std::nullopt});
    CHECK(abandoned && abandoned->revision == 2 && !abandoned->first_registration && abandoned->files > 0);
    CHECK(!fixture.store.has_file_rows(fixture.archive_uuid, 2).value_or(true));

    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(relocated && relocated->revision == 2);
    // Revision 2 is the relocation's: the new carrier's identity, and the index of its manifest only.
    auto reader = va::ManifestReader::open(*fixture.repository.files,
        va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 2));
    CHECK(reader && reader->header().identity.filesystem_uuid == "5555666677778888");
    const auto rows = fixture.store.file_rows(fixture.archive_uuid, 2);
    CHECK(rows && rows->size() == 2);
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
    request.new_revision = false;
    request.resume = true;
    const auto resumed = fixture.register_root(request);
    CHECK(!resumed && resumed.error().message == "there is no interrupted registration of this root to resume");
}

} // namespace

void relocate_tests()
{
    relocation_keeps_identity_and_history();
    differing_copy_is_refused();
    known_unreadable_entries_are_carried();
    relocations_of_one_archive_record_the_revision_once();
    a_relocation_ends_a_registration_that_saved_nothing();
    a_relocation_removes_the_plan_of_its_revision();
    a_relocation_leaves_a_registration_its_saved_files();
    an_abandoned_registration_lets_the_relocation_record();
    a_relocation_waits_for_the_write_lock(WhileWaiting::OtherCommits);
    a_relocation_waits_for_the_write_lock(WhileWaiting::OtherRecordsIt);
    a_relocation_waits_for_the_write_lock(WhileWaiting::RelocationCancelled);
}
