#include "test_support.h"

#include "engine_fixture.h"

#include "core/engine/item_rules.h"

#include <algorithm>
#include <fstream>

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
    CHECK(!again && again.error().kind == va::VerifyStop::Kind::NotFound);
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
    item_rules();
}
