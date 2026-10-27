#include "test_support.h"

#include "engine_fixture.h"

#include "app/history.h"
#include "app/unfinished_operations.h"
#include "common/shell_argument.h"
#include "common/time.h"
#include "core/engine/abandon_registration.h"
#include "core/engine/archive_status.h"
#include "core/engine/comparison.h"
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
#include <format>
#include <functional>
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

// More sample files, of 10 bytes each, in many/, which sorts before photos/:
// enough for the checkpoints every 1000 entries (sample data).
std::string many_name(int index)
{
    return std::format("many/m{:05}.txt", index);
}
constexpr std::uint64_t many_file_bytes = 10;

// Volume 1 is mounted at /vol, volume 2 at /vol/new (a different disk). The
// archive is registered at /vol/old: two small files, or with `sample` the
// 40 sample files and `many` more.
struct RelocationFixture : test::EngineFixture {
    va::Uuid archive_uuid;
    bool sample = false;
    int many = 0;

    explicit RelocationFixture(bool with_sample = false, int many_files = 0) : sample(with_sample), many(many_files)
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
            for (int index = 0; index < many; ++index) {
                archive.add_file("old/" + many_name(index), std::string(many_file_bytes, 'm'));
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
            for (int index = 0; index < many; ++index) {
                archive.add_file(folder + "/" + many_name(index), std::string(many_file_bytes, 'm'));
            }
            return;
        }
        archive.add_file(folder + "/photos/a.jpg", "jpeg").mtime_ns += 3'600'000'000'000; // copy changed mtime
        archive.add_file(folder + "/notes.txt", "notes");
    }

    std::expected<va::RelocateOutcome, va::RelocateFailure> relocate(
        const std::filesystem::path& root, bool resume = false)
    {
        va::RelocateServices services{archive_files(), *repository.files, *repository.writer, media, store,
            repository.context.repo, repository.context.repository_uuid, host, processes};
        va::RelocateRequest request{archive_uuid, root, io, resume};
        request.rotational_override = rotational_override;
        return va::relocate_archive(services, request, cancel, progress);
    }

    std::map<std::string, bool> rotational_override; // the config's io.rotational_override (R-111)
    std::optional<bool> io;                          // --io hdd (true) or ssd (false) (R-111)

    std::string key() const { return va::relocation_key(archive_uuid); }

    // The relocation's record (decision 0026), or null when there is none.
    nlohmann::json record()
    {
        return nlohmann::json::parse(store.setting(key()).value_or(""), nullptr, false);
    }

    // The new disk's root as a command names it, to resume the relocation
    // (decision 0022, amended 2026-10-27).
    std::string new_root() const { return va::shell_argument(va::path_to_utf8(archive.root() / "new")); }

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
                                                 archive_uuid + " " + new_root() + " --resume`");
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

// The match result that the media evidence of the run `run_id` records (R-96).
std::string match_result_of(RelocationFixture& fixture, const std::string& run_id)
{
    sqlite3* database = nullptr;
    const auto name = fixture.repository.context.repo.database().u8string();
    std::string result;
    if (sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &database, SQLITE_OPEN_READONLY, nullptr) ==
        SQLITE_OK) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT match_result FROM media_evidence WHERE run_id = ?", -1, &statement,
                nullptr) == SQLITE_OK &&
            sqlite3_bind_text(statement, 1, run_id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            result = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close(database);
    return result;
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

// Decision 0005: a relocation carries copy_of forward. A copy of the archive
// (sample data) is registered at /vol/copy with copy_of naming the archive,
// then relocated to the new disk: its next revision names the same archive.
void a_relocation_carries_copy_of_forward()
{
    RelocationFixture fixture;
    fixture.archive.add_file("copy/photos/a.jpg", "jpeg");
    fixture.archive.add_file("copy/notes.txt", "notes");
    va::RegisterRequest request;
    request.root = fixture.archive.root() / "copy";
    request.copy_of = fixture.archive_uuid;
    const auto copy = fixture.register_root(request);
    CHECK(copy.has_value());
    if (!copy) {
        std::cerr << "  register the copy: " << copy.error().message << '\n';
        return;
    }
    fixture.archive.add_file("new/photos/a.jpg", "jpeg");
    fixture.archive.add_file("new/notes.txt", "notes");
    va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media, fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid,
        fixture.host, fixture.processes};
    const auto moved = va::relocate_archive(services,
        va::RelocateRequest{copy->archive_uuid, fixture.archive.root() / "new", std::nullopt, false}, fixture.cancel,
        fixture.progress);
    CHECK(moved && moved->revision == 2);
    if (!moved) {
        std::cerr << "  relocate the copy: " << moved.error().message << '\n';
        return;
    }
    const auto reader = va::ManifestReader::open(*fixture.repository.files, moved->manifest);
    CHECK(reader && reader->header().supersedes == 1u &&
          reader->header().copy_of == std::optional<va::Uuid>(fixture.archive_uuid));
}

// Decision 0004: an earlier revision's manifest is read only where matching
// looks at the earlier carriers, which is where they decide the outcome. The
// archive is relocated from /vol/old to the folder sub of the new disk, so its
// first revision names its earlier carrier, and another archive, B, is
// registered at /vol/b (sample data); then the manifest of the first revision
// is moved away. Where a latest carrier matches, none is read: the archive
// verifies by its uuid and on its new carrier, and B by its uuid and by root.
// A root that no archive's root overlaps registers, as no archive can refuse
// it (decision 0001). Where no latest carrier matches, the earlier carriers
// tell an earlier carrier from media that is no archive's: verifying the old
// carrier, and registering /vol/sub (the archive's root, on another volume),
// stop with a repository error that names the first revision and how to go
// on, recording nothing. With the manifest back, the old carrier is
// PREVIOUS_CARRIER, and /vol/sub registers.
void an_earlier_revision_is_read_only_where_matching_needs_it()
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk("new/sub");
    CHECK(fixture.relocate(fixture.archive.root() / "new" / "sub").has_value());
    fixture.archive.add_file("b/two.txt", "b (sample data)");
    const auto b = fixture.register_root(fixture.archive.root() / "b");
    CHECK(b.has_value());
    if (!b) {
        return;
    }
    const auto first = va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 1);
    auto away = first;
    away += ".away";
    std::filesystem::rename(first, away);

    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = fixture.archive_uuid;
    const auto named = fixture.verify(by_uuid);
    CHECK(named && named->result == "PASSED");
    const auto on_new = fixture.verify(fixture.archive.root() / "new" / "sub");
    CHECK(on_new && on_new->archive_uuid == fixture.archive_uuid && on_new->result == "PASSED");
    va::VerifyRequest b_by_uuid;
    b_by_uuid.archive_uuid = b->archive_uuid;
    const auto b_named = fixture.verify(b_by_uuid);
    CHECK(b_named && b_named->result == "PASSED");
    const auto b_by_root = fixture.verify(fixture.archive.root() / "b");
    CHECK(b_by_root && b_by_root->archive_uuid == b->archive_uuid && b_by_root->result == "PASSED");
    for (const auto* stopped : {&named, &on_new, &b_named, &b_by_root}) {
        if (!*stopped) {
            std::cerr << "  stopped: " << stopped->error().message << '\n';
        }
    }
    fixture.archive.add_file("c/three.txt", "c (sample data)");
    const auto c = fixture.register_root(fixture.archive.root() / "c");
    CHECK(c.has_value());
    if (!c) {
        std::cerr << "  register /vol/c: " << c.error().message << '\n';
    }
    // A relocation of B looks at no earlier carrier of another archive: they
    // never make a root overlap (decision 0001).
    fixture.archive.add_file("b-copy/two.txt", "b (sample data)");
    va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media, fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid,
        fixture.host, fixture.processes};
    const auto b_moved = va::relocate_archive(services,
        va::RelocateRequest{b->archive_uuid, fixture.archive.root() / "b-copy", std::nullopt, false}, fixture.cancel,
        fixture.progress);
    CHECK(b_moved && b_moved->revision == 2);
    if (!b_moved) {
        std::cerr << "  relocate B: " << b_moved.error().message << '\n';
    }

    const auto names_first = [&](const std::string& message) {
        return message == "the manifest of revision 1 (manifests/" + fixture.archive_uuid + "/r0001.vam) is missing" +
                              test::put_back(fixture.archive_uuid, 1, false);
    };
    const auto old = fixture.verify(fixture.archive.root() / "old");
    CHECK(!old && old.error().kind == va::VerifyStop::Kind::Repository && names_first(old.error().message));
    fixture.archive.add_file("sub/four.txt", "d (sample data)");
    const auto refused = fixture.register_root(fixture.archive.root() / "sub");
    CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Repository &&
          names_first(refused.error().message));
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 3); // the relocation's, by uuid, on the new carrier
    const auto archives = fixture.store.archives();
    CHECK(archives && archives->size() == 3);

    std::filesystem::rename(away, first);
    const auto previous = fixture.verify(fixture.archive.root() / "old");
    CHECK(!previous && previous.error().kind == va::VerifyStop::Kind::Decision &&
          previous.error().identity.result == va::MatchResult::PreviousCarrier);
    const auto registered = fixture.register_root(fixture.archive.root() / "sub");
    CHECK(registered && registered->archive_uuid != fixture.archive_uuid);
}

