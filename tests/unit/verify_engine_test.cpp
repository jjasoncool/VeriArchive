#include "test_support.h"

#include "engine_fixture.h"

#include "common/time.h"
#include "core/engine/archive_status.h"
#include "core/engine/compare_archives.h"
#include "core/engine/item_rules.h"
#include "core/engine/rebuild_index.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/transfer.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_writer.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

namespace {

using test::EngineFixture;

std::string status_of(const va::VerifyOutcome& outcome, const std::string& p)
{
    for (const auto& item : outcome.items) {
        if (item.path.p == p) {
            return item.status;
        }
    }
    return "Verified";
}

// A registered archive with a few files, ready to be verified.
struct Registered : EngineFixture {
    va::Uuid archive_uuid;

    explicit Registered(std::string os = "windows")
    {
        host.os = std::move(os);
        archive.add_file("docs/report.pdf", "report bytes");
        archive.add_file("photos/a.jpg", std::string(20000, 'a'));
        archive.add_file("notes.txt", "hello");
        archive.add_directory("empty");
        const auto outcome = register_root(archive.root());
        CHECK(outcome.has_value());
        if (outcome) {
            archive_uuid = outcome->archive_uuid;
        }
    }

    va::VerifyOutcome verify_ok()
    {
        auto outcome = verify(archive.root());
        CHECK(outcome.has_value());
        if (!outcome) {
            std::cerr << "verify stopped: " << outcome.error().message << '\n';
            std::abort();
        }
        return std::move(*outcome);
    }
};

class RecordingProgress final : public va::IProgressSink {
public:
    void on_progress(const va::Progress& progress) override { reports.push_back(progress); }
    std::vector<va::Progress> reports;
};

// Verification counts bytes, as registration does: hashing reports the bytes
// to read (the sizes just scanned), and the last report has read them all.
void progress_counts_bytes()
{
    Registered fixture;
    RecordingProgress progress;
    va::VerifyServices services{fixture.archive, *fixture.repository.files, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host};
    va::VerifyRequest request;
    request.root = fixture.archive.root();
    const auto outcome = va::verify_full(services, request, fixture.cancel, progress);
    CHECK(outcome && outcome->result == "PASSED");
    CHECK(!progress.reports.empty());
    if (progress.reports.empty()) {
        return;
    }
    const auto& last = progress.reports.back();
    CHECK(last.items_total == 3 && last.items_done == 3);
    CHECK(last.bytes_total == 12 + 20000 + 5);
    CHECK(last.bytes_done == last.bytes_total);
    for (const auto& report : progress.reports) {
        CHECK(report.bytes_done <= report.bytes_total);
    }
}

void unchanged_archive_passes()
{
    Registered fixture;
    const auto outcome = fixture.verify_ok();
    CHECK(outcome.result == "PASSED");
    CHECK(outcome.items.empty());
    CHECK(outcome.counts.at("Verified") == 4);
    CHECK(outcome.coverage_bytes == 12 + 20000 + 5);
    CHECK(outcome.coverage_bytes == outcome.registered_bytes);

    // The run is in the hash chain (R-18).
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1 && runs->front().result == "PASSED");
    const auto chain = fixture.store.check_history(fixture.repository.context.repository_uuid);
    CHECK(chain && !chain->first_broken_run);
}

// M3 fault matrix.
void content_changes_fail()
{
    Registered fixture;
    fixture.archive.node("notes.txt").content = "hellp";             // 1-byte change
    fixture.archive.remove("docs/report.pdf");                       // delete
    fixture.archive.add_file("added.txt", "new");                     // add
    fixture.archive.remove("empty");
    fixture.archive.add_file("empty", "now a file");                 // dir -> file
    fixture.archive.add_file("System Volume Information/x", "os");   // excluded, not reported

    const auto outcome = fixture.verify_ok();
    CHECK(outcome.result == "FAILED");
    CHECK(status_of(outcome, "notes.txt") == "Modified");
    CHECK(status_of(outcome, "docs/report.pdf") == "Missing");
    CHECK(status_of(outcome, "added.txt") == "New");
    CHECK(status_of(outcome, "empty") == "TypeChanged");
    CHECK(status_of(outcome, "System Volume Information/x") == "Verified");
    CHECK(status_of(outcome, "photos/a.jpg") == "Verified");
    CHECK(outcome.coverage_bytes == 20000); // only bytes that were read and matched
}

// Items come in canonical order (R-43), not in the order they were found: a
// missing file is found while merging, a modified one only after re-hashing.
void items_are_in_canonical_order()
{
    EngineFixture fixture;
    fixture.archive.add_file("a.txt", "a");
    fixture.archive.add_file("m/b.txt", "b");
    fixture.archive.add_file("z.txt", "z");
    CHECK(fixture.register_root(fixture.archive.root()).has_value());
    fixture.archive.node("a.txt").content = "A";                // Modified, found by re-hashing
    fixture.archive.node("m/b.txt").mtime_ns += 5'000'000'000;  // MetadataMismatch, found by re-hashing
    fixture.archive.remove("z.txt");                            // Missing, found while merging
    fixture.archive.add_file("n.txt", "n");                     // New, found while merging

    const auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome.has_value());
    if (!outcome) {
        return;
    }
    std::vector<std::string> order;
    for (const auto& item : outcome->items) {
        order.push_back(item.path.p + " " + item.status);
    }
    CHECK((order == std::vector<std::string>{
        "a.txt Modified", "m/b.txt MetadataMismatch", "n.txt New", "z.txt Missing"}));
}

void unreadable_files_fail_with_their_reason()
{
    Registered fixture;
    fixture.archive.node("notes.txt").open_error =
        va::IoError{va::IoErrorKind::PermissionDenied, 5, "denied"};
    fixture.archive.node("photos/a.jpg").read_error_offset = 4096;

    const auto outcome = fixture.verify_ok();
    CHECK(outcome.result == "FAILED");
    CHECK(status_of(outcome, "notes.txt") == "Unreadable(permission)");
    CHECK(status_of(outcome, "photos/a.jpg") == "Unreadable(media)");
}

