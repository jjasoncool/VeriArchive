#include "test_support.h"

#include "engine_fixture.h"
#include "manifest_samples.h"

#include "core/engine/abandon_registration.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/transfer.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_writer.h"
#include "core/scan/scanner.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <semaphore>
#include <set>
#include <thread>
#include <utility>

namespace {

std::filesystem::path out_directory(const test::EngineFixture& fixture, const char* name)
{
    return fixture.repository.context.repo.root.parent_path() / name;
}

std::string read_bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::set<std::string> names_in(const std::filesystem::path& directory)
{
    std::set<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        names.insert(va::path_to_utf8(entry.path().filename()));
    }
    return names;
}

// Decision 0031: a manifest with chunk lines imports, and loads with the
// chunk hashes of its files. One whose content hashes do not give its
// merkle_root, though its body matches its header, is a ManifestHashMismatch:
// at import, which then records nothing, and when a recorded revision with
// such bytes is loaded.
void chunked_manifests_import_and_load()
{
    constexpr std::uint64_t chunk_size = 64 * 1024;
    test::TestRepository target;
    auto store = target.store();
    const auto& layout = target.context.repo;
    auto& files = *target.files;
    auto& writer = *target.writer;
    const auto outside = layout.root.parent_path() / "outside";
    std::filesystem::create_directories(outside / "changed");

    va::ManifestHeader header;
    header.archive_uuid = "5b0e1d3c-7a1f-4c2e-9d6a-0f1e2d3c4b5a";
    header.label = "ARCH-0001";
    header.revision = 1;
    header.created = "2026-10-16T00:00:00Z";
    header.app_version = "0.1.0";
    header.os = "windows";
    header.host = "test-host";
    header.exclusions = va::default_exclusions();
    const std::vector<va::Entry> entries{
        test::sample_file("big.bin", test::patterned(2 * chunk_size + 7, 1), chunk_size),
        test::sample_file("small.txt", "small", chunk_size)};
    const auto good = outside / "r0001.vam";
    {
        va::ManifestWriter manifest(writer, files, good, chunk_size);
        for (const auto& entry : entries) {
            CHECK(manifest.add(entry));
        }
        auto written = manifest.finish(header);
        CHECK(written && written->file->commit());
    }

    // The same manifest with one chunk hash changed and a header that
    // describes its body: only the Merkle root disagrees.
    std::string bytes;
    {
        std::ifstream input(good, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    auto parts = test::split_manifest(bytes);
    CHECK(parts.lines.size() == 5); // two entries, three chunk lines
    if (parts.lines.size() != 5) {
        return;
    }
    auto chunk = va::chunk_line_from_json(nlohmann::json::parse(parts.lines[3]));
    chunk.b3 = va::blake3_hex("other");
    parts.lines[3] = va::chunk_line_to_json(chunk);
    const auto changed = outside / "changed" / "r0001.vam";
    std::ofstream(changed, std::ios::binary) << test::assemble_manifest(parts);

    const auto rejected =
        va::import_manifest(store, files, writer, layout, changed, false, test::not_cancelled(), test::no_progress());
    CHECK(!rejected && rejected.error().failure_class == "ManifestHashMismatch");
    const auto nothing = store.revisions(header.archive_uuid);
    CHECK(nothing && nothing->empty());

    const auto imported =
        va::import_manifest(store, files, writer, layout, good, false, test::not_cancelled(), test::no_progress());
    CHECK(imported && imported->new_archive && imported->revision == 1);
    const auto loaded = va::load_recorded_manifest(store, files, layout, header.archive_uuid, 1);
    CHECK(loaded && loaded->header.chunk_size == chunk_size && loaded->entries.size() == 2);
    if (loaded && loaded->entries.size() == 2) {
        CHECK(loaded->entries[0].chunks == entries[0].chunks && loaded->entries[0].chunks.size() == 3);
        CHECK(loaded->entries[1].chunks.empty());
    }

    // Revision 2 recorded with such bytes, as a faulty writer would leave
    // them: loading it is a hash mismatch, not a repository error.
    parts.header.revision = 2;
    parts.header.supersedes = 1;
    const auto second = test::assemble_manifest(parts);
    const auto line = second.substr(0, second.find('\n'));
    std::ofstream(va::manifest_path(layout.manifests(), header.archive_uuid, 2), std::ios::binary) << second;
    const auto recorded = va::header_from_json(nlohmann::json::parse(line));
    CHECK(store.insert_revision({header.archive_uuid, 2, "manifests/" + header.archive_uuid + "/r0002.vam", line,
        va::blake3_hex(line), recorded.body_blake3, "", header.created}));
    const auto mismatched = va::load_recorded_manifest(store, files, layout, header.archive_uuid, 2);
    CHECK(!mismatched && mismatched.error().failure_class == "ManifestHashMismatch");

    // Revision 1 changed in the repository to a chunk size no manifest may
    // have, its body as recorded: a changed manifest (the hashes come first),
    // not a malformed one.
    const auto stored = va::manifest_path(layout.manifests(), header.archive_uuid, 1);
    auto changed_header = test::split_manifest(bytes);
    changed_header.header.chunk_size = 1000;
    std::ofstream(stored, std::ios::binary | std::ios::trunc) << test::assemble_manifest(changed_header);
    const auto changed_size = va::load_recorded_manifest(store, files, layout, header.archive_uuid, 1);
    CHECK(!changed_size && changed_size.error().failure_class == "ManifestHashMismatch");
}

// M4: export -> import round trip, duplicate detection, rollback protection.
void export_import_round_trip()
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
        out_directory(source, "export-r1"), test::not_cancelled(), test::no_progress());
    CHECK(r1.has_value());
    CHECK(r1 && std::filesystem::exists(r1->identity));

    va::RegisterRequest rebaseline;
    rebaseline.root = source.archive.root();
    rebaseline.new_revision = true;
    source.archive.add_file("b.txt", "b");
    CHECK(source.register_root(rebaseline).has_value());
    const auto r2 = va::export_archive(source.store, files, writer, layout, first->archive_uuid,
        out_directory(source, "export-r2"), test::not_cancelled(), test::no_progress());
    CHECK(r2.has_value());
    if (!r1 || !r2) {
        return;
    }

    // Import into a second repository.
    test::TestRepository target;
    auto store = target.store();
    const auto& target_layout = target.context.repo;
    auto imported = va::import_manifest(store, *target.files, *target.writer, target_layout, r2->manifest, false, test::not_cancelled(), test::no_progress());
    CHECK(imported && imported->new_archive && imported->revision == 2 && imported->label == "ARCH-0001");
    const auto archive = store.find_archive(first->archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);

    imported = va::import_manifest(store, *target.files, *target.writer, target_layout, r2->manifest, false, test::not_cancelled(), test::no_progress());
    CHECK(imported && imported->already_present);

    // R-15: an older revision is refused unless forced.
    auto rollback = va::import_manifest(store, *target.files, *target.writer, target_layout, r1->manifest, false, test::not_cancelled(), test::no_progress());
    CHECK(!rollback && rollback.error().failure_class == "ManifestRevisionRollback");
    rollback = va::import_manifest(store, *target.files, *target.writer, target_layout, r1->manifest, true, test::not_cancelled(), test::no_progress());
    CHECK(rollback && rollback->revision == 1);
    const auto kept = store.find_archive(first->archive_uuid);
    CHECK(kept && *kept && (*kept)->current_revision == 2);

    // A changed copy of a known revision is a hash mismatch.
    auto bytes = read_bytes(r2->manifest);
    bytes[bytes.size() - 4] ^= 0x01;
    const auto tampered = out_directory(source, "tampered.vam");
    std::ofstream(tampered, std::ios::binary) << bytes;
    const auto rejected = va::import_manifest(store, *target.files, *target.writer, target_layout, tampered, false, test::not_cancelled(), test::no_progress());
    CHECK(!rejected && rejected.error().failure_class == "ManifestHashMismatch");
}