// A relocation records the evidence of the new carrier as matching saw it
// (R-96). Moved to the folder old of the new disk and then back onto the old
// disk, the archive returns to a carrier it had, at the root it had there:
// PREVIOUS_CARRIER, which only the manifest of its first revision tells
// (decision 0004), read as the latest carrier does not match.
void a_relocation_back_onto_an_earlier_carrier_records_it_so()
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk("new/old");
    const auto moved = fixture.relocate(fixture.archive.root() / "new" / "old");
    CHECK(moved && moved->revision == 2);
    const auto back = fixture.relocate(fixture.archive.root() / "old");
    CHECK(back && back->revision == 3);
    if (!moved || !back) {
        std::cerr << "  relocation: " << (moved ? back.error().message : moved.error().message) << '\n';
        return;
    }
    CHECK(match_result_of(fixture, moved->run_id) == "UNKNOWN");
    CHECK(match_result_of(fixture, back->run_id) == "PREVIOUS_CARRIER");
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
    return "the new carrier does not hold the recorded content; nothing was changed: copy the archive again from "
           "its carrier to the new one, then relocate again";
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

// How a relocation whose new carrier was found to differ is refused, whether
// it is resumed or a new relocation is started (decision 0026).
std::string differs_refusal(RelocationFixture& fixture, const std::filesystem::path& root)
{
    return "the unfinished relocation of archive " + fixture.archive_uuid + " (to \"" + va::path_to_utf8(root) +
           "\") found that the new carrier does not hold the recorded content and cannot be resumed; abandon it "
           "with `archive abandon-relocation " +
           fixture.archive_uuid + "`, then relocate again";
}

// When the record and saved progress of a relocation whose copy differs still
// cannot be removed (here a trigger makes the removal fail, standing in for a
// lock held for longer than the record waits), the refusal says so, with the
// cause, and how to go on: abandon the relocation before relocating again.
// The record says that the new carrier differs ("differs"), so it is never
// resumed: a resume and a relocation without --resume are both refused,
// saying to abandon it, and read nothing. Abandoned, the next relocation
// reads the whole carrier.
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
    // that matched now. The record says the new carrier differs.
    const auto record = fixture.record();
    CHECK(record.is_object() && va::parse_timestamp(record.value("differs", std::string{})).has_value() &&
          record.contains("stopped"));
    CHECK(fixture.saved(run_id).files == 39);

    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!resumed && resumed.error().kind == va::RelocateFailure::Kind::Refused &&
          resumed.error().message == differs_refusal(fixture, fixture.archive.root() / "new"));
    const auto fresh = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!fresh && fresh.error().kind == va::RelocateFailure::Kind::Refused &&
          fresh.error().message == differs_refusal(fixture, fixture.archive.root() / "new"));
    CHECK(fixture.archive.bytes_read == 0);
    CHECK(fixture.record() == record);
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

// The record says that the new carrier differs as soon as the first entry
// that differs is found, while the relocation still reads on to list every
// such entry (decision 0026). A record that an attempt leaves behind, as when
// its process is killed meanwhile, is then never resumed. Here a photo is
// missing from the copy (sample data), which the comparison finds before it
// hashes anything; another process reads the record when the first file is
// opened. Put back as it was then, with what it had saved, the record is
// refused by a resume, which reads nothing, and the Archive List shows it as
// a relocation whose new carrier differs, to be abandoned, in both languages
// and as --json says it.
void a_differing_copy_marks_its_relocation_at_once()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    const auto missing = fixture.archive.node("new/photos/p101.bin");
    fixture.archive.remove("new/photos/p101.bin"); // left out of the copy (sample data)
    nlohmann::json seen;
    test::HookedFiles hooked(fixture.archive, "p100.bin", [&] {
        auto store = fixture.repository.store();
        seen = nlohmann::json::parse(store.setting(fixture.key()).value_or(""), nullptr, false);
    });
    va::RelocateServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media,
        fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto relocated = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Differs &&
          relocated.error().items.size() == 1 && relocated.error().items.front().status == "Missing" &&
          relocated.error().message == differs_message());
    CHECK(seen.is_object() && va::parse_timestamp(seen.value("differs", std::string{})).has_value() &&
          !seen.contains("stopped"));
    // It ended: its record and saved progress went.
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    CHECK(count_rows(fixture, "verify_progress") == 0);

    // As its process killed while it read on would have left it.
    const auto run_id = seen.value("run_id", std::string{});
    CHECK(fixture.store.set_setting(fixture.key(), seen.dump()));
    va::Entry photo;
    photo.path.p = "photos/p100.bin";
    CHECK(fixture.store.add_progress(run_id, {va::entry_key(photo)}));
    fixture.archive.add_file("new/photos/p101.bin", missing.content); // copied again
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!resumed && resumed.error().kind == va::RelocateFailure::Kind::Refused &&
          resumed.error().message == differs_refusal(fixture, fixture.archive.root() / "new"));
    CHECK(fixture.archive.bytes_read == 0);

    // The Archive List judges its attempt ended (its process is this one,
    // which runs no operation now).
    const va::app::AttemptCheck attempts{&fixture.processes, fixture.host.host, {}};
    const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
        [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; }, attempts);
    CHECK(read && read->operations.size() == 1);
    if (read && read->operations.size() == 1) {
        const auto& operation = read->operations.front();
        CHECK(operation.differs && !operation.resumable && operation.files_done == 1);
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        CHECK(va::app::state_code(va::app::state_of_one_read(operation, now)) == "differs");
        va::app::OperationWatch watch;
        const va::app::OperationWatch::Clock::time_point start{};
        watch.update(operation, start, now);
        const auto row = va::app::describe_operation(va::app::Text{}, watch, start, now);
        CHECK(row.state_kind == va::app::OperationState::Differs && row.abandonable &&
              row.state == "new carrier differs; abandon it");
        CHECK(va::app::describe_operation(va::app::Text(va::app::Language::TraditionalChinese), watch, start, now)
                  .state == "新載體上的內容不符；請放棄它");
    }

    va::AbandonServices abandoning{fixture.store, fixture.processes, fixture.host.host};
    const auto abandoned = va::abandon_relocation(abandoning, {fixture.archive_uuid, run_id});
    CHECK(abandoned && abandoned->files == 1);
    const auto again = fixture.relocate(fixture.archive.root() / "new");
    CHECK(again && again->revision == 2 && fixture.archive.bytes_read == sample_bytes);
}

// A name that cannot exist where the relocation reads (here a sample name with
// "?", which Windows refuses) is only a warning to a verification, but it
// would drop out of the new revision: it ends the relocation as an entry that
// differs does, and the record says so at once (decision 0026).
void an_unrepresentable_name_marks_the_relocation_at_once()
{
    test::EngineFixture fixture; // the engines run as on Windows
    auto second = test::fake_volume(fixture.archive.root() / "new");
    second.filesystem_uuid = "5555666677778888";
    second.devices.front().serial = "NEW-DISK";
    fixture.media.extra_volumes.push_back(second);
    fixture.archive.add_file("old/what?.txt", "a name from another system (sample data)");
    fixture.archive.add_file("old/z.txt", "z");
    fixture.host.os = "linux"; // registered there (R-44 holds across systems only)
    const auto registered = fixture.register_root(fixture.archive.root() / "old");
    fixture.host.os = "windows";
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    fixture.archive.add_file("new/z.txt", "z"); // the copy could not take the other name
    const auto key = va::relocation_key(registered->archive_uuid);
    nlohmann::json seen;
    test::HookedFiles hooked(fixture.archive, "z.txt", [&] {
        auto store = fixture.repository.store();
        seen = nlohmann::json::parse(store.setting(key).value_or(""), nullptr, false);
    });
    va::RelocateServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media,
        fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto relocated = va::relocate_archive(services,
        {registered->archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Differs &&
          relocated.error().items.size() == 1 && relocated.error().items.front().status == "Unrepresentable");
    CHECK(seen.is_object() && va::parse_timestamp(seen.value("differs", std::string{})).has_value());
    CHECK(fixture.store.setting(key).value_or("x").empty());
}

// A mark that cannot be written when the first entry that differs is found
// (another process holds the write lock then) is written at a later chance:
// here by the attempt's stop mark, once the removal of its record has failed
// (a trigger, standing in for a lock held for longer than the record waits).
// Scaled down: the relocation's statements wait 100 ms for the lock, which
// the other process holds for 1 s from when the damaged photo is opened.
void a_differs_mark_is_written_at_a_later_chance()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    fixture.archive.node("new/photos/p101.bin").content[3] ^= 0x01; // a corrupted copy (sample data)
    fixture.repository.execute("CREATE TRIGGER sample_failure BEFORE DELETE ON settings "
                               "WHEN OLD.key LIKE 'relocation:%' BEGIN SELECT RAISE(ABORT, 'sample failure'); END;");
    std::thread holder;
    std::binary_semaphore held{0};
    nlohmann::json while_held;
    test::HookedFiles hooked(fixture.archive, "p101.bin", [&] {
        holder = std::thread([&] {
            auto store = fixture.repository.store();
            CHECK(store.begin());
            held.release();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            while_held = nlohmann::json::parse(store.setting(fixture.key()).value_or(""), nullptr, false);
            CHECK(store.commit());
        });
        CHECK(held.try_acquire_for(std::chrono::seconds(60)));
    });
    auto store = fixture.repository.store(std::chrono::milliseconds(100));
    va::RelocateServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media,
        store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto relocated = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    if (holder.joinable()) {
        holder.join();
    }
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Differs &&
          relocated.error().items.size() == 1 && relocated.error().message.contains("could not be removed"));
    // Not written while the lock was held; written since.
    CHECK(while_held.is_object() && !while_held.contains("differs"));
    const auto record = fixture.record();
    CHECK(record.is_object() && va::parse_timestamp(record.value("differs", std::string{})).has_value() &&
          record.contains("stopped"));
    fixture.repository.execute("DROP TRIGGER sample_failure;");
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!resumed && resumed.error().message == differs_refusal(fixture, fixture.archive.root() / "new"));
}

