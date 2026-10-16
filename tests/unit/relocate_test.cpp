#include "test_support.h"

#include "engine_fixture.h"

#include "common/time.h"
#include "core/engine/abandon_registration.h"
#include "core/engine/abandon_relocation.h"
#include "core/engine/heartbeat.h"
#include "core/engine/registration_attempt.h"
#include "core/engine/relocate_engine.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <semaphore>
#include <set>
#include <thread>
#include <utility>

namespace {

// The sample files of the larger archive (sample data): 40 files of about 500
// bytes in two folders, 20780 bytes in all; the first 20, the photos, hold
// 10190 of them.
std::vector<std::pair<std::string, std::string>> sample_files()
{
    std::vector<std::pair<std::string, std::string>> files;
    for (int index = 0; index < 40; ++index) {
        files.emplace_back((index < 20 ? "photos/p" : "videos/v") + std::to_string(100 + index) + ".bin",
            std::string(500 + index, static_cast<char>('a' + index % 26)));
    }
    return files;
}
constexpr std::uint64_t sample_bytes = 20780;
constexpr std::uint64_t sample_photo_bytes = 10190;

// Volume 1 is mounted at /vol, volume 2 at /vol/new (a different disk). The
// archive is registered at /vol/old: two small files, or with `sample` the
// 40 sample files.
struct RelocationFixture : test::EngineFixture {
    va::Uuid archive_uuid;
    bool sample = false;

    explicit RelocationFixture(bool with_sample = false) : sample(with_sample)
    {
        auto second = test::fake_volume(archive.root() / "new");
        second.filesystem_uuid = "5555666677778888";
        second.filesystem_uuid_short = "77778888";
        second.devices.front().serial = "NEW-DISK";
        media.extra_volumes.push_back(second);

        if (sample) {
            for (const auto& [name, content] : sample_files()) {
                archive.add_file("old/" + name, content);
            }
        } else {
            archive.add_file("old/photos/a.jpg", "jpeg");
            archive.add_file("old/notes.txt", "notes");
        }
        const auto registered = register_root(archive.root() / "old");
        CHECK(registered.has_value());
        if (registered) {
            archive_uuid = registered->archive_uuid;
        }
    }

    // The operator's own copy of the archive onto the new disk (at `folder`).
    void copy_to_new_disk(const std::string& folder = "new")
    {
        if (sample) {
            for (const auto& [name, content] : sample_files()) {
                archive.add_file(folder + "/" + name, content);
            }
            return;
        }
        archive.add_file(folder + "/photos/a.jpg", "jpeg").mtime_ns += 3'600'000'000'000; // copy changed mtime
        archive.add_file(folder + "/notes.txt", "notes");
    }

    std::expected<va::RelocateOutcome, va::RelocateFailure> relocate(
        const std::filesystem::path& root, bool resume = false)
    {
        va::RelocateServices services{archive, *repository.files, *repository.writer, media, store,
            repository.context.repo, repository.context.repository_uuid, host, processes};
        va::RelocateRequest request{archive_uuid, root, std::nullopt, resume};
        return va::relocate_archive(services, request, cancel, progress);
    }

    std::string key() const { return va::relocation_key(archive_uuid); }

    // The relocation's record (decision 0026), or null when there is none.
    nlohmann::json record()
    {
        return nlohmann::json::parse(store.setting(key()).value_or(""), nullptr, false);
    }

    // The checkpoints of the run `run_id`: entries and their recorded bytes.
    va::CheckpointCount saved(const std::string& run_id)
    {
        return store.progress_count(run_id, archive_uuid, 1).value_or(va::CheckpointCount{999, 999});
    }

    // Relocates the sample to the new disk, which goes away when the 21st of
    // its 40 files is opened: the relocation stops at about half, having
    // matched the 20 photos, in the process `process`. With `killed`, that
    // process ended without writing the record's stop mark. Returns the
    // relocation's run id.
    std::string interrupted_relocation(const va::ProcessIdentity& process, bool killed)
    {
        copy_to_new_disk();
        const auto runs_in = processes.self;
        processes.self = process;
        archive.node("new/videos/v120.bin").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
        const auto interrupted = relocate(archive.root() / "new");
        archive.node("new/videos/v120.bin").open_error.reset();
        processes.self = runs_in;
        CHECK(!interrupted && interrupted.error().kind == va::RelocateFailure::Kind::Incomplete &&
              interrupted.error().message == "relocation was interrupted; continue it with `archive relocate " +
                                                 archive_uuid + " <root_path> --resume`");
        auto stored = record();
        if (killed && stored.is_object()) {
            stored.erase("stopped");
            CHECK(store.set_setting(key(), stored.dump()));
        }
        return stored.is_object() ? stored.value("run_id", std::string{}) : std::string{};
    }
};

// The rows of a table of the repository database, as another process would count them.
std::int64_t count_rows(RelocationFixture& fixture, const std::string& table)
{
    sqlite3* database = nullptr;
    const auto name = fixture.repository.context.repo.database().u8string();
    std::int64_t rows = -1;
    if (sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &database, SQLITE_OPEN_READONLY, nullptr) ==
        SQLITE_OK) {
        sqlite3_stmt* statement = nullptr;
        const auto sql = "SELECT COUNT(*) FROM " + table;
        if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            rows = sqlite3_column_int64(statement, 0);
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close(database);
    return rows;
}

// Each entry of a manifest: its type and hashes, by path.
std::map<std::string, std::string> hashes_of(RelocationFixture& fixture, std::uint32_t revision)
{
    std::map<std::string, std::string> hashes;
    auto reader = va::ManifestReader::open(*fixture.repository.files,
        va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, revision));
    CHECK(reader.has_value());
    while (reader) {
        auto next = reader->next();
        if (!next || !*next) {
            break;
        }
        hashes[(**next).path.p] = std::string(1, (**next).type) + ":" + (**next).b3 + ":" + (**next).sha256;
    }
    return hashes;
}

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
    CHECK(!relocated->resumed);

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
    // Finished: its record and its checkpoints are gone (decision 0026).
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    CHECK(fixture.saved(relocated->run_id).files == 0);
}