// ---- An imported manifest gets its name in the record's transaction (decision 0030)

// The archive of the sample manifests below (sample data).
const va::Uuid sample_uuid = "c0ffee00-0000-4000-8000-000000000030";

// Revision `revision` of the sample archive, written into `directory` as an
// export of another repository holds it (sample data): three files, and
// `host` in its `created_by`, which tells two manifests of one revision apart.
std::filesystem::path sample_manifest(test::TestRepository& repository, const std::filesystem::path& directory,
    std::uint32_t revision, const std::string& host)
{
    std::filesystem::create_directories(directory);
    const auto path = directory / std::format("r{:04}.vam", revision);
    va::ManifestWriter writer(*repository.writer, *repository.files, path, 0);
    const std::pair<const char*, const char*> files[] = {
        {"a.txt", "a (sample data)"}, {"b.txt", "b (sample data)"}, {"photos/c.jpg", "c (sample data)"}};
    for (const auto& [name, content] : files) {
        va::Entry entry;
        entry.path.p = name;
        entry.size = std::string_view(content).size();
        entry.mtime_ns = 1'700'000'000'000'000'000;
        entry.b3 = va::blake3_hex(content);
        entry.sha256 = va::sha256_hex(content);
        CHECK(writer.add(entry).has_value());
    }
    va::ManifestHeader header;
    header.archive_uuid = sample_uuid;
    header.label = "ARCH-0007";
    header.revision = revision;
    if (revision > 1) {
        header.supersedes = revision - 1;
    }
    header.created = "2026-10-16T01:00:00Z";
    header.app_version = "0.1.0";
    header.os = "windows";
    header.host = host;
    header.identity = test::fake_volume("/vol");
    header.root_hint = "/vol";
    header.exclusions = va::default_exclusions();
    auto written = writer.finish(header);
    CHECK(written.has_value());
    if (written) {
        CHECK(written->file->commit().has_value());
    }
    return path;
}