// R-51: metadata is a warning unless --strict-metadata.
void metadata_changes_warn()
{
    Registered fixture;
    fixture.archive.node("notes.txt").mtime_ns += 5'000'000'000;
    auto outcome = fixture.verify_ok();
    CHECK(outcome.result == "PASSED_WITH_WARNINGS");
    CHECK(status_of(outcome, "notes.txt") == "MetadataMismatch");

    va::VerifyRequest strict;
    strict.root = fixture.archive.root();
    strict.strict_metadata = true;
    const auto strict_outcome = fixture.verify(strict);
    CHECK(strict_outcome && strict_outcome->result == "FAILED");
}

// Decision 0010.
void known_unreadable_entries_warn()
{
    EngineFixture fixture;
    fixture.archive.add_file("good.txt", "good");
    fixture.archive.add_file("bad.bin", std::string(5000, 'b')).read_error_offset = 10;
    CHECK(fixture.register_root(fixture.archive.root()).has_value());

    auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS");
    CHECK(outcome && status_of(*outcome, "bad.bin") == "Unreadable(media)");

    fixture.archive.node("bad.bin").read_error_offset.reset(); // sector remapped: readable now
    outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS");
    CHECK(outcome && status_of(*outcome, "bad.bin") == "Unbaselined");
}

// R-14 / R-62: a changed manifest byte fails the run before any content check.
void tampered_manifest_fails()
{
    Registered fixture;
    const auto revisions = fixture.store.revisions(fixture.archive_uuid);
    if (!revisions || revisions->empty()) {
        CHECK(false);
        return;
    }
    const auto path = fixture.repository.context.repo.root / revisions->front().manifest_relpath;
    std::string bytes;
    {
        std::ifstream input(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    bytes[bytes.size() - 5] ^= 0x01;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;

    const auto outcome = fixture.verify_ok();
    CHECK(outcome.result == "FAILED");
    CHECK(outcome.failure_classes == std::vector<std::string>{"ManifestHashMismatch"});
}

// §B10 outcomes that need an operator, and MATCH_WEAK.
void identity_outcomes()
{
    Registered fixture;
    fixture.media.volume.devices.front().serial = "OTHER-SERIAL";
    auto stopped = fixture.verify(fixture.archive.root());
    CHECK(!stopped && stopped.error().kind == va::VerifyStop::Kind::Decision);
    CHECK(!stopped && stopped.error().identity.result == va::MatchResult::Migrated);

    fixture.media.volume.filesystem_uuid = "9999999999999999";
    stopped = fixture.verify(fixture.archive.root());
    CHECK(!stopped && stopped.error().identity.result == va::MatchResult::Unknown);

    fixture.media.volume = test::fake_volume(fixture.archive.root());
    fixture.media.volume.devices.front().serial.clear();
    const auto weak = fixture.verify(fixture.archive.root());
    CHECK(weak && weak->result == "PASSED_WITH_WARNINGS");
    CHECK(weak && weak->identity.result == va::MatchResult::MatchWeak);
}

void verify_by_archive_uuid()
{
    Registered fixture;
    va::VerifyRequest request;
    request.archive_uuid = fixture.archive_uuid;
    const auto outcome = fixture.verify(request);
    CHECK(outcome && outcome->result == "PASSED");
}

// R-44: a name that cannot exist on Windows is not reported as Missing.
void unrepresentable_names_warn()
{
    EngineFixture fixture;
    fixture.host.os = "linux";
    fixture.archive.add_file("what?.txt", "linux allows question marks");
    fixture.archive.add_file("ok.txt", "ok");
    CHECK(fixture.register_root(fixture.archive.root()).has_value());

    fixture.host.os = "windows";
    fixture.archive.remove("what?.txt");
    const auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS");
    CHECK(outcome && status_of(*outcome, "what?.txt") == "Unrepresentable");
}

// Decision 0008: absolute targets may be translated across OSes.
void link_targets_across_systems()
{
    EngineFixture fixture;
    fixture.archive.add_link("absolute", "C:/Data");
    fixture.archive.add_link("relative", "../Data");
    CHECK(fixture.register_root(fixture.archive.root()).has_value());

    fixture.host.os = "linux";
    fixture.archive.node("absolute").link_target = "../../Data";
    auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS");
    CHECK(outcome && status_of(*outcome, "absolute") == "MetadataMismatch");

    fixture.archive.node("relative").link_target = "../Other";
    outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && status_of(*outcome, "relative") == "Modified");

    fixture.host.os = "windows";
    fixture.archive.node("relative").link_target = "../Data";
    outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && status_of(*outcome, "absolute") == "Modified"); // same OS: exact
}

// M4 / R-104: an interrupted run resumes without re-reading what it verified.
void interrupted_run_resumes_without_rehashing()
{
    EngineFixture fixture;
    for (int index = 1; index <= 9; ++index) {
        fixture.archive.add_file("f" + std::to_string(index), std::string(1000, static_cast<char>('0' + index)));
    }
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered.has_value());

    fixture.archive.node("f5").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.verify(fixture.archive.root());
    CHECK(interrupted && interrupted->result == "INCOMPLETE");
    if (!interrupted) {
        return;
    }

    fixture.archive.node("f5").open_error.reset();
    fixture.archive.bytes_read = 0;
    va::VerifyRequest request;
    request.root = fixture.archive.root();
    request.resume_run = interrupted->run_id;
    const auto resumed = fixture.verify(request);
    CHECK(resumed && resumed->result == "PASSED");
    CHECK(resumed && resumed->resumed_from == interrupted->run_id);
    CHECK(resumed && resumed->counts.at("Verified") == 9);
    CHECK(resumed && resumed->coverage_bytes == 9000);
    // Only what the interrupted run had not verified is read again.
    CHECK(interrupted->coverage_bytes >= 4000);
    CHECK(fixture.archive.bytes_read == 9000 - interrupted->coverage_bytes);

    // A finished run cannot be resumed again.
    const auto again = fixture.verify(request);
    CHECK(!again && again.error().kind == va::VerifyStop::Kind::NotFound &&
          again.error().message == "run " + interrupted->run_id + " is not an interrupted run");
}