// An attempt interrupted after it found an entry that differs (here Ctrl+C
// while it hashes, after a photo was found missing from the copy) ends the
// relocation as a copy that differs does, with the entries found so far: it
// is not left for --resume, which could never finish it (decision 0026). It
// is recorded INCOMPLETE as every interrupted attempt is: a run of its own of
// revision 1, naming the relocation, with the entry found missing and the new
// carrier's evidence. That claims nothing for the archive, which still has no
// last verify. The copy completed, a new relocation reads the whole carrier.
void a_relocation_interrupted_after_its_copy_differed_ends()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    const auto missing = fixture.archive.node("new/photos/p101.bin");
    fixture.archive.remove("new/photos/p101.bin"); // left out of the copy (sample data)
    const auto evidence_before = count_rows(fixture, "media_evidence");
    va::CancelToken cancel;
    std::string run_id;
    test::HookedFiles hooked(fixture.archive, "v130.bin", [&] {
        auto store = fixture.repository.store();
        const auto record = nlohmann::json::parse(store.setting(fixture.key()).value_or(""), nullptr, false);
        run_id = record.is_object() ? record.value("run_id", std::string{}) : std::string{};
        cancel.cancel();
    });
    va::RelocateServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media,
        fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto relocated = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, cancel, fixture.progress);
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Differs &&
          relocated.error().message == differs_message() && relocated.error().items.size() == 1 &&
          relocated.error().items.front().path.p == "photos/p101.bin");
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    CHECK(count_rows(fixture, "verify_progress") == 0);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1);
    if (runs && runs->size() == 1) {
        const auto& attempt = runs->front();
        const auto noted = nlohmann::json::parse(attempt.counts_json, nullptr, false);
        CHECK(attempt.result == "INCOMPLETE" && attempt.mode == "FULL" && attempt.revision == 1 &&
              !run_id.empty() && attempt.run_id != run_id && noted.is_object() &&
              noted.value("relocation", std::string{}) == run_id && noted["items"].value("Missing", 0) == 1);
        const auto items = fixture.store.verify_items(attempt.run_id);
        CHECK(items && items->size() == 1 && items->front().path.p == "photos/p101.bin" &&
              items->front().status == "Missing");
    }
    CHECK(count_rows(fixture, "media_evidence") == evidence_before + 1);
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto statuses = va::archive_statuses(
        fixture.store, *fixture.repository.files, fixture.repository.context.repo, va::Policy{}, now);
    CHECK(statuses && statuses->size() == 1 && !statuses->front().last_any && !statuses->front().last_full);
    const auto chain = fixture.store.check_history(fixture.repository.context.repository_uuid);
    CHECK(chain && chain->runs == 1 && !chain->first_broken_run);

    fixture.archive.add_file("new/photos/p101.bin", missing.content);
    fixture.archive.bytes_read = 0;
    const auto again = fixture.relocate(fixture.archive.root() / "new");
    CHECK(again && again->revision == 2 && !again->resumed && fixture.archive.bytes_read == sample_bytes);
}

// The size of photos/p119.bin, the 20th sample file (sample data).
constexpr std::uint64_t p119_bytes = 519;

// An entry that could not be read differs once the comparison has read
// everything (decision 0026): here the read of a photo fails with a media
// error (a bad sector, sample data), and the reading goes on to the end. The
// relocation ends as a copy that differs does: its record and saved progress
// go, and no run is recorded.
void an_unreadable_entry_ends_the_relocation_once_everything_was_read()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    fixture.archive.node("new/photos/p119.bin").read_error_offset = 0;
    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Differs &&
          relocated.error().message == differs_message() && relocated.error().items.size() == 1 &&
          relocated.error().items.front().path.p == "photos/p119.bin" &&
          relocated.error().items.front().status == "Unreadable(media)");
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    CHECK(count_rows(fixture, "verify_progress") == 0);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->empty());
}

// A disk that drops off while a file is read usually fails that read with an
// I/O error before the next file cannot be opened (its device removed). Here
// the read of photo p119 fails with a media error, and video v120 then finds
// the device removed (sample data). The read that failed says nothing of what
// the disk holds: the attempt is interrupted, and the relocation does not end
// (decision 0026). Its record stays, saying that it stopped and not that the
// new carrier differs, so that a resume goes on, also after a process killed
// at that point; it keeps the 19 photos that matched; and the attempt is
// recorded INCOMPLETE without an item: an I/O error is the entry's only once
// a later read succeeds (decision 0034), and none did. Plugged in again, the
// resume reads only the rest, that entry included, as it never matched, and
// records revision 2.
void a_read_that_fails_as_the_disk_drops_off_leaves_the_relocation_resumable()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    fixture.archive.node("new/photos/p119.bin").read_error_offset = 0;
    fixture.archive.node("new/videos/v120.bin").open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!interrupted && interrupted.error().kind == va::RelocateFailure::Kind::Incomplete &&
          interrupted.error().message == "relocation was interrupted; continue it with `archive relocate " +
                                             fixture.archive_uuid + " " + fixture.new_root() + " --resume`");
    const auto record = fixture.record();
    CHECK(record.is_object() && record.contains("stopped") && !record.contains("differs"));
    const auto run_id = record.is_object() ? record.value("run_id", std::string{}) : std::string{};
    const auto saved = fixture.saved(run_id);
    CHECK(saved.files == 19 && saved.bytes == sample_photo_bytes - p119_bytes);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1);
    if (runs && runs->size() == 1) {
        const auto& attempt = runs->front();
        const auto noted = nlohmann::json::parse(attempt.counts_json, nullptr, false);
        CHECK(attempt.result == "INCOMPLETE" && attempt.revision == 1 && attempt.run_id != run_id &&
              attempt.coverage_bytes == sample_photo_bytes - p119_bytes && noted.is_object() &&
              noted.value("relocation", std::string{}) == run_id);
        const auto items = fixture.store.verify_items(attempt.run_id);
        CHECK(items && items->empty());
    }

    fixture.archive.node("new/photos/p119.bin").read_error_offset.reset();
    fixture.archive.node("new/videos/v120.bin").open_error.reset();
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(resumed && resumed->revision == 2 && resumed->run_id == run_id && resumed->result == "PASSED");
    CHECK(fixture.archive.bytes_read == sample_bytes - (sample_photo_bytes - p119_bytes));
}

// Decision 0034: the new carrier's device goes away halfway through the
// relocation, its reads failing only with I/O errors, as Linux gives for a USB
// disk pulled out: the volume of the new root is looked up again before the
// entry is classified, and it is gone. The attempt is interrupted, not a
// carrier that differs, and its INCOMPLETE run lists no entry as unreadable;
// the relocation keeps the 20 photos that matched. Attached again, the resume
// reads only the rest and records revision 2. Sample data.
void a_new_carrier_that_goes_away_leaves_the_relocation_resumable()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    test::RemovableDevice device(fixture.archive);
    device.file = "v120.bin";
    device.at_offset = 100;
    device.on_removed = [&fixture] {
        fixture.media.error = va::IoError{va::IoErrorKind::NotFound, 2, "no volume at the root (sample)"};
    };
    fixture.media_files = &device;
    const auto interrupted = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!interrupted && interrupted.error().kind == va::RelocateFailure::Kind::Incomplete &&
          interrupted.error().items.empty() &&
          interrupted.error().message == "relocation was interrupted; continue it with `archive relocate " +
                                             fixture.archive_uuid + " " + fixture.new_root() + " --resume`");
    const auto record = fixture.record();
    CHECK(record.is_object() && record.contains("stopped") && !record.contains("differs"));
    const auto run_id = record.is_object() ? record.value("run_id", std::string{}) : std::string{};
    const auto saved = fixture.saved(run_id);
    CHECK(saved.files == 20 && saved.bytes == sample_photo_bytes);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1 && runs->front().result == "INCOMPLETE");
    if (runs && runs->size() == 1) {
        const auto items = fixture.store.verify_items(runs->front().run_id);
        CHECK(items && items->empty());
    }

    device.attach_again();
    fixture.media.error.reset();
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(resumed && resumed->revision == 2 && resumed->run_id == run_id && resumed->result == "PASSED");
    CHECK(fixture.archive.bytes_read == sample_bytes - sample_photo_bytes);
}

