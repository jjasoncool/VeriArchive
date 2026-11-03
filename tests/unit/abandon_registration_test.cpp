// Decision 0025: abandoning an unfinished registration on purpose removes its
// plan and its saved progress in one transaction, and nothing else; it is
// refused while an attempt of the registration still runs (decision 0024).
// Sample data on a fake volume (/vol) only.
#include "test_support.h"

#include "engine_fixture.h"

#include "common/time.h"
#include "common/utf8.h"
#include "core/engine/abandon_registration.h"
#include "core/engine/registration_attempt.h"
#include "core/manifest/manifest_writer.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <fstream>
#include <functional>
#include <iterator>
#include <string>

namespace {

va::RegisterRequest at(const std::filesystem::path& root)
{
    va::RegisterRequest request;
    request.root = root;
    return request;
}

// One number read from the repository database, as another process would.
std::int64_t count(const test::TestRepository& repository, const std::string& sql)
{
    sqlite3* database = nullptr;
    const auto name = repository.context.repo.database().u8string();
    std::int64_t value = -1;
    if (sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &database, SQLITE_OPEN_READONLY, nullptr) ==
        SQLITE_OK) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            value = sqlite3_column_int64(statement, 0);
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close(database);
    return value;
}

// How many rows each table of the repository holds.
struct Rows {
    std::int64_t archives = 0;
    std::int64_t revisions = 0;
    std::int64_t files = 0;
    std::int64_t evidence = 0;
    std::int64_t runs = 0;
    std::int64_t items = 0;
    std::int64_t progress = 0;
    std::int64_t settings = 0;
    bool operator==(const Rows&) const = default;
};

Rows rows_of(const test::TestRepository& repository)
{
    return {count(repository, "SELECT COUNT(*) FROM archives"),
        count(repository, "SELECT COUNT(*) FROM archive_revisions"), count(repository, "SELECT COUNT(*) FROM files"),
        count(repository, "SELECT COUNT(*) FROM media_evidence"), count(repository, "SELECT COUNT(*) FROM verify_runs"),
        count(repository, "SELECT COUNT(*) FROM verify_items"),
        count(repository, "SELECT COUNT(*) FROM verify_progress"), count(repository, "SELECT COUNT(*) FROM settings")};
}

std::string read_bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

nlohmann::json plan_of(va::ArchiveStore& store, const va::Uuid& archive_uuid)
{
    return nlohmann::json::parse(store.setting("registration:" + archive_uuid).value_or(""), nullptr, false);
}

// The archive whose first registration has not finished.
va::Uuid unfinished_archive(va::ArchiveStore& store)
{
    if (const auto archives = store.archives()) {
        for (const auto& archive : *archives) {
            if (archive.status == "REGISTRATION_INCOMPLETE") {
                return archive.archive_uuid;
            }
        }
    }
    CHECK(false);
    return {};
}

// A first registration of /vol/broken, interrupted when its device went away
// after it saved 4 files of 100 bytes each (sample data). With `killed`, its
// process ended without writing its stop mark.
va::Uuid interrupted_first_registration(test::EngineFixture& fixture, bool killed = false,
    bool media_copy = false)
{
    for (int index = 0; index < 4; ++index) {
        fixture.archive.add_file("broken/f" + std::to_string(index) + ".bin", std::string(100, 'a'));
    }
    fixture.archive.add_file("broken/z-gone.bin", std::string(100, 'z')).open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    auto request = at(fixture.archive.root() / "broken");
    request.write_media_copy = media_copy;
    const auto interrupted = fixture.register_root(request);
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("broken/z-gone.bin").open_error.reset();
    const auto archive_uuid = unfinished_archive(fixture.store);
    if (killed) {
        auto plan = plan_of(fixture.store, archive_uuid);
        plan.erase("stopped");
        CHECK(fixture.store.set_setting("registration:" + archive_uuid, plan.dump()));
    }
    return archive_uuid;
}

// A registered and verified archive of /vol/kept, whose revision, index,
// manifest and history an abandon elsewhere must leave as they are.
va::Uuid kept_archive(test::EngineFixture& fixture)
{
    fixture.archive.add_file("kept/a.txt", "kept (sample data)");
    fixture.archive.add_file("kept/b.txt", "also kept (sample data)");
    const auto kept = fixture.register_root(fixture.archive.root() / "kept");
    CHECK(kept.has_value());
    const auto verified = fixture.verify(fixture.archive.root() / "kept");
    CHECK(verified && verified->result == "PASSED");
    return kept ? kept->archive_uuid : va::Uuid{};
}