std::filesystem::path exports_of(const test::TestRepository& repository, const char* name)
{
    return repository.context.repo.root.parent_path() / "exports" / name;
}

// Where the repository keeps revision `revision` of the sample archive (R-63).
std::filesystem::path recorded_path(const test::TestRepository& repository, std::uint32_t revision)
{
    return va::manifest_path(repository.context.repo.manifests(), sample_uuid, revision);
}

std::expected<va::ImportResult, va::ImportFailure> import(test::TestRepository& repository, va::ArchiveStore& store,
    const std::filesystem::path& manifest, bool force_rollback = false)
{
    return va::import_manifest(store, *repository.files, *repository.writer, repository.context.repo, manifest,
        force_rollback, test::not_cancelled(), test::no_progress());
}

// How an import that finds its revision recorded stops.
std::string lost_message(std::uint32_t revision)
{
    return "revision " + std::to_string(revision) + " of archive " + sample_uuid +
           " was recorded by another operation while this import ran; nothing was imported (import it again to "
           "compare it with the recorded manifest)";
}

// That revision `revision` of the sample archive is recorded with the bytes of
// `manifest` (R-14): its record, its index, and the manifest the repository
// holds; the latest revision also loads as verification loads it (an older
// one is refused there, R-15).
void check_recorded_as(test::TestRepository& repository, va::ArchiveStore& store, std::uint32_t revision,
    const std::filesystem::path& manifest, bool latest = true)
{
    const auto bytes = read_bytes(manifest);
    const auto header_line = bytes.substr(0, bytes.find('\n'));
    CHECK(read_bytes(recorded_path(repository, revision)) == bytes);
    const auto record = store.revision(sample_uuid, revision);
    CHECK(record && *record && (*record)->header_json == header_line &&
          (*record)->header_blake3 == va::blake3_hex(header_line));
    if (latest) {
        const auto loaded =
            va::load_recorded_manifest(store, *repository.files, repository.context.repo, sample_uuid, revision);
        CHECK(loaded && loaded->entries.size() == 3);
    }
    const auto rows = store.file_rows(sample_uuid, revision);
    CHECK(rows && rows->size() == 3);
}

// One import in a thread of its own, as in a process of its own: its own
// database connection. Once its copy of the manifest is written and flushed,
// it says so (`staged`) and waits (`go`); `on_naming` runs when it gives the
// manifest its name, inside the transaction that records the revision.
// `lock_wait`: how long its statements wait for another connection's write lock.
struct Import {
    Import(test::TestRepository& repository, std::filesystem::path manifest,
        std::chrono::milliseconds lock_wait = va::statement_lock_wait)
        : repository(repository), manifest(std::move(manifest)), store(repository.store(lock_wait)),
          writer(
              *repository.writer,
              [this] {
                  staged.release();
                  (void)go.try_acquire_for(std::chrono::seconds(60));
              },
              [this] {
                  if (on_naming) {
                      on_naming();
                  }
              })
    {
    }

    void start()
    {
        thread = std::thread([this] {
            outcome = va::import_manifest(store, *repository.files, writer, repository.context.repo, manifest, false, test::not_cancelled(), test::no_progress());
        });
    }