void resume_refused_after_new_revision()
{
    EngineFixture fixture;
    fixture.archive.add_file("a", "a");
    fixture.archive.add_file("b", "b");
    CHECK(fixture.register_root(fixture.archive.root()).has_value());
    fixture.archive.node("b").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.verify(fixture.archive.root());
    CHECK(interrupted && interrupted->result == "INCOMPLETE");
    fixture.archive.node("b").open_error.reset();

    va::RegisterRequest rebaseline;
    rebaseline.root = fixture.archive.root();
    rebaseline.new_revision = true;
    CHECK(fixture.register_root(rebaseline).has_value());

    va::VerifyRequest request;
    request.root = fixture.archive.root();
    request.resume_run = interrupted ? interrupted->run_id : "";
    const auto resumed = fixture.verify(request);
    CHECK(!resumed && resumed.error().kind == va::VerifyStop::Kind::Decision);
}

// Reads the run's record whenever hashing reports progress, as the GUI's
// Archive List would read it from another process.
class RunWatcher final : public va::IProgressSink {
public:
    explicit RunWatcher(va::ArchiveStore& store) : store_(store) {}
    void on_progress(const va::Progress& progress) override
    {
        if (progress.items_total == 0) {
            return; // scanning
        }
        if (const auto runs = store_.settings_with_prefix("run:"); runs && runs->size() == 1) {
            records_seen.push_back(nlohmann::json::parse(runs->front().second, nullptr, false));
        }
    }
    std::vector<nlohmann::json> records_seen;

private:
    va::ArchiveStore& store_;
};

// Decision 0022: the run's record says where it reads, how much there is to
// read (the files with content hashes; one unreadable at registration has
// none and is never checkpointed) and when it last worked.
void run_record_says_where_and_how_much()
{
    EngineFixture fixture;
    fixture.archive.add_file("a.txt", "aaaa");
    fixture.archive.add_file("b.txt", "bbbbbb");
    fixture.archive.add_file("locked.txt", "secret").open_error =
        va::IoError{va::IoErrorKind::PermissionDenied, 5, "access denied"};
    CHECK(fixture.register_root(fixture.archive.root()).has_value());

    RunWatcher watcher(fixture.store);
    va::VerifyServices services{fixture.archive, *fixture.repository.files, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host};
    va::VerifyRequest request;
    request.root = fixture.archive.root();
    const auto outcome = va::verify_full(services, request, fixture.cancel, watcher);
    CHECK(outcome.has_value());
    CHECK(!watcher.records_seen.empty());
    for (const auto& record : watcher.records_seen) {
        CHECK(record.value("root", std::string{}) == va::path_to_utf8(fixture.archive.root()));
        CHECK(record.value("files_total", 0) == 2 && record.value("bytes_total", 0) == 10);
        CHECK(va::parse_timestamp(record.value("updated", std::string{})).has_value());
    }
    // Finished: the record is gone.
    const auto left = fixture.store.settings_with_prefix("run:");
    CHECK(left && left->empty());
}

// The mount point of a volume mounted in a folder ends with a separator on
// Windows ("C:\mnt\vol\"). An archive at that volume's root, verified by its
// uuid, is read at the folder without it, as the user interfaces make a typed
// root, and the run's record and outcome name that folder; the root of a
// volume at a drive letter keeps its own separator ("K:\", sample letter).
void archive_found_by_uuid_is_read_at_its_folder()
{
    Registered fixture;
    fixture.media.volume.mount_point = fixture.archive.root() / ""; // "/vol/"
    RunWatcher watcher(fixture.store);
    va::VerifyServices services{fixture.archive, *fixture.repository.files, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host};
    va::VerifyRequest request;
    request.archive_uuid = fixture.archive_uuid;
    const auto outcome = va::verify_full(services, request, fixture.cancel, watcher);
    const auto folder = va::path_to_utf8(fixture.archive.root());
    CHECK(outcome && outcome->result == "PASSED" && va::path_to_utf8(outcome->root) == folder);
    CHECK(!watcher.records_seen.empty());
    for (const auto& record : watcher.records_seen) {
        CHECK(record.value("root", std::string{}) == folder);
    }

    if (std::filesystem::path::preferred_separator == '\\') {
        EngineFixture drive(R"(K:\)");
        drive.archive.add_file("a.txt", "a (sample data)");
        const auto registered = drive.register_root(drive.archive.root());
        CHECK(registered.has_value());
        if (registered) {
            va::VerifyRequest by_uuid;
            by_uuid.archive_uuid = registered->archive_uuid;
            const auto verified = drive.verify(by_uuid);
            CHECK(verified && verified->result == "PASSED" && va::path_to_utf8(verified->root) == R"(K:\)");
        }
    }
}

// ---- The index holds a copy of each manifest's header, never in its place (R-14)