// The archive's first registration never finished: abandoning it removes its
// plan, its saved progress and the archive's record, and nothing of another
// archive. Asking first changes nothing. The root then takes a registration
// again, under a label of its own.
void abandoning_a_first_registration_removes_it_whole()
{
    test::EngineFixture fixture;
    const auto kept = kept_archive(fixture);
    const auto archive_uuid = interrupted_first_registration(fixture);
    // Another operation's record, which must stay (sample data).
    CHECK(fixture.store.set_setting("run:sample", R"({"archive_uuid":")" + kept + R"(","revision":1})"));
    const auto manifest = va::manifest_path(fixture.repository.context.repo.manifests(), kept, 1);
    const auto manifest_bytes = read_bytes(manifest);
    const auto before = rows_of(fixture.repository);

    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto shown = va::abandonable_registration(services, {archive_uuid, std::nullopt, std::nullopt});
    CHECK(shown.has_value());
    if (!shown) {
        std::cerr << "  " << shown.error().message << '\n';
        return;
    }
    CHECK(shown->archive_uuid == archive_uuid && shown->label == "ARCH-0002" && shown->revision == 1);
    CHECK(shown->root == va::path_to_utf8(fixture.archive.root() / "broken"));
    CHECK(shown->files == 4 && shown->bytes == 400);
    CHECK(shown->first_registration && !shown->media_copy);
    CHECK(va::parse_timestamp(shown->created).has_value());
    CHECK(rows_of(fixture.repository) == before);  // asking changes nothing

    const auto abandoned = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
    CHECK(abandoned && abandoned->files == 4 && abandoned->bytes == 400 && abandoned->first_registration);
    auto expected = before;
    expected.archives -= 1;  // the record of the archive without a revision
    expected.files -= 4;     // its saved progress
    expected.settings -= 1;  // its plan
    CHECK(rows_of(fixture.repository) == expected);
    CHECK(fixture.store.setting("registration:" + archive_uuid) == std::string{});
    const auto gone = fixture.store.find_archive(archive_uuid);
    CHECK(gone && !*gone);
    const auto rows = fixture.store.file_rows(archive_uuid, 1);
    CHECK(rows && rows->empty());

    // The other archive is as it was: record, manifest, history.
    CHECK(read_bytes(manifest) == manifest_bytes);
    const auto archive = fixture.store.find_archive(kept);
    CHECK(archive && *archive && (*archive)->status == "REGISTERED" && (*archive)->current_revision == 1);
    CHECK(fixture.store.setting("run:sample") != std::string{});
    const auto history = fixture.store.check_history(fixture.repository.context.repository_uuid);
    CHECK(history && history->runs == 1 && !history->first_broken_run);
    const auto verified = fixture.verify(fixture.archive.root() / "kept");
    CHECK(verified && verified->result == "PASSED");

    // Nothing is left to abandon, and the root takes a new registration.
    const auto again = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
    CHECK(!again && again.error().kind == va::AbandonFailure::Kind::Refused &&
          again.error().message == "archive " + archive_uuid + " has no unfinished registration");
    const auto registered = fixture.register_root(fixture.archive.root() / "broken");
    CHECK(registered && registered->revision == 1 && registered->archive_uuid != archive_uuid);
    // The abandoned registration's label is not given out again.
    CHECK(registered && registered->label == "ARCH-0003");
}