    test::TestRepository& repository;
    std::filesystem::path manifest;
    va::ArchiveStore store;
    std::binary_semaphore staged{0};
    std::binary_semaphore go{0};
    test::StagingHookWriter writer;
    std::function<void()> on_naming;
    std::expected<va::ImportResult, va::ImportFailure> outcome = std::unexpected(va::ImportFailure{});
    std::thread thread;
};

// Two imports of one revision at once (two exports of it, from two
// repositories), interleaved at the manifest: both have copied and flushed
// theirs when one is let through to record the revision, then the other. The
// second finds the revision recorded and stops (exit 12): the recorded
// manifest, its record and its index are the first one's, and neither leaves
// a file behind. Imported again, each is compared with the recorded manifest
// (R-87): the loser's bytes differ, the winner's are already present.
void imports_of_one_revision_record_it_once(bool first_started_records)
{
    test::TestRepository repository;
    Import first(repository, sample_manifest(repository, exports_of(repository, "first"), 1, "host-first"));
    Import second(repository, sample_manifest(repository, exports_of(repository, "second"), 1, "host-second"));
    first.start();
    second.start();
    const bool first_staged = first.staged.try_acquire_for(std::chrono::seconds(60));
    const bool second_staged = second.staged.try_acquire_for(std::chrono::seconds(60));
    CHECK(first_staged && second_staged);
    auto& winner = first_started_records ? first : second;
    auto& loser = first_started_records ? second : first;
    winner.go.release();
    winner.thread.join();
    loser.go.release();
    loser.thread.join();

    CHECK(winner.outcome && winner.outcome->new_archive && winner.outcome->revision == 1);
    CHECK(!loser.outcome && loser.outcome.error().kind == va::ImportFailure::Kind::Refused &&
          loser.outcome.error().message == lost_message(1));
    auto store = repository.store();
    check_recorded_as(repository, store, 1, winner.manifest);
    CHECK(names_in(recorded_path(repository, 1).parent_path()) == std::set<std::string>{"r0001.vam"});
    const auto archive = store.find_archive(sample_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 1 && (*archive)->status == "REGISTERED");

    const auto again = import(repository, store, loser.manifest);
    CHECK(!again && again.error().kind == va::ImportFailure::Kind::Failed &&
          again.error().failure_class == "ManifestHashMismatch");
    const auto present = import(repository, store, winner.manifest);
    CHECK(present && present->already_present);
    check_recorded_as(repository, store, 1, winner.manifest);
}

// The record of a large manifest holds the write lock for seconds, longer
// than a statement waits for it. An import that reaches its record meanwhile
// waits for the lock and then finds the revision recorded (exit 12), rather
// than failing because the database is locked. Scaled down: the loser's
// statements wait 100 ms for the lock, and the winner holds it 400 ms, in its
// record.
void an_import_waits_for_the_record_of_another()
{
    test::TestRepository repository;
    Import winner(repository, sample_manifest(repository, exports_of(repository, "first"), 1, "host-winner"));
    Import loser(repository, sample_manifest(repository, exports_of(repository, "second"), 1, "host-loser"),
        std::chrono::milliseconds(100));
    std::binary_semaphore recording{0};
    winner.on_naming = [&] {
        recording.release();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    };
    winner.start();
    loser.start();
    const bool winner_staged = winner.staged.try_acquire_for(std::chrono::seconds(60));
    const bool loser_staged = loser.staged.try_acquire_for(std::chrono::seconds(60));
    CHECK(winner_staged && loser_staged);
    winner.go.release();
    // The winner holds the write lock now, until it has slept.
    CHECK(recording.try_acquire_for(std::chrono::seconds(60)));
    loser.go.release();
    winner.thread.join();
    loser.thread.join();

    CHECK(winner.outcome && winner.outcome->revision == 1);
    const bool lost = !loser.outcome && loser.outcome.error().kind == va::ImportFailure::Kind::Refused &&
                      loser.outcome.error().message == lost_message(1);
    CHECK(lost);
    if (!lost && !loser.outcome) {
        std::cerr << "  loser: " << loser.outcome.error().message << '\n';
    }
    auto store = repository.store();
    check_recorded_as(repository, store, 1, winner.manifest);
    CHECK(names_in(recorded_path(repository, 1).parent_path()) == std::set<std::string>{"r0001.vam"});
}