// Decision 0026: a copy that does not hold the recorded content ends the
// relocation. Its record and saved progress go: the operator copies again,
// which changes the carrier, so a new relocation reads it all.
void differing_copy_is_refused()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    fixture.archive.node("new/videos/v139.bin").content[7] ^= 0x01; // corrupted copy (sample data)
    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Differs);
    CHECK(!relocated && relocated.error().items.size() == 1);
    const auto archive = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 1);
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    CHECK(count_rows(fixture, "verify_progress") == 0);

    // Copied again, the carrier is read in full, and accepted.
    fixture.archive.node("new/videos/v139.bin").content[7] ^= 0x01;
    fixture.archive.bytes_read = 0;
    const auto again = fixture.relocate(fixture.archive.root() / "new");
    CHECK(again && again->revision == 2 && !again->resumed);
    CHECK(fixture.archive.bytes_read == sample_bytes);
}

std::string differs_message()
{
    return "the new carrier does not hold the recorded content; nothing was changed (use `register "
           "--new-revision` to re-baseline changed content)";
}

// Decision 0026: a copy that differs ends the relocation also while another
// operation holds the write lock for longer than a statement waits for it
// (`repo rebuild-index`, the record of a large archive): its record and saved
// progress go in a transaction that waits for the lock, as its record would.
// Left behind, they would let a resume accept a second copy without reading
// again what matched in this one. Here a resumed relocation finds the copy
// different while another connection holds the lock (scaled down: the
// relocation's statements wait 100 ms, the lock is held 1 s). The operator
// copies again, and the copy now damages a photo that matched before, keeping
// its size (sample data): the next relocation reads the whole carrier and
// finds it.
void a_differing_copy_ends_the_relocation_while_the_lock_is_held()
{
    RelocationFixture fixture(true);
    const auto run_id = fixture.interrupted_relocation(fixture.processes.self, false);
    CHECK(fixture.saved(run_id).files == 20);
    fixture.archive.node("new/videos/v139.bin").content[7] ^= 0x01; // a corrupted copy (sample data)
    std::thread holder;
    std::binary_semaphore held{0};
    // When the last file is opened, another process takes the write lock.
    test::HookedFiles hooked(fixture.archive, "v139.bin", [&] {
        holder = std::thread([&] {
            auto store = fixture.repository.store();
            CHECK(store.begin());
            held.release();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            CHECK(store.commit());
        });
        CHECK(held.try_acquire_for(std::chrono::seconds(60)));
    });
    auto store = fixture.repository.store(std::chrono::milliseconds(100));
    va::RelocateServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media,
        store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto differs = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt, true}, fixture.cancel, fixture.progress);
    if (holder.joinable()) {
        holder.join();
    }
    CHECK(!differs && differs.error().kind == va::RelocateFailure::Kind::Differs &&
          differs.error().message == differs_message());
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    CHECK(count_rows(fixture, "verify_progress") == 0);

    fixture.archive.node("new/videos/v139.bin").content[7] ^= 0x01;
    fixture.archive.node("new/photos/p100.bin").content[3] ^= 0x01; // damaged by the second copy (sample data)
    fixture.archive.bytes_read = 0;
    const auto again = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!again && again.error().kind == va::RelocateFailure::Kind::Differs && again.error().items.size() == 1 &&
          again.error().items.front().path.p == "photos/p100.bin");
    CHECK(fixture.archive.bytes_read == sample_bytes);
    const auto archive = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 1);
}