std::expected<std::vector<va::ArchiveStatus>, va::IoError> statuses_of(Registered& fixture)
{
    return va::archive_statuses(fixture.store, *fixture.repository.files, fixture.repository.context.repo,
        va::Policy{}, std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::string file_bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

// The rows of `table` in the fixture's database.
std::int64_t count_rows(EngineFixture& fixture, const std::string& table)
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

// The repository's files, except that the file named `name` cannot be opened
// (sample: access denied), as a manifest that this process may not read:
// every time, or only the `nth` time it is opened.
class DeniedOpen final : public va::IFileSystemAccess {
public:
    DeniedOpen(va::IFileSystemAccess& inner, std::string name, int nth = 0)
        : inner_(inner), name_(std::move(name)), nth_(nth)
    {
    }
    std::expected<std::vector<va::DirEntry>, va::IoError> list(const std::filesystem::path& directory) override
    {
        return inner_.list(directory);
    }
    std::expected<std::unique_ptr<va::IByteSource>, va::IoError> open(
        const std::filesystem::path& file, va::ReadOptions options) override
    {
        if (va::path_to_utf8(file.filename()) == name_ && (++opened_ == nth_ || nth_ == 0)) {
            return std::unexpected(va::IoError{va::IoErrorKind::PermissionDenied, 5, "access denied (sample)"});
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
};

// Decision 0028: the plain message of the manifest of revision 1 of
// `archive_uuid` that cannot be used: what is wrong with it, naming it by its
// place in the repository, then how to fix it.
std::string restore_message(const va::Uuid& archive_uuid, const std::string& what)
{
    return "the manifest of revision 1 (manifests/" + archive_uuid + "/r0001.vam) " + what +
           "; restore it from a backup of the repository";
}

// What is wrong when the index differs from the manifest at the place of
// revision 1's, `revision` being the revision as the index has it.
std::string index_differs(const va::Uuid& archive_uuid, std::uint32_t revision = 1)
{
    return "the repository's index differs from the manifest of revision " + std::to_string(revision) +
           " (manifests/" + archive_uuid + "/r0001.vam)";
}

std::string rebuild_message(const va::Uuid& archive_uuid, std::uint32_t revision = 1)
{
    return index_differs(archive_uuid, revision) + "; rebuild the index with `repo rebuild-index`";
}

constexpr const char* first_line_changed = "has changed: its first line differs from what the repository recorded";

// Decision 0028: how a verification by root stops when the latest manifest of
// archive `label` (`archive_uuid`) cannot be used, `what` being wrong with it:
// which archive, why, and to verify it by its uuid, then fix its manifest.
std::string root_may_belong_to(const std::string& label, const va::Uuid& archive_uuid, const std::string& what,
    bool rebuild = false)
{
    return "this root may belong to archive " + label + " (" + archive_uuid +
           "), whose manifest cannot be used: " + what + "; verify that archive by its uuid (`verify --full " +
           archive_uuid + "`) to record the failure, then " +
           (rebuild ? "rebuild the index with `repo rebuild-index`"
                    : "restore its manifest from a backup of the repository");
}

std::string manifest_of(const va::Uuid& archive_uuid)
{
    return "the manifest of revision 1 (manifests/" + archive_uuid + "/r0001.vam)";
}

// CLAUDE.md rule 3: validated manifest bytes are the truth, the database is
// an index. Here the manifest of revision 1 is the one recorded (its line 1
// has the BLAKE3 recorded for it, and its body is the one that line names),
// and the index says otherwise of it (sample data): its copy of the header
// names another volume label, or the record names another body, or another
// revision (2, as the index then has the archive) than that line names.
// Neither header is taken, and as the manifest is intact, the fix is to
// rebuild the index (decision 0028). Named by its uuid, the archive is known,
// and its verification FAILS with a RepositoryError whose message says to
// rebuild the index, recorded without reading the media; by root it stops,
// naming the archive and saying to verify it by its uuid, then to rebuild the
// index; a new revision of the archive and loading the manifest stop with
// the plain message, and the archive list lists the archive with it. Rebuilt
// from the manifests, the index holds the manifest's record again, and the
// archive verifies.
void an_index_that_differs_from_its_manifest_is_never_taken()
{
    enum class Case { CopyDiffers, RecordNamesAnotherBody, RecordNamesAnotherRevision };
    for (const auto what : {Case::CopyDiffers, Case::RecordNamesAnotherBody, Case::RecordNamesAnotherRevision}) {
        Registered fixture;
        std::uint32_t revision = 1; // as the index has it
        if (what == Case::CopyDiffers) {
            fixture.repository.execute("UPDATE archive_revisions SET header_json = replace(header_json, "
                                       "'\"volume_label\":\"ARCHIVE\"', '\"volume_label\":\"OTHER\"')");
            const auto record = fixture.store.revision(fixture.archive_uuid, 1);
            CHECK(record && *record && (*record)->header_json.contains("\"volume_label\":\"OTHER\""));
        } else if (what == Case::RecordNamesAnotherBody) {
            fixture.repository.execute("UPDATE archive_revisions SET body_blake3 = "
                                       "'0000000000000000000000000000000000000000000000000000000000000000'");
        } else {
            revision = 2;
            fixture.repository.execute("UPDATE archive_revisions SET revision = 2");
            fixture.repository.execute("UPDATE archives SET current_revision = 2");
        }
        const auto message = rebuild_message(fixture.archive_uuid, revision);

        const auto by_root = fixture.verify(fixture.archive.root());
        CHECK(!by_root && by_root.error().kind == va::VerifyStop::Kind::Repository &&
              by_root.error().message ==
                  root_may_belong_to("ARCH-0001", fixture.archive_uuid, index_differs(fixture.archive_uuid, revision),
                      true));
        if (!by_root) {
            std::cerr << "  by root: " << by_root.error().message << '\n';
        }
        va::VerifyRequest by_uuid;
        by_uuid.archive_uuid = fixture.archive_uuid;
        const auto named = fixture.verify(by_uuid);
        CHECK(named && named->result == "FAILED" && !named->media_read &&
              named->failure_classes == std::vector<std::string>{"RepositoryError"} &&
              named->warnings == std::vector<std::string>{"manifest: " + message});
        const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
        CHECK(runs && runs->size() == 1 && runs->front().result == "FAILED" &&
              runs->front().failure_classes == "RepositoryError");

        va::RegisterRequest next;
        next.root = fixture.archive.root();
        next.new_revision = true;
        const auto registered = fixture.register_root(next);
        CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Repository &&
              registered.error().message == message);

        // Listed with the problem, not stopping the list (decision 0028).
        const auto statuses = statuses_of(fixture);
        CHECK(statuses && statuses->size() == 1 && !statuses->front().header &&
              statuses->front().unusable_manifest &&
              statuses->front().unusable_manifest->kind == va::UnusableManifest::Kind::IndexDiffers &&
              va::plain_message(*statuses->front().unusable_manifest) == message);
        const auto loaded = va::load_recorded_manifest(fixture.store, *fixture.repository.files,
            fixture.repository.context.repo, fixture.archive_uuid, revision);
        CHECK(!loaded && loaded.error().failure_class == "RepositoryError" && loaded.error().message == message &&
              loaded.error().unusable && loaded.error().unusable->kind == va::UnusableManifest::Kind::IndexDiffers);

        const auto rebuilt =
            va::rebuild_index(fixture.store, *fixture.repository.files, fixture.repository.context.repo);
        CHECK(rebuilt && rebuilt->revisions == 1 && rebuilt->skipped.empty());
        const auto verified = fixture.verify(fixture.archive.root());
        CHECK(verified && verified->result == "PASSED" && verified->revision == 1);
        const auto listed = statuses_of(fixture);
        CHECK(listed && listed->size() == 1 && listed->front().header && !listed->front().unusable_manifest &&
              listed->front().header->identity.volume_label == "ARCHIVE");
    }
}

// The manifest is not the one recorded: one byte of its header line changed
// (sample data), or it is gone. The record no longer vouches for the
// manifest, and the index's copy of its header is no manifest. Verification
// by root stops (decision 0028): the root may be this archive's, whose
// carrier only that manifest records. It names the archive and what is
// wrong, and says to verify it by its uuid to record the failure, then to
// restore its manifest from a backup of the repository, never to rebuild the
// index, which would take a changed manifest for the truth. A new revision
// stops with the plain message, and the archive list lists the archive with
// it. Nothing is recorded.
void a_manifest_that_is_not_the_one_recorded_stops_every_use_by_root()
{
    for (const bool gone : {false, true}) {
        Registered fixture;
        const auto path = va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 1);
        if (gone) {
            std::filesystem::remove(path);
        } else {
            auto bytes = file_bytes(path);
            bytes[bytes.find("\"volume_label\":\"ARCHIVE\"") + 16] ^= 0x01; // "ARCHIVE" -> "@RCHIVE"
            write_bytes(path, bytes);
        }
        const std::string wrong = gone ? "is missing" : first_line_changed;
        const auto message = restore_message(fixture.archive_uuid, wrong);
        const auto stopped = root_may_belong_to("ARCH-0001", fixture.archive_uuid,
            manifest_of(fixture.archive_uuid) + " " + wrong);
        const auto verified = fixture.verify(fixture.archive.root());
        CHECK(!verified && verified.error().kind == va::VerifyStop::Kind::Repository &&
              verified.error().message == stopped);
        if (!verified && verified.error().message != stopped) {
            std::cerr << "  verify: " << verified.error().message << '\n';
        }
        // Listed with the problem, not stopping the list (decision 0028).
        const auto statuses = statuses_of(fixture);
        CHECK(statuses && statuses->size() == 1 && !statuses->front().header &&
              statuses->front().unusable_manifest &&
              va::plain_message(*statuses->front().unusable_manifest) == message);
        va::RegisterRequest next;
        next.root = fixture.archive.root();
        next.new_revision = true;
        const auto registered = fixture.register_root(next);
        CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Repository &&
              registered.error().message == message);
        const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
        CHECK(runs && runs->empty());
    }
}

// Decision 0028: an archive named by its uuid is known before anything is
// looked for on the media, and so is the manifest of its current revision.
// When that manifest cannot be used, the verification FAILS at once, without
// reading the media (here the fake media cannot even be listed, and no byte
// of the archive is read) and without media evidence: a manifest missing or
// unreadable fails with RepositoryError, one whose header line or body has
// changed with ManifestHashMismatch (sample data: one byte each). The failure
// names the manifest and says to restore it from a backup of the repository.
// The run is recorded as FULL FAILED of the archive's revision, with nothing
// covered and nothing to resume, in an intact hash chain. With the manifest
// restored, the archive verifies.
void a_known_archive_whose_manifest_cannot_be_used_fails_unread()
{
    enum class Case { Missing, Unreadable, HeaderChanged, BodyChanged };
    for (const auto what : {Case::Missing, Case::Unreadable, Case::HeaderChanged, Case::BodyChanged}) {
        Registered fixture;
        const auto path = va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 1);
        const auto kept = file_bytes(path);
        auto changed = kept;
        std::string failure_class = "RepositoryError";
        std::string message;
        if (what == Case::Missing) {
            std::filesystem::remove(path);
            message = restore_message(fixture.archive_uuid, "is missing");
        } else if (what == Case::Unreadable) {
            message = restore_message(fixture.archive_uuid, "cannot be read: access denied (sample)");
        } else if (what == Case::HeaderChanged) {
            changed[changed.find("\"volume_label\":\"ARCHIVE\"") + 16] ^= 0x01; // "ARCHIVE" -> "@RCHIVE"
            failure_class = "ManifestHashMismatch";
            message = restore_message(fixture.archive_uuid, first_line_changed);
        } else {
            changed[changed.size() - 5] ^= 0x01;
            failure_class = "ManifestHashMismatch";
            message = restore_message(fixture.archive_uuid, "has changed: its body does not match its header");
        }
        if (changed != kept) {
            write_bytes(path, changed);
        }
        fixture.media.error = va::IoError{va::IoErrorKind::Other, 0, "the media is not looked for (sample)"};
        fixture.archive.bytes_read = 0;
        const auto evidence_before = count_rows(fixture, "media_evidence");

        DeniedOpen denied(*fixture.repository.files, "r0001.vam");
        va::IFileSystemAccess& repository_files =
            what == Case::Unreadable ? static_cast<va::IFileSystemAccess&>(denied) : *fixture.repository.files;
        va::VerifyServices services{fixture.archive, repository_files, fixture.media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host};
        va::VerifyRequest by_uuid;
        by_uuid.archive_uuid = fixture.archive_uuid;
        const auto outcome = va::verify_full(services, by_uuid, fixture.cancel, fixture.progress);
        CHECK(outcome && outcome->result == "FAILED" && !outcome->media_read &&
              outcome->archive_uuid == fixture.archive_uuid && outcome->revision == 1 &&
              outcome->failure_classes == std::vector<std::string>{failure_class} &&
              outcome->warnings == std::vector<std::string>{"manifest: " + message} && outcome->items.empty() &&
              outcome->coverage_bytes == 0);
        if (!outcome) {
            std::cerr << "  by uuid stopped: " << outcome.error().message << '\n';
        } else if (outcome->warnings != std::vector<std::string>{"manifest: " + message}) {
            std::cerr << "  warnings: " << (outcome->warnings.empty() ? "" : outcome->warnings.front()) << '\n';
        }
        CHECK(fixture.archive.bytes_read == 0);
        CHECK(count_rows(fixture, "media_evidence") == evidence_before);
        const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
        CHECK(runs && runs->size() == 1);
        if (outcome && runs && runs->size() == 1) {
            const auto& run = runs->front();
            CHECK(run.run_id == outcome->run_id && run.mode == "FULL" && run.result == "FAILED" &&
                  run.revision == 1 && run.coverage_bytes == 0u && run.failure_classes == failure_class);
        }
        const auto chain = fixture.store.check_history(fixture.repository.context.repository_uuid);
        CHECK(chain && chain->runs == 1 && !chain->first_broken_run);
        const auto resumable = fixture.store.settings_with_prefix("run:");
        CHECK(resumable && resumable->empty());

        write_bytes(path, kept);
        fixture.media.error.reset();
        const auto restored = fixture.verify(by_uuid);
        CHECK(restored && restored->result == "PASSED" && restored->media_read);
    }
}