// Decision 0034 (operator, 2026-10-27): the new carrier's device stops
// answering while the computer still lists its volume, halfway through the
// first of the videos: every read fails with an I/O error, and ten in a row
// interrupt the relocation's attempt, as a device that went away does. It is
// recorded INCOMPLETE without an item, the relocation keeps the 20 photos
// matched, and its resume, once the device is attached again, reads only the
// rest and records revision 2. Sample data.
void a_new_carrier_that_stops_answering_leaves_the_relocation_resumable()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    test::RemovableDevice device(fixture.archive); // its volume stays listed
    device.file = "v120.bin";
    device.at_offset = 100;
    fixture.media_files = &device;
    const auto interrupted = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!interrupted && interrupted.error().kind == va::RelocateFailure::Kind::Incomplete &&
          interrupted.error().items.empty() &&
          interrupted.error().message ==
              "relocation was interrupted: the device of the new carrier stopped answering (10 entries in a row could "
              "not be read); unplug it and attach it again, then continue it with `archive relocate " +
                  fixture.archive_uuid +
                  " " + fixture.new_root() +
                  " --resume`; if it stops again at the same place, the device may be damaged");
    const auto record = fixture.record();
    CHECK(record.is_object() && record.contains("stopped") && !record.contains("differs"));
    const auto run_id = record.is_object() ? record.value("run_id", std::string{}) : std::string{};
    const auto saved = fixture.saved(run_id);
    CHECK(saved.files == 20 && saved.bytes == sample_photo_bytes);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1 && runs->front().result == "INCOMPLETE");
    if (runs && runs->size() == 1) {
        const auto items = fixture.store.verify_items(runs->front().run_id);
        CHECK(items && items->empty());
    }

    device.attach_again();
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(resumed && resumed->revision == 2 && resumed->run_id == run_id && resumed->result == "PASSED");
    CHECK(fixture.archive.bytes_read == sample_bytes - sample_photo_bytes);
}

// R-111: the config's io.rotational_override applies to a relocation: the
// sample device of the new carrier reports an HDD (one reader), and the
// override of its serial says SSD, so several readers hash the copy at once
// (min(8, cores), on a computer with more than one core).
void the_rotational_override_applies_to_a_relocation()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    test::OpenFilesWatch watch(fixture.archive, true);
    fixture.media_files = &watch;
    fixture.rotational_override = {{"NEW-DISK", false}};
    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(relocated && relocated->revision == 2 && relocated->result == "PASSED");
    CHECK(watch.most_open_at_once() >= (std::thread::hardware_concurrency() >= 2 ? 2 : 1));
}

// Decision 0013, §A6: the FULL verification of the new carrier that a
// relocation records for the new revision reports the portability warnings
// that the revision records, as every later FULL verification of it does:
// PASSED_WITH_WARNINGS with the plain warning line, in the outcome and in the
// history. Sample names: "café" composed and decomposed in one folder.
void a_relocation_reports_the_recorded_name_collisions()
{
    test::EngineFixture fixture;
    auto second = test::fake_volume(fixture.archive.root() / "new");
    second.filesystem_uuid = "5555666677778888";
    second.devices.front().serial = "NEW-DISK";
    fixture.media.extra_volumes.push_back(second);
    const auto add_sample = [&](const std::string& folder) {
        fixture.archive.add_file(folder + "/SAMPLE-nfc/caf\xC3\xA9.txt", "composed");    // e-acute as one character
        fixture.archive.add_file(folder + "/SAMPLE-nfc/cafe\xCC\x81.txt", "decomposed"); // e, then a combining acute
    };
    add_sample("old");
    const auto registered = fixture.register_root(fixture.archive.root() / "old");
    if (!registered) {
        CHECK(false);
        return;
    }
    add_sample("new");

    va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media, fixture.store, fixture.repository.context.repo,
        fixture.repository.context.repository_uuid, fixture.host, fixture.processes};
    const auto relocated = va::relocate_archive(services,
        {registered->archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel,
        fixture.progress);
    CHECK(relocated && relocated->revision == 2 && relocated->result == "PASSED_WITH_WARNINGS" &&
          relocated->items.empty() && relocated->warnings == std::vector<std::string>{va::name_collision_warning});
    const auto runs = fixture.store.verify_runs(registered->archive_uuid);
    CHECK(runs && runs->size() == 1 && runs->front().revision == 2 &&
          runs->front().result == "PASSED_WITH_WARNINGS");
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->result == "PASSED_WITH_WARNINGS" &&
          verified->warnings == std::vector<std::string>{va::name_collision_warning});
}

// R-111: --io wins over what the platform detected and over the config's
// override. --io ssd on a new carrier whose sample device reports an HDD:
// several readers hash the copy at once (on a computer with more than one
// core). --io hdd on one whose device reports an SSD and whose serial the
// override names an SSD as well: one reader, which waits a second for a
// second file that never comes.
void the_io_option_wins_in_a_relocation()
{
    for (const bool ssd : {true, false}) {
        RelocationFixture fixture(true);
        fixture.copy_to_new_disk();
        fixture.media.extra_volumes.front().devices.front().rotational = ssd; // an HDD for --io ssd, an SSD for --io hdd
        test::OpenFilesWatch watch(fixture.archive, true, std::chrono::seconds(1));
        fixture.media_files = &watch;
        fixture.io = !ssd; // --io ssd, --io hdd
        if (!ssd) {
            fixture.rotational_override = {{"NEW-DISK", false}};
        }
        const auto relocated = fixture.relocate(fixture.archive.root() / "new");
        CHECK(relocated && relocated->revision == 2 && relocated->result == "PASSED");
        if (ssd) {
            CHECK(watch.most_open_at_once() >= (std::thread::hardware_concurrency() >= 2 ? 2 : 1));
        } else {
            CHECK(watch.most_open_at_once() == 1);
        }
    }
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
                                       " " + fixture.new_root() +
                                       " --resume`, or abandon it with `archive abandon-relocation " +
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
    // In the history, the interrupted attempt: an INCOMPLETE verification of
    // revision 1 under a run of its own, naming the relocation, from when it
    // started, with the 20 photos it matched. Then the relocation's run, the
    // FULL verification of revision 2, which covered every byte, started
    // when the first attempt started, and says it was resumed once. The hash
    // chain holds.
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 2);
    if (runs && runs->size() == 2) {
        const auto& interrupted = runs->front();
        CHECK(interrupted.run_id != run_id && interrupted.revision == 1 && interrupted.mode == "FULL" &&
              interrupted.result == "INCOMPLETE");
        CHECK(interrupted.coverage_bytes == sample_photo_bytes &&
              interrupted.started == record.value("started", std::string{}));
        auto noted = nlohmann::json::parse(interrupted.counts_json, nullptr, false);
        CHECK(noted.is_object() && noted.value("relocation", std::string{}) == run_id &&
              noted["items"].value("Verified", 0) == 20);
        const auto& run = runs->back();
        CHECK(run.run_id == run_id && run.revision == 2 && run.mode == "FULL" && run.result == "PASSED");
        CHECK(run.coverage_bytes == sample_bytes && run.started == record.value("started", std::string{}));
        const auto counts = nlohmann::json::parse(run.counts_json, nullptr, false);
        CHECK(counts.is_object() && counts.value("resumed", 0) == 1 &&
              counts.value("relocated_from_revision", 0) == 1);
    }
    const auto chain = fixture.store.check_history(fixture.repository.context.repository_uuid);
    CHECK(chain && chain->runs == 2 && !chain->first_broken_run);
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
// formatted since (another filesystem id), the same filesystem in another
// enclosure since (MIGRATED: the checkpoints were made through another
// device) or another folder says nothing of what matched before; the resume
// is refused, reads nothing and changes nothing. The same disk reached
// through another mount point (a drive letter that changed) is the same
// volume, and resumes.
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
    disk.devices.front().serial = "OTHER-ENCLOSURE"; // sample data
    const auto migrated = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!migrated && migrated.error().kind == va::RelocateFailure::Kind::Refused &&
          migrated.error().message == refusal);
    disk.devices.front().serial = "NEW-DISK";
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

// The record names where the latest attempt reads, and the FULL verification
// that a resumed relocation records starts when its first attempt started
// (decision 0026). Here the first attempt started days before (a sample
// time); the second reads the same disk at another mount point and is
// interrupted there as well; the third records. Each interrupted attempt is
// in the history as an INCOMPLETE verification of revision 1, from when it
// started, counting what the relocation had matched by its stop (the second
// one the photos the first matched, too); the FULL verification of revision
// 2 spans them and is recorded after them, and the chain, which follows the
// order recorded, holds.
void a_resumed_relocation_names_where_it_reads_and_keeps_its_start()
{
    RelocationFixture fixture(true);
    const auto run_id = fixture.interrupted_relocation(fixture.processes.self, false);
    auto record = fixture.record();
    const auto first_started = record.value("started", std::string{});
    const std::string started = "2026-10-01T08:00:00Z"; // sample data
    record["started"] = started;
    CHECK(fixture.store.set_setting(fixture.key(), record.dump()));

    fixture.copy_to_new_disk("moved");
    fixture.media.extra_volumes.front().mount_point = fixture.archive.root() / "moved";
    fixture.archive.node("moved/videos/v130.bin").open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto again = fixture.relocate(fixture.archive.root() / "moved", true);
    fixture.archive.node("moved/videos/v130.bin").open_error.reset();
    CHECK(!again && again.error().kind == va::RelocateFailure::Kind::Incomplete);
    const auto moved = fixture.record();
    CHECK(moved.is_object() && moved.value("run_id", std::string{}) == run_id);
    CHECK(moved.value("root", std::string{}) == va::path_to_utf8(fixture.archive.root() / "moved"));
    CHECK(moved.value("started", std::string{}) == started && moved.value("resumes", 0) == 1);

    const auto resumed = fixture.relocate(fixture.archive.root() / "moved", true);
    CHECK(resumed && resumed->revision == 2 && resumed->run_id == run_id);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 3);
    if (runs && runs->size() == 3) {
        const auto& first = (*runs)[0];
        const auto& second = (*runs)[1];
        const auto& relocated = (*runs)[2];
        for (const auto* attempt : {&first, &second}) {
            const auto noted = nlohmann::json::parse(attempt->counts_json, nullptr, false);
            CHECK(attempt->result == "INCOMPLETE" && attempt->revision == 1 && attempt->run_id != run_id &&
                  noted.is_object() && noted.value("relocation", std::string{}) == run_id);
        }
        // The photos; then the photos and the 10 videos before v130 (sample data).
        CHECK(first.started == first_started && first.coverage_bytes == sample_photo_bytes);
        CHECK(second.coverage_bytes == sample_photo_bytes + 5245 && second.started >= first.finished);
        CHECK(relocated.started == started && relocated.started < first.started &&
              relocated.coverage_bytes == sample_bytes && relocated.revision == 2);
        const auto counts = nlohmann::json::parse(relocated.counts_json, nullptr, false);
        CHECK(counts.is_object() && counts.value("resumed", 0) == 2);
    }
    const auto chain = fixture.store.check_history(fixture.repository.context.repository_uuid);
    CHECK(chain && chain->runs == 3 && !chain->first_broken_run);
}