// When the record and saved progress of a relocation whose copy differs still
// cannot be removed (here a trigger makes the removal fail, standing in for a
// lock held for longer than the record waits), the refusal says so, with the
// cause, and how to go on: abandon the relocation before relocating again.
// Abandoned, the next relocation reads the whole carrier.
void a_differing_copy_whose_relocation_cannot_end_says_to_abandon_it()
{
    RelocationFixture fixture(true);
    const auto run_id = fixture.interrupted_relocation(fixture.processes.self, false);
    fixture.archive.node("new/videos/v139.bin").content[7] ^= 0x01; // a corrupted copy (sample data)
    fixture.repository.execute("CREATE TRIGGER sample_failure BEFORE DELETE ON verify_progress "
                               "BEGIN SELECT RAISE(ABORT, 'sample failure'); END;");
    const auto differs = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!differs && differs.error().kind == va::RelocateFailure::Kind::Differs &&
          differs.error().items.size() == 1);
    CHECK(!differs && differs.error().message ==
                          "the new carrier does not hold the recorded content; nothing was changed, but the "
                          "relocation's saved progress could not be removed: cannot run SQL: sample failure; "
                          "abandon the relocation with `archive abandon-relocation " +
                              fixture.archive_uuid + "` before relocating again");
    // Nothing of it went: the 20 photos matched before, and the 19 videos
    // that matched now.
    CHECK(fixture.record().is_object());
    CHECK(fixture.saved(run_id).files == 39);
    fixture.repository.execute("DROP TRIGGER sample_failure;");

    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto abandoned = va::abandon_relocation(services, {fixture.archive_uuid, run_id});
    CHECK(abandoned && abandoned->files == 39);
    fixture.archive.node("new/videos/v139.bin").content[7] ^= 0x01;
    fixture.archive.bytes_read = 0;
    const auto again = fixture.relocate(fixture.archive.root() / "new");
    CHECK(again && again->revision == 2 && !again->resumed);
    CHECK(fixture.archive.bytes_read == sample_bytes);
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
        fixture.repository.context.repository_uuid, fixture.host, fixture.processes};
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

// ---- A relocation resumes (decision 0026) -----------------------------------

// R-104: killed at about half, a relocation keeps a record of its own and the
// entries that matched. Started again without --resume it is refused, naming
// both ways out, and reads nothing. `--resume` reads only the rest, measured
// by the bytes read, carries the entries matched before with the recorded
// hashes, and records revision 2 with one FULL verification in the history,
// which says that it was resumed. Its record and checkpoints are gone then.
void an_interrupted_relocation_resumes_without_reading_again()
{
    RelocationFixture fixture(true);
    const va::ProcessIdentity first{1000, "boot-1:1000", "veriarchive-cli.exe"};
    const auto run_id = fixture.interrupted_relocation(first, true);
    CHECK(!run_id.empty());

    // The record: the archive, the revision it verifies, where it reads, the
    // new carrier's evidence at the start, the totals, when it last worked,
    // and the process that ran it.
    const auto record = fixture.record();
    CHECK(record.is_object());
    if (!record.is_object()) {
        return;
    }
    CHECK(record.value("archive_uuid", std::string{}) == fixture.archive_uuid);
    CHECK(record.value("revision", 0) == 1 && record.value("label", std::string{}) == "ARCH-0001");
    CHECK(record.value("root", std::string{}) == va::path_to_utf8(fixture.archive.root() / "new"));
    CHECK(record.value("root_subpath", std::string{"x"}).empty());
    CHECK(record.contains("evidence") &&
          record["evidence"]["volume"].value("filesystem_uuid", std::string{}) == "5555666677778888" &&
          va::parse_timestamp(record["evidence"].value("seen_at", std::string{})).has_value());
    CHECK(record.value("files_total", 0) == 40 && record.value("bytes_total", std::uint64_t{0}) == sample_bytes);
    CHECK(va::parse_timestamp(record.value("started", std::string{})).has_value());
    CHECK(va::parse_timestamp(record.value("updated", std::string{})).has_value());
    CHECK(!record.contains("stopped")); // killed
    const auto attempt = va::recorded_attempt(record);
    CHECK(attempt && attempt->process.id == 1000 && attempt->host == "test-host");
    // A record of its own, never a verification's (decision 0022).
    const auto runs_listed = fixture.store.settings_with_prefix("run:");
    CHECK(runs_listed && runs_listed->empty());
    const auto saved = fixture.saved(run_id);
    CHECK(saved.files == 20 && saved.bytes == sample_photo_bytes);

    // Started again without --resume: refused, naming both ways out.
    fixture.archive.bytes_read = 0;
    const auto again = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!again && again.error().kind == va::RelocateFailure::Kind::Refused &&
          again.error().message == "an unfinished relocation of archive " + fixture.archive_uuid + " (to \"" +
                                       va::path_to_utf8(fixture.archive.root() / "new") +
                                       "\") exists; resume it with `archive relocate " + fixture.archive_uuid +
                                       " <root_path> --resume`, or abandon it with `archive abandon-relocation " +
                                       fixture.archive_uuid + "`");
    CHECK(fixture.archive.bytes_read == 0);
    CHECK(fixture.record() == record);

    // Resumed (process 1000 has ended): only the rest is read.
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(resumed && resumed->revision == 2 && resumed->resumed && resumed->run_id == run_id &&
          resumed->result == "PASSED");
    if (!resumed) {
        std::cerr << "  resume: " << resumed.error().message << '\n';
        return;
    }
    CHECK(fixture.archive.bytes_read == sample_bytes - sample_photo_bytes);
    // Every entry of revision 2 carries the hashes revision 1 recorded.
    const auto before = hashes_of(fixture, 1);
    CHECK(before.size() == 40 && hashes_of(fixture, 2) == before);
    const auto rows = fixture.store.file_rows(fixture.archive_uuid, 2);
    CHECK(rows && rows->size() == 40);
    // One FULL verification of revision 2 in the history, the relocation's
    // run, which covered every byte and says it was resumed once.
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1);
    if (runs && runs->size() == 1) {
        const auto& run = runs->front();
        CHECK(run.run_id == run_id && run.revision == 2 && run.mode == "FULL" && run.result == "PASSED");
        CHECK(run.coverage_bytes == sample_bytes && run.started == record.value("started", std::string{}));
        const auto counts = nlohmann::json::parse(run.counts_json, nullptr, false);
        CHECK(counts.is_object() && counts.value("resumed", 0) == 1 &&
              counts.value("relocated_from_revision", 0) == 1);
    }
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    CHECK(fixture.saved(run_id).files == 0);
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
    // Nothing is left to resume.
    const auto after = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!after &&
          after.error().message == "archive " + fixture.archive_uuid + " has no unfinished relocation to resume");
}