// `register --new-revision` of a registered archive, interrupted: abandoning
// it removes its plan and the files it saved for revision 2, and keeps the
// archive, its revision 1, that revision's index and manifest, and its
// history. The archive verifies, and takes its next registration.
void abandoning_a_new_revision_keeps_the_archive()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a.txt", "a (sample data)");
    fixture.archive.add_file("b.txt", "b (sample data)");
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    const auto& archive_uuid = registered->archive_uuid;
    CHECK(fixture.verify(fixture.archive.root()).has_value());
    const auto manifest = va::manifest_path(fixture.repository.context.repo.manifests(), archive_uuid, 1);
    const auto manifest_bytes = read_bytes(manifest);
    const auto index = fixture.store.file_rows(archive_uuid, 1);

    // New content, and the device goes away after one of the new files.
    fixture.archive.add_file("c.txt", "c (sample data)");
    fixture.archive.add_file("d.txt", "d (sample data)").open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    auto request = at(fixture.archive.root());
    request.new_revision = true;
    const auto interrupted = fixture.register_root(request);
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    CHECK(fixture.store.has_file_rows(archive_uuid, 2).value_or(false));
    const auto before = rows_of(fixture.repository);
    const auto saved = fixture.store.file_rows(archive_uuid, 2);

    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto abandoned = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
    CHECK(abandoned && abandoned->revision == 2 && !abandoned->first_registration);
    CHECK(abandoned && saved && abandoned->files == saved->size());
    auto expected = before;
    expected.files -= saved ? static_cast<std::int64_t>(saved->size()) : 0;
    expected.settings -= 1;
    CHECK(rows_of(fixture.repository) == expected);
    CHECK(!fixture.store.has_file_rows(archive_uuid, 2).value_or(true));
    const auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->status == "REGISTERED" && (*archive)->current_revision == 1);
    const auto kept_index = fixture.store.file_rows(archive_uuid, 1);
    CHECK(index && kept_index && kept_index->size() == index->size());
    CHECK(read_bytes(manifest) == manifest_bytes);

    // The content goes back as registered: revision 1 verifies.
    fixture.archive.remove("c.txt");
    fixture.archive.remove("d.txt");
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED" && verified->revision == 1);
    // Nothing keeps the archive from its next registration.
    const auto next = fixture.register_root(request);
    CHECK(next && next->revision == 2);
}

// R-14: the index never decides alone that an archive had no revision. A
// registered and verified archive, whose `register --new-revision` was
// interrupted, and whose index then lost its revision: the record of revision
// 1 and its rows of files removed from the database by hand (sample damage;
// `repo rebuild-index` keeps the record of a revision whose manifest it cannot
// read, decision 0028), with the archive's record left. Abandoning the
// registration removes its plan, and keeps the archive's record, its history
// and its media evidence: the record says revision 1, and the plan creates
// revision 2 of an archive that exists.
void abandoning_keeps_an_archive_whose_index_lost_its_revision()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a.txt", "a (sample data)");
    fixture.archive.add_file("b.txt", "b (sample data)");
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    const auto& archive_uuid = registered->archive_uuid;
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED");
    fixture.archive.add_file("c.txt", "c (sample data)");
    fixture.archive.add_file("d.txt", "d (sample data)").open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    auto request = at(fixture.archive.root());
    request.new_revision = true;
    const auto interrupted = fixture.register_root(request);
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.repository.execute(("DELETE FROM archive_revisions WHERE archive_uuid = '" + archive_uuid +
                                   "'; DELETE FROM files WHERE archive_uuid = '" + archive_uuid + "' AND revision = 1")
                                   .c_str());
    const auto revisions = fixture.store.revisions(archive_uuid);
    CHECK(revisions && revisions->empty());
    const auto before = rows_of(fixture.repository);
    CHECK(before.runs == 1 && before.evidence >= 2);
    CHECK(!fixture.store.archive_without_revision(archive_uuid).value_or(true));

    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto shown = va::abandonable_registration(services, {archive_uuid, std::nullopt, std::nullopt});
    CHECK(shown && shown->revision == 2 && !shown->first_registration);
    const auto abandoned = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
    CHECK(abandoned && abandoned->revision == 2 && !abandoned->first_registration && abandoned->files == 3);
    auto expected = before;
    expected.settings -= 1;  // its plan
    expected.files -= 3;     // its saved progress (a, b and c)
    CHECK(rows_of(fixture.repository) == expected);
    const auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->status == "REGISTERED" && (*archive)->current_revision == 1 &&
          (*archive)->label == registered->label);
    const auto runs = fixture.store.verify_runs(archive_uuid);
    CHECK(runs && runs->size() == 1);
    const auto history = fixture.store.check_history(fixture.repository.context.repository_uuid);
    CHECK(history && history->runs == 1 && !history->first_broken_run);
}