// An import leaves the revision to an unfinished registration that has saved
// files hashed for it (R-104), as a relocation does (decision 0023): recorded
// over them, its insert failed on their keys with SQLite's own words. It is
// refused (exit 12), naming both ways on, and changes nothing. Abandoned on
// purpose (decision 0025), the registration leaves the revision to the import.
void an_import_leaves_a_registration_its_saved_files()
{
    test::TestRepository repository;
    auto store = repository.store();
    CHECK(import(repository, store, sample_manifest(repository, exports_of(repository, "r1"), 1, "host")));
    // `register --new-revision` of the archive in this repository, which has
    // saved one file it hashed (sample data).
    const auto key = "registration:" + sample_uuid;
    const nlohmann::json plan{{"archive_uuid", sample_uuid}, {"revision", 2}, {"label", "ARCH-0007"},
        {"created", "2026-10-16T02:00:00Z"}, {"new_archive", false}, {"root", "/vol/photos"}};
    CHECK(store.set_setting(key, plan.dump()));
    va::Entry hashed;
    hashed.path.p = "a.txt";
    hashed.size = 26;
    hashed.b3 = va::blake3_hex("a, hashed by the registration (sample data)");
    hashed.sha256 = va::sha256_hex("a, hashed by the registration (sample data)");
    CHECK(store.upsert_file_rows(sample_uuid, 2, {hashed}));

    const auto r2 = sample_manifest(repository, exports_of(repository, "r2"), 2, "host");
    const auto refused = import(repository, store, r2);
    CHECK(!refused && refused.error().kind == va::ImportFailure::Kind::Refused &&
          refused.error().message ==
              "an unfinished registration of archive " + sample_uuid +
                  " (root \"/vol/photos\") is creating revision 2; nothing was imported (finish that registration "
                  "with `register /vol/photos --resume`, or abandon it with `archive abandon-registration " +
                  sample_uuid + "` and import again)");
    const auto revision = store.revision(sample_uuid, 2);
    CHECK(revision && !*revision);
    const auto archive = store.find_archive(sample_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 1);
    CHECK(store.setting(key) == plan.dump());
    const auto saved = store.file_rows(sample_uuid, 2);
    CHECK(saved && saved->size() == 1);
    CHECK(names_in(recorded_path(repository, 1).parent_path()) == std::set<std::string>{"r0001.vam"});

    test::FakeProcessTable processes;
    va::AbandonServices services{store, processes, "test-host"};
    const auto abandoned = va::abandon_registration(services, {sample_uuid, std::nullopt, std::nullopt});
    CHECK(abandoned && abandoned->files == 1 && !abandoned->first_registration);
    const auto imported = import(repository, store, r2);
    CHECK(imported && imported->revision == 2 && !imported->new_archive);
    check_recorded_as(repository, store, 2, r2);
    const auto after = store.find_archive(sample_uuid);
    CHECK(after && *after && (*after)->current_revision == 2);
}