// A resume needs the volume and the folder the relocation started on: a disk
// formatted since (another filesystem id) or another folder says nothing of
// what matched before; the resume is refused, reads nothing and changes
// nothing. The same disk reached through another mount point (a drive
// letter that changed) is the same volume, and resumes.
void a_resume_needs_the_volume_it_started_on()
{
    RelocationFixture fixture(true);
    const auto run_id = fixture.interrupted_relocation(fixture.processes.self, false);
    const auto record = fixture.record();
    CHECK(record.is_object() && record.contains("stopped"));
    const auto refusal = "the volume or folder of this root is not the one the unfinished relocation of archive " +
                         fixture.archive_uuid + " started on (\"" +
                         va::path_to_utf8(fixture.archive.root() / "new") +
                         "\"); resume it there, or abandon it with `archive abandon-relocation " +
                         fixture.archive_uuid + "` and start again";

    auto& disk = fixture.media.extra_volumes.front();
    disk.filesystem_uuid = "9999aaaabbbbcccc"; // formatted since (sample data)
    disk.filesystem_uuid_short = "bbbbcccc";
    fixture.archive.bytes_read = 0;
    const auto formatted = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!formatted && formatted.error().kind == va::RelocateFailure::Kind::Refused &&
          formatted.error().message == refusal);
    disk.filesystem_uuid = "5555666677778888";
    disk.filesystem_uuid_short = "77778888";
    const auto folder = fixture.relocate(fixture.archive.root() / "new" / "photos", true);
    CHECK(!folder && folder.error().message == refusal);
    CHECK(fixture.archive.bytes_read == 0);
    CHECK(fixture.record() == record);

    // The same disk at another mount point: resumed there, reading the rest.
    fixture.copy_to_new_disk("moved");
    disk.mount_point = fixture.archive.root() / "moved";
    const auto resumed = fixture.relocate(fixture.archive.root() / "moved", true);
    CHECK(resumed && resumed->revision == 2 && resumed->run_id == run_id);
    CHECK(fixture.archive.bytes_read == sample_bytes - sample_photo_bytes);
    const auto verified = fixture.verify(fixture.archive.root() / "moved");
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
}

// R-104: a relocation resumes only while its archive is at the revision it
// verifies. The old carrier registered again with changed content (`register
// --new-revision`) records revision 2, the revision the relocation would
// create: the registration's record removes the relocation's record and
// saved progress, which can never finish now, and nothing is left to resume.
// A record that another operation left behind (one that does not remove it,
// such as an import) goes with the resume that finds its revision recorded.
// One whose archive is at another revision without that (rebuild-index after
// a manifest it skipped) is refused, and stays for the operator to abandon.
void a_resume_needs_the_revision_it_verifies()
{
    RelocationFixture fixture(true);
    const auto run_id = fixture.interrupted_relocation(fixture.processes.self, false);
    const auto record = fixture.record();
    fixture.archive.add_file("old/added.txt", "added on the old carrier (sample data)");
    va::RegisterRequest request;
    request.root = fixture.archive.root() / "old";
    request.new_revision = true;
    const auto rebaselined = fixture.register_root(request);
    CHECK(rebaselined && rebaselined->revision == 2);
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    CHECK(fixture.saved(run_id).files == 0);
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!resumed && resumed.error().kind == va::RelocateFailure::Kind::Refused &&
          resumed.error().message == "archive " + fixture.archive_uuid + " has no unfinished relocation to resume");

    // Left behind by another operation, with its checkpoints.
    CHECK(fixture.store.set_setting(fixture.key(), record.dump()));
    va::Entry photo;
    photo.path.p = "photos/p100.bin";
    CHECK(fixture.store.add_progress(run_id, {va::entry_key(photo)}));
    const auto dead = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!dead && dead.error().message == "revision 2 of archive " + fixture.archive_uuid +
                                               " was recorded by another operation; the unfinished relocation can "
                                               "no longer finish, and its saved progress was removed");
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    CHECK(fixture.store.progress(run_id).value_or(std::set<std::string>{"x"}).empty());

    // Verifying a revision the archive is not at, which is not recorded either.
    auto ahead = record;
    ahead["revision"] = 3;
    CHECK(fixture.store.set_setting(fixture.key(), ahead.dump()));
    const auto refused = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!refused && refused.error().message == "archive " + fixture.archive_uuid +
                                                     " is at revision 2, not at revision 3 that its unfinished "
                                                     "relocation verifies; it cannot be resumed (abandon it with "
                                                     "`archive abandon-relocation " +
                                                     fixture.archive_uuid + "`)");
    CHECK(fixture.record() == ahead);
    CHECK(fixture.archive.bytes_read == 0);
}

std::string still_running_message(const va::ProcessIdentity& process)
{
    return "this relocation is still running in process " + std::to_string(process.id) + " (" + process.program +
           ") on this computer; resume it once that process has ended";
}