// Whether the run `run_id` is listed with the unfinished operations, as the
// Archive List reads them (decision 0022): stopped, and one to resume.
bool listed_to_resume(EngineFixture& fixture, const std::string& run_id)
{
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto runs = fixture.store.unfinished_verifications(now);
    return runs && std::ranges::any_of(*runs, [&](const va::UnfinishedVerification& run) {
        return run.run_id == run_id && run.recorded_incomplete && run.resumable;
    });
}

// A resumed run (`verify --resume`) names its archive too. When the manifest
// of the archive's revision cannot be used (here it is missing), the resume
// FAILS at once, recorded as above, its counts saying that it read no media,
// and leaves the interrupted run as it was: its record and its checkpoints
// stay, and the Archive List still lists it as one to resume, which a
// verification that read no media never supersedes; so does a verification
// of the archive by its uuid that fails the same way. With the manifest
// restored, the run resumes without reading again what it verified.
void a_resume_whose_manifest_cannot_be_used_fails_and_keeps_the_run()
{
    EngineFixture fixture;
    for (int index = 1; index <= 9; ++index) {
        fixture.archive.add_file("f" + std::to_string(index), std::string(1000, static_cast<char>('0' + index)));
    }
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered.has_value());
    fixture.archive.node("f5").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.verify(fixture.archive.root());
    CHECK(interrupted && interrupted->result == "INCOMPLETE");
    if (!registered || !interrupted) {
        return;
    }
    fixture.archive.node("f5").open_error.reset();
    const auto checkpoints = fixture.store.progress(interrupted->run_id);
    CHECK(checkpoints && !checkpoints->empty());
    CHECK(listed_to_resume(fixture, interrupted->run_id));

    const auto path = va::manifest_path(fixture.repository.context.repo.manifests(), registered->archive_uuid, 1);
    const auto kept = file_bytes(path);
    std::filesystem::remove(path);
    fixture.archive.bytes_read = 0;
    va::VerifyRequest request;
    request.root = fixture.archive.root();
    request.resume_run = interrupted->run_id;
    const auto failed = fixture.verify(request);
    CHECK(failed && failed->result == "FAILED" && !failed->media_read && failed->resumed_from.empty() &&
          failed->failure_classes == std::vector<std::string>{"RepositoryError"} &&
          failed->warnings ==
              std::vector<std::string>{"manifest: " + restore_message(registered->archive_uuid, "is missing")});
    CHECK(fixture.archive.bytes_read == 0);
    const auto record = fixture.store.setting("run:" + interrupted->run_id);
    CHECK(record && !record->empty());
    const auto still = fixture.store.progress(interrupted->run_id);
    CHECK(still && checkpoints && *still == *checkpoints);
    const auto recorded = fixture.store.verify_runs(registered->archive_uuid);
    CHECK(recorded && recorded->size() == 2 &&
          nlohmann::json::parse(recorded->back().counts_json, nullptr, false).value("media_read", true) == false);
    CHECK(listed_to_resume(fixture, interrupted->run_id));

    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = registered->archive_uuid;
    const auto named = fixture.verify(by_uuid);
    CHECK(named && named->result == "FAILED" && !named->media_read);
    CHECK(listed_to_resume(fixture, interrupted->run_id));

    write_bytes(path, kept);
    CHECK(listed_to_resume(fixture, interrupted->run_id));
    const auto resumed = fixture.verify(request);
    CHECK(resumed && resumed->result == "PASSED" && resumed->resumed_from == interrupted->run_id);
    CHECK(fixture.archive.bytes_read == 9000 - interrupted->coverage_bytes);
    const auto runs = fixture.store.verify_runs(registered->archive_uuid);
    CHECK(runs && runs->size() == 4);
    CHECK(!listed_to_resume(fixture, interrupted->run_id));
}