// R-104: an attempt cancelled (Ctrl+C) while it hashes is recorded in the
// history as an interrupted verification is: an INCOMPLETE FULL verification
// of the revision it verifies against, under a run of its own that names the
// relocation, from when the attempt started, with what it had matched, the
// items found so far (a photo whose time the copy changed, sample data) and
// the new carrier's evidence. It is no run for `verify --resume`, which
// names `archive relocate --resume` instead, and it makes no claim: the
// archive still has no last verify. A verification of the old carrier stopped
// before it is no longer listed, as a later verification of the same archive
// supersedes it (decision 0022); the relocation is. Resumed, it reads only
// the rest and records the FULL verification of revision 2. The hash chain
// holds, and `verify --resume` of the run then says that nothing is left to
// resume.
void an_interrupted_attempt_is_recorded_incomplete()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    fixture.archive.node("new/photos/p105.bin").mtime_ns += 3'600'000'000'000; // sample data
    const va::app::AttemptCheck attempts{&fixture.processes, fixture.host.host, {}};
    const auto listed = [&] {
        std::vector<va::app::OperationKind> kinds;
        const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
            [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; }, attempts);
        CHECK(read.has_value());
        for (const auto& operation : read ? read->operations : std::vector<va::app::UnfinishedOperation>{}) {
            kinds.push_back(operation.kind);
        }
        return kinds;
    };

    // A verification of the old carrier, cancelled: listed as stopped.
    {
        va::CancelToken cancel;
        test::HookedFiles hooked(fixture.archive, "p110.bin", [&] { cancel.cancel(); });
        va::VerifyServices services{hooked, *fixture.repository.files, fixture.media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host};
        va::VerifyRequest request;
        request.root = fixture.archive.root() / "old";
        const auto verified = va::verify_full(services, request, cancel, fixture.progress);
        CHECK(verified && verified->result == "INCOMPLETE");
    }
    CHECK(listed() == std::vector<va::app::OperationKind>{va::app::OperationKind::Verification});

    const auto evidence_before = count_rows(fixture, "media_evidence");
    va::CancelToken cancel;
    test::HookedFiles hooked(fixture.archive, "v130.bin", [&] { cancel.cancel(); });
    va::RelocateServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media,
        fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto interrupted = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, cancel, fixture.progress);
    CHECK(!interrupted && interrupted.error().kind == va::RelocateFailure::Kind::Incomplete &&
          interrupted.error().message == "relocation was interrupted; continue it with `archive relocate " +
                                             fixture.archive_uuid + " " + fixture.new_root() + " --resume`");
    const auto record = fixture.record();
    const auto run_id = record.value("run_id", std::string{});
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 2);
    if (!runs || runs->size() != 2) {
        return;
    }
    const auto attempt = runs->back();
    CHECK(attempt.result == "INCOMPLETE" && attempt.mode == "FULL" && attempt.revision == 1 &&
          attempt.run_id != run_id && attempt.started == record.value("started", std::string{}) &&
          !attempt.finished.empty() && attempt.finished >= attempt.started);
    // The 20 photos and the 10 videos before v130 (sample data), one with its time changed.
    CHECK(attempt.coverage_bytes == sample_photo_bytes + 5245);
    auto noted = nlohmann::json::parse(attempt.counts_json, nullptr, false);
    CHECK(noted.is_object() && noted.value("relocation", std::string{}) == run_id &&
          noted["items"].value("Verified", 0) == 29 && noted["items"].value("MetadataMismatch", 0) == 1);
    const auto items = fixture.store.verify_items(attempt.run_id);
    CHECK(items && items->size() == 1 && items->front().path.p == "photos/p105.bin" &&
          items->front().status == "MetadataMismatch");
    CHECK(count_rows(fixture, "media_evidence") == evidence_before + 1);

    // No verification to resume: `verify --resume` names the relocation's
    // resume instead. And no claim about the archive.
    va::VerifyRequest resume_it;
    resume_it.root = fixture.archive.root() / "new";
    resume_it.resume_run = attempt.run_id;
    const auto refused = fixture.verify(resume_it);
    CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::NotFound &&
          refused.error().message == "run " + attempt.run_id +
                                         " is an interrupted attempt of the unfinished relocation of archive " +
                                         fixture.archive_uuid + "; continue it with `archive relocate " +
                                         fixture.archive_uuid + " " + fixture.new_root() + " --resume`");
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto statuses = va::archive_statuses(
        fixture.store, *fixture.repository.files, fixture.repository.context.repo, va::Policy{}, now);
    CHECK(statuses && statuses->size() == 1 && !statuses->front().last_any && !statuses->front().last_full);
    CHECK(listed() == std::vector<va::app::OperationKind>{va::app::OperationKind::Relocation});

    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(resumed && resumed->revision == 2 && resumed->run_id == run_id &&
          resumed->result == "PASSED_WITH_WARNINGS");
    CHECK(fixture.archive.bytes_read == sample_bytes - sample_photo_bytes - 5245);
    const auto all = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(all && all->size() == 3 && all->back().run_id == run_id && all->back().result == "PASSED_WITH_WARNINGS" &&
          all->back().started == attempt.started && all->back().coverage_bytes == sample_bytes);
    const auto chain = fixture.store.check_history(fixture.repository.context.repository_uuid);
    CHECK(chain && chain->runs == 3 && !chain->first_broken_run);
    // The relocation has ended: nothing is left to resume.
    const auto ended = fixture.verify(resume_it);
    CHECK(!ended && ended.error().kind == va::VerifyStop::Kind::NotFound &&
          ended.error().message == "run " + attempt.run_id + " is an interrupted attempt of a relocation of archive " +
                                       fixture.archive_uuid + " that has ended; nothing is left to resume");
}

// The history says which runs are a relocation's (decision 0026): of a
// verification of the old carrier, an attempt of a relocation interrupted when
// the new disk went away, and the FULL verification with which its resume
// recorded revision 2, the attempt reads "interrupted relocation attempt" and
// the last "relocation from r1", in both languages, from what the relocation
// wrote into their counts; the verification reads nothing. A run whose counts
// say nothing of a relocation, or cannot be read, is no relocation's.
void the_history_marks_the_runs_of_a_relocation()
{
    RelocationFixture fixture(true);
    const auto verified = fixture.verify(fixture.archive.root() / "old");
    CHECK(verified && verified->result == "PASSED");
    const auto run_id = fixture.interrupted_relocation(fixture.processes.self, false);
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(resumed && resumed->revision == 2 && resumed->run_id == run_id);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 3);
    if (!runs || runs->size() != 3 || run_id.empty()) {
        return;
    }
    const auto& verification = (*runs)[0];
    const auto& attempt = (*runs)[1];
    const auto& relocation = (*runs)[2];
    CHECK(attempt.result == "INCOMPLETE" && attempt.revision == 1 && attempt.run_id != run_id);
    CHECK(relocation.run_id == run_id && relocation.revision == 2);

    CHECK(!va::relocation_part(verification));
    const auto attempt_part = va::relocation_part(attempt);
    CHECK(attempt_part && attempt_part->relocation == run_id && attempt_part->from_revision == 1 &&
          attempt_part->interrupted_attempt);
    const auto recorded_part = va::relocation_part(relocation);
    CHECK(recorded_part && recorded_part->relocation == run_id && recorded_part->from_revision == 1 &&
          !recorded_part->interrupted_attempt);

    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    CHECK(va::app::history_note(english, verification).empty());
    CHECK(va::app::history_note(chinese, verification).empty());
    CHECK(va::app::history_note(english, attempt) == "interrupted relocation attempt");
    CHECK(va::app::history_note(chinese, attempt) == "中斷的搬移嘗試");
    CHECK(va::app::history_note(english, relocation) == "relocation from r1");
    CHECK(va::app::history_note(chinese, relocation) == "從 r1 搬移");
    // The relocation's own run is no interrupted attempt for `verify --resume` either.
    va::VerifyRequest resume_it;
    resume_it.root = fixture.archive.root() / "new";
    resume_it.resume_run = run_id;
    const auto refused = fixture.verify(resume_it);
    CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::NotFound &&
          refused.error().message == "run " + run_id + " is not an interrupted run");

    va::VerifyRunRecord other = verification;
    other.counts_json = "{\"items\":{\"Verified\":2},\"resumed_from\":\"c0ffee00-0000-4000-8000-000000000001\"}";
    CHECK(!va::relocation_part(other) && va::app::history_note(english, other).empty());
    other.counts_json = "not json";
    CHECK(!va::relocation_part(other));
    other.counts_json.clear();
    CHECK(!va::relocation_part(other));
}