// A first registration, whose archive's record goes with it, is told by its
// plan and by the archive's record, both, never by the index alone (sample
// data, both cases written by hand):
// - a plan that does not create a new archive (`new_archive`), of revision 1,
//   which no version writes, while the record says the archive never had a
//   revision;
// - a first registration's plan, whose revision 1 another operation recorded
//   and left the plan (one of an older version, decision 0023), after which
//   the index lost that revision: the record says revision 1, though the
//   index has none.
// Each keeps the archive's record; the plan and the saved progress go.
void a_first_registration_is_told_by_its_plan_and_the_record()
{
    for (const bool plan_says_new_revision : {true, false}) {
        test::EngineFixture fixture;
        const auto archive_uuid = interrupted_first_registration(fixture);
        if (plan_says_new_revision) {
            auto plan = plan_of(fixture.store, archive_uuid);
            plan["new_archive"] = false;
            CHECK(fixture.store.set_setting("registration:" + archive_uuid, plan.dump()));
        } else if (const auto found = fixture.store.find_archive(archive_uuid); found && *found) {
            auto record = **found;
            record.current_revision = 1;  // as that operation recorded it
            CHECK(fixture.store.update_archive(record));
        }
        const auto revisions = fixture.store.revisions(archive_uuid);
        CHECK(revisions && revisions->empty());
        va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
        const auto shown = va::abandonable_registration(services, {archive_uuid, std::nullopt, std::nullopt});
        CHECK(shown && !shown->first_registration);
        const auto abandoned = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
        CHECK(abandoned && abandoned->files == 4 && !abandoned->first_registration);
        const auto archive = fixture.store.find_archive(archive_uuid);
        CHECK(archive && *archive);
        CHECK(fixture.store.setting("registration:" + archive_uuid) == std::string{});
    }
}

// Decision 0024: never while the process of an attempt of the registration
// still runs on this computer: refused, naming that process, and nothing
// changes. Once that process has ended, or its id belongs to a later process,
// the registration is abandoned at once.
void abandoning_is_refused_while_the_attempt_runs()
{
    for (const bool reused : {false, true}) {
        test::EngineFixture fixture;
        const va::ProcessIdentity attempt{1000, "boot-1:1000", "veriarchive-cli.exe"};
        fixture.processes.self = attempt;
        const auto archive_uuid = interrupted_first_registration(fixture, true);
        // The abandon runs in a process of its own, and the attempt's still runs.
        fixture.processes.self = {2000, "boot-1:2000", "veriarchive-gui.exe"};
        fixture.processes.others = {attempt};
        const auto before = rows_of(fixture.repository);
        const auto plan = fixture.store.setting("registration:" + archive_uuid).value_or("");

        va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
        const std::string running = "the registration of archive " + archive_uuid +
                                    " is still running in process 1000 (veriarchive-cli.exe) on this computer; "
                                    "abandon it once that process has ended";
        const auto asked = va::abandonable_registration(services, {archive_uuid, std::nullopt, std::nullopt});
        CHECK(!asked && asked.error().kind == va::AbandonFailure::Kind::Refused && asked.error().message == running);
        const auto refused = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
        CHECK(!refused && refused.error().kind == va::AbandonFailure::Kind::Refused &&
              refused.error().message == running);
        CHECK(rows_of(fixture.repository) == before);
        CHECK(fixture.store.setting("registration:" + archive_uuid) == plan);

        // Ended (killed), or the id now names a later process.
        fixture.processes.others.clear();
        if (reused) {
            fixture.processes.others.push_back({1000, "boot-1:98765", "notepad.exe"});
        }
        const auto abandoned = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
        CHECK(abandoned && abandoned->files == 4);
        const auto gone = fixture.store.find_archive(archive_uuid);
        CHECK(gone && !*gone);
    }
}