// What records a revision removes what creates it and can no longer finish,
// as a registration's and a relocation's records do (decisions 0023, 0026):
// the plan of a registration that has saved nothing of it, and the record and
// saved progress of a relocation. A plan and a record of another revision
// stay. Rows under the revision that no plan names are no saved progress
// (decision 0026): the index replaces them. An archive whose first
// registration in this repository was unfinished is registered once a
// revision of it is imported, keeping its own label.
void an_import_removes_what_can_no_longer_finish()
{
    test::TestRepository repository;
    auto store = repository.store();
    const auto plan_key = "registration:" + sample_uuid;
    const auto relocation_key = va::relocation_key(sample_uuid);
    const auto plan_of = [](std::uint32_t revision) {
        return nlohmann::json{{"archive_uuid", sample_uuid}, {"revision", revision}, {"label", "ARCH-0001"},
            {"created", "2026-10-16T00:30:00Z"}, {"new_archive", revision == 1}, {"root", "/vol"}}
            .dump();
    };
    const auto relocation_of = [](std::uint32_t verifies, const std::string& run_id) {
        return nlohmann::json{{"archive_uuid", sample_uuid}, {"revision", verifies}, {"run_id", run_id},
            {"root", "/new"}}
            .dump();
    };
    va::Entry matched;
    matched.path.p = "a.txt";

    // The first registration of the archive here is unfinished, and has saved
    // nothing (sample data).
    CHECK(store.insert_archive(
        {sample_uuid, "ARCH-0001", "2026-10-16T00:30:00Z", 0, "REGISTRATION_INCOMPLETE", "", ""}));
    CHECK(store.set_setting(plan_key, plan_of(1)));
    const auto first = import(repository, store, sample_manifest(repository, exports_of(repository, "r1"), 1, "host"));
    CHECK(first && first->revision == 1 && !first->new_archive);
    CHECK(store.setting(plan_key).value_or("x").empty());
    const auto archive = store.find_archive(sample_uuid);
    CHECK(archive && *archive && (*archive)->status == "REGISTERED" && (*archive)->current_revision == 1 &&
          (*archive)->label == "ARCH-0001");

    // A registration of revision 2 and a relocation against revision 1.
    CHECK(store.set_setting(plan_key, plan_of(2)));
    CHECK(store.set_setting(relocation_key, relocation_of(1, "run-against-1")));
    CHECK(store.add_progress("run-against-1", {va::entry_key(matched)}));
    const auto r2 = sample_manifest(repository, exports_of(repository, "r2"), 2, "host");
    CHECK(import(repository, store, r2));
    CHECK(store.setting(plan_key).value_or("x").empty());
    CHECK(store.setting(relocation_key).value_or("x").empty());
    CHECK(store.progress("run-against-1").value_or(std::set<std::string>{"x"}).empty());

    // Of another revision: a plan of revision 4, and a relocation against 3,
    // which creates 4, stay when revision 3 is imported. Rows under 3 that no
    // plan names, left behind by a crash (sample data), go.
    CHECK(store.set_setting(plan_key, plan_of(4)));
    CHECK(store.set_setting(relocation_key, relocation_of(3, "run-against-3")));
    CHECK(store.add_progress("run-against-3", {va::entry_key(matched)}));
    va::Entry left_behind;
    left_behind.path.p = "left behind.txt";
    left_behind.size = 1;
    left_behind.b3 = va::blake3_hex("x");
    CHECK(store.upsert_file_rows(sample_uuid, 3, {left_behind}));
    const auto r3 = sample_manifest(repository, exports_of(repository, "r3"), 3, "host");
    const auto third = import(repository, store, r3);
    CHECK(third && third->revision == 3);
    if (!third) {
        std::cerr << "  revision 3: " << third.error().message << '\n';
    }
    check_recorded_as(repository, store, 3, r3);
    CHECK(store.setting(plan_key) == plan_of(4));
    CHECK(store.setting(relocation_key) == relocation_of(3, "run-against-3"));
    CHECK(store.progress("run-against-3").value_or(std::set<std::string>{}).size() == 1);
}

// R-15 holds in the record's transaction: a revision older than one that
// another operation recorded while this import copied its manifest is
// refused unless forced (ManifestRevisionRollback), and changes nothing.
void an_import_checks_the_revision_in_its_record()
{
    test::TestRepository repository;
    const auto r1 = sample_manifest(repository, exports_of(repository, "r1"), 1, "host");
    const auto r2 = sample_manifest(repository, exports_of(repository, "r2"), 2, "host");
    std::expected<va::ImportResult, va::ImportFailure> newer = std::unexpected(va::ImportFailure{});
    test::StagingHookWriter writer(*repository.writer, [&] {
        // Another process imports revision 2 meanwhile.
        auto other = repository.store();
        newer = import(repository, other, r2);
    });
    auto store = repository.store();
    const auto refused =
        va::import_manifest(store, *repository.files, writer, repository.context.repo, r1, false, test::not_cancelled(), test::no_progress());
    CHECK(newer && newer->revision == 2);
    CHECK(!refused && refused.error().kind == va::ImportFailure::Kind::Failed &&
          refused.error().failure_class == "ManifestRevisionRollback" &&
          refused.error().message == "revision 1 is older than the known revision 2; pass --force-rollback to "
                                     "import it anyway");
    const auto revision = store.revision(sample_uuid, 1);
    CHECK(revision && !*revision);
    CHECK(names_in(recorded_path(repository, 1).parent_path()) == std::set<std::string>{"r0002.vam"});

    const auto forced = import(repository, store, r1, true);
    CHECK(forced && forced->revision == 1);
    check_recorded_as(repository, store, 1, r1, false);
    const auto archive = store.find_archive(sample_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);
}