// R-104: a relocation resumes only while its archive is at the revision it
// verifies. The old carrier registered again with changed content (`register
// --new-revision`) records revision 2, the revision the relocation would
// create: the registration's record removes the relocation's record and
// saved progress, which can never finish now, and nothing is left to resume.
// A record that another operation left behind (one of an older version, which
// does not remove it) goes with the resume that finds its revision recorded.
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

// A record whose revision another operation recorded, leaving the record
// behind (as one of an older version does), is not unfinished (decision
// 0026, point 4): a relocation without --resume removes it and its
// checkpoints, begins one of its own, reads the whole carrier and records the
// revision after.
void a_relocation_replaces_a_record_that_can_no_longer_finish()
{
    RelocationFixture fixture(true);
    const auto run_id = fixture.interrupted_relocation(fixture.processes.self, false);
    const auto record = fixture.record();
    const std::string added = "added on the old carrier (sample data)";
    fixture.archive.add_file("old/added.txt", added);
    va::RegisterRequest request;
    request.root = fixture.archive.root() / "old";
    request.new_revision = true;
    const auto rebaselined = fixture.register_root(request);
    CHECK(rebaselined && rebaselined->revision == 2);
    CHECK(fixture.store.set_setting(fixture.key(), record.dump()));
    va::Entry photo;
    photo.path.p = "photos/p100.bin";
    CHECK(fixture.store.add_progress(run_id, {va::entry_key(photo)}));

    fixture.archive.add_file("new/added.txt", added); // the new carrier holds revision 2 too
    fixture.archive.bytes_read = 0;
    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(relocated && relocated->revision == 3 && !relocated->resumed && relocated->run_id != run_id);
    CHECK(fixture.archive.bytes_read == sample_bytes + added.size());
    CHECK(fixture.store.progress(run_id).value_or(std::set<std::string>{"x"}).empty());
    CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->result == "PASSED" && verified->revision == 3);
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

// How a relocation to the new disk made for an older revision is refused
// (decision 0030).
std::string made_for_an_older_revision(
    RelocationFixture& fixture, std::uint32_t verifies, std::uint32_t archive_revision)
{
    return "the unfinished relocation of archive " + fixture.archive_uuid + " (to \"" +
           va::path_to_utf8(fixture.archive.root() / "new") + "\") verifies revision " + std::to_string(verifies) +
           ", but the archive now has a later revision (" + std::to_string(archive_revision) +
           "); the relocation can no longer finish: abandon it with `archive abandon-relocation " +
           fixture.archive_uuid + "`, then relocate again";
}

// Decision 0026, point 2: a relocation that can no longer finish stops
// reading at its next checkpoint, after at most 1000 more entries, rather than
// reading the rest of the carrier for nothing. While the relocation of 2540
// sample files reads its 101st, another process records the revision it
// creates (`register --new-revision` of the old carrier), abandons it, or
// gives the archive a later revision than the one it verifies (an import,
// decision 0030).
void a_relocation_that_can_no_longer_finish_stops_reading()
{
    constexpr int many = 2500;
    enum class Meanwhile { RevisionRecorded, Abandoned, LaterRevision };
    for (const auto meanwhile : {Meanwhile::RevisionRecorded, Meanwhile::Abandoned, Meanwhile::LaterRevision}) {
        RelocationFixture fixture(true, many);
        fixture.copy_to_new_disk();
        std::uint64_t read_before = 0;
        test::HookedFiles hooked(fixture.archive, "m00100.txt", [&] {
            auto store = fixture.repository.store();
            if (meanwhile == Meanwhile::Abandoned) {
                const auto stored =
                    nlohmann::json::parse(store.setting(fixture.key()).value_or(""), nullptr, false);
                const auto run_id = stored.is_object() ? stored.value("run_id", std::string{}) : std::string{};
                CHECK(store.write_transaction([&] { return store.remove_relocation(fixture.archive_uuid, run_id); }));
            } else if (meanwhile == Meanwhile::LaterRevision) {
                test::import_later_revision(fixture, store, fixture.archive_uuid, 3);
            } else {
                fixture.archive.add_file("old/added.txt", "added on the old carrier (sample data)");
                va::RegisterRequest request;
                request.root = fixture.archive.root() / "old";
                request.new_revision = true;
                va::RegisterServices services{fixture.archive, *fixture.repository.files,
                    *fixture.repository.writer, fixture.media_writer, fixture.media, store,
                    fixture.repository.context.repo, fixture.host, fixture.processes};
                const auto registered = va::register_archive(services, request, fixture.cancel, fixture.progress);
                CHECK(registered && registered->revision == 2);
            }
            read_before = fixture.archive.bytes_read;
        });
        va::RelocateServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media,
            fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
            fixture.processes};
        const auto relocated = va::relocate_archive(services,
            {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
        CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Refused);
        if (meanwhile == Meanwhile::LaterRevision) {
            CHECK(!relocated && relocated.error().message == made_for_an_older_revision(fixture, 1, 3));
        }
        // From the 101st file on, the carrier holds 2400 more of them and the
        // 40 others; it stops after about 900.
        const auto unread = (many - 100) * many_file_bytes + sample_bytes;
        const auto read_after = fixture.archive.bytes_read - read_before;
        CHECK(read_after < unread / 2);
        if (read_after >= unread / 2) {
            std::cerr << "  read " << read_after << " of " << unread << " bytes after the relocation could no "
                      << "longer finish (case " << static_cast<int>(meanwhile) << ")\n";
        }
    }
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
           "\") is creating revision 2; this relocation recorded nothing (finish that registration with `register " +
           va::shell_argument(va::path_to_utf8(root)) +
           " --resume`, then relocate again; or abandon it with `archive abandon-registration " +
           fixture.archive_uuid + "`, then continue this relocation with `archive relocate " + fixture.archive_uuid +
           " " + fixture.new_root() + " --resume`)";
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

// Decision 0026, points 5 and 8: rows under the revision a relocation records
// that no plan names are no registration's saved progress, as rows left by a
// crash, or by a registration of an older version abandoned while it ran
// (sample data): one of a file the manifest holds, with other hashes, and one
// of a file it does not. The relocation's record removes them, as an import's
// does (decision 0030, point 3), where it refused, naming a registration with
// an empty root; it records the revision with its manifest's index, and the
// new carrier verifies. A plan of another revision names none of them, and
// stays. Rows that a plan of the revision names keep the revision for that
// registration (a_relocation_leaves_a_registration_its_saved_files).
void a_relocation_replaces_rows_that_no_plan_names()
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk();
    va::Entry changed;
    changed.path.p = "notes.txt";
    changed.size = 5;
    changed.b3 = va::blake3_hex("not the notes (sample data)");
    va::Entry left_behind;
    left_behind.path.p = "left behind.txt";
    left_behind.size = 1;
    left_behind.b3 = va::blake3_hex("x");
    CHECK(fixture.store.upsert_file_rows(fixture.archive_uuid, 2, {changed, left_behind}));
    const auto key = "registration:" + fixture.archive_uuid;
    const nlohmann::json other_revision{{"archive_uuid", fixture.archive_uuid}, {"revision", 3},
        {"label", "ARCH-0001"}, {"created", "2026-10-16T03:00:00Z"}, {"new_archive", false},
        {"root", va::path_to_utf8(fixture.archive.root() / "old")}};
    CHECK(fixture.store.set_setting(key, other_revision.dump()));

    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(relocated && relocated->revision == 2);
    if (!relocated) {
        std::cerr << "  relocation over rows that no plan names: " << relocated.error().message << '\n';
        return;
    }
    std::map<std::string, std::string> indexed;
    if (const auto rows = fixture.store.file_rows(fixture.archive_uuid, 2)) {
        for (const auto& row : *rows) {
            indexed[row.path.p] = std::string(1, row.type) + ":" + row.b3 + ":" + row.sha256;
        }
    }
    CHECK(!indexed.empty() && indexed == hashes_of(fixture, 2));
    CHECK(fixture.store.setting(key) == other_revision.dump());
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
}

// Writes the repository through `inner`, except the files of a manifest,
// which cannot be created (a sample failure, as a full disk would be).
// `meanwhile` runs once, as the first of them is about to fail.
class ManifestFailingWriter final : public va::IFileWriter {
public:
    explicit ManifestFailingWriter(va::IFileWriter& inner, std::function<void()> meanwhile = {})
        : inner_(inner), meanwhile_(std::move(meanwhile))
    {
    }

    std::expected<std::unique_ptr<va::IOutputFile>, va::IoError> create(
        const std::filesystem::path& destination) override
    {
        if (va::path_to_utf8(destination.filename()).contains(".vam")) {
            if (meanwhile_) {
                const auto hook = std::move(meanwhile_);
                meanwhile_ = nullptr;
                hook();
            }
            return std::unexpected(va::IoError{va::IoErrorKind::Other, 0, "disk full (sample)"});
        }
        return inner_.create(destination);
    }
    std::expected<void, va::IoError> remove(const std::filesystem::path& file) override
    {
        return inner_.remove(file);
    }
    std::expected<void, va::IoError> create_directories(const std::filesystem::path& directory) override
    {
        return inner_.create_directories(directory);
    }

private:
    va::IFileWriter& inner_;
    std::function<void()> meanwhile_;
};