// Decision 0024: a resume is refused while the process that ran the attempt
// before still runs on this computer, naming it; it reads nothing and leaves
// the record as it is. Once that process has ended it goes ahead at once,
// also when its id belongs to a later process now.
void a_resume_waits_while_the_attempt_before_runs()
{
    RelocationFixture fixture(true);
    const va::ProcessIdentity first{1000, "boot-1:1000", "veriarchive-cli.exe"};
    fixture.interrupted_relocation(first, true);
    const auto record = fixture.record();
    fixture.processes.self = {2000, "boot-1:2000", "veriarchive-cli.exe"};
    fixture.processes.others = {first};
    fixture.archive.bytes_read = 0;
    const auto refused = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!refused && refused.error().kind == va::RelocateFailure::Kind::Refused &&
          refused.error().message == still_running_message(first));
    CHECK(fixture.archive.bytes_read == 0);
    CHECK(fixture.record() == record);

    fixture.processes.others = {{1000, "boot-1:98765", "notepad.exe"}}; // a later process with that id
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(resumed && resumed->revision == 2);
}

// An archive has one unfinished relocation at a time (decision 0026). While
// a resumed relocation runs (here: its manifest is written), another
// relocation of the archive is refused before it reads anything, and so is a
// second resume on the same computer, which finds the first's process
// running. The one that runs records.
void attempts_of_one_relocation_take_turns()
{
    RelocationFixture fixture(true);
    fixture.interrupted_relocation(fixture.processes.self, false);
    test::FakeProcessTable other_processes;
    other_processes.self = {3000, "boot-1:3000", "veriarchive-cli.exe"};
    other_processes.others = {{2000, "boot-1:2000", "veriarchive-cli.exe"}};
    std::expected<va::RelocateOutcome, va::RelocateFailure> fresh = std::unexpected(va::RelocateFailure{});
    std::expected<va::RelocateOutcome, va::RelocateFailure> second = std::unexpected(va::RelocateFailure{});
    std::uint64_t read_by_others = 0;
    bool ran = false;
    test::StagingHookWriter hooked(*fixture.repository.writer, [&] {
        if (std::exchange(ran, true)) {
            return;
        }
        // Another process, with a connection of its own.
        auto store = fixture.repository.store();
        va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
            fixture.media, store, fixture.repository.context.repo, fixture.repository.context.repository_uuid,
            fixture.host, other_processes};
        const auto read_before = fixture.archive.bytes_read;
        fresh = va::relocate_archive(services, {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt},
            fixture.cancel, fixture.progress);
        second = va::relocate_archive(services,
            {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt, true}, fixture.cancel,
            fixture.progress);
        read_by_others = fixture.archive.bytes_read - read_before;
    });
    fixture.processes.self = {2000, "boot-1:2000", "veriarchive-cli.exe"};
    va::RelocateServices services{fixture.archive, *fixture.repository.files, hooked, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto first = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt, true}, fixture.cancel, fixture.progress);

    CHECK(ran && first && first->revision == 2);
    CHECK(!fresh && fresh.error().kind == va::RelocateFailure::Kind::Refused &&
          fresh.error().message.starts_with("an unfinished relocation of archive " + fixture.archive_uuid));
    CHECK(!second && second.error().message == still_running_message({2000, "boot-1:2000", "veriarchive-cli.exe"}));
    CHECK(read_by_others == 0);
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
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

// Decision 0023: two attempts of one relocation at once, on two computers
// sharing the repository, where neither can see the other's process. The
// second resumes the first's relocation while the first has written and
// flushed its manifest of revision 2, records revision 2 (reading nothing:
// the first had matched every entry), and the first finds the revision
// recorded and stops. The recorded manifest keeps the second one's bytes.
void attempts_of_one_relocation_on_two_computers_record_the_revision_once()
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk();
    std::expected<va::RelocateOutcome, va::RelocateFailure> other = std::unexpected(va::RelocateFailure{});
    bool ran = false;
    test::StagingHookWriter hooked(*fixture.repository.writer, [&] {
        if (std::exchange(ran, true)) {
            return;
        }
        auto store = fixture.repository.store();
        auto host = fixture.host;
        host.host = "host-first";
        va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
            fixture.media, store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, host,
            fixture.processes};
        other = va::relocate_archive(services,
            {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt, true}, fixture.cancel,
            fixture.progress);
    });
    auto host = fixture.host;
    host.host = "host-second";
    va::RelocateServices services{fixture.archive, *fixture.repository.files, hooked, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, host, fixture.processes};
    const auto first = va::relocate_archive(services, {fixture.archive_uuid, fixture.archive.root() / "new",
        std::nullopt}, fixture.cancel, fixture.progress);

    CHECK(ran && other && other->revision == 2 && other->resumed);
    CHECK(!first && first.error().kind == va::RelocateFailure::Kind::Refused &&
          first.error().message == "revision 2 of archive " + fixture.archive_uuid +
                                       " was recorded by another operation while this relocation ran; "
                                       "nothing was changed");
    const auto recorded = fixture.store.revision(fixture.archive_uuid, 2);
    auto reader = va::ManifestReader::open(*fixture.repository.files,
        va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 2));
    CHECK(recorded && *recorded && reader && reader->header_blake3() == (*recorded)->header_blake3 &&
          reader->header().host == "host-first");
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
    CHECK(manifest_names(fixture) == (std::set<std::string>{"r0001.vam", "r0002.vam"}));
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 2);
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
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
            fixture.host, fixture.processes};
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