// Passes everything to `inner`, and runs `hook` when a file named `name` is
// opened for the `nth` time.
class NthOpenHook final : public va::IFileSystemAccess {
public:
    NthOpenHook(va::IFileSystemAccess& inner, std::string name, int nth, std::function<void()> hook)
        : inner_(inner), name_(std::move(name)), nth_(nth), hook_(std::move(hook))
    {
    }
    std::expected<std::vector<va::DirEntry>, va::IoError> list(const std::filesystem::path& directory) override
    {
        return inner_.list(directory);
    }
    std::expected<std::unique_ptr<va::IByteSource>, va::IoError> open(
        const std::filesystem::path& file, va::ReadOptions options) override
    {
        if (va::path_to_utf8(file.filename()) == name_ && ++opened_ == nth_) {
            hook_();
        }
        return inner_.open(file, options);
    }
    std::expected<std::string, va::IoError> read_link(const std::filesystem::path& link) override
    {
        return inner_.read_link(link);
    }
    std::expected<void, va::IoError> drop_cache(const std::filesystem::path& file) override
    {
        return inner_.drop_cache(file);
    }

private:
    va::IFileSystemAccess& inner_;
    std::string name_;
    int nth_;
    int opened_ = 0;
    std::function<void()> hook_;
};

// The manifest is copied as it was validated. A source that changes after it
// was validated and before it is copied (one byte of its body, or of its
// header; a second read of removable media may differ as well) is not
// imported (ManifestHashMismatch), and leaves nothing behind.
void an_import_copies_the_bytes_it_validated()
{
    for (const bool in_header : {false, true}) {
        test::TestRepository repository;
        const auto manifest = sample_manifest(repository, exports_of(repository, "r1"), 1, "host");
        const auto bytes = read_bytes(manifest);
        // The reader opens the source first, the copy second.
        NthOpenHook files(*repository.files, "r0001.vam", 2, [&] {
            auto changed = bytes;
            changed[in_header ? changed.find("ARCH-0007") + 8 : changed.size() - 5] ^= 0x01;
            std::ofstream(manifest, std::ios::binary | std::ios::trunc) << changed;
        });
        auto store = repository.store();
        const auto imported =
            va::import_manifest(store, files, *repository.writer, repository.context.repo, manifest, false, test::not_cancelled(), test::no_progress());
        CHECK(!imported && imported.error().failure_class == "ManifestHashMismatch" &&
              imported.error().message == "the manifest changed while it was imported; nothing was imported");
        const auto archives = store.archives();
        CHECK(archives && archives->empty());
        CHECK(names_in(recorded_path(repository, 1).parent_path()).empty());
    }
}

// The index is made of the entries read from the source a second time, after
// the reader validated its body (R-62). A source that gives other bytes only
// in that read, as removable media can (sample data: one hex digit of the
// first entry's BLAKE3, or a quote made an apostrophe, so that a line no
// longer parses, decision 0028), would index entries that the manifest does
// not hold, while the manifest copied would be the one validated: it is not
// imported (ManifestHashMismatch), and nothing is recorded or left behind.
// Read as it is, it imports.
void an_import_indexes_the_entries_it_validated()
{
    const auto b3 = va::blake3_hex("a (sample data)"); // the first entry, a.txt
    auto other = b3;
    other[0] = other[0] == '0' ? '1' : '0';
    for (const auto& to : {"\"b3\":\"" + other, "\"b3\":'" + b3}) {
        test::TestRepository repository;
        const auto manifest = sample_manifest(repository, exports_of(repository, "r1"), 1, "host");
        const auto bytes = read_bytes(manifest);
        test::SecondReadDiffers files(*repository.files, "r0001.vam", 1, bytes.find('\n') + 1, "\"b3\":\"" + b3, to);
        auto store = repository.store();
        const auto imported =
            va::import_manifest(store, files, *repository.writer, repository.context.repo, manifest, false, test::not_cancelled(), test::no_progress());
        CHECK(files.differed());
        CHECK(!imported && imported.error().failure_class == "ManifestHashMismatch" &&
              imported.error().message == "the manifest changed while it was imported; nothing was imported");
        if (!imported && imported.error().failure_class != "ManifestHashMismatch") {
            std::cerr << "  read as " << to << ": " << imported.error().message << '\n';
        }
        const auto archives = store.archives();
        CHECK(archives && archives->empty());
        const auto rows = store.file_rows(sample_uuid, 1);
        CHECK(rows && rows->empty());
        CHECK(names_in(recorded_path(repository, 1).parent_path()).empty());

        const auto again = import(repository, store, manifest);
        CHECK(again && again->revision == 1);
        check_recorded_as(repository, store, 1, manifest);
    }
}