// Where the plan names no process this computer can check (an older
// version's plan, or one from another computer sharing the repository), an
// attempt that said it worked less than 3 minutes ago may still run: refused,
// as the Archive List shows it running (decision 0022). An older or a silent
// one is abandoned. So is a plan of a version before decision 0022, which
// says nothing of its state at all.
void abandoning_judges_an_unseen_attempt_by_its_plan()
{
    const auto old_time = std::string("2026-01-01T00:00:00Z");
    enum class Plan { Older, OtherComputer, BeforeUpdated };
    for (const auto kind : {Plan::Older, Plan::OtherComputer, Plan::BeforeUpdated}) {
        test::EngineFixture fixture;
        const auto archive_uuid = interrupted_first_registration(fixture, true);
        auto plan = plan_of(fixture.store, archive_uuid);
        if (kind == Plan::OtherComputer) {
            plan["process"]["host"] = "another-computer";
            // An unrelated process of this computer has the same id.
            fixture.processes.others = {{plan["process"].value("id", std::uint64_t{0}), "boot-1:1", "other.exe"}};
            fixture.processes.self = {7, "boot-1:7", "veriarchive-gui.exe"};
        } else {
            plan.erase("process");
        }
        if (kind == Plan::BeforeUpdated) {
            plan.erase("updated");
        } else {
            plan["updated"] = va::current_timestamp();
        }
        CHECK(fixture.store.set_setting("registration:" + archive_uuid, plan.dump()));
        va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
        if (kind != Plan::BeforeUpdated) {
            const auto refused = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
            CHECK(!refused && refused.error().kind == va::AbandonFailure::Kind::Refused &&
                  refused.error().message == "an attempt of the registration of archive " + archive_uuid +
                                                 " said it was working less than 3 minutes ago; abandon it once "
                                                 "it has stopped");
            CHECK(!fixture.store.setting("registration:" + archive_uuid).value_or("").empty());
            plan["updated"] = old_time;
            CHECK(fixture.store.set_setting("registration:" + archive_uuid, plan.dump()));
        }
        const auto abandoned = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
        CHECK(abandoned && abandoned->files == 4);
    }
    // A plan that says it stopped is abandoned at once, however fresh the
    // "updated" written with the mark, and whether its process still runs:
    // the GUI's window goes on after its registration was cancelled.
    test::EngineFixture fixture;
    const va::ProcessIdentity window{1000, "boot-1:1000", "veriarchive-gui.exe"};
    fixture.processes.self = window;
    const auto archive_uuid = interrupted_first_registration(fixture);
    fixture.processes.self = {2000, "boot-1:2000", "veriarchive-cli.exe"};
    fixture.processes.others = {window};
    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto abandoned = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
    CHECK(abandoned.has_value());
}

// A plan whose revision another operation recorded and left (one of an older
// version; decision 0023): its rows are the recorded revision's index,
// not saved progress. Abandoning removes the plan alone, and the archive,
// which has a revision, stays.
void abandoning_leaves_a_recorded_revision_alone()
{
    test::EngineFixture fixture;
    const auto archive_uuid = interrupted_first_registration(fixture);
    // Recorded by another operation, which left the plan (sample data).
    CHECK(fixture.store.insert_revision({archive_uuid, 1, "manifests/" + archive_uuid + "/r0001.vam", "{}", "h", "b",
        "", "2026-10-15T01:00:00Z", ""}));
    const auto before = rows_of(fixture.repository);
    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto abandoned = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
    CHECK(abandoned && abandoned->files == 0 && abandoned->bytes == 0 && !abandoned->first_registration);
    auto expected = before;
    expected.settings -= 1;
    CHECK(rows_of(fixture.repository) == expected);
    const auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive);
}

// The GUI names the registration it showed: when the archive's plan is
// another one by now, nothing is abandoned. An archive without an unfinished
// registration has nothing to abandon.
void abandoning_names_the_registration_shown()
{
    test::EngineFixture fixture;
    const auto archive_uuid = interrupted_first_registration(fixture);
    const auto plan = plan_of(fixture.store, archive_uuid);
    const auto created = plan.value("created", std::string{});
    const auto before = rows_of(fixture.repository);
    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const std::string changed = "the unfinished registration of archive " + archive_uuid +
                                " is no longer the one shown (it was resumed, finished or abandoned meanwhile); "
                                "nothing was changed";
    const auto other_revision = va::abandon_registration(services, {archive_uuid, 2u, created});
    CHECK(!other_revision && other_revision.error().message == changed);
    const auto other_plan = va::abandon_registration(services, {archive_uuid, 1u, std::string("2026-01-01T00:00:00Z")});
    CHECK(!other_plan && other_plan.error().message == changed);
    CHECK(rows_of(fixture.repository) == before);
    const auto shown = va::abandon_registration(services, {archive_uuid, 1u, created});
    CHECK(shown.has_value());

    const auto kept = kept_archive(fixture);
    const auto nothing = va::abandon_registration(services, {kept, std::nullopt, std::nullopt});
    CHECK(!nothing && nothing.error().kind == va::AbandonFailure::Kind::Refused &&
          nothing.error().message == "archive " + kept + " has no unfinished registration");
}