// Decision 0026: a relocation still hashing when a registration of the
// archive records the revision it creates (`register --new-revision` of the
// old carrier, in another process). The registration's record removes the
// relocation's record and saved progress, as the relocation can never
// finish; the relocation stops at its next checkpoint and records nothing.
void a_relocation_that_loses_while_hashing_records_nothing()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    fixture.archive.add_file("old/added.txt", "added on the old carrier (sample data)");
    std::expected<va::RegisterOutcome, va::RegisterFailure> registered = std::unexpected(va::RegisterFailure{});
    std::string run_id;
    test::HookedFiles hooked(fixture.archive, "p110.bin", [&] {
        auto store = fixture.repository.store();
        const auto stored = nlohmann::json::parse(store.setting(fixture.key()).value_or(""), nullptr, false);
        run_id = stored.is_object() ? stored.value("run_id", std::string{}) : std::string{};
        va::RegisterRequest request;
        request.root = fixture.archive.root() / "old";
        request.new_revision = true;
        va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
            fixture.media_writer, fixture.media, store, fixture.repository.context.repo, fixture.host,
            fixture.processes};
        registered = va::register_archive(services, request, fixture.cancel, fixture.progress);
    });
    // Whether the relocation got as far as writing its manifest: it stops at
    // its checkpoint, before.
    bool staged = false;
    test::StagingHookWriter watched(*fixture.repository.writer, [&] { staged = true; });
    va::RelocateServices services{hooked, *fixture.repository.files, watched, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto relocated = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    CHECK(!staged);
    CHECK(registered && registered->revision == 2);
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Refused &&
          relocated.error().message == "revision 2 of archive " + fixture.archive_uuid +
                                           " was recorded by another operation while this relocation ran; "
                                           "nothing was changed");
    CHECK(!run_id.empty() && fixture.saved(run_id).files == 0);
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    // Revision 2 is the registration's, of the old carrier.
    auto reader = va::ManifestReader::open(*fixture.repository.files,
        va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 2));
    CHECK(reader && reader->header().identity.filesystem_uuid == "1111222233334444");
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->empty());
}

// Decision 0026: the record of a relocation is written only while the key
// holds that relocation's record and the revision it creates is not
// recorded. Once the relocation was abandoned and another one began, or its
// revision was recorded, neither a beat nor the stop mark of its attempt
// changes what the key holds.
void the_heartbeat_writes_only_its_own_relocation()
{
    test::TestRepository repository;
    auto store = repository.store();
    const va::Uuid uuid = "c0ffee00-0000-4000-8000-000000000026";
    const auto key = va::relocation_key(uuid);
    const nlohmann::json mine{{"archive_uuid", uuid}, {"revision", 1}, {"run_id", "run-mine"}};
    CHECK(store.set_setting(key, mine.dump()));
    test::NullProgress progress;
    va::Heartbeat heartbeat(store, key, mine, progress);
    heartbeat.only_while_relocating(uuid, "run-mine", 2);
    heartbeat.beat();
    const auto beaten = nlohmann::json::parse(store.setting(key).value_or(""), nullptr, false);
    CHECK(beaten.is_object() && beaten.contains("updated") && beaten.value("run_id", std::string{}) == "run-mine");

    const nlohmann::json other{{"archive_uuid", uuid}, {"revision", 1}, {"run_id", "run-other"}};
    CHECK(store.set_setting(key, other.dump()));
    heartbeat.beat();
    heartbeat.stop();
    CHECK(store.setting(key).value_or("") == other.dump());

    CHECK(store.set_setting(key, mine.dump()));
    CHECK(store.insert_revision(
        {uuid, 2, "manifests/" + uuid + "/r0002.vam", "{}", "h", "b", "", "2026-10-16T01:00:00Z"}));
    heartbeat.stop();
    CHECK(store.setting(key).value_or("") == mine.dump());
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

// The refusal of a relocation that leaves the revision to a registration
// with saved files (decisions 0023, 0025, 0026).
std::string registering_message(RelocationFixture& fixture, const std::filesystem::path& root)
{
    return "an unfinished registration of archive " + fixture.archive_uuid + " (root \"" + va::path_to_utf8(root) +
           "\") is creating revision 2; this relocation recorded nothing (finish that registration with `register "
           "--resume`, then relocate again; or abandon it with `archive abandon-registration " +
           fixture.archive_uuid + "`, then continue this relocation with `archive relocate " + fixture.archive_uuid +
           " <root_path> --resume`)";
}

// Decision 0023: a relocation does not take the revision that an unfinished
// registration creates once the registration has saved files it hashed for
// it (checkpoints, R-104): recorded over them, they would join its index or
// fail its insert. It stops (exit 12) and changes nothing of the archive; it
// keeps its own saved progress (decision 0026). The registration finishes,
// which ends the relocation (its revision is taken), and a new relocation
// then records the revision after it.
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
          refused.error().message == registering_message(fixture, request.root));
    const auto archive = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 1);
    const auto revision = fixture.store.revision(fixture.archive_uuid, 2);
    CHECK(revision && !*revision);
    CHECK(fixture.store.setting(key).value_or("") == plan);
    CHECK(fixture.store.has_file_rows(fixture.archive_uuid, 2).value_or(false));
    CHECK(manifest_names(fixture) == std::set<std::string>{"r0001.vam"});
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->empty());
    // The relocation kept what it matched, marked stopped.
    const auto record = fixture.record();
    CHECK(record.is_object() && record.contains("stopped"));
    CHECK(fixture.saved(record.value("run_id", std::string{})).files == 2);

    request.new_revision = false;
    request.resume = true;
    const auto resumed = fixture.register_root(request);
    CHECK(resumed && resumed->revision == 2);
    // Revision 2 is taken: the relocation can never finish, and its record went.
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
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
// stops as interrupted (exit 4). Nothing is changed unless it records; a
// relocation cancelled keeps everything it matched (decision 0026), and its
// resume reads no file again. Scaled down: the relocation's statements wait
// 100 ms for the lock, and the other operation holds it 400 ms.
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
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
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
                   relocated.error().message == "relocation was interrupted; continue it with `archive relocate " +
                                                    fixture.archive_uuid + " <root_path> --resume`";
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
    if (what == WhileWaiting::OtherRecordsIt) {
        // Its revision is taken: it can never finish, and its record went.
        CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
        return;
    }
    // Cancelled at its record: its record stays (its stop mark, best effort,
    // may be lost while the other operation still holds the lock), and a
    // resume reads nothing again, and records.
    const auto record = fixture.record();
    CHECK(record.is_object() && record.value("run_id", std::string{}).size() == 36);
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(resumed && resumed->revision == 2 && fixture.archive.bytes_read == 0);
}