// A named archive is never "the archive that may be at this root" (decision
// 0028): when its manifest passes the check made before the media is looked
// for, and then cannot be opened as its first line is read for matching (the
// second opening, sample data), the verification stops with the plain message
// alone.
void a_named_archive_is_never_the_one_that_may_be_at_a_root()
{
    Registered fixture;
    DeniedOpen second(*fixture.repository.files, "r0001.vam", 2);
    va::VerifyServices services{fixture.archive, second, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host};
    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = fixture.archive_uuid;
    const auto stopped = va::verify_full(services, by_uuid, fixture.cancel, fixture.progress);
    CHECK(!stopped && stopped.error().kind == va::VerifyStop::Kind::Repository &&
          stopped.error().message ==
              restore_message(fixture.archive_uuid, "cannot be read: access denied (sample)"));
}

// Two archives on one volume (sample data): A at /vol/a and B at /vol/b. A's
// manifest is gone, or one byte of its header line changed. Named by its
// uuid, B is the one candidate and no other archive's manifest is read: it
// verifies, and its status reads with its header, as `archive show` reads it,
// and every archive's label without one, as `history` and the GUI's archive
// choices read them. A named by its uuid FAILS on its manifest, recorded
// (decision 0028). By root, every archive's latest carrier is needed (A's
// root, which only its manifest names, may be this one): B's verification by
// root stops, naming A, what is wrong with its manifest, and to verify A by
// its uuid, and records nothing.
void a_named_archive_reads_no_other_manifest()
{
    for (const bool gone : {true, false}) {
        EngineFixture fixture;
        fixture.archive.add_file("a/one.txt", "a (sample data)");
        fixture.archive.add_file("b/two.txt", "b (sample data)");
        const auto a = fixture.register_root(fixture.archive.root() / "a");
        const auto b = fixture.register_root(fixture.archive.root() / "b");
        CHECK(a && b);
        if (!a || !b) {
            return;
        }
        const auto path = va::manifest_path(fixture.repository.context.repo.manifests(), a->archive_uuid, 1);
        if (gone) {
            std::filesystem::remove(path);
        } else {
            auto bytes = file_bytes(path);
            bytes[bytes.find("\"volume_label\":\"ARCHIVE\"") + 16] ^= 0x01; // "ARCHIVE" -> "@RCHIVE"
            write_bytes(path, bytes);
        }

        va::VerifyRequest b_by_uuid;
        b_by_uuid.archive_uuid = b->archive_uuid;
        const auto named = fixture.verify(b_by_uuid);
        CHECK(named && named->archive_uuid == b->archive_uuid && named->result == "PASSED");
        if (!named) {
            std::cerr << "  B by uuid: " << named.error().message << '\n';
        }
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto shown = va::archive_statuses(fixture.store, *fixture.repository.files,
            fixture.repository.context.repo, va::Policy{}, now, {b->archive_uuid, true});
        CHECK(shown && shown->size() == 1 && shown->front().header &&
              shown->front().header->archive_uuid == b->archive_uuid);
        const auto labels = va::archive_statuses(fixture.store, *fixture.repository.files,
            fixture.repository.context.repo, va::Policy{}, now, {std::nullopt, false});
        CHECK(labels && labels->size() == 2 &&
              std::ranges::none_of(*labels, [](const va::ArchiveStatus& status) { return status.header.has_value(); }));

        const std::string what = gone ? "is missing" : first_line_changed;
        const auto message = restore_message(a->archive_uuid, what);
        va::VerifyRequest a_by_uuid;
        a_by_uuid.archive_uuid = a->archive_uuid;
        const auto own = fixture.verify(a_by_uuid);
        CHECK(own && own->archive_uuid == a->archive_uuid && own->result == "FAILED" && !own->media_read &&
              own->warnings == std::vector<std::string>{"manifest: " + message});
        const auto b_by_root = fixture.verify(fixture.archive.root() / "b");
        CHECK(!b_by_root && b_by_root.error().kind == va::VerifyStop::Kind::Repository &&
              b_by_root.error().message ==
                  root_may_belong_to(a->label, a->archive_uuid, manifest_of(a->archive_uuid) + " " + what));
        const auto a_runs = fixture.store.verify_runs(a->archive_uuid);
        const auto b_runs = fixture.store.verify_runs(b->archive_uuid);
        CHECK(a_runs && a_runs->size() == 1 && a_runs->front().result == "FAILED" && b_runs && b_runs->size() == 1);
    }
}