// A first registration whose attempt wrote .veriarchive/identity.json on the
// media (R-89) says so: abandoning never writes the media (R-03), and the
// file would name an archive that no longer exists.
void abandoning_reports_an_identity_file_on_the_media()
{
    test::EngineFixture fixture;
    const auto archive_uuid = interrupted_first_registration(fixture, false, true);
    CHECK(fixture.archive.contains("broken/.veriarchive/identity.json"));
    CHECK(plan_of(fixture.store, archive_uuid).value("media_copy", false));
    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto abandoned = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
    CHECK(abandoned && abandoned->media_copy && abandoned->first_registration);
    CHECK(fixture.archive.contains("broken/.veriarchive/identity.json"));
}

// The repository's own guard: the record of an archive is removed only while
// it holds nothing but itself, whoever asks. Not one with a recorded
// revision; not one whose record says it has a revision, or was registered,
// though the index has none (R-14); not one that a verification or media
// evidence refers to. One whose first registration never finished is, also
// once the operator retired it.
void an_archive_with_a_revision_is_never_removed()
{
    test::EngineFixture fixture;
    const auto kept = kept_archive(fixture);
    CHECK(!fixture.store.archive_without_revision(kept).value_or(true));
    const auto refused = fixture.store.remove_archive_without_revision(kept);
    CHECK(refused && !*refused);
    const auto archive = fixture.store.find_archive(kept);
    CHECK(archive && *archive);

    // Records written by hand (sample data), each but one held by one thing.
    enum class Held { Nothing, Retired, RecordSaysRevision, RecordSaysRegistered, Revision, Verification, Evidence };
    std::uint32_t number = 0;
    for (const auto held : {Held::Nothing, Held::Retired, Held::RecordSaysRevision, Held::RecordSaysRegistered,
             Held::Revision, Held::Verification, Held::Evidence}) {
        const va::Uuid archive_uuid = "c0ffee00-0000-4000-8000-0000000000" + std::to_string(30 + number++);
        const std::string status = held == Held::Retired                ? "RETIRED"
                                   : held == Held::RecordSaysRegistered ? "REGISTERED"
                                                                        : "REGISTRATION_INCOMPLETE";
        CHECK(fixture.store.insert_archive({archive_uuid, "ARCH-0099", "2026-10-16T01:00:00Z",
            held == Held::RecordSaysRevision ? 1u : 0u, status, "", ""}));
        if (held == Held::Revision) {
            CHECK(fixture.store.insert_revision({archive_uuid, 1, "manifests/" + archive_uuid + "/r0001.vam", "{}",
                "h", "b", "", "2026-10-16T01:00:00Z", ""}));
        } else if (held == Held::Verification) {
            va::VerifyRunRecord run;
            run.run_id = "c0ffee00-0000-4000-8000-0000000000ff";
            run.archive_uuid = archive_uuid;
            run.mode = "FULL";
            run.started = "2026-10-16T01:00:00Z";
            run.finished = "2026-10-16T01:00:01Z";
            run.result = "FAILED";
            CHECK(fixture.store.append_verify_run(run, fixture.repository.context.repository_uuid).has_value());
        } else if (held == Held::Evidence) {
            CHECK(fixture.store.insert_evidence({archive_uuid, "", "2026-10-16T01:00:00Z", "{}", "MATCH", ""}));
        }
        const bool removable = held == Held::Nothing || held == Held::Retired;
        CHECK(fixture.store.archive_without_revision(archive_uuid).value_or(!removable) == removable);
        const auto removed = fixture.store.remove_archive_without_revision(archive_uuid);
        CHECK(removed && *removed == removable);
        const auto found = fixture.store.find_archive(archive_uuid);
        CHECK(found && found->has_value() == !removable);
        if (!removed || *removed != removable) {
            std::cerr << "  case " << static_cast<int>(held) << '\n';
        }
    }
}