// Decision 0025: the old carrier failed while `register --new-revision` of
// it was unfinished, after it had saved files for revision 2. The relocation
// of the archive to its new carrier leaves the revision to that registration
// and says how to give it up; once the operator abandons it, the relocation
// is resumed (decision 0026) and records revision 2 without reading any file
// again, and nothing of the registration is left to resume.
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
          refused.error().message == registering_message(fixture, request.root));

    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto abandoned = va::abandon_registration(services, {fixture.archive_uuid, std::nullopt, std::nullopt});
    CHECK(abandoned && abandoned->revision == 2 && !abandoned->first_registration && abandoned->files > 0);
    CHECK(!fixture.store.has_file_rows(fixture.archive_uuid, 2).value_or(true));

    fixture.archive.bytes_read = 0;
    const auto relocated = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(relocated && relocated->revision == 2 && fixture.archive.bytes_read == 0);
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

// ---- Abandoning a relocation (decision 0026) --------------------------------

// How many rows each table of the repository holds, to compare before and after.
std::map<std::string, std::int64_t> tables_of(RelocationFixture& fixture)
{
    std::map<std::string, std::int64_t> tables;
    for (const auto* table : {"archives", "archive_revisions", "files", "media_evidence", "verify_runs",
             "verify_items", "verify_progress", "settings"}) {
        tables[table] = count_rows(fixture, table);
    }
    return tables;
}

// Abandoning an unfinished relocation removes its record and its saved
// progress, and nothing else: no archive, revision, index row, evidence or
// history. Asking first changes nothing. The archive then takes a relocation
// that reads its new carrier in full.
void abandoning_a_relocation_removes_only_its_record_and_saved_progress()
{
    RelocationFixture fixture(true);
    const auto run_id = fixture.interrupted_relocation(fixture.processes.self, false);
    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto before = tables_of(fixture);
    const auto shown = va::abandonable_relocation(services, {fixture.archive_uuid, std::nullopt});
    CHECK(shown && shown->run_id == run_id && shown->revision == 1 && shown->files == 20 &&
          shown->bytes == sample_photo_bytes && shown->label == "ARCH-0001" &&
          shown->root == va::path_to_utf8(fixture.archive.root() / "new"));
    CHECK(tables_of(fixture) == before);

    const auto abandoned = va::abandon_relocation(services, {fixture.archive_uuid, run_id});
    CHECK(abandoned && abandoned->files == 20 && abandoned->run_id == run_id);
    auto after = before;
    CHECK(before.at("verify_progress") == 20);
    after["verify_progress"] = 0;
    after["settings"] = before.at("settings") - 1;
    CHECK(tables_of(fixture) == after);
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());

    const auto again = va::abandon_relocation(services, {fixture.archive_uuid, std::nullopt});
    CHECK(!again && again.error().kind == va::AbandonFailure::Kind::Refused &&
          again.error().message == "archive " + fixture.archive_uuid + " has no unfinished relocation");
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!resumed && resumed.error().message == "archive " + fixture.archive_uuid +
                                                     " has no unfinished relocation to resume");
    fixture.archive.bytes_read = 0;
    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(relocated && relocated->revision == 2 && fixture.archive.bytes_read == sample_bytes);
}