// Decision 0028: line 1 of a recorded manifest is compared with the BLAKE3
// recorded for it before it is parsed. A header changed into text that no
// longer parses (sample data: its opening brace made a bracket), one cut off
// before its newline, and an empty manifest are changed manifests: wherever
// a whole manifest is loaded (verification, comparison, export) they fail
// with ManifestHashMismatch, never as a malformed manifest or with a
// RepositoryError, and so does a verification of the archive by its uuid,
// which is recorded; line 1 alone (the archive lists, identity matching)
// says that its first line differs from what the repository recorded, not
// why it does not parse.
void a_first_line_that_no_longer_parses_has_changed()
{
    enum class Case { Unparseable, CutOff, Empty };
    for (const auto what : {Case::Unparseable, Case::CutOff, Case::Empty}) {
        Registered fixture;
        const auto& layout = fixture.repository.context.repo;
        const auto path = va::manifest_path(layout.manifests(), fixture.archive_uuid, 1);
        auto bytes = file_bytes(path);
        if (what == Case::Unparseable) {
            bytes[0] = '[';
        } else if (what == Case::CutOff) {
            bytes.resize(bytes.find('\n') / 2);
        } else {
            bytes.clear();
        }
        write_bytes(path, bytes);
        const auto message = restore_message(fixture.archive_uuid, first_line_changed);

        const auto loaded =
            va::load_recorded_manifest(fixture.store, *fixture.repository.files, layout, fixture.archive_uuid, 1);
        CHECK(!loaded && loaded.error().failure_class == "ManifestHashMismatch" && loaded.error().message == message);
        if (!loaded && loaded.error().failure_class != "ManifestHashMismatch") {
            std::cerr << "  loaded: " << loaded.error().failure_class << ": " << loaded.error().message << '\n';
        }
        const auto compared = va::compare_archives(fixture.store, *fixture.repository.files, layout,
            fixture.archive_uuid, fixture.archive_uuid, fixture.cancel, fixture.progress);
        CHECK(!compared && compared.error().failure_class == "ManifestHashMismatch");
        const auto exported = va::export_archive(fixture.store, *fixture.repository.files,
            *fixture.repository.writer, layout, fixture.archive_uuid, layout.root.parent_path() / "export");
        CHECK(!exported && exported.error() == "ManifestHashMismatch: " + message);
        va::VerifyRequest by_uuid;
        by_uuid.archive_uuid = fixture.archive_uuid;
        const auto verified = fixture.verify(by_uuid);
        CHECK(verified && verified->result == "FAILED" &&
              verified->failure_classes == std::vector<std::string>{"ManifestHashMismatch"});

        const auto revision = fixture.store.revision(fixture.archive_uuid, 1);
        CHECK(revision && *revision);
        if (revision && *revision) {
            const auto header = va::load_recorded_header(*fixture.repository.files, layout, **revision);
            CHECK(!header && va::plain_message(header.error()) == message &&
                  va::failure_class_of(header.error()) == "ManifestHashMismatch");
        }
    }
}