// Decision 0026: a failure that stops a relocation (its scan, its manifest,
// its record) keeps its record, marked stopped, and its saved progress, as an
// interruption does. It keeps its cause and its kind (exit 11), and says how
// to go on once the cause is dealt with. Here the new root cannot be listed
// first; the resume then matches every entry, but its manifest cannot be
// written; the next one reads nothing again, but its record cannot be written
// (a trigger fails it); the last one records the revision, reading nothing.
void a_relocation_stopped_by_a_failure_says_how_to_go_on()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    const auto kept = "; the relocation keeps its saved progress: continue it with `archive relocate " +
                      fixture.archive_uuid + " " + fixture.new_root() + " --resume`";
    const auto stopped_with = [&](std::uint64_t matched) {
        const auto record = fixture.record();
        return record.is_object() && record.contains("stopped") &&
               fixture.saved(record.value("run_id", std::string{})).files == matched;
    };

    fixture.archive.node("new").list_error = va::IoError{va::IoErrorKind::Other, 5, "access denied (sample)"};
    const auto unlisted = fixture.relocate(fixture.archive.root() / "new");
    fixture.archive.node("new").list_error.reset();
    CHECK(!unlisted && unlisted.error().kind == va::RelocateFailure::Kind::Media &&
          unlisted.error().message == "cannot scan the new root: access denied (sample)" + kept);
    CHECK(stopped_with(0));

    ManifestFailingWriter failing(*fixture.repository.writer);
    va::RelocateServices services{fixture.archive, *fixture.repository.files, failing, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    fixture.archive.bytes_read = 0;
    const auto unwritten = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt, true}, fixture.cancel, fixture.progress);
    CHECK(!unwritten && unwritten.error().kind == va::RelocateFailure::Kind::Repository &&
          unwritten.error().message == "cannot write the manifest: disk full (sample)" + kept);
    CHECK(fixture.archive.bytes_read == sample_bytes);
    CHECK(stopped_with(40));

    fixture.repository.execute("CREATE TRIGGER sample_failure BEFORE INSERT ON archive_revisions "
                               "BEGIN SELECT RAISE(ABORT, 'sample failure'); END;");
    fixture.archive.bytes_read = 0;
    const auto unrecorded = fixture.relocate(fixture.archive.root() / "new", true);
    fixture.repository.execute("DROP TRIGGER sample_failure;");
    CHECK(!unrecorded && unrecorded.error().kind == va::RelocateFailure::Kind::Repository &&
          unrecorded.error().message == "cannot record the relocation: cannot run SQL: sample failure" + kept);
    CHECK(fixture.archive.bytes_read == 0);
    CHECK(stopped_with(40));
    CHECK(manifest_names(fixture) == std::set<std::string>{"r0001.vam"});
    const auto archive = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 1);

    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(resumed && resumed->revision == 2 && resumed->resumed && fixture.archive.bytes_read == 0);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1 && runs->front().coverage_bytes == sample_bytes &&
          nlohmann::json::parse(runs->front().counts_json, nullptr, false).value("resumed", 0) == 3);
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
}

// What happens while a relocation waits for the write lock.
enum class WhileWaiting {
    OtherCommits,         // another operation commits something else
    OtherRecordsIt,       // another operation records the same revision
    RelocationCancelled,  // the operator cancels the relocation (Ctrl+C)
    CancelledThenFree,    // the operator cancels it, and the other operation lets go of the lock a moment later
};

// Decision 0023: the record of a relocation waits for the write lock that
// another operation's record holds, also longer than a statement waits for
// it (the record of a large archive holds it for seconds). It then records,
// or stops (exit 12) when that operation recorded the same revision, rather
// than failing because the database is locked; cancelled while it waits, it
// stops as interrupted (exit 4). Nothing is changed unless it records; a
// relocation cancelled keeps everything it matched (decision 0026), and its
// resume reads no file again. Cancelled, it is recorded INCOMPLETE where the
// database allows it: not while the other operation still holds the lock,
// but once that one lets go within a statement's wait. Scaled down: the
// relocation's statements wait 100 ms for the lock, and the other operation
// holds it 400 ms; or 1 s and 1.5 s, where the INCOMPLETE run is recorded
// once the lock is free.
void a_relocation_waits_for_the_write_lock(WhileWaiting what)
{
    RelocationFixture fixture;
    fixture.copy_to_new_disk();
    const bool freed = what == WhileWaiting::CancelledThenFree;
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
            std::this_thread::sleep_for(std::chrono::milliseconds(freed ? 1500 : 400));
            CHECK(store.commit());
        });
        CHECK(held.try_acquire_for(std::chrono::seconds(60)));
        if (what == WhileWaiting::RelocationCancelled || freed) {
            cancel.cancel();
        }
    });
    auto store = fixture.repository.store(std::chrono::milliseconds(freed ? 1000 : 100));
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
    case WhileWaiting::CancelledThenFree:
        expected = !relocated && relocated.error().kind == va::RelocateFailure::Kind::Incomplete &&
                   relocated.error().message == "relocation was interrupted; continue it with `archive relocate " +
                                                    fixture.archive_uuid + " " + fixture.new_root() + " --resume`";
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
    // No FULL verification of revision 2. A relocation cancelled is
    // recorded INCOMPLETE where the database allows it (decision 0026),
    // which the other process's lock may not.
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && (what == WhileWaiting::OtherRecordsIt ? runs->empty() : runs->size() <= 1) &&
          std::ranges::all_of(*runs, [](const va::VerifyRunRecord& run) {
              return run.result == "INCOMPLETE" && run.revision == 1;
          }));
    if (freed) {
        // Every entry had matched: the attempt was cancelled at its record.
        CHECK(runs && runs->size() == 1 && runs->front().coverage_bytes == 9 &&
              nlohmann::json::parse(runs->front().counts_json, nullptr, false).value("relocation", std::string{}) ==
                  fixture.record().value("run_id", std::string{}));
    }
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

// ---- A relocation made for an older revision (decision 0030) ---------------

// A relocation against revision 1, interrupted at about half, and then an
// import gives the archive revision 3 (another repository's export, sample
// data). The relocation can no longer finish: revision 2 would set the
// archive back (R-15). Its resume is refused before it reads anything, saying
// so and naming the abandon, and so is a relocation without --resume, which
// would otherwise be told to resume it, and `verify --resume` of the
// INCOMPLETE run of the interrupted attempt, which names the relocation's
// resume while it can finish. Its record and saved progress stay, and the
// archive stays at revision 3. Abandoned, the archive takes a relocation
// against revision 3, which reads the whole carrier.
void a_relocation_made_for_an_older_revision_never_records()
{
    RelocationFixture fixture(true);
    const auto run_id = fixture.interrupted_relocation(fixture.processes.self, false);
    const auto record = fixture.record();
    const auto matched = fixture.saved(run_id);
    CHECK(matched.files == 20);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1 && runs->front().result == "INCOMPLETE");
    va::VerifyRequest resume_run;
    resume_run.root = fixture.archive.root() / "new";
    resume_run.resume_run = runs && !runs->empty() ? runs->front().run_id : std::string{};
    const auto attempt = "run " + *resume_run.resume_run + " is an interrupted attempt of the unfinished relocation of "
                         "archive " + fixture.archive_uuid;
    const auto continued = fixture.verify(resume_run);
    CHECK(!continued && continued.error().message == attempt + "; continue it with `archive relocate " +
                                                         fixture.archive_uuid + " " + fixture.new_root() +
                                                         " --resume`");
    test::import_later_revision(fixture, fixture.store, fixture.archive_uuid, 3);
    const auto refusal = made_for_an_older_revision(fixture, 1, 3);
    const auto older = fixture.verify(resume_run);
    CHECK(!older && older.error().kind == va::VerifyStop::Kind::NotFound &&
          older.error().message == attempt +
                                       ", which verifies revision 1, but the archive now has a later revision (3); "
                                       "the relocation can no longer finish: abandon it with `archive "
                                       "abandon-relocation " +
                                       fixture.archive_uuid + "`, then relocate again");

    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!resumed && resumed.error().kind == va::RelocateFailure::Kind::Refused &&
          resumed.error().message == refusal);
    const auto restarted = fixture.relocate(fixture.archive.root() / "new");
    CHECK(!restarted && restarted.error().kind == va::RelocateFailure::Kind::Refused &&
          restarted.error().message == refusal);
    CHECK(fixture.archive.bytes_read == 0);
    CHECK(fixture.record() == record);
    const auto kept = fixture.saved(run_id);
    CHECK(kept.files == matched.files && kept.bytes == matched.bytes);
    const auto revision = fixture.store.revision(fixture.archive_uuid, 2);
    CHECK(revision && !*revision);
    const auto archive = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 3);

    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto abandoned = va::abandon_relocation(services, {fixture.archive_uuid, std::nullopt});
    CHECK(abandoned && abandoned->revision == 1 && abandoned->files == matched.files);
    const auto relocated = fixture.relocate(fixture.archive.root() / "new");
    CHECK(relocated && relocated->revision == 4 && !relocated->resumed && fixture.archive.bytes_read == sample_bytes);
    const auto verified = fixture.verify(fixture.archive.root() / "new");
    CHECK(verified && verified->result == "PASSED" && verified->revision == 4);
}