// Decision 0024: what the platform's process table says, judged. Fake
// processes (R-93); platform_test.cpp judges real ones.
void judges_the_attempt_a_plan_names()
{
    test::FakeProcessTable processes;
    processes.self = {4242, "boot-1:4242", "veriarchive-gui.exe"};
    const auto plan_naming = [](std::uint64_t id, const std::string& start, const std::string& host) {
        nlohmann::json plan{{"archive_uuid", "c0ffee00-0000-4000-8000-000000000001"}, {"revision", 1}};
        va::record_attempt(plan, {{id, start, "veriarchive-cli.exe"}, host});
        return plan;
    };
    using va::AttemptState;
    // Whether this process runs the operation of the plan, how often that was
    // asked, and of which plan.
    bool runs_an_operation = false;
    int asked = 0;
    nlohmann::json asked_of;
    const std::function<bool(const nlohmann::json&)> operation = [&](const nlohmann::json& judged_plan) {
        ++asked;
        asked_of = judged_plan;
        return runs_an_operation;
    };
    const auto judged = [&](const nlohmann::json& plan) {
        return va::judge_attempt(plan, processes, "test-host", operation).state;
    };

    const auto plan = plan_naming(1000, "boot-1:1000", "test-host");
    const auto recorded = va::recorded_attempt(plan);
    CHECK(recorded && recorded->process.id == 1000 && recorded->process.start == "boot-1:1000" &&
          recorded->process.program == "veriarchive-cli.exe" && recorded->host == "test-host");
    CHECK(judged(plan) == AttemptState::Ended);                    // no process 1000
    processes.others = {{1000, "boot-1:1000", "veriarchive-cli.exe"}};
    CHECK(judged(plan) == AttemptState::Running);
    processes.others = {{1000, "boot-1:1001", "notepad.exe"}};     // a later process with that id
    CHECK(judged(plan) == AttemptState::Ended);
    processes.others = {{1000, "boot-2:1000", "veriarchive-cli.exe"}};  // the same ticks, after a reboot
    CHECK(judged(plan) == AttemptState::Ended);

    processes.others = {{1000, "boot-1:1000", "veriarchive-cli.exe"}};
    auto stopped = plan;
    stopped["stopped"] = "2026-10-16T01:00:00Z";
    CHECK(judged(stopped) == AttemptState::Ended);                 // its last word
    CHECK(judged(plan_naming(1000, "boot-1:1000", "another-host")) == AttemptState::Unknown);
    CHECK(judged(nlohmann::json{{"archive_uuid", "x"}, {"revision", 1}}) == AttemptState::Unknown);
    CHECK(judged(nlohmann::json("not a plan")) == AttemptState::Unknown);
    auto damaged = plan;
    damaged["process"]["id"] = "1000";
    CHECK(judged(damaged) == AttemptState::Unknown);
    processes.error = va::IoError{va::IoErrorKind::PermissionDenied, 5, "OpenProcess failed (Win32 error 5)"};
    CHECK(judged(plan) == AttemptState::Unknown);                  // cannot tell
    processes.error.reset();
    // Whether this process runs an operation is asked of no plan but one
    // that names this process.
    CHECK(asked == 0);

    // This very process: an attempt it ran has ended, unless it runs an
    // operation now, asked as the plan is judged; without the question, it
    // runs none.
    const auto own = plan_naming(4242, "boot-1:4242", "test-host");
    CHECK(judged(own) == AttemptState::Ended && asked == 1 && asked_of == own);
    runs_an_operation = true;
    CHECK(judged(own) == AttemptState::Running && asked == 2);
    CHECK(va::judge_attempt(own, processes, "test-host").state == AttemptState::Ended);
    CHECK(judged(plan_naming(4242, "boot-1:1", "test-host")) == AttemptState::Ended);  // an earlier process 4242
    CHECK(asked == 2);
}

} // namespace

void abandon_registration_tests()
{
    judges_the_attempt_a_plan_names();
    abandoning_a_first_registration_removes_it_whole();
    abandoning_a_new_revision_keeps_the_archive();
    abandoning_keeps_an_archive_whose_index_lost_its_revision();
    a_first_registration_is_told_by_its_plan_and_the_record();
    abandoning_is_refused_while_the_attempt_runs();
    abandoning_judges_an_unseen_attempt_by_its_plan();
    abandoning_leaves_a_recorded_revision_alone();
    abandoning_names_the_registration_shown();
    abandoning_reports_an_identity_file_on_the_media();
    an_archive_with_a_revision_is_never_removed();
}