// R-94: export and import report their progress, and a cancel stops each
// with nothing written. An export counts the manifest's bytes copied; one
// cancelled at its first piece leaves neither the manifest nor
// identity.json in the export's folder. An import counts the entries read,
// then the bytes copied; one cancelled after reading the entries, and one
// cancelled while it copies, record nothing and leave no copy in the
// repository. Not cancelled, the import records the revision.
void import_and_export_report_progress_and_stop_when_cancelled()
{
    test::EngineFixture source;
    for (int index = 0; index < 1001; ++index) {
        source.archive.add_file(std::format("sample-{:04}.txt", index), "sample");
    }
    const auto registered = source.register_root(source.archive.root());
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    const auto& layout = source.repository.context.repo;
    auto& files = *source.repository.files;
    auto& writer = *source.repository.writer;
    const auto uuid = registered->archive_uuid;

    va::CancelToken stop_export;
    test::CancelAfter cancel_export(stop_export, 1);
    const auto stopped = va::export_archive(source.store, files, writer, layout, uuid,
        out_directory(source, "export-cancelled"), stop_export, cancel_export);
    CHECK(!stopped && stopped.error().cancelled &&
          stopped.error().message == "the export was cancelled; nothing was written");
    CHECK(names_in(out_directory(source, "export-cancelled")).empty());

    test::RecordedProgress exporting;
    va::CancelToken not_cancelled;
    const auto exported = va::export_archive(source.store, files, writer, layout, uuid,
        out_directory(source, "export"), not_cancelled, exporting);
    CHECK(exported.has_value());
    if (!exported) {
        return;
    }
    const auto size = std::filesystem::file_size(exported->manifest);
    CHECK(!exporting.reports.empty() && exporting.reports.back().bytes_total == size &&
          exporting.reports.back().bytes_done == size);

    test::EngineFixture target;
    auto& store = target.store;
    const auto& target_layout = target.repository.context.repo;
    const auto import = [&](va::CancelToken& cancel, va::IProgressSink& progress) {
        return va::import_manifest(store, *target.repository.files, *target.repository.writer, target_layout,
            exported->manifest, false, cancel, progress);
    };
    const auto nothing_recorded = [&] {
        const auto known = store.revisions(uuid);
        CHECK(known && known->empty());
        CHECK(names_in(target_layout.manifests() / uuid).empty());
    };
    // 1001 entries read report at 1000 and at 1001; the copy then reports
    // each piece. Cancelled at 1000 entries it reads no further, and at the
    // copy's first piece it records nothing.
    for (const std::size_t at : {std::size_t{1}, std::size_t{3}}) {
        va::CancelToken stop;
        test::CancelAfter cancel_import(stop, at);
        const auto cancelled = import(stop, cancel_import);
        CHECK(!cancelled && cancelled.error().kind == va::ImportFailure::Kind::Cancelled &&
              cancelled.error().message == "the import was cancelled; nothing was imported");
        CHECK(cancel_import.seen() == at);
        nothing_recorded();
    }

    test::RecordedProgress importing;
    va::CancelToken go_on;
    const auto imported = import(go_on, importing);
    CHECK(imported && imported->revision == 1 && imported->new_archive);
    const auto entries_read = std::ranges::count_if(importing.reports, [](const va::Progress& report) {
        return report.items_total == 1001 && report.bytes_total == 0;
    });
    const auto copied = std::ranges::find_if(importing.reports, [&](const va::Progress& report) {
        return report.bytes_total == size && report.bytes_done == size;
    });
    CHECK(entries_read == 2 && copied != importing.reports.end());
}

} // namespace

void transfer_tests()
{
    export_import_round_trip();
    chunked_manifests_import_and_load();
    imports_of_one_revision_record_it_once(true);
    imports_of_one_revision_record_it_once(false);
    an_import_waits_for_the_record_of_another();
    an_import_leaves_a_registration_its_saved_files();
    an_import_removes_what_can_no_longer_finish();
    an_import_checks_the_revision_in_its_record();
    an_import_copies_the_bytes_it_validated();
    an_import_indexes_the_entries_it_validated();
    import_and_export_report_progress_and_stop_when_cancelled();
}