// An import gives the archive revision 3 while a relocation against revision
// 1 writes its manifest of revision 2. Its record finds the archive at a later
// revision and records nothing: no revision 2, no manifest under its name, no
// verification, the archive at revision 3. Its record says that the attempt
// stopped, and keeps every entry it matched.
void a_relocation_meets_a_later_revision_at_its_record()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    test::StagingHookWriter writer(*fixture.repository.writer, [&] {
        auto store = fixture.repository.store(); // another process's connection
        test::import_later_revision(fixture, store, fixture.archive_uuid, 3);
    });
    va::RelocateServices services{fixture.archive, *fixture.repository.files, writer, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto relocated = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Refused &&
          relocated.error().message == made_for_an_older_revision(fixture, 1, 3));
    const auto revision = fixture.store.revision(fixture.archive_uuid, 2);
    CHECK(revision && !*revision);
    const auto archive = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 3);
    CHECK(manifest_names(fixture) == (std::set<std::string>{"r0001.vam", "r0003.vam"}));
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->empty());
    const auto record = fixture.record();
    CHECK(record.is_object() && record.value("revision", 0) == 1 && record.contains("stopped"));
    const auto run_id = record.is_object() ? record.value("run_id", std::string{}) : std::string{};
    CHECK(!run_id.empty() && fixture.saved(run_id).files == 40);
}

// An interrupted attempt says to go on with --resume (decision 0026), but not
// once the archive has a later revision than the one its relocation verifies,
// which a resume refuses (decision 0030). Here an import gives the archive
// revision 3 as the relocation opens its first file on the new carrier, where
// the new disk goes away: no checkpoint had entries to save, so none looked.
// The attempt ends as one made for an older revision, in the words of its
// resume, which reads nothing, and records no run; its record stays, saying
// that it stopped.
void an_interrupted_relocation_made_for_an_older_revision_says_to_abandon_it()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    test::HookedFiles hooked(fixture.archive, "p100.bin", [&] {
        auto store = fixture.repository.store(); // another process's connection
        test::import_later_revision(fixture, store, fixture.archive_uuid, 3);
        fixture.archive.node("new/photos/p100.bin").open_error =
            va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    });
    va::RelocateServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media,
        fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto relocated = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    const auto refusal = made_for_an_older_revision(fixture, 1, 3);
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Refused &&
          relocated.error().message == refusal);
    if (!relocated && relocated.error().message != refusal) {
        std::cerr << "  interrupted after an import: " << relocated.error().message << '\n';
    }
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->empty());
    const auto record = fixture.record();
    CHECK(record.is_object() && record.value("revision", 0) == 1 && record.contains("stopped"));

    fixture.archive.node("new/photos/p100.bin").open_error.reset();
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.relocate(fixture.archive.root() / "new", true);
    CHECK(!resumed && resumed.error().message == refusal && fixture.archive.bytes_read == 0);
}

// A failure that stops a relocation says to go on with --resume (decision
// 0026), but not once the archive has a later revision than the one the
// relocation verifies (decision 0030). Here an import gives the archive
// revision 3 after every entry of the new carrier matched and was saved, as
// the manifest of revision 2 is about to be written, which fails (a sample
// failure): the relocation ends as one made for an older revision, not with
// that failure, and records nothing; its record stays, saying that it
// stopped, with what it matched.
void a_relocation_stopped_by_a_failure_after_an_import_says_to_abandon_it()
{
    RelocationFixture fixture(true);
    fixture.copy_to_new_disk();
    ManifestFailingWriter failing(*fixture.repository.writer, [&] {
        auto store = fixture.repository.store(); // another process's connection
        test::import_later_revision(fixture, store, fixture.archive_uuid, 3);
    });
    va::RelocateServices services{fixture.archive, *fixture.repository.files, failing, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto relocated = va::relocate_archive(services,
        {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    const auto refusal = made_for_an_older_revision(fixture, 1, 3);
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Refused &&
          relocated.error().message == refusal);
    if (!relocated && relocated.error().message != refusal) {
        std::cerr << "  stopped by a failure after an import: " << relocated.error().message << '\n';
    }
    const auto revision = fixture.store.revision(fixture.archive_uuid, 2);
    CHECK(revision && !*revision);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->empty());
    const auto record = fixture.record();
    CHECK(record.is_object() && record.value("revision", 0) == 1 && record.contains("stopped"));
    const auto run_id = record.is_object() ? record.value("run_id", std::string{}) : std::string{};
    CHECK(!run_id.empty() && fixture.saved(run_id).files == 40);
}

// Decision 0030, point 6: a relocation that is not a resume is made against
// the revision the archive is at when it starts. Here another operation
// records a later revision after the relocation read the archive at revision
// 1, as it reads the manifest of revision 1 (the second opening of
// r0001.vam), before it writes its record: an import of revision 3, which
// would leave the relocation made for an older revision, or of revision 2,
// the one it would create. The transaction that would write the record finds
// the archive at that revision: it writes nothing, reads nothing of the new
// carrier, records no run, and says to run it again, which then verifies
// that revision and records the one after it.
void a_relocation_overtaken_before_its_record_writes_nothing()
{
    for (const std::uint32_t imported : {3u, 2u}) {
        RelocationFixture fixture(true);
        fixture.copy_to_new_disk();
        test::HookedFiles hooked(
            *fixture.repository.files, "r0001.vam",
            [&] {
                auto store = fixture.repository.store(); // another process's connection
                test::import_later_revision(fixture, store, fixture.archive_uuid, imported);
            },
            2);
        va::RelocateServices services{fixture.archive, hooked, *fixture.repository.writer, fixture.media,
            fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
            fixture.processes};
        fixture.archive.bytes_read = 0;
        const auto relocated = va::relocate_archive(services,
            {fixture.archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
        const auto refusal = "revision " + std::to_string(imported) + " of archive " + fixture.archive_uuid +
                             " was recorded by another operation before this relocation could begin; nothing was "
                             "changed (run `archive relocate` again)";
        CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Refused &&
              relocated.error().message == refusal);
        if (!relocated && relocated.error().message != refusal) {
            std::cerr << "  overtaken by revision " << imported << ": " << relocated.error().message << '\n';
        }
        CHECK(fixture.archive.bytes_read == 0);
        CHECK(fixture.store.setting(fixture.key()).value_or("x").empty());
        const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
        CHECK(runs && runs->empty());
        const auto again = fixture.relocate(fixture.archive.root() / "new");
        CHECK(again && again->revision == imported + 1 && fixture.archive.bytes_read == sample_bytes);
    }
}

} // namespace

void relocate_tests()
{
    relocation_keeps_identity_and_history();
    a_relocation_carries_copy_of_forward();
    an_earlier_revision_is_read_only_where_matching_needs_it();
    a_relocation_back_onto_an_earlier_carrier_records_it_so();
    differing_copy_is_refused();
    a_differing_copy_ends_the_relocation_while_the_lock_is_held();
    a_differing_copy_whose_relocation_cannot_end_says_to_abandon_it();
    a_differing_copy_marks_its_relocation_at_once();
    an_unrepresentable_name_marks_the_relocation_at_once();
    a_differs_mark_is_written_at_a_later_chance();
    a_relocation_interrupted_after_its_copy_differed_ends();
    an_unreadable_entry_ends_the_relocation_once_everything_was_read();
    a_read_that_fails_as_the_disk_drops_off_leaves_the_relocation_resumable();
    a_new_carrier_that_goes_away_leaves_the_relocation_resumable();
    a_new_carrier_that_stops_answering_leaves_the_relocation_resumable();
    the_rotational_override_applies_to_a_relocation();
    the_io_option_wins_in_a_relocation();
    known_unreadable_entries_are_carried();
    a_relocation_reports_the_recorded_name_collisions();
    an_interrupted_relocation_resumes_without_reading_again();
    a_resume_needs_the_volume_it_started_on();
    a_resumed_relocation_names_where_it_reads_and_keeps_its_start();
    an_interrupted_attempt_is_recorded_incomplete();
    the_history_marks_the_runs_of_a_relocation();
    a_resume_needs_the_revision_it_verifies();
    a_relocation_replaces_a_record_that_can_no_longer_finish();
    a_resume_waits_while_the_attempt_before_runs();
    attempts_of_one_relocation_take_turns();
    attempts_of_one_relocation_on_two_computers_record_the_revision_once();
    a_relocation_ends_a_registration_that_saved_nothing();
    a_relocation_that_loses_while_hashing_records_nothing();
    a_relocation_that_can_no_longer_finish_stops_reading();
    the_heartbeat_writes_only_its_own_relocation();
    a_relocation_removes_the_plan_of_its_revision();
    a_relocation_leaves_a_registration_its_saved_files();
    a_relocation_replaces_rows_that_no_plan_names();
    an_abandoned_registration_lets_the_relocation_record();
    a_relocation_stopped_by_a_failure_says_how_to_go_on();
    a_relocation_waits_for_the_write_lock(WhileWaiting::OtherCommits);
    a_relocation_waits_for_the_write_lock(WhileWaiting::OtherRecordsIt);
    a_relocation_waits_for_the_write_lock(WhileWaiting::RelocationCancelled);
    a_relocation_waits_for_the_write_lock(WhileWaiting::CancelledThenFree);
    abandoning_a_relocation_removes_only_its_record_and_saved_progress();
    a_running_relocation_is_not_abandoned();
    an_attempt_stops_when_its_relocation_is_abandoned();
    a_relocation_made_for_an_older_revision_never_records();
    a_relocation_meets_a_later_revision_at_its_record();
    an_interrupted_relocation_made_for_an_older_revision_says_to_abandon_it();
    a_relocation_stopped_by_a_failure_after_an_import_says_to_abandon_it();
    a_relocation_overtaken_before_its_record_writes_nothing();
}