// R-14: the archive is compared with the entries of the manifest validated.
// They come from a second read of the recorded manifest; when that read gives
// other bytes (sample data: one hex digit of the first entry's BLAKE3), the
// verification fails with ManifestHashMismatch, recorded in the history,
// rather than compare the archive with entries the manifest does not hold.
void entries_read_otherwise_than_validated_fail()
{
    Registered fixture;
    const auto path = va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 1);
    std::string bytes;
    {
        std::ifstream input(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    const auto b3 = va::blake3_hex("report bytes"); // the first entry, docs/report.pdf
    auto other = b3;
    other[0] = other[0] == '0' ? '1' : '0';
    // Its first opening reads line 1 alone (the candidates); the second loads it.
    test::SecondReadDiffers repository_files(*fixture.repository.files, "r0001.vam", 2, bytes.find('\n') + 1,
        "\"b3\":\"" + b3, "\"b3\":\"" + other);
    va::VerifyServices services{fixture.archive, repository_files, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host};
    va::VerifyRequest request;
    request.root = fixture.archive.root();
    const auto outcome = va::verify_full(services, request, fixture.cancel, fixture.progress);
    CHECK(repository_files.differed());
    CHECK(outcome && outcome->result == "FAILED" &&
          outcome->failure_classes == std::vector<std::string>{"ManifestHashMismatch"} && outcome->items.empty());
    // By root, the media was looked at to find the archive (decision 0028).
    CHECK(outcome && outcome->media_read &&
          outcome->warnings == std::vector<std::string>{"manifest: " + restore_message(fixture.archive_uuid,
                                                                       "has changed: it gave other bytes when it "
                                                                       "was read again")});
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1 && runs->front().failure_classes == "ManifestHashMismatch");
}

void item_rules()
{
    CHECK(va::mtime_matches("ntfs", 1'000'000'000, 1'900'000'000));
    CHECK(!va::mtime_matches("ntfs", 1'000'000'000, 2'100'000'000));
    CHECK(va::mtime_matches("vfat", 0, 999'000'000'000));
    CHECK(va::mtime_matches("exfat", 0, 8'000'000));
    CHECK(va::mtime_matches("exfat", 0, 8LL * 3600 * 1'000'000'000)); // UTC+8 shift
    CHECK(!va::mtime_matches("exfat", 0, 7'000'000'000));
    CHECK(va::representable_on_windows("Projects/2026/report.pdf"));
    CHECK(!va::representable_on_windows("a/NUL.txt"));
    CHECK(!va::representable_on_windows("trailing./x"));
    CHECK(!va::representable_on_windows("question?"));
    CHECK(va::representable_on_windows("COM10"));
}

} // namespace

// Decision 0018: a filesystem id that can no longer be read (Linux without
// the udev database) on the same device and size is a weak match: the content
// is verified and the result says why it is only PASSED_WITH_WARNINGS.
void unreadable_filesystem_id_warns()
{
    Registered fixture;
    fixture.media.volume.filesystem_uuid.clear();
    fixture.media.volume.filesystem_uuid_short.clear();
    const auto outcome = fixture.verify_ok();
    CHECK(outcome.result == "PASSED_WITH_WARNINGS");
    CHECK(outcome.identity.result == va::MatchResult::MatchWeak);
    CHECK(std::ranges::count(outcome.warnings,
              "identity: the filesystem id cannot be read; the volume was recognised by its device (MATCH_WEAK)") == 1);
    CHECK(std::ranges::count(outcome.warnings, "identity: filesystem-id-unavailable") == 0);
}

void verify_engine_tests()
{
    unchanged_archive_passes();
    progress_counts_bytes();
    content_changes_fail();
    items_are_in_canonical_order();
    unreadable_files_fail_with_their_reason();
    metadata_changes_warn();
    known_unreadable_entries_warn();
    tampered_manifest_fails();
    identity_outcomes();
    unreadable_filesystem_id_warns();
    verify_by_archive_uuid();
    unrepresentable_names_warn();
    link_targets_across_systems();
    interrupted_run_resumes_without_rehashing();
    resume_refused_after_new_revision();
    run_record_says_where_and_how_much();
    archive_found_by_uuid_is_read_at_its_folder();
    an_index_that_differs_from_its_manifest_is_never_taken();
    a_manifest_that_is_not_the_one_recorded_stops_every_use_by_root();
    a_known_archive_whose_manifest_cannot_be_used_fails_unread();
    a_resume_whose_manifest_cannot_be_used_fails_and_keeps_the_run();
    a_named_archive_is_never_the_one_that_may_be_at_a_root();
    a_named_archive_reads_no_other_manifest();
    a_first_line_that_no_longer_parses_has_changed();
    entries_read_otherwise_than_validated_fail();
    item_rules();
}