// Never while its attempt runs (decision 0024), nor while a record that names
// no process this computer can check said a moment ago that it works; never a
// relocation other than the one shown. Abandoned once its process has ended.
void a_running_relocation_is_not_abandoned()
{
    RelocationFixture fixture(true);
    const va::ProcessIdentity first{1000, "boot-1:1000", "veriarchive-cli.exe"};
    const auto run_id = fixture.interrupted_relocation(first, true);
    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    fixture.processes.others = {first};
    const auto running = va::abandon_relocation(services, {fixture.archive_uuid, std::nullopt});
    CHECK(!running && running.error().kind == va::AbandonFailure::Kind::Refused &&
          running.error().message == "the relocation of archive " + fixture.archive_uuid +
                                         " is still running in process 1000 (veriarchive-cli.exe) on this computer; "
                                         "abandon it once that process has ended");

    // From another computer, a moment ago.
    auto record = fixture.record();
    const auto own = record;
    record["process"]["host"] = "another-computer";
    record["updated"] = va::current_timestamp();
    CHECK(fixture.store.set_setting(fixture.key(), record.dump()));
    const auto elsewhere = va::abandon_relocation(services, {fixture.archive_uuid, std::nullopt});
    CHECK(!elsewhere && elsewhere.error().message == "an attempt of the relocation of archive " +
                                                         fixture.archive_uuid +
                                                         " said it was working less than 3 minutes ago; abandon it "
                                                         "once it has stopped");
    CHECK(fixture.store.set_setting(fixture.key(), own.dump()));

    fixture.processes.others.clear();
    const auto other = va::abandon_relocation(services, {fixture.archive_uuid, "another-run"});
    CHECK(!other && other.error().message ==
                        "the unfinished relocation of archive " + fixture.archive_uuid +
                            " is no longer the one shown (it was resumed, finished or abandoned meanwhile); nothing "
                            "was changed");
    const auto abandoned = va::abandon_relocation(services, {fixture.archive_uuid, run_id});
    CHECK(abandoned && abandoned->files == 20);
}

// A relocation abandoned while an attempt of it runs where this computer
// cannot see it (another computer): the attempt stops at its next checkpoint,
// saves nothing and records nothing.
void an_attempt_stops_when_its_relocation_is_abandoned()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    std::string run_id;
    test::HookedFiles hooked(fixture.archive, "p110.bin", [&] {
        auto store = fixture.repository.store();
        const auto stored =
            nlohmann::json::parse(store.setting(fixture.key()).value_or(""), nullptr, false);
        run_id = stored.is_object() ? stored.value("run_id", std::string{}) : std::string{};
        CHECK(store.write_transaction([&] { return store.remove_relocation(fixture.archive_uuid, run_id); }));
    });
    va::RelocateServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media,
        fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto relocated = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Refused &&
          relocated.error().message == "the unfinished relocation of archive " + fixture.archive_uuid +
                                           " was abandoned while this attempt ran; nothing was changed");
    CHECK(!run_id.empty() && fixture.saved(run_id).files == 0);
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    const auto archive = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 1);
    CHECK(manifest_names(fixture) == std::set<std::string>{"r0001.vam"});

    // Abandoned after its last checkpoint, while it writes its manifest: its
    // record finds the relocation abandoned, and records nothing.
    test::StagingHookWriter abandoning(*fixture.repository.writer, [&] {
        auto store = fixture.repository.store();
        const auto stored = nlohmann::json::parse(store.setting(fixture.key()).value_or(""), nullptr, false);
        run_id = stored.is_object() ? stored.value("run_id", std::string{}) : std::string{};
        CHECK(store.write_transaction([&] { return store.remove_relocation(fixture.archive_uuid, run_id); }));
    });
    va::RelocateServices late{fixture.archive, *fixture.repository.files, abandoning, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto recorded = va::relocate_archive(late,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    CHECK(!recorded && recorded.error().message == "the unfinished relocation of archive " + fixture.archive_uuid +
                                                       " was abandoned while this attempt ran; nothing was changed");
    const auto still = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(still && *still && (*still)->current_revision == 1);
    CHECK(manifest_names(fixture) == std::set<std::string>{"r0001.vam"});
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->empty());
}

} // namespace

void relocate_tests()
{
    relocation_keeps_identity_and_history();
    differing_copy_is_refused();
    a_differing_copy_ends_the_relocation_while_the_lock_is_held();
    a_differing_copy_whose_relocation_cannot_end_says_to_abandon_it();
    known_unreadable_entries_are_carried();
    an_interrupted_relocation_resumes_without_reading_again();
    a_resume_needs_the_volume_it_started_on();
    a_resume_needs_the_revision_it_verifies();
    a_resume_waits_while_the_attempt_before_runs();
    attempts_of_one_relocation_take_turns();
    attempts_of_one_relocation_on_two_computers_record_the_revision_once();
    a_relocation_ends_a_registration_that_saved_nothing();
    a_relocation_that_loses_while_hashing_records_nothing();
    the_heartbeat_writes_only_its_own_relocation();
    a_relocation_removes_the_plan_of_its_revision();
    a_relocation_leaves_a_registration_its_saved_files();
    an_abandoned_registration_lets_the_relocation_record();
    a_relocation_waits_for_the_write_lock(WhileWaiting::OtherCommits);
    a_relocation_waits_for_the_write_lock(WhileWaiting::OtherRecordsIt);
    a_relocation_waits_for_the_write_lock(WhileWaiting::RelocationCancelled);
    abandoning_a_relocation_removes_only_its_record_and_saved_progress();
    a_running_relocation_is_not_abandoned();
    an_attempt_stops_when_its_relocation_is_abandoned();
}
