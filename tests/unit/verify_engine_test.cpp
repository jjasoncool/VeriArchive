#include "test_support.h"

#include "engine_fixture.h"
#include "manifest_samples.h"
#include "sparse_file.h"

#include "app/typed_path.h"
#include "common/time.h"
#include "core/engine/archive_status.h"
#include "core/engine/compare_archives.h"
#include "core/engine/comparison.h"
#include "core/engine/item_rules.h"
#include "core/engine/rebuild_index.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/transfer.h"
#include "core/engine/volume_watch.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_writer.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

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
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        *fixture.repository.writer, fixture.processes};
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

// R-50, as archive comparison tells it (decision 0020): a file or a link
// replaced by a folder with contents, and a folder with contents replaced by a
// file or a link, changed type at that path. A folder with contents is no entry
// (R-49), so it is told from what lies in it: the path is TypeChanged, beside
// the folder's entries, New where the folder is new and Missing where it went,
// and the run fails. An empty folder stays the same folder once it holds
// files. Sample data.
void a_type_change_to_or_from_a_folder_with_contents_is_reported()
{
    EngineFixture fixture;
    fixture.archive.add_file("SAMPLE-file-to-folder.txt", "file");
    fixture.archive.add_file("SAMPLE-folder-to-file/inside.txt", "inside");
    fixture.archive.add_file("SAMPLE-folder-to-link/inside.txt", "inside");
    fixture.archive.add_link("SAMPLE-link-to-folder", "SAMPLE-folder-to-file");
    fixture.archive.add_directory("SAMPLE-empty-folder");
    CHECK(fixture.register_root(fixture.archive.root()).has_value());

    fixture.archive.remove("SAMPLE-file-to-folder.txt");
    fixture.archive.add_file("SAMPLE-file-to-folder.txt/new-inside.txt", "new");
    fixture.archive.remove("SAMPLE-folder-to-file");
    fixture.archive.add_file("SAMPLE-folder-to-file", "now a file");
    fixture.archive.remove("SAMPLE-folder-to-link");
    fixture.archive.add_link("SAMPLE-folder-to-link", "elsewhere");
    fixture.archive.remove("SAMPLE-link-to-folder");
    fixture.archive.add_file("SAMPLE-link-to-folder/inside.txt", "inside");
    fixture.archive.add_file("SAMPLE-empty-folder/new.txt", "new");

    const auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "FAILED");
    if (!outcome) {
        return;
    }
    std::vector<std::string> items;
    for (const auto& item : outcome->items) {
        items.push_back(item.path.p + " " + item.status + (item.detail.empty() ? "" : " (" + item.detail + ")"));
    }
    CHECK((items == std::vector<std::string>{
        "SAMPLE-empty-folder/new.txt New",
        "SAMPLE-file-to-folder.txt TypeChanged (f -> d)",
        "SAMPLE-file-to-folder.txt/new-inside.txt New",
        "SAMPLE-folder-to-file TypeChanged (d -> f)",
        "SAMPLE-folder-to-file/inside.txt Missing",
        "SAMPLE-folder-to-link TypeChanged (d -> l)",
        "SAMPLE-folder-to-link/inside.txt Missing",
        "SAMPLE-link-to-folder TypeChanged (l -> d)",
        "SAMPLE-link-to-folder/inside.txt New",
    }));
    if (outcome->items.size() != 9) {
        for (const auto& item : items) {
            std::cerr << "  " << item << '\n';
        }
    }
}

// R-30, §A6: a mount point below the root is never descended, and every
// verification reports it (SkippedMountPoint). One that was there at
// registration holds nothing the archive recorded: its item says so and the
// run stays PASSED, as every registered byte was read. A volume mounted later
// over a folder with recorded files, or over a recorded empty folder, hides
// recorded content: each such entry is SkippedMountPoint and the run fails.
// Sample data.
void mount_points_below_the_root_are_reported()
{
    EngineFixture fixture;
    fixture.archive.add_file("SAMPLE-a.txt", "a");
    fixture.archive.add_file("SAMPLE-mounted/SAMPLE-other-volume.txt", "another volume's");
    fixture.archive.node("SAMPLE-mounted").mount_point = true;
    fixture.archive.add_file("SAMPLE-photos/p.jpg", "photo");
    fixture.archive.add_directory("SAMPLE-empty");
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered && registered->skipped_mount_points.size() == 1 &&
          registered->skipped_mount_points.front().p == "SAMPLE-mounted" && registered->counts.files == 2);

    auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "PASSED" && outcome->items.size() == 1 &&
          outcome->counts.at("SkippedMountPoint") == 1 && outcome->counts.at("Verified") == 3);
    CHECK(outcome && !outcome->items.empty() && outcome->items.front().path.p == "SAMPLE-mounted" &&
          outcome->items.front().status == "SkippedMountPoint" &&
          outcome->items.front().detail == "another volume is mounted here; it is not part of the archive");

    fixture.archive.node("SAMPLE-photos").mount_point = true;
    fixture.archive.node("SAMPLE-empty").mount_point = true;
    outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "FAILED" && outcome->items.size() == 3);
    CHECK(outcome && status_of(*outcome, "SAMPLE-empty") == "SkippedMountPoint" &&
          status_of(*outcome, "SAMPLE-photos/p.jpg") == "SkippedMountPoint" &&
          status_of(*outcome, "SAMPLE-mounted") == "SkippedMountPoint" &&
          status_of(*outcome, "SAMPLE-photos") == "Verified");
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

// The media enumerator still finds the volume at the root, so these read
// errors are the entries' own (decision 0034).
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

// Decision 0034 (a request the system aborted): Windows' error 995 is a
// request the device did not answer (Timeout). While the volume stays, the file
// is held as an I/O error and then is the entry's own, Unreadable(other), and
// the run fails: it cannot pass without having read the file. Sample data.
void a_read_the_system_aborted_fails_the_run()
{
    Registered fixture;
    fixture.archive.node("notes.txt").open_error = va::IoError{
        va::IoErrorKind::Timeout, 995, "aborted before it finished (CreateFileW, Windows error 995) (sample)"};

    const auto outcome = fixture.verify_ok();
    CHECK(outcome.result == "FAILED");
    CHECK(status_of(outcome, "notes.txt") == "Unreadable(other)");
    CHECK(outcome.coverage_bytes < outcome.registered_bytes);
}

// A Cancelled error that the hash pipeline did not cause (a file system that
// reports one; neither platform does) is the entry's like any other read
// error: the file is reported, never left out of a run that passes without
// it. Sample data.
void a_cancelled_error_is_never_left_out()
{
    Registered fixture;
    fixture.archive.node("notes.txt").open_error =
        va::IoError{va::IoErrorKind::Cancelled, 995, "aborted before it finished (sample)"};

    const auto outcome = fixture.verify_ok();
    CHECK(outcome.result == "FAILED");
    CHECK(status_of(outcome, "notes.txt") == "Unreadable(other)");
    CHECK(outcome.coverage_bytes < outcome.registered_bytes);
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

// R-53: the mode (POSIX bits) and the attributes (Windows bits) are recorded
// where the system lists them, and compared only under --strict-metadata
// (R-51): without it a change of either is no item at all; with it, each is
// MetadataMismatch, naming what changed, and the run fails. Sample data.
void mode_and_attributes_count_only_under_strict_metadata()
{
    EngineFixture fixture;
    fixture.archive.add_file("SAMPLE-mode.txt", "mode").mode = 0644;
    fixture.archive.add_file("SAMPLE-attrs.txt", "attrs").attrs = 0x20; // archive
    fixture.archive.add_file("SAMPLE-same.txt", "same").mode = 0600;
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered.has_value());
    if (registered) {
        const auto manifest = va::load_recorded_manifest(fixture.store, *fixture.repository.files,
            fixture.repository.context.repo, registered->archive_uuid, registered->revision);
        CHECK(manifest.has_value());
        if (manifest) {
            for (const auto& entry : manifest->entries) {
                if (entry.path.p == "SAMPLE-mode.txt") {
                    CHECK(entry.mode == 0644u && !entry.attrs);
                } else if (entry.path.p == "SAMPLE-attrs.txt") {
                    CHECK(entry.attrs == 0x20u && !entry.mode);
                }
            }
        }
    }
    fixture.archive.node("SAMPLE-mode.txt").mode = 0600;
    fixture.archive.node("SAMPLE-attrs.txt").attrs = 0x21; // read-only as well

    const auto relaxed = fixture.verify(fixture.archive.root());
    CHECK(relaxed && relaxed->result == "PASSED" && relaxed->items.empty() && relaxed->counts.at("Verified") == 3);

    va::VerifyRequest strict;
    strict.root = fixture.archive.root();
    strict.strict_metadata = true;
    const auto failed = fixture.verify(strict);
    CHECK(failed && failed->result == "FAILED" && failed->items.size() == 2);
    if (failed && failed->items.size() == 2) {
        CHECK(failed->items[0].path.p == "SAMPLE-attrs.txt" && failed->items[0].status == "MetadataMismatch" &&
              failed->items[0].detail == "attrs");
        CHECK(failed->items[1].path.p == "SAMPLE-mode.txt" && failed->items[1].status == "MetadataMismatch" &&
              failed->items[1].detail == "mode");
    }
}

// §A6: a root that cannot be listed while its volume stays attached fails the
// run with FilesystemAccessError (not INCOMPLETE: the device did not go away,
// decision 0034), saying why, and reads nothing. Sample data.
void a_root_that_cannot_be_scanned_fails_with_a_filesystem_access_error()
{
    Registered fixture;
    fixture.archive.node("").list_error = va::IoError{va::IoErrorKind::PermissionDenied, 5, "access denied (sample)"};
    fixture.archive.bytes_read = 0;
    const auto outcome = fixture.verify_ok();
    CHECK(outcome.result == "FAILED" && outcome.failure_classes == std::vector<std::string>{"FilesystemAccessError"});
    CHECK(outcome.warnings == std::vector<std::string>{"scan: access denied (sample)"});
    CHECK(outcome.items.empty() && outcome.coverage_bytes == 0 && fixture.archive.bytes_read == 0);
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && !runs->empty() && runs->back().result == "FAILED" &&
          runs->back().failure_classes == "FilesystemAccessError");
}

// R-47, R-48 on a real folder (sample data in the temp folder): two names of
// one file (a hardlink) are entries of their own, each read and hashed, and a
// sparse file is hashed by its logical content, its hole read as zeros. Both
// are registered and verified; a change through one name of the hardlinked
// file shows at both entries.
void hardlinks_and_sparse_files_are_hashed_by_content()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("va-sample-links-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const std::string content = "SAMPLE hardlinked content\n";
    std::ofstream(root / "SAMPLE-original.txt", std::ios::binary) << content;
    std::error_code linked;
    std::filesystem::create_hard_link(root / "SAMPLE-original.txt", root / "SAMPLE-hardlink.txt", linked);
    CHECK(!linked);
    const std::uint64_t hole = 3 * 1024 * 1024 + 123; // never written: reads as zeros
    const std::string tail = "end of the SAMPLE sparse file\n";
    const auto sparse = test::make_sparse_file(root / "SAMPLE-sparse.bin", hole, tail);
    CHECK(sparse.created);
    if (!sparse.sparse) {
        std::cerr << "  note: the temp folder's filesystem keeps no hole; the sparse sample is checked by content\n";
    }

    const auto files = va::platform::make_file_system_access();
    {
        EngineFixture fixture(root);
        fixture.media_files = files.get();
        const auto registered = fixture.register_root(root);
        CHECK(registered && registered->counts.files == 3 && registered->unreadable.empty());
        if (registered) {
            const auto manifest = va::load_recorded_manifest(fixture.store, *fixture.repository.files,
                fixture.repository.context.repo, registered->archive_uuid, registered->revision);
            CHECK(manifest.has_value());
            std::map<std::string, va::Entry> entries;
            if (manifest) {
                for (const auto& entry : manifest->entries) {
                    entries[entry.path.p] = entry;
                }
            }
            const auto content_hash = va::blake3_hex(content);
            CHECK(entries["SAMPLE-original.txt"].b3 == content_hash &&
                  entries["SAMPLE-hardlink.txt"].b3 == content_hash &&
                  entries["SAMPLE-hardlink.txt"].type == 'f' && entries["SAMPLE-hardlink.txt"].size == content.size());
            CHECK(entries["SAMPLE-sparse.bin"].b3 == va::blake3_hex(std::string(hole, '\0') + tail) &&
                  entries["SAMPLE-sparse.bin"].size == hole + tail.size());
        }
        const auto verified = fixture.verify(root);
        CHECK(verified && verified->result == "PASSED" && verified->counts.at("Verified") == 3 &&
              verified->coverage_bytes == 2 * content.size() + hole + tail.size());

        std::ofstream(root / "SAMPLE-hardlink.txt", std::ios::binary | std::ios::trunc) << "SAMPLE changed\n";
        const auto changed = fixture.verify(root);
        CHECK(changed && changed->result == "FAILED" && status_of(*changed, "SAMPLE-original.txt") == "Modified" &&
              status_of(*changed, "SAMPLE-hardlink.txt") == "Modified");
    }
    std::error_code removed;
    std::filesystem::remove_all(root, removed);
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

// Decision 0029, point 7: a root that is no registered archive (UNKNOWN)
// and is an empty folder, as a mount point on Linux whose device is not
// attached (sample), stops as not found, asking to check that the device is
// attached, never offering `register`; a folder with something in it stays
// UNKNOWN.
void an_empty_folder_asks_whether_its_device_is_attached()
{
    EngineFixture fixture;
    fixture.archive.add_directory("mnt-SAMPLE");
    fixture.archive.add_file("other-SAMPLE/a.txt", "a (sample data)");
    const auto empty = fixture.verify(fixture.archive.root() / "mnt-SAMPLE");
    CHECK(!empty && empty.error().kind == va::VerifyStop::Kind::NotFound &&
          empty.error().message == "the folder " + va::path_to_utf8(fixture.archive.root() / "mnt-SAMPLE") +
                                       " is empty, and no archive is registered there; if a volume is mounted at "
                                       "this folder, check that the device that holds it is attached");
    const auto other = fixture.verify(fixture.archive.root() / "other-SAMPLE");
    CHECK(!other && other.error().kind == va::VerifyStop::Kind::Decision &&
          other.error().identity.result == va::MatchResult::Unknown);
}

void verify_by_archive_uuid()
{
    Registered fixture;
    va::VerifyRequest request;
    request.archive_uuid = fixture.archive_uuid;
    const auto outcome = fixture.verify(request);
    CHECK(outcome && outcome->result == "PASSED");
}

// R-44: a name that cannot exist on Windows, registered on Linux, is not
// reported as Missing.
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

// R-44 holds only where another system registered the revision (decision
// 0008): on the system that registered it, a name that cannot be created
// there existed all the same (Windows reads names from NTFS that it lets no
// program create, such as a reserved name or a name ending in a dot or a
// space, as Linux drivers write them), so a recorded entry that is gone is
// Missing and the run fails. Sample names.
void a_name_registered_on_this_system_that_is_gone_is_missing()
{
    EngineFixture fixture; // registered and verified on Windows
    const std::vector<std::string> names{"AUX.SAMPLE.txt", "SAMPLE-trailing-dot.", "SAMPLE-trailing-space "};
    for (const auto& name : names) {
        CHECK(!va::representable_on_windows(name));
        fixture.archive.add_file(name, "sample");
    }
    fixture.archive.add_file("SAMPLE-kept.txt", "kept");
    CHECK(fixture.register_root(fixture.archive.root()).has_value());
    for (const auto& name : names) {
        fixture.archive.remove(name);
    }
    const auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "FAILED" && outcome->items.size() == names.size());
    for (const auto& name : names) {
        CHECK(outcome && status_of(*outcome, name) == "Missing");
    }
}

// §A6, R-42: names in one folder that differ only by case, or only by Unicode
// normalisation, are portability warnings that the manifest records. A FULL
// verification of that revision is PASSED_WITH_WARNINGS, saying so in one
// plain line, with every entry verified; an archive without them stays
// PASSED (unchanged_archive_passes). Sample names.
void recorded_portability_warnings_make_the_run_pass_with_warnings()
{
    EngineFixture fixture;
    fixture.archive.add_file("SAMPLE-case/Report.txt", "upper");
    fixture.archive.add_file("SAMPLE-case/report.txt", "lower");
    fixture.archive.add_file("SAMPLE-nfc/caf\xC3\xA9.txt", "composed");    // e-acute as one character
    fixture.archive.add_file("SAMPLE-nfc/cafe\xCC\x81.txt", "decomposed"); // e, then a combining acute
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered && registered->warnings == std::vector<std::string>{"4 entries carry portability warnings"});
    const auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS" && outcome->items.empty() &&
          outcome->counts.at("Verified") == 4 &&
          outcome->warnings ==
              std::vector<std::string>{
                  "some names differ from another name in the same folder only in upper and lower case, or in how a "
                  "letter with an accent or a mark is encoded (Unicode normalisation); a volume or system that "
                  "does not tell such names apart cannot keep them all"});

    // Names that differ only in case, alone.
    EngineFixture case_only;
    case_only.archive.add_file("SAMPLE-case/Report.txt", "upper");
    case_only.archive.add_file("SAMPLE-case/report.txt", "lower");
    CHECK(case_only.register_root(case_only.archive.root()).has_value());
    const auto case_outcome = case_only.verify(case_only.archive.root());
    CHECK(case_outcome && case_outcome->result == "PASSED_WITH_WARNINGS" &&
          case_outcome->warnings == std::vector<std::string>{va::name_collision_warning});
}

// Decision 0006: a verification through a FUSE program instead of the kernel's
// driver of the format (ntfs-3g on Linux) notes it in its report, and nothing
// else: the result, the identity and the recorded manifest never carry it.
// (A sample volume.)
void the_mount_driver_is_only_noted()
{
    Registered fixture("linux");
    fixture.media.volume.driver = "ntfs-3g";
    const auto outcome = fixture.verify_ok();
    CHECK(outcome.result == "PASSED" && outcome.warnings.empty() &&
          outcome.notes == std::vector<std::string>{"driver: ntfs-3g"});
    CHECK(outcome.identity.result == va::MatchResult::Match && outcome.identity.details.empty());
    // A registration through it records no driver in the manifest header.
    va::RegisterRequest request;
    request.root = fixture.archive.root();
    request.new_revision = true;
    const auto registered = fixture.register_root(request);
    CHECK(registered.has_value());
    std::string manifest;
    if (registered) {
        std::ifstream input(registered->manifest, std::ios::binary);
        manifest.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    CHECK(!manifest.empty() && manifest.find("ntfs-3g") == std::string::npos &&
          manifest.find("driver") == std::string::npos);
    fixture.media.volume.driver.clear();
    const auto without = fixture.verify_ok();
    CHECK(without.result == "PASSED" && without.notes.empty());
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

// R-111: the config's io.rotational_override applies to a verification: the
// sample device reports an HDD (one reader), and the override of its serial
// says SSD, so several readers hash its files at once (min(8, cores), on a
// computer with more than one core). Which wins over which, and how serials
// compare, is the_rotational_override_goes_by_serial.
void the_rotational_override_applies_to_a_verification()
{
    EngineFixture fixture; // its device SERIAL-1 reports an HDD
    for (int index = 1; index <= 6; ++index) {
        fixture.archive.add_file(std::format("SAMPLE-{}.bin", index), std::string(1000, 'v'));
    }
    CHECK(fixture.register_root(fixture.archive.root()).has_value());
    test::OpenFilesWatch watch(fixture.archive, true);
    fixture.media_files = &watch;
    va::VerifyRequest request;
    request.root = fixture.archive.root();
    request.rotational_override = {{"SERIAL-1", false}};
    const auto outcome = fixture.verify(request);
    CHECK(outcome && outcome->result == "PASSED");
    CHECK(watch.most_open_at_once() >= (std::thread::hardware_concurrency() >= 2 ? 2 : 1));
}

// R-111: --io wins over what the platform detected and over the config's
// override. --io ssd on a sample device that reports an HDD: several readers
// hash its files at once (on a computer with more than one core). --io hdd on
// a sample device that reports an SSD and whose serial the override names an
// SSD as well: one reader, which waits a second for a second file that never
// comes.
void the_io_option_wins_in_a_verification()
{
    for (const bool ssd : {true, false}) {
        EngineFixture fixture; // its device SERIAL-1 reports an HDD
        for (int index = 1; index <= 6; ++index) {
            fixture.archive.add_file(std::format("SAMPLE-{}.bin", index), std::string(1000, 'v'));
        }
        CHECK(fixture.register_root(fixture.archive.root()).has_value());
        fixture.media.volume.devices.front().rotational = ssd; // an HDD for --io ssd, an SSD for --io hdd
        test::OpenFilesWatch watch(fixture.archive, true, std::chrono::seconds(1));
        fixture.media_files = &watch;
        va::VerifyRequest request;
        request.root = fixture.archive.root();
        request.rotational = !ssd; // --io ssd, --io hdd
        if (!ssd) {
            request.rotational_override = {{"SERIAL-1", false}};
        }
        const auto outcome = fixture.verify(request);
        CHECK(outcome && outcome->result == "PASSED");
        if (ssd) {
            CHECK(watch.most_open_at_once() >= (std::thread::hardware_concurrency() >= 2 ? 2 : 1));
        } else {
            CHECK(watch.most_open_at_once() == 1);
        }
    }
}

// What an INCOMPLETE run whose device went away says (decision 0034).
const std::string device_went_away = "the device that holds the archive went away while it was read (unplugged, or no "
                                     "longer answering); attach it again, then resume this run";

// Decision 0034: a device that goes away during a FULL verification ends it
// INCOMPLETE and resumable, never FAILED with Unreadable items, also where its
// reads fail only with I/O errors (as Linux gives for a USB disk pulled out):
// the volume is looked up again before such an entry is classified, and it is
// gone. At the first file, as it is opened, and at a later file, halfway
// through its read; and while the folders are scanned. Attached again, the
// run's resume reads only what it had not verified. A read error while the
// device stays attached stays Unreadable(media)
// (unreadable_files_fail_with_their_reason). Sample data.
void a_device_that_goes_away_leaves_the_run_incomplete()
{
    for (const int at_file : {0, 1, 5}) { // 0: while scanning
        EngineFixture fixture;
        for (int index = 1; index <= 8; ++index) {
            fixture.archive.add_file(std::format("SAMPLE-dir/SAMPLE-{}.bin", index),
                std::string(1000, static_cast<char>('a' + index)));
        }
        CHECK(fixture.register_root(fixture.archive.root()).has_value());

        test::RemovableDevice device(fixture.archive);
        if (at_file == 0) {
            device.directory = "SAMPLE-dir";
        } else {
            device.file = std::format("SAMPLE-{}.bin", at_file);
            if (at_file > 1) {
                device.at_offset = 500;
            }
        }
        device.on_removed = [&fixture] {
            fixture.media.error = va::IoError{va::IoErrorKind::NotFound, 2, "no volume at the root (sample)"};
        };
        fixture.media_files = &device;
        const auto interrupted = fixture.verify(fixture.archive.root());
        CHECK(interrupted && interrupted->result == "INCOMPLETE" && interrupted->items.empty() &&
              interrupted->failure_classes.empty() &&
              interrupted->warnings == std::vector<std::string>{device_went_away});
        const auto verified_before = static_cast<std::uint64_t>(std::max(0, at_file - 1)) * 1000;
        CHECK(interrupted && interrupted->coverage_bytes == verified_before);
        if (!interrupted) {
            continue;
        }

        device.attach_again();
        fixture.media.error.reset();
        fixture.archive.bytes_read = 0;
        va::VerifyRequest request;
        request.root = fixture.archive.root();
        request.resume_run = interrupted->run_id;
        const auto resumed = fixture.verify(request);
        CHECK(resumed && resumed->result == "PASSED" && resumed->counts.at("Verified") == 8);
        CHECK(fixture.archive.bytes_read == 8000 - verified_before);
    }
}

// Decision 0034: a device that goes away as the root is listed, or as a
// link's target is read, its reads failing with I/O errors and its volume no
// longer found, leaves the run INCOMPLETE and resumable, never FAILED for a
// root that cannot be scanned (FilesystemAccessError) or for a link that
// cannot be read. The archive of links holds no file, so nothing read later
// would find the device gone instead. Attached again, the resume passes.
// Sample data.
void a_device_that_goes_away_at_the_root_or_a_link_leaves_the_run_incomplete()
{
    for (const bool at_link : {false, true}) {
        EngineFixture fixture;
        if (at_link) {
            fixture.archive.add_link("SAMPLE-link-1", "SAMPLE-target-1.bin");
            fixture.archive.add_link("SAMPLE-link-2", "SAMPLE-target-2.bin");
        } else {
            fixture.archive.add_file("SAMPLE-1.bin", "sample one");
            fixture.archive.add_file("SAMPLE-dir/SAMPLE-2.bin", "sample two");
        }
        CHECK(fixture.register_root(fixture.archive.root()).has_value());

        test::RemovableDevice device(fixture.archive);
        if (at_link) {
            device.link = "SAMPLE-link-2";
        } else {
            device.directory = va::path_to_utf8(fixture.archive.root().filename());
        }
        device.on_removed = [&fixture] {
            fixture.media.error = va::IoError{va::IoErrorKind::NotFound, 2, "no volume at the root (sample)"};
        };
        fixture.media_files = &device;
        const auto interrupted = fixture.verify(fixture.archive.root());
        CHECK(interrupted && interrupted->result == "INCOMPLETE" && interrupted->items.empty() &&
              interrupted->failure_classes.empty() &&
              interrupted->warnings == std::vector<std::string>{device_went_away});
        if (!interrupted) {
            continue;
        }

        device.attach_again();
        fixture.media.error.reset();
        va::VerifyRequest request;
        request.root = fixture.archive.root();
        request.resume_run = interrupted->run_id;
        const auto resumed = fixture.verify(request);
        CHECK(resumed && resumed->result == "PASSED" && resumed->counts.at("Verified") == 2);
    }
}

// What an INCOMPLETE run whose device stopped answering says (decision 0034).
const std::string device_stopped_answering =
    "the device that holds the archive stopped answering while it was read: 10 entries in a row could not be read; "
    "unplug it and attach it again, then resume this run; if it stops again at the same place, the device may be "
    "damaged";

// Decision 0034 (operator, 2026-10-27): a device that stops answering while
// the computer still lists its volume (a USB enclosure that hangs) fails every
// read with an I/O error. Ten such reads in a row, with no read between them
// that succeeds, end the run INCOMPLETE and resumable, with none of those
// entries classified: at a file's opening, halfway through a file's read, and
// while the folders are listed. Attached again, the resume reads only what
// was not verified. Sample data.
void a_device_that_stops_answering_leaves_the_run_incomplete()
{
    for (const std::string at : {"opening", "reading", "scanning"}) {
        EngineFixture fixture;
        for (int index = 1; index <= 15; ++index) {
            fixture.archive.add_file(std::format("SAMPLE-dir-{:02}/SAMPLE-{:02}.bin", index, index),
                std::string(1000, static_cast<char>('a' + index)));
        }
        CHECK(fixture.register_root(fixture.archive.root()).has_value());

        test::RemovableDevice device(fixture.archive); // its volume stays listed
        if (at == "scanning") {
            device.directory = "SAMPLE-dir-15"; // the first folder the fake lists: then no folder can be
        } else {
            device.file = "SAMPLE-04.bin";
            if (at == "reading") {
                device.at_offset = 500;
            }
        }
        fixture.media_files = &device;
        const auto interrupted = fixture.verify(fixture.archive.root());
        CHECK(interrupted && interrupted->result == "INCOMPLETE" && interrupted->items.empty() &&
              interrupted->failure_classes.empty() &&
              interrupted->warnings == std::vector<std::string>{device_stopped_answering});
        const std::uint64_t verified_before = at == "scanning" ? 0 : 3000;
        CHECK(interrupted && interrupted->coverage_bytes == verified_before);
        if (!interrupted) {
            continue;
        }

        device.attach_again();
        fixture.archive.bytes_read = 0;
        va::VerifyRequest request;
        request.root = fixture.archive.root();
        request.resume_run = interrupted->run_id;
        const auto resumed = fixture.verify(request);
        CHECK(resumed && resumed->result == "PASSED" && resumed->counts.at("Verified") == 15);
        CHECK(fixture.archive.bytes_read == 15000 - verified_before);
    }
}

// Decision 0034: fewer than ten I/O errors in a row are the entries' own
// (Unreadable(media), the run FAILED), once a read succeeds after them or the
// last file was read; a read that succeeds starts the count again, and an
// error that is no I/O error (permission) neither counts nor ends a run of
// them. Ten in a row stop the run, also where the damage is the disk's own:
// none of the ten is classified, and its resume stops at the same place
// again. Sample damage on twenty sample files.
void io_errors_stop_a_run_only_ten_in_a_row()
{
    const auto damaged_run = [](const std::vector<int>& media, int permission) {
        EngineFixture fixture;
        for (int index = 1; index <= 20; ++index) {
            fixture.archive.add_file(std::format("SAMPLE-{:02}.bin", index), std::string(1000, 'd'));
        }
        CHECK(fixture.register_root(fixture.archive.root()).has_value());
        for (const int index : media) {
            fixture.archive.node(std::format("SAMPLE-{:02}.bin", index)).read_error_offset = 0;
        }
        if (permission > 0) {
            fixture.archive.node(std::format("SAMPLE-{:02}.bin", permission)).open_error =
                va::IoError{va::IoErrorKind::PermissionDenied, 5, "denied (sample)"};
        }
        auto outcome = fixture.verify(fixture.archive.root());
        CHECK(outcome.has_value());
        std::vector<std::string> items;
        for (const auto& item : outcome ? outcome->items : std::vector<va::VerifyItemRecord>{}) {
            items.push_back(item.path.p + " " + item.status);
        }
        auto resumed = outcome;
        if (outcome && outcome->result == "INCOMPLETE") {
            va::VerifyRequest request;
            request.root = fixture.archive.root();
            request.resume_run = outcome->run_id;
            resumed = fixture.verify(request);
        }
        return std::tuple{outcome ? outcome->result : "", items, outcome ? outcome->warnings : std::vector<std::string>{},
            resumed ? resumed->result : ""};
    };
    const auto unreadable = [](const std::vector<int>& indexes, const std::string& status = "Unreadable(media)") {
        std::vector<std::string> items;
        for (const int index : indexes) {
            items.push_back(std::format("SAMPLE-{:02}.bin {}", index, status));
        }
        return items;
    };

    // Two runs of five, a readable file between them.
    auto [result, items, warnings, again] = damaged_run({2, 3, 4, 5, 6, 8, 9, 10, 11, 12}, 0);
    CHECK(result == "FAILED" && items == unreadable({2, 3, 4, 5, 6, 8, 9, 10, 11, 12}) && warnings.empty());
    // Nine, then a file that may not be read: the ten are the entries' own.
    std::tie(result, items, warnings, again) = damaged_run({2, 3, 4, 5, 6, 7, 8, 9, 10}, 11);
    auto expected = unreadable({2, 3, 4, 5, 6, 7, 8, 9, 10});
    expected.push_back("SAMPLE-11.bin Unreadable(permission)");
    CHECK(result == "FAILED" && items == expected && warnings.empty());
    // Nine at the end, with no file after them.
    std::tie(result, items, warnings, again) = damaged_run({12, 13, 14, 15, 16, 17, 18, 19, 20}, 0);
    CHECK(result == "FAILED" && items == unreadable({12, 13, 14, 15, 16, 17, 18, 19, 20}) && warnings.empty());
    // Ten, a file that may not be read among them: the run stops, and so does its resume.
    std::tie(result, items, warnings, again) = damaged_run({2, 3, 4, 5, 6, 8, 9, 10, 11, 12}, 7);
    CHECK(result == "INCOMPLETE" && items == unreadable({7}, "Unreadable(permission)") &&
          warnings == std::vector<std::string>{device_stopped_answering} && again == "INCOMPLETE");

    // Folders count as their listings: five that cannot be listed, then a
    // folder listed (whichever way the scan walks them, one of SAMPLE-a and
    // SAMPLE-c comes after them), then five damaged files: two runs of five.
    EngineFixture fixture;
    for (const std::string folder : {"SAMPLE-a", "SAMPLE-b1", "SAMPLE-b2", "SAMPLE-b3", "SAMPLE-b4", "SAMPLE-b5",
             "SAMPLE-c"}) {
        fixture.archive.add_file(folder + "/SAMPLE.bin", std::string(1000, 'f'));
    }
    for (int index = 1; index <= 6; ++index) {
        fixture.archive.add_file(std::format("SAMPLE-{:02}.bin", index), std::string(1000, 'd'));
    }
    CHECK(fixture.register_root(fixture.archive.root()).has_value());
    for (int index = 1; index <= 5; ++index) {
        fixture.archive.node(std::format("SAMPLE-b{}", index)).list_error =
            va::IoError{va::IoErrorKind::MediaError, 5, "Input/output error (sample)"};
        fixture.archive.node(std::format("SAMPLE-{:02}.bin", index)).read_error_offset = 0;
    }
    const auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "FAILED" && outcome->counts.at("Unreadable(media)") == 10 &&
          outcome->warnings.empty());
}

// Decision 0034: how a watch tells a read error. Its own for an error that is
// no I/O error; held for an I/O error while the volume stays, the tenth in a
// row without a read that succeeded between them making it a device that
// stopped answering; a device that went away at once, by its error or by its
// volume no longer found. Once stopped, it stays stopped. (Sample volumes.)
void a_watch_tells_a_device_that_stopped_answering()
{
    test::FakeMediaEnumerator media;
    media.volume = test::fake_volume("/vol");
    const va::IoError io{va::IoErrorKind::MediaError, 5, "Input/output error (sample)"};
    const va::IoError timeout{va::IoErrorKind::Timeout, 121, "timeout (sample)"};
    const va::IoError denied{va::IoErrorKind::PermissionDenied, 13, "denied (sample)"};

    va::VolumeWatch watch(media, "/vol", media.volume);
    for (int round = 0; round < 3; ++round) {
        for (std::size_t index = 1; index < va::io_errors_that_stop; ++index) {
            CHECK(watch.failed(index % 2 == 0 ? io : timeout) == va::ReadFailure::Held);
            CHECK(watch.failed(denied) == va::ReadFailure::Entry);
        }
        watch.answered();
    }
    CHECK(watch.stop() == va::DeviceStop::None);
    for (std::size_t index = 1; index < va::io_errors_that_stop; ++index) {
        CHECK(watch.failed(io) == va::ReadFailure::Held);
    }
    CHECK(watch.failed(io) == va::ReadFailure::Stop && watch.stop() == va::DeviceStop::NotAnswering);
    watch.answered();
    CHECK(watch.failed(denied) == va::ReadFailure::Stop && watch.stop() == va::DeviceStop::NotAnswering);

    va::VolumeWatch removed(media, "/vol", media.volume);
    CHECK(removed.failed(va::IoError{va::IoErrorKind::DeviceRemoved, 1167, "not connected (sample)"}) ==
              va::ReadFailure::Stop &&
          removed.stop() == va::DeviceStop::WentAway);
    va::VolumeWatch gone(media, "/vol", media.volume);
    media.error = va::IoError{va::IoErrorKind::NotFound, 2, "no volume at the root (sample)"};
    CHECK(gone.failed(denied) == va::ReadFailure::Stop && gone.stop() == va::DeviceStop::WentAway);
}

// Decision 0034: whether the volume an operation began with is still the one
// at its root: the same filesystem id and the same devices. What may read
// otherwise while it stays attached does not count; a lookup that finds the
// device without its names, or the volume without its devices or its full
// id, is a volume gone. (Sample volumes.)
void a_volume_is_the_same_while_its_ids_and_devices_are()
{
    const auto before = test::fake_volume("/vol");
    auto renamed = before;
    renamed.volume_label = "SAMPLE-RENAMED";
    renamed.capacity_bytes += 4096;
    renamed.devices.front().capacity_bytes += 4096;
    renamed.devices.front().rotational.reset();
    CHECK(va::same_volume(before, renamed));
    auto other = before;
    other.filesystem_uuid = "9999888877776666";
    CHECK(!va::same_volume(before, other));
    auto short_id_only = before;
    short_id_only.filesystem_uuid.clear();
    CHECK(!va::same_volume(before, short_id_only));
    auto without_devices = before;
    without_devices.devices.clear();
    CHECK(!va::same_volume(before, without_devices));
    auto nameless = before;
    nameless.devices.front().model.clear();
    nameless.devices.front().serial.clear();
    CHECK(!va::same_volume(before, nameless));
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

// A sample volume behind a USB bridge that gives no serial: an archive on it
// is recognised MATCH_WEAK (§B10).
va::VolumeInfo serial_less_volume(const std::filesystem::path& mount_point)
{
    auto volume = test::fake_volume(mount_point);
    volume.devices.front().serial.clear();
    volume.devices.front().wwn.clear();
    return volume;
}

// The rows of `table` in the fixture's database (defined below).
std::int64_t count_rows(EngineFixture& fixture, const std::string& table);

// Decision 0022, point 7: a resume by the archive's uuid, as `verify --full
// <archive_uuid> --resume <run_id>` and Resume in the Archive List make it,
// looks at every attached volume. A device and a sector clone of it attached at
// once, both behind bridges that give no serial, both hold the archive
// (MATCH_WEAK) and cannot be told apart: the resume stops (AMBIGUOUS, an
// operator decision), naming both roots and the root the run read, reads
// nothing and leaves the run to resume. Continued on the other volume, it
// would join the two into one result: a FULL pass for entries never read
// there. Resumed at the root it read, it reads only the rest there and finds
// the files the clone lost. A verification by uuid that is not a resume, as
// `verify --full <archive_uuid>` and Verify Archive make it, stops the same
// way, never reading the volume listed first by itself: it names both roots
// in its words, and gives them apart for the GUI's, reading and recording
// nothing. With the clone detached, it reads the one volume left. (Sample
// volumes, sample files.)
void a_resume_by_uuid_stops_where_several_volumes_hold_the_archive()
{
    EngineFixture fixture;
    const auto original = fixture.archive.root() / "SAMPLE-source";
    const auto clone = fixture.archive.root() / "SAMPLE-cloned";
    fixture.media.volume = serial_less_volume(original);
    fixture.media.extra_volumes = {serial_less_volume(clone)};
    for (int index = 1; index <= 8; ++index) {
        const auto name = "f" + std::to_string(index);
        const std::string content(1000, static_cast<char>('a' + index));
        fixture.archive.add_file("SAMPLE-source/" + name, content);
        // The clone's last three files are damaged (same size and time).
        fixture.archive.add_file("SAMPLE-cloned/" + name, index >= 6 ? std::string(1000, 'Z') : content);
    }
    const auto registered = fixture.register_root(original);
    CHECK(registered.has_value());
    // The clone verified by its root, its device gone at f5.
    fixture.archive.node("SAMPLE-cloned/f5").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.verify(clone);
    CHECK(interrupted && interrupted->result == "INCOMPLETE" && interrupted->root == clone);
    if (!registered || !interrupted) {
        return;
    }
    fixture.archive.node("SAMPLE-cloned/f5").open_error.reset();
    const auto& run = interrupted->run_id;
    const auto checkpoints = fixture.store.progress(run);
    CHECK(checkpoints && checkpoints->size() == 4 && interrupted->coverage_bytes == 4000);

    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = registered->archive_uuid;
    by_uuid.resume_run = run;
    fixture.archive.bytes_read = 0;
    const auto stopped = fixture.verify(by_uuid);
    CHECK(!stopped && stopped.error().kind == va::VerifyStop::Kind::Decision);
    CHECK(!stopped && stopped.error().identity.result == va::MatchResult::Ambiguous);
    const auto quoted = [](const std::filesystem::path& root) { return "\"" + va::path_to_utf8(root) + "\""; };
    CHECK(!stopped && stopped.error().message ==
                          "AMBIGUOUS: archive " + registered->archive_uuid +
                              " is on more than one attached volume (cloned media): " + quoted(original) + ", " +
                              quoted(clone) + "; run " + run + " read " + quoted(clone) +
                              ": resume it at the root on the volume it read with `verify --full <root_path> "
                              "--resume " + run + "`, or detach the other volumes and resume it again");
    CHECK(!stopped && stopped.error().roots_found == (std::vector<std::filesystem::path>{original, clone}));
    if (stopped) {
        std::cerr << "  resumed by uuid on two volumes: FULL VERIFY " << stopped->result << " at "
                  << va::path_to_utf8(stopped->root) << '\n';
    }
    CHECK(fixture.archive.bytes_read == 0);
    // The run is left as it was, to resume.
    const auto kept = fixture.store.progress(run);
    CHECK(kept && checkpoints && *kept == *checkpoints);
    const auto record = fixture.store.setting("run:" + run).value_or("");
    CHECK(!record.empty());
    const auto runs = fixture.store.verify_runs(registered->archive_uuid);
    CHECK(runs && runs->size() == 1);
    // The record of an older version's run does not say where it read.
    auto older = nlohmann::json::parse(record, nullptr, false);
    if (older.is_object()) {
        older.erase("root");
        CHECK(fixture.store.set_setting("run:" + run, older.dump()).has_value());
        const auto unsaid = fixture.verify(by_uuid);
        CHECK(!unsaid && unsaid.error().kind == va::VerifyStop::Kind::Decision &&
              unsaid.error().message ==
                  "AMBIGUOUS: archive " + registered->archive_uuid +
                      " is on more than one attached volume (cloned media): " + quoted(original) + ", " +
                      quoted(clone) + "; resume run " + run +
                      " at the root on the volume it read with `verify --full <root_path> --resume " + run +
                      "`, or detach the other volumes and resume it again");
        CHECK(fixture.store.set_setting("run:" + run, record).has_value() && fixture.archive.bytes_read == 0);
    }

    // At the root it read: only the rest, on the clone, which lost three files.
    va::VerifyRequest by_root;
    by_root.root = clone;
    by_root.resume_run = run;
    const auto resumed = fixture.verify(by_root);
    CHECK(resumed && resumed->result == "FAILED" && resumed->root == clone && resumed->resumed_from == run);
    CHECK(resumed && status_of(*resumed, "f6") == "Modified" && status_of(*resumed, "f7") == "Modified" &&
          status_of(*resumed, "f8") == "Modified" && status_of(*resumed, "f5") == "Verified");
    CHECK(fixture.archive.bytes_read == 4000);

    // Not a resume: it stops too, naming both roots, reading and recording
    // nothing. With the clone detached, the one volume left is read in full.
    va::VerifyRequest fresh;
    fresh.archive_uuid = registered->archive_uuid;
    fixture.archive.bytes_read = 0;
    const auto runs_before = count_rows(fixture, "verify_runs");
    const auto evidence_before = count_rows(fixture, "media_evidence");
    const auto chosen = fixture.verify(fresh);
    CHECK(!chosen && chosen.error().kind == va::VerifyStop::Kind::Decision &&
          chosen.error().identity.result == va::MatchResult::Ambiguous);
    CHECK(!chosen && chosen.error().message ==
                         "AMBIGUOUS: archive " + registered->archive_uuid +
                             " is on more than one attached volume (cloned media): " + quoted(original) + ", " +
                             quoted(clone) + "; verify it at the root on one of them with `verify --full "
                             "<root_path>`, or detach the other volumes and verify it again");
    CHECK(!chosen && chosen.error().roots_found == (std::vector<std::filesystem::path>{original, clone}));
    if (chosen) {
        std::cerr << "  verified by uuid on two volumes: FULL VERIFY " << chosen->result << " at "
                  << va::path_to_utf8(chosen->root) << '\n';
    }
    CHECK(fixture.archive.bytes_read == 0 && count_rows(fixture, "verify_runs") == runs_before &&
          count_rows(fixture, "media_evidence") == evidence_before);
    fixture.media.extra_volumes.clear();
    const auto verified = fixture.verify(fresh);
    CHECK(verified && verified->result == "PASSED_WITH_WARNINGS" && verified->root == original);
    CHECK(verified && verified->identity.result == va::MatchResult::MatchWeak && fixture.archive.bytes_read == 8000);
}

// §B10 and decision 0022, point 7: a device and a sector-by-sector clone of
// it, registered as two archives (decision 0005), both behind bridges that
// give no serial (sample volumes): A on the device, B on the clone, registered
// with --force-new-archive. Attached together, the device's root matches both
// (AMBIGUOUS), and the message says to select one by its uuid and, where
// their volumes are attached together, to detach all but the volume of the one
// to verify first, as a verification by A's uuid finds A on both volumes and
// stops too. With the clone detached, A verifies by its uuid on the device.
// (Sample volumes, sample files.)
void clones_registered_as_two_archives_say_to_detach_the_other_volumes()
{
    EngineFixture fixture;
    const auto original = fixture.archive.root() / "SAMPLE-source";
    const auto clone = fixture.archive.root() / "SAMPLE-cloned";
    fixture.media.volume = serial_less_volume(original);
    fixture.media.extra_volumes = {serial_less_volume(clone)};
    fixture.archive.add_file("SAMPLE-source/SAMPLE-one.txt", "one (sample data)");
    fixture.archive.add_file("SAMPLE-cloned/SAMPLE-one.txt", "one (sample data)");
    const auto a = fixture.register_root(original);
    va::RegisterRequest separately;
    separately.root = clone;
    separately.force_new_archive = true;
    const auto b = fixture.register_root(separately);
    CHECK(a && b && a->archive_uuid != b->archive_uuid);
    if (!a || !b) {
        return;
    }
    const auto by_root = fixture.verify(original);
    CHECK(!by_root && by_root.error().kind == va::VerifyStop::Kind::Decision &&
          by_root.error().identity.result == va::MatchResult::Ambiguous &&
          by_root.error().message ==
              "AMBIGUOUS: the media matches several archives (cloned media); select one with `verify --full "
              "<archive_uuid>`; where the volumes of those archives are attached together, detach all but the volume "
              "of the one to verify, then verify it");
    if (!by_root) {
        std::cerr << "  by root: " << by_root.error().message << '\n';
    }
    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = a->archive_uuid;
    const auto selected = fixture.verify(by_uuid);
    CHECK(!selected && selected.error().kind == va::VerifyStop::Kind::Decision &&
          selected.error().roots_found == (std::vector<std::filesystem::path>{original, clone}));
    fixture.media.extra_volumes.clear();
    const auto detached = fixture.verify(by_uuid);
    CHECK(detached && detached->archive_uuid == a->archive_uuid && detached->root == original &&
          detached->result == "PASSED_WITH_WARNINGS" && detached->identity.result == va::MatchResult::MatchWeak);
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
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        *fixture.repository.writer, fixture.processes};
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
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        *fixture.repository.writer, fixture.processes};
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

// Decision 0029, point 7: an archive verified by its uuid whose volume is
// attached but whose folder is not there (moved away, sample) stops as not
// found, naming the folder, never as an archive that is not on any attached
// volume; with its volume detached it is not on any attached volume.
void an_archive_whose_folder_is_gone_is_not_found()
{
    EngineFixture fixture;
    fixture.archive.add_file("photos/a.txt", "a (sample data)");
    const auto registered = fixture.register_root(fixture.archive.root() / "photos");
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    fixture.archive.remove("photos");
    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = registered->archive_uuid;
    const auto gone = fixture.verify(by_uuid);
    CHECK(!gone && gone.error().kind == va::VerifyStop::Kind::NotFound &&
          gone.error().message == "the folder " + va::path_to_utf8(fixture.archive.root() / "photos") +
                                      " of archive " + registered->archive_uuid +
                                      " was not found: its volume is attached, but the folder is not there");
    fixture.media.volume.filesystem_uuid = "c0ffee00-0000-4000-8000-00000000beef";
    fixture.media.volume.filesystem_uuid_short.clear();
    fixture.media.volume.devices.clear();
    const auto detached = fixture.verify(by_uuid);
    CHECK(!detached && detached.error().kind == va::VerifyStop::Kind::NotFound &&
          detached.error().message == "archive " + registered->archive_uuid + " is not on any attached volume");
}

// Decision 0029, point 3: an archive on a volume that has no drive letter or
// folder path, whose mount point Windows gives as its GUID path, registered
// through that path as the user interfaces take it, and verified by its uuid.
// The root VeriArchive finds there itself, the volume's root folder with its
// separator or a folder below it, is read, recorded with the run while it
// works, and never refused. Windows only: elsewhere no volume is named so.
// (A sample GUID, sample files.)
void an_archive_on_a_volume_known_by_its_guid_path_verifies_by_uuid()
{
    if (std::filesystem::path::preferred_separator != '\\') {
        return;
    }
    const std::filesystem::path volume_root = R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000029}\)";
    for (const std::string folder : {"", "SAMPLE"}) {
        EngineFixture fixture(volume_root);
        fixture.archive.add_file(folder.empty() ? "a.txt" : folder + "/a.txt", "a (sample data)");
        const auto root = folder.empty() ? volume_root : volume_root / folder;
        const auto typed = va::app::typed_root(va::path_to_utf8(root) + (folder.empty() ? "" : R"(\)"));
        CHECK(typed == root && !va::app::root_refusal(typed));
        const auto registered = fixture.register_root(typed);
        CHECK(registered.has_value());
        if (!registered) {
            continue;
        }
        RunWatcher watcher(fixture.store);
        va::VerifyServices services{fixture.archive, *fixture.repository.files, fixture.media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host, *fixture.repository.writer, fixture.processes};
        va::VerifyRequest request;
        request.archive_uuid = registered->archive_uuid;
        const auto verified = va::verify_full(services, request, fixture.cancel, watcher);
        CHECK(verified && verified->result == "PASSED" && verified->root == root);
        CHECK(verified && !va::app::root_refusal(verified->root));
        CHECK(!watcher.records_seen.empty());
        for (const auto& record : watcher.records_seen) {
            CHECK(record.value("root", std::string{}) == va::path_to_utf8(root));
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

// The repository's files, except that the file named `name` opens and then
// cannot be read (sample: a CRC error), as a manifest on a bad sector.
class FailedRead final : public va::IFileSystemAccess {
public:
    FailedRead(va::IFileSystemAccess& inner, std::string name) : inner_(inner), name_(std::move(name)) {}
    std::expected<std::vector<va::DirEntry>, va::IoError> list(const std::filesystem::path& directory) override
    {
        return inner_.list(directory);
    }
    std::expected<std::unique_ptr<va::IByteSource>, va::IoError> open(
        const std::filesystem::path& file, va::ReadOptions options) override
    {
        auto source = inner_.open(file, options);
        if (!source || va::path_to_utf8(file.filename()) != name_) {
            return source;
        }
        return std::make_unique<Source>(std::move(*source));
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
    class Source final : public va::IByteSource {
    public:
        explicit Source(std::unique_ptr<va::IByteSource> inner) : inner_(std::move(inner)) {}
        std::expected<std::size_t, va::IoError> read(std::uint64_t, std::span<std::byte>) override
        {
            return std::unexpected(va::IoError{va::IoErrorKind::MediaError, 23, "CRC error (sample)"});
        }
        std::uint64_t size() const override { return inner_->size(); }
        std::size_t alignment() const override { return inner_->alignment(); }

    private:
        std::unique_ptr<va::IByteSource> inner_;
    };

    va::IFileSystemAccess& inner_;
    std::string name_;
};

// Decision 0028: the plain message of the manifest of revision 1 of
// `archive_uuid` that cannot be used: what is wrong with it, naming it by its
// place in the repository, then how to fix it.
std::string restore_message(const va::Uuid& archive_uuid, const std::string& what)
{
    return "the manifest of revision 1 (manifests/" + archive_uuid + "/r0001.vam) " + what +
           test::put_back(archive_uuid, 1);
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
// archive `label` (`archive_uuid`), of revision 1, cannot be used, `what`
// being wrong with it: which archive, why, and to verify it by its uuid, then
// the steps of the plain message, its manifest put back with `import` and the
// last resort, or the index rebuilt.
std::string root_may_belong_to(const std::string& label, const va::Uuid& archive_uuid, const std::string& what,
    bool rebuild = false)
{
    return "this root may belong to archive " + label + " (" + archive_uuid +
           "), whose manifest cannot be used: " + what + "; verify that archive by its uuid (`verify --full " +
           archive_uuid + "`) to record the failure, then " +
           (rebuild ? std::string("rebuild the index with `repo rebuild-index`")
                    : test::put_back_steps(archive_uuid, 1, true, "its manifest"));
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
            va::rebuild_index(fixture.store, *fixture.repository.files, fixture.repository.context.repo, test::not_cancelled(), test::no_progress());
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
// wrong, and says to verify it by its uuid to record the failure, then to put
// its manifest back with `import`, or where nothing holds it to register its
// next revision over the lost manifest, never to rebuild the index, which
// would take a changed manifest for the truth. A new revision
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
// unreadable (it cannot be opened, or opens and then cannot be read, as on a
// bad sector) fails with RepositoryError, one whose header line or body has
// changed with ManifestHashMismatch (sample data: one byte each). The failure
// names the manifest and says to put it back with `import`, or where nothing
// holds it to register its next revision over the lost manifest.
// The run is recorded as FULL FAILED of the archive's revision, with nothing
// covered and nothing to resume, in an intact hash chain. With the manifest
// restored, the archive verifies.
void a_known_archive_whose_manifest_cannot_be_used_fails_unread()
{
    enum class Case { Missing, Unreadable, ReadFails, HeaderChanged, BodyChanged };
    for (const auto what :
        {Case::Missing, Case::Unreadable, Case::ReadFails, Case::HeaderChanged, Case::BodyChanged}) {
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
        } else if (what == Case::ReadFails) {
            message = restore_message(fixture.archive_uuid, "cannot be read: CRC error (sample)");
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
        FailedRead bad_sector(*fixture.repository.files, "r0001.vam");
        va::IFileSystemAccess& repository_files = what == Case::Unreadable ? static_cast<va::IFileSystemAccess&>(denied)
                                                  : what == Case::ReadFails ? static_cast<va::IFileSystemAccess&>(bad_sector)
                                                                            : *fixture.repository.files;
        va::VerifyServices services{fixture.archive, repository_files, fixture.media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
            *fixture.repository.writer, fixture.processes};
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

// Decision 0028: an archive whose record says a revision of which the index
// has no record, as where `repo rebuild-index` could not read its manifest
// and no record of it was left (here the record of revision 2 and its index
// rows removed by hand, sample damage). Its manifest cannot be used, and
// what the file at its place is says how to fix it, read as rebuild-index
// reads a manifest: it reads as the manifest of revision 2 (rebuild the
// index), it is gone, it is another revision's (a copy of revision 1's), or
// it cannot be opened (put it back with `import`, which records it). Named by its uuid,
// the archive FAILS at revision 2 with RepositoryError, without reading the
// media (here it cannot even be listed), recorded; by root it stops naming
// the archive and both steps; a new revision, an export and a comparison stop
// with the plain message; the archive list lists it with its kind, never at
// revision 1. Fixed as the message says, the archive verifies at revision 2.
void an_archive_at_a_revision_the_index_has_no_record_of()
{
    enum class Case { Readable, Missing, AnotherRevision, Denied };
    for (const auto what : {Case::Readable, Case::Missing, Case::AnotherRevision, Case::Denied}) {
        Registered fixture;
        fixture.archive.add_file("added.txt", "added for revision 2 (sample data)");
        va::RegisterRequest again;
        again.root = fixture.archive.root();
        again.new_revision = true;
        CHECK(fixture.register_root(again).has_value());
        const auto& layout = fixture.repository.context.repo;
        const auto second = va::manifest_path(layout.manifests(), fixture.archive_uuid, 2);
        const auto kept = file_bytes(second);
        fixture.repository.execute("DELETE FROM archive_revisions WHERE revision = 2; "
                                   "DELETE FROM files WHERE revision = 2");
        const auto manifest = "manifests/" + fixture.archive_uuid + "/r0002.vam";
        const auto no_record = "the repository's index has no record of the manifest of revision 2 (" + manifest + ")";
        const std::string rebuild = "; rebuild the index with `repo rebuild-index`";
        const auto restore = test::put_back(fixture.archive_uuid, 2);
        std::string what_is_wrong = no_record;
        std::string message = no_record + rebuild;
        auto file = va::UnusableManifest::File::Readable;
        if (what == Case::Missing) {
            std::filesystem::remove(second);
            what_is_wrong = no_record + ", which is missing";
            message = what_is_wrong + restore;
            file = va::UnusableManifest::File::Missing;
        } else if (what == Case::AnotherRevision) {
            write_bytes(second, file_bytes(va::manifest_path(layout.manifests(), fixture.archive_uuid, 1)));
            what_is_wrong = no_record + ", which cannot be read: header does not match its file name";
            message = what_is_wrong + restore;
            file = va::UnusableManifest::File::Unreadable;
        } else if (what == Case::Denied) {
            what_is_wrong = no_record + ", which cannot be read: access denied (sample)";
            message = what_is_wrong + restore;
            file = va::UnusableManifest::File::Unreadable;
        }
        DeniedOpen denied(*fixture.repository.files, "r0002.vam");
        va::IFileSystemAccess& repository_files =
            what == Case::Denied ? static_cast<va::IFileSystemAccess&>(denied) : *fixture.repository.files;

        // Listed with the problem, at revision 2.
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto statuses = va::archive_statuses(fixture.store, repository_files, layout, va::Policy{}, now);
        CHECK(statuses && statuses->size() == 1 && statuses->front().archive.current_revision == 2 &&
              !statuses->front().header && statuses->front().unusable_manifest &&
              statuses->front().unusable_manifest->kind == va::UnusableManifest::Kind::NotIndexed &&
              statuses->front().unusable_manifest->file == file &&
              va::kind_code(statuses->front().unusable_manifest->kind) == "not_indexed" &&
              va::failure_class_of(*statuses->front().unusable_manifest) == "RepositoryError" &&
              va::plain_message(*statuses->front().unusable_manifest) == message);
        if (statuses && statuses->size() == 1 && statuses->front().unusable_manifest &&
            va::plain_message(*statuses->front().unusable_manifest) != message) {
            std::cerr << "  listed: " << va::plain_message(*statuses->front().unusable_manifest) << '\n';
        }

        // By root: the root may be this archive's.
        va::VerifyServices services{fixture.archive, repository_files, fixture.media, fixture.store, layout,
            fixture.repository.context.repository_uuid, fixture.host, *fixture.repository.writer, fixture.processes};
        va::VerifyRequest by_root;
        by_root.root = fixture.archive.root();
        const auto stopped = va::verify_full(services, by_root, fixture.cancel, fixture.progress);
        // Put back, which records it, unless the file reads as the manifest.
        const auto steps = what == Case::Readable
                               ? std::string("rebuild the index with `repo rebuild-index`")
                               : test::put_back_steps(fixture.archive_uuid, 2, true, "its manifest");
        const auto may_belong = "this root may belong to archive ARCH-0001 (" + fixture.archive_uuid +
                                "), whose manifest cannot be used: " + what_is_wrong +
                                "; verify that archive by its uuid (`verify --full " + fixture.archive_uuid +
                                "`) to record the failure, then " + steps;
        CHECK(!stopped && stopped.error().kind == va::VerifyStop::Kind::Repository &&
              stopped.error().message == may_belong);
        if (!stopped && stopped.error().message != may_belong) {
            std::cerr << "  by root: " << stopped.error().message << '\n';
        }

        // Named by its uuid: FAILED at revision 2, the media not read.
        fixture.media.error = va::IoError{va::IoErrorKind::Other, 0, "the media is not looked for (sample)"};
        fixture.archive.bytes_read = 0;
        const auto evidence_before = count_rows(fixture, "media_evidence");
        va::VerifyRequest by_uuid;
        by_uuid.archive_uuid = fixture.archive_uuid;
        const auto named = va::verify_full(services, by_uuid, fixture.cancel, fixture.progress);
        CHECK(named && named->result == "FAILED" && !named->media_read && named->revision == 2 &&
              named->failure_classes == std::vector<std::string>{"RepositoryError"} &&
              named->warnings == std::vector<std::string>{"manifest: " + message});
        if (!named) {
            std::cerr << "  by uuid stopped: " << named.error().message << '\n';
        } else if (named->warnings != std::vector<std::string>{"manifest: " + message} && !named->warnings.empty()) {
            std::cerr << "  by uuid: " << named->warnings.front() << '\n';
        }
        CHECK(fixture.archive.bytes_read == 0 && count_rows(fixture, "media_evidence") == evidence_before);
        const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
        CHECK(runs && runs->size() == 1 && runs->back().result == "FAILED" && runs->back().revision == 2 &&
              runs->back().failure_classes == "RepositoryError");
        fixture.media.error.reset();

        // Every other use stops with the plain message.
        const auto loaded = va::load_recorded_manifest(fixture.store, repository_files, layout, fixture.archive_uuid, 2);
        CHECK(!loaded && loaded.error().failure_class == "RepositoryError" && loaded.error().message == message &&
              loaded.error().unusable && loaded.error().unusable->kind == va::UnusableManifest::Kind::NotIndexed);
        const auto exported = va::export_archive(fixture.store, repository_files, *fixture.repository.writer, layout,
            fixture.archive_uuid, layout.root.parent_path() / "export", test::not_cancelled(), test::no_progress());
        CHECK(!exported && exported.error().message == "RepositoryError: " + message);
        const auto compared = va::compare_archives(fixture.store, repository_files, layout, fixture.archive_uuid,
            fixture.archive_uuid, fixture.cancel, fixture.progress);
        CHECK(!compared && compared.error().failure_class == "RepositoryError");
        va::RegisterServices register_services{fixture.archive, repository_files, *fixture.repository.writer,
            fixture.media_writer, fixture.media, fixture.store, layout, fixture.host, fixture.processes};
        const auto registered = va::register_archive(register_services, again, fixture.cancel, fixture.progress);
        CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Repository &&
              registered.error().message == message);

        // Fixed as the message says: the manifest restored where it cannot
        // be read, then the index rebuilt.
        if (what != Case::Readable) {
            write_bytes(second, kept);
        }
        const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, layout, test::not_cancelled(), test::no_progress());
        CHECK(rebuilt && rebuilt->skipped.empty() && rebuilt->revisions == 2 && rebuilt->revisions_kept.empty());
        const auto verified = fixture.verify(by_uuid);
        CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
    }
}

// An archive whose first registration never finished has no revision
// (revision 0) and no manifest, which is nothing that cannot be used
// (decision 0028): A at /vol/a is registered, B's first registration at
// /vol/b is interrupted (sample data). B is listed without a problem, and A
// verifies by its root, which matching against every archive's latest carrier
// would not let it do were B taken for one whose manifest cannot be used. A
// whole load of B's revision, as `export` and `export-sha256sums` make one,
// finds revision 0 not recorded, never a manifest of it that the index has no
// record of (which would say to restore r0000.vam).
void an_unfinished_first_registration_is_no_manifest_that_cannot_be_used()
{
    EngineFixture fixture;
    fixture.archive.add_file("a/one.txt", "a (sample data)");
    fixture.archive.add_file("b/two.txt", "b (sample data)").open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged (sample)"};
    const auto a = fixture.register_root(fixture.archive.root() / "a");
    const auto b = fixture.register_root(fixture.archive.root() / "b");
    CHECK(a && !b && b.error().kind == va::RegisterFailure::Kind::Incomplete);
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto statuses = va::archive_statuses(
        fixture.store, *fixture.repository.files, fixture.repository.context.repo, va::Policy{}, now);
    CHECK(statuses && statuses->size() == 2 &&
          std::ranges::none_of(*statuses,
              [](const va::ArchiveStatus& status) { return status.unusable_manifest.has_value(); }) &&
          std::ranges::count_if(*statuses,
              [](const va::ArchiveStatus& status) { return status.archive.current_revision == 0; }) == 1);
    const auto verified = fixture.verify(fixture.archive.root() / "a");
    CHECK(verified && verified->result == "PASSED" && a && verified->archive_uuid == a->archive_uuid);
    if (!verified) {
        std::cerr << "  A by root: " << verified.error().message << '\n';
    }

    va::Uuid b_uuid;
    if (statuses) {
        for (const auto& status : *statuses) {
            if (status.archive.current_revision == 0) {
                b_uuid = status.archive.archive_uuid;
            }
        }
    }
    const auto& layout = fixture.repository.context.repo;
    const auto loaded = va::load_recorded_manifest(fixture.store, *fixture.repository.files, layout, b_uuid, 0);
    CHECK(!loaded && loaded.error().failure_class == "RepositoryError" &&
          loaded.error().message == "revision 0 is not recorded" && !loaded.error().unusable);
    const auto exported = va::export_archive(fixture.store, *fixture.repository.files, *fixture.repository.writer,
        layout, b_uuid, layout.root.parent_path() / "export-b", test::not_cancelled(), test::no_progress());
    const bool not_recorded = !exported && exported.error().message == "RepositoryError: revision 0 is not recorded";
    CHECK(not_recorded);
    if (!not_recorded && !exported) {
        std::cerr << "  B exported: " << exported.error().message << '\n';
    }
}

// A current revision older than the highest known (R-15) is no manifest that
// cannot be used (sample data: the archive's record set back to revision 1 of
// 2), and keeps what it did before decision 0028: the verification by uuid
// finds its media and FAILS with ManifestRevisionRollback, the media read and
// its evidence recorded.
void a_rolled_back_revision_looks_for_the_media()
{
    {
        Registered fixture;
        va::RegisterRequest again;
        again.root = fixture.archive.root();
        again.new_revision = true;
        CHECK(fixture.register_root(again).has_value());
        fixture.repository.execute("UPDATE archives SET current_revision = 1");
        const auto evidence_before = count_rows(fixture, "media_evidence");
        va::VerifyRequest by_uuid;
        by_uuid.archive_uuid = fixture.archive_uuid;
        const auto rolled_back = fixture.verify(by_uuid);
        CHECK(rolled_back && rolled_back->result == "FAILED" && rolled_back->media_read &&
              rolled_back->revision == 1 &&
              rolled_back->failure_classes == std::vector<std::string>{"ManifestRevisionRollback"} &&
              rolled_back->warnings ==
                  std::vector<std::string>{"manifest: revision 1 is older than the known revision 2"});
        CHECK(count_rows(fixture, "media_evidence") == evidence_before + 1);
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
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        *fixture.repository.writer, fixture.processes};
    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = fixture.archive_uuid;
    const auto stopped = va::verify_full(services, by_uuid, fixture.cancel, fixture.progress);
    CHECK(!stopped && stopped.error().kind == va::VerifyStop::Kind::Repository &&
          stopped.error().message ==
              restore_message(fixture.archive_uuid, "cannot be read: access denied (sample)"));

    // Retired as well (decision 0028, point 9): matching by root leaves out a
    // retired archive whose latest manifest is lost, but named by its uuid
    // the archive is the one candidate, and a manifest that goes missing
    // after its check (removed as its first line is opened for matching, the
    // second opening, sample damage) stops the verification with the plain
    // message alone, never as no registered archive; for a retired archive
    // it ends with the import. Nothing is recorded.
    auto archive = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(archive && *archive);
    if (!archive || !*archive) {
        return;
    }
    (*archive)->status = "RETIRED";
    CHECK(fixture.store.update_archive(**archive));
    const auto manifest = va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 1);
    test::HookedFiles removed(
        *fixture.repository.files, "r0001.vam", [&] { std::filesystem::remove(manifest); }, 2);
    va::VerifyServices race{fixture.archive, removed, fixture.media, fixture.store, fixture.repository.context.repo,
        fixture.repository.context.repository_uuid, fixture.host, *fixture.repository.writer, fixture.processes};
    const auto missing = va::verify_full(race, by_uuid, fixture.cancel, fixture.progress);
    CHECK(!missing && missing.error().kind == va::VerifyStop::Kind::Repository &&
          missing.error().message == "the manifest of revision 1 (manifests/" + fixture.archive_uuid +
                                         "/r0001.vam) is missing" + test::put_back_only(1));
    if (missing) {
        std::cerr << "  a retired archive verified by its uuid: " << missing->result << '\n';
    } else if (missing.error().kind != va::VerifyStop::Kind::Repository) {
        std::cerr << "  a retired archive verified by its uuid: " << missing.error().message << '\n';
    }
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->empty());
}

// Two archives on one volume (sample data): A at /vol/a and B at /vol/b. A's
// manifest is gone, or one byte of its header line changed. Named by its
// uuid, B is the one candidate and no other archive's manifest is read: it
// verifies, and its status reads with its header, as `archive show` reads it,
// and every archive's label without one, as `history` and the GUI's archive
// choices read them. A named by its uuid FAILS on its manifest, recorded
// (decision 0028). By root, every archive's latest carrier is needed where
// the root could be A's, which only its manifest names: A's root stops,
// naming A, what is wrong with its manifest, and to verify A by its uuid,
// and records nothing; B's root, plainly B's (its MATCH at its root),
// verifies (point 4).
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
        const auto a_by_root = fixture.verify(fixture.archive.root() / "a");
        CHECK(!a_by_root && a_by_root.error().kind == va::VerifyStop::Kind::Repository &&
              a_by_root.error().message ==
                  root_may_belong_to(a->label, a->archive_uuid, manifest_of(a->archive_uuid) + " " + what));
        const auto b_by_root = fixture.verify(fixture.archive.root() / "b");
        CHECK(b_by_root && b_by_root->archive_uuid == b->archive_uuid && b_by_root->result == "PASSED");
        const auto a_runs = fixture.store.verify_runs(a->archive_uuid);
        const auto b_runs = fixture.store.verify_runs(b->archive_uuid);
        CHECK(a_runs && a_runs->size() == 1 && a_runs->front().result == "FAILED" && b_runs && b_runs->size() == 2);
    }
}

// Decision 0028, point 4: an archive whose latest manifest cannot be used
// stops a verification by root only where the root could be its. Two
// archives on one volume (sample data): A at /vol/a, whose manifest is gone,
// and B at /vol/b. B's root, whose volume has the filesystem id and the
// device that B's manifest records (MATCH), is B's: it verifies, also with B
// retired, and with the index's record of A's manifest lost as well. Every
// other root stops, naming A, recording nothing (no run, no media
// evidence): A's own root and a folder that is no archive's root (UNKNOWN
// among the others), and B's root with no device serial to compare
// (MATCH_WEAK: a sector-by-sector clone reads the same), with no filesystem
// id to compare (MATCH_WEAK by the device: a later filesystem on it reads the
// same), on another device (MIGRATED), on another filesystem of its device
// (MISMATCH), with an identity file that names A, and registered twice
// (AMBIGUOUS). With A's manifest restored, A's root verifies.
void a_damaged_archive_stops_only_a_root_that_could_be_its()
{
    EngineFixture fixture;
    fixture.archive.add_file("a/one.txt", "a (sample data)");
    fixture.archive.add_file("b/two.txt", "b (sample data)");
    fixture.archive.add_file("c/three.txt", "c (sample data)");
    const auto a = fixture.register_root(fixture.archive.root() / "a");
    const auto b = fixture.register_root(fixture.archive.root() / "b");
    CHECK(a && b);
    if (!a || !b) {
        return;
    }
    const auto& layout = fixture.repository.context.repo;
    const auto path = va::manifest_path(layout.manifests(), a->archive_uuid, 1);
    const auto kept = file_bytes(path);
    std::filesystem::remove(path);
    const auto stopped = root_may_belong_to(a->label, a->archive_uuid, manifest_of(a->archive_uuid) + " is missing");
    const auto stops = [&](const std::filesystem::path& root, const char* what) {
        const auto runs = count_rows(fixture, "verify_runs");
        const auto evidence = count_rows(fixture, "media_evidence");
        const auto verified = fixture.verify(root);
        CHECK(!verified && verified.error().kind == va::VerifyStop::Kind::Repository &&
              verified.error().message == stopped);
        if (verified) {
            std::cerr << "  " << what << ": verified, " << verified->result << '\n';
        } else if (verified.error().message != stopped) {
            std::cerr << "  " << what << ": " << verified.error().message << '\n';
        }
        CHECK(count_rows(fixture, "verify_runs") == runs && count_rows(fixture, "media_evidence") == evidence);
    };
    const auto verifies = [&](const char* what) {
        const auto verified = fixture.verify(fixture.archive.root() / "b");
        CHECK(verified && verified->archive_uuid == b->archive_uuid && verified->result.starts_with("PASSED") &&
              verified->identity.result == va::MatchResult::Match);
        if (!verified) {
            std::cerr << "  " << what << ": " << verified.error().message << '\n';
        }
    };
    const auto volume = fixture.media.volume;

    verifies("B's root");
    stops(fixture.archive.root() / "a", "A's root");
    stops(fixture.archive.root() / "c", "no archive's root");
    fixture.media.volume.devices.front().serial.clear();
    stops(fixture.archive.root() / "b", "no device serial");
    fixture.media.volume = volume;
    fixture.media.volume.filesystem_uuid.clear();
    fixture.media.volume.filesystem_uuid_short.clear();
    stops(fixture.archive.root() / "b", "no filesystem id");
    fixture.media.volume = volume;
    fixture.media.volume.devices.front().serial = "OTHER-SERIAL";
    stops(fixture.archive.root() / "b", "another device");
    fixture.media.volume = volume;
    fixture.media.volume.filesystem_uuid = "9999999999999999";
    fixture.media.volume.filesystem_uuid_short = "99999999";
    stops(fixture.archive.root() / "b", "another filesystem");
    fixture.media.volume = volume;
    fixture.archive.add_file("b/.veriarchive/identity.json", "{\"archive_uuid\":\"" + a->archive_uuid + "\"}");
    stops(fixture.archive.root() / "b", "an identity file naming A");
    fixture.archive.remove("b/.veriarchive/identity.json");

    auto archive_b = fixture.store.find_archive(b->archive_uuid);
    CHECK(archive_b && *archive_b);
    if (archive_b && *archive_b) {
        (*archive_b)->status = "RETIRED";
        CHECK(fixture.store.update_archive(**archive_b));
        verifies("B retired");
        (*archive_b)->status = "REGISTERED";
        CHECK(fixture.store.update_archive(**archive_b));
    }
    fixture.repository.execute(("DELETE FROM archive_revisions WHERE archive_uuid = '" + a->archive_uuid + "'").c_str());
    verifies("A's manifest not indexed");

    // A's manifest restored (and the index rebuilt): A's root verifies.
    write_bytes(path, kept);
    const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, layout, test::not_cancelled(), test::no_progress());
    CHECK(rebuilt && rebuilt->skipped.empty() && rebuilt->revisions == 2);
    const auto a_verified = fixture.verify(fixture.archive.root() / "a");
    CHECK(a_verified && a_verified->archive_uuid == a->archive_uuid && a_verified->result == "PASSED");

    // B's root registered twice on purpose (sample data), then A's manifest
    // gone again: AMBIGUOUS among the others, so the root could be A's too.
    va::RegisterRequest twice;
    twice.root = fixture.archive.root() / "b";
    twice.force_new_archive = true;
    CHECK(fixture.register_root(twice).has_value());
    std::filesystem::remove(path);
    stops(fixture.archive.root() / "b", "B's root registered twice");
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
            *fixture.repository.writer, layout, fixture.archive_uuid, layout.root.parent_path() / "export", test::not_cancelled(), test::no_progress());
        CHECK(!exported && exported.error().message == "ManifestHashMismatch: " + message);
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
// other bytes (sample data: one hex digit of the first file's BLAKE3; a quote
// made an apostrophe, so that a line no longer parses; or a name that takes
// an entry out of canonical order), the verification fails with
// ManifestHashMismatch, the manifest having given other bytes when it was
// read again (decision 0028), recorded in the history, rather than compare
// the archive with entries the manifest does not hold.
void entries_read_otherwise_than_validated_fail()
{
    const auto b3 = va::blake3_hex("report bytes"); // the first file, docs/report.pdf
    auto other = b3;
    other[0] = other[0] == '0' ? '1' : '0';
    const std::pair<std::string, std::string> read_otherwise[] = {
        {"\"b3\":\"" + b3, "\"b3\":\"" + other},
        {"\"b3\":\"" + b3, "\"b3\":'" + b3},
        {"\"p\":\"notes.txt\"", "\"p\":\"aaaaa.txt\""}, // after "empty"
    };
    for (const auto& [from, to] : read_otherwise) {
        Registered fixture;
        const auto path = va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 1);
        const auto bytes = file_bytes(path);
        // Its first opening reads line 1 alone (the candidates); the second loads it.
        test::SecondReadDiffers repository_files(*fixture.repository.files, "r0001.vam", 2, bytes.find('\n') + 1,
            from, to);
        va::VerifyServices services{fixture.archive, repository_files, fixture.media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
            *fixture.repository.writer, fixture.processes};
        va::VerifyRequest request;
        request.root = fixture.archive.root();
        const auto outcome = va::verify_full(services, request, fixture.cancel, fixture.progress);
        CHECK(repository_files.differed());
        CHECK(outcome && outcome->result == "FAILED" &&
              outcome->failure_classes == std::vector<std::string>{"ManifestHashMismatch"} &&
              outcome->items.empty());
        // By root, the media was looked at to find the archive (decision 0028).
        const std::vector<std::string> warnings{"manifest: " +
            restore_message(fixture.archive_uuid, "has changed: it gave other bytes when it was read again")};
        CHECK(outcome && outcome->media_read && outcome->warnings == warnings);
        if (outcome && outcome->warnings != warnings && !outcome->warnings.empty()) {
            std::cerr << "  read as " << to << ": " << outcome->warnings.front() << '\n';
        }
        const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
        CHECK(runs && runs->size() == 1 && runs->front().failure_classes == "ManifestHashMismatch");
    }
}

// ---- Verifying by chunk (decision 0032) -------------------------------------

constexpr std::uint64_t test_chunk_size = 64 * 1024;

// An archive registered with 64 KiB chunks, or with `chunk_size` (0: none, as
// V1 registered): big.bin of three chunks, the last 100 bytes long, a small
// file, and the files of `more` (sample data).
struct Chunked : EngineFixture {
    va::Uuid archive_uuid;
    std::string big = test::patterned(2 * test_chunk_size + 100, 1);

    explicit Chunked(std::uint64_t chunk_size = test_chunk_size,
        const std::vector<std::pair<std::string, std::string>>& more = {})
    {
        archive.add_file("big.bin", big);
        archive.add_file("notes.txt", "notes (sample data)");
        for (const auto& [p, content] : more) {
            archive.add_file(p, content);
        }
        va::RegisterRequest request;
        request.root = archive.root();
        request.chunk_size = chunk_size;
        const auto registered = register_root(request);
        CHECK(registered.has_value());
        archive_uuid = registered ? registered->archive_uuid : va::Uuid{};
    }

    // big.bin as registered, but for the byte at each of `offsets`.
    void change_big(std::initializer_list<std::uint64_t> offsets)
    {
        auto content = big;
        for (const auto offset : offsets) {
            content[offset] = static_cast<char>(content[offset] ^ 0x01);
        }
        archive.node("big.bin").content = content;
    }
};

// The item of `p`, if it is not Verified.
const va::VerifyItemRecord* item_of(const va::VerifyOutcome& outcome, const std::string& p)
{
    const auto found = std::ranges::find_if(outcome.items, [&](const auto& item) { return item.path.p == p; });
    return found == outcome.items.end() ? nullptr : &*found;
}

// A file whose content changed names the chunks that differ, from the read
// that hashes it whole (R-101): each file is read once. The detail says which
// chunks of how many differ and from which byte the first begins; the item
// gives their idx, their number and that offset.
void a_modified_file_names_the_chunks_that_differ()
{
    Chunked fixture;
    fixture.change_big({test_chunk_size + 1000}); // one byte in chunk 1
    fixture.archive.bytes_read = 0;
    auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "FAILED");
    const auto* item = outcome ? item_of(*outcome, "big.bin") : nullptr;
    CHECK(item && item->status == "Modified" &&
          item->detail == "content differs: chunk 1 of 3 differs, from byte 65,536" &&
          item->differing_chunks == std::vector<std::uint64_t>{1} && item->chunks == 3 &&
          item->first_differing_offset == test_chunk_size);
    if (item && item->detail != "content differs: chunk 1 of 3 differs, from byte 65,536") {
        std::cerr << "  detail: " << item->detail << '\n';
    }
    CHECK(outcome && outcome->items.size() == 1);
    CHECK(fixture.archive.bytes_read == fixture.big.size() + std::string("notes (sample data)").size());

    // Two chunks, the first and the last, shorter one.
    fixture.change_big({10, 2 * test_chunk_size + 50});
    outcome = fixture.verify(fixture.archive.root());
    item = outcome ? item_of(*outcome, "big.bin") : nullptr;
    CHECK(item && item->detail == "content differs: chunks 0 and 2 of 3 differ, the first from byte 0" &&
          item->differing_chunks == (std::vector<std::uint64_t>{0, 2}) && item->first_differing_offset == 0);
    fixture.change_big({2 * test_chunk_size + 99});
    outcome = fixture.verify(fixture.archive.root());
    item = outcome ? item_of(*outcome, "big.bin") : nullptr;
    CHECK(item && item->detail == "content differs: chunk 2 of 3 differs, from byte 131,072" &&
          item->first_differing_offset == 2 * test_chunk_size);
}

// The chunks that differ are named in runs of consecutive chunks, at most ten
// runs, and the item holds every one of them (sample data: a file of 40
// chunks).
void the_chunks_that_differ_are_named_in_runs()
{
    EngineFixture fixture;
    const auto original = test::patterned(40 * test_chunk_size, 2);
    fixture.archive.add_file("disk.img", original);
    va::RegisterRequest request;
    request.root = fixture.archive.root();
    request.chunk_size = test_chunk_size;
    CHECK(fixture.register_root(request).has_value());
    const auto modified = [&](std::initializer_list<std::uint64_t> chunks) {
        auto content = original;
        for (const auto chunk : chunks) {
            content[chunk * test_chunk_size + 7] = static_cast<char>(content[chunk * test_chunk_size + 7] ^ 0x01);
        }
        fixture.archive.node("disk.img").content = content;
        const auto outcome = fixture.verify(fixture.archive.root());
        const auto* item = outcome ? item_of(*outcome, "disk.img") : nullptr;
        const bool all_named = item && item->differing_chunks == std::vector<std::uint64_t>(chunks) && item->chunks == 40;
        CHECK(all_named);
        return item ? item->detail : std::string{};
    };
    CHECK(modified({1, 3, 5}) == "content differs: chunks 1, 3 and 5 of 40 differ, the first from byte 65,536");
    CHECK(modified({1, 2, 3, 5}) == "content differs: chunks 1-3 and 5 of 40 differ, the first from byte 65,536");
    CHECK(modified({20, 21}) == "content differs: chunks 20-21 of 40 differ, the first from byte 1,310,720");
    const auto eleven_runs = modified({1, 2, 3, 5, 8, 9, 12, 14, 16, 18, 20, 22, 24, 26, 27, 28, 29});
    CHECK(eleven_runs == "content differs: chunks 1-3, 5, 8-9, 12, 14, 16, 18, 20, 22, 24 and 4 more of 40 differ, "
                         "the first from byte 65,536");
    if (eleven_runs.find("and 4 more") == std::string::npos) {
        std::cerr << "  detail: " << eleven_runs << '\n';
    }
}

// What a change of size, a small file, and a revision without a chunk size
// (as V1 registered it) say: their content is compared whole, as before.
void whole_files_are_compared_as_before()
{
    Chunked fixture;
    fixture.archive.node("big.bin").content = fixture.big + "x";
    fixture.archive.node("notes.txt").content = "notes (sample data!";
    auto outcome = fixture.verify(fixture.archive.root());
    const auto* big = outcome ? item_of(*outcome, "big.bin") : nullptr;
    const auto* notes = outcome ? item_of(*outcome, "notes.txt") : nullptr;
    CHECK(big && big->detail == "size differs" && big->differing_chunks.empty() && big->chunks == 0);
    CHECK(notes && notes->detail == "content differs" && notes->differing_chunks.empty());

    Chunked unchunked(0);
    unchunked.change_big({test_chunk_size + 1000});
    unchunked.archive.bytes_read = 0;
    outcome = unchunked.verify(unchunked.archive.root());
    big = outcome ? item_of(*outcome, "big.bin") : nullptr;
    CHECK(big && big->status == "Modified" && big->detail == "content differs" && big->differing_chunks.empty());
    CHECK(unchunked.archive.bytes_read == unchunked.big.size() + std::string("notes (sample data)").size());
}

// Writes revision 1 of `fixture`'s archive again with its entry of big.bin
// as `edit` makes it, and rebuilds the index from it: a manifest valid in
// itself, its body hash and Merkle root those of what it holds, but written
// wrong, as by a fault of the computer that registered it (sample data).
void write_big_wrong(Chunked& fixture, const std::function<void(va::Entry&)>& edit)
{
    auto loaded = va::load_recorded_manifest(
        fixture.store, *fixture.repository.files, fixture.repository.context.repo, fixture.archive_uuid, 1);
    CHECK(loaded.has_value());
    if (!loaded) {
        return;
    }
    for (auto& entry : loaded->entries) {
        if (entry.path.p == "big.bin") {
            edit(entry);
        }
    }
    const auto path = va::manifest_path(fixture.repository.context.repo.manifests(), fixture.archive_uuid, 1);
    va::ManifestWriter writer(*fixture.repository.writer, *fixture.repository.files, path, loaded->header.chunk_size);
    for (const auto& entry : loaded->entries) {
        CHECK(writer.add(entry).has_value());
    }
    auto written = writer.finish(loaded->header);
    CHECK(written && written->file->commit());
    const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, fixture.repository.context.repo,
        test::not_cancelled(), test::no_progress());
    CHECK(rebuilt && rebuilt->revisions == 1 && rebuilt->skipped.empty());
}

// Revision 1 written again with the hash of each chunk of big.bin in `wrong`
// replaced (sample data).
void write_wrong_chunk_hashes(Chunked& fixture, std::initializer_list<std::size_t> wrong)
{
    write_big_wrong(fixture, [&](va::Entry& entry) {
        entry.chunks = test::chunk_hashes(fixture.big, test_chunk_size);
        for (const auto idx : wrong) {
            entry.chunks.at(idx) = va::blake3_hex("not chunk " + std::to_string(idx) + " (sample data)");
        }
    });
}

// Revision 1 written again with big.bin's whole-file BLAKE3, its SHA-256 or
// both replaced, and its chunk hashes as they were read (sample data).
void write_wrong_file_hashes(Chunked& fixture, bool blake3, bool sha256)
{
    write_big_wrong(fixture, [&](va::Entry& entry) {
        entry.chunks = test::chunk_hashes(fixture.big, test_chunk_size);
        entry.b3 = blake3 ? va::blake3_hex("not big.bin (sample data)") : va::blake3_hex(fixture.big);
        entry.sha256 = sha256 ? va::sha256_hex("not big.bin (sample data)") : va::sha256_hex(fixture.big);
    });
}

// A file whose content matches both its file hashes while a chunk hash of it
// does not is fine: its manifest was written wrong (decision 0032). It is
// Verified, its bytes read and matched, and the run warns that its chunk
// hashes cannot be relied on and that a new revision fixes them, which makes
// it PASSED_WITH_WARNINGS, also beside a difference of its metadata. A new
// revision reads the file again and records chunk hashes that match it.
void chunk_hashes_that_do_not_match_a_file_that_matches_warn()
{
    Chunked fixture;
    write_wrong_chunk_hashes(fixture, {1});
    auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS" && outcome->items.empty() &&
          outcome->counts.at("Verified") == 2 && outcome->coverage_bytes == outcome->registered_bytes);
    const std::vector<std::string> one{"the chunk hashes of big.bin cannot be relied on: chunk 1 of 3 does not match, "
                                       "while the whole file matches its hashes; the file is fine, and a new revision "
                                       "fixes them"};
    CHECK(outcome && outcome->warnings == one);
    if (outcome && outcome->warnings != one) {
        for (const auto& warning : outcome->warnings) {
            std::cerr << "  warning: " << warning << '\n';
        }
    }
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1 && runs->back().result == "PASSED_WITH_WARNINGS");

    write_wrong_chunk_hashes(fixture, {0, 2});
    fixture.archive.node("big.bin").mtime_ns += 5'000'000'000;
    outcome = fixture.verify(fixture.archive.root());
    const auto* big = outcome ? item_of(*outcome, "big.bin") : nullptr;
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS" && big && big->status == "MetadataMismatch");
    CHECK(outcome && outcome->warnings == std::vector<std::string>{"the chunk hashes of big.bin cannot be relied on: "
                                                                   "chunks 0 and 2 of 3 do not match, while the whole "
                                                                   "file matches its hashes; the file is fine, and a "
                                                                   "new revision fixes them"});

    va::RegisterRequest next;
    next.root = fixture.archive.root();
    next.new_revision = true;
    CHECK(fixture.register_root(next).has_value());
    outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->revision == 2 && outcome->result == "PASSED" && outcome->warnings.empty());
}

// The warning of a file whose whole-file `hash` (BLAKE3 or SHA-256) cannot be
// relied on, while each of its 3 chunks matches (decision 0032).
std::string unreliable_file_hash(const std::string& hash)
{
    return "the whole-file " + hash + " hash of big.bin cannot be relied on: it does not match, while each of the "
           "file's 3 chunks matches its chunk hash; the file is fine, and a new revision fixes it";
}

// The other half (decision 0032): a file with chunk lines whose every chunk
// hash matches what was read while its whole-file BLAKE3, its SHA-256 or both
// do not is fine as well: its manifest was written wrong. It is Verified, its
// bytes read once and matched, and the run warns that the whole-file hash
// cannot be relied on and that a new revision fixes it, which makes it
// PASSED_WITH_WARNINGS, also beside a difference of its metadata. A file
// whose chunks do not all match stays Modified, naming them, whatever its
// file hashes. A new revision records hashes that match the file.
void a_file_hash_that_does_not_match_a_file_whose_chunks_match_warns()
{
    Chunked fixture;
    write_wrong_file_hashes(fixture, true, false);
    fixture.archive.bytes_read = 0;
    auto outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS" && outcome->items.empty() &&
          outcome->counts.at("Verified") == 2 && outcome->coverage_bytes == outcome->registered_bytes);
    CHECK(outcome && outcome->warnings == std::vector<std::string>{unreliable_file_hash("BLAKE3")});
    if (outcome && outcome->warnings != std::vector<std::string>{unreliable_file_hash("BLAKE3")}) {
        for (const auto& warning : outcome->warnings) {
            std::cerr << "  warning: " << warning << '\n';
        }
    }
    CHECK(fixture.archive.bytes_read == fixture.big.size() + std::string("notes (sample data)").size());
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1 && runs->back().result == "PASSED_WITH_WARNINGS");

    write_wrong_file_hashes(fixture, false, true);
    fixture.archive.node("big.bin").mtime_ns += 5'000'000'000;
    outcome = fixture.verify(fixture.archive.root());
    const auto* big = outcome ? item_of(*outcome, "big.bin") : nullptr;
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS" && big && big->status == "MetadataMismatch" &&
          outcome->warnings == std::vector<std::string>{unreliable_file_hash("SHA-256")});

    write_wrong_file_hashes(fixture, true, true);
    outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS" &&
          outcome->warnings == std::vector<std::string>{"the whole-file BLAKE3 and SHA-256 hashes of big.bin cannot "
                                                        "be relied on: they do not match, while each of the file's "
                                                        "3 chunks matches its chunk hash; the file is fine, and a new "
                                                        "revision fixes them"});

    // Chunks that do not all match: Modified, whatever the file hashes say.
    fixture.change_big({test_chunk_size + 1000});
    outcome = fixture.verify(fixture.archive.root());
    big = outcome ? item_of(*outcome, "big.bin") : nullptr;
    CHECK(outcome && outcome->result == "FAILED" && outcome->warnings.empty() && big && big->status == "Modified" &&
          big->detail == "content differs: chunk 1 of 3 differs, from byte 65,536");

    fixture.archive.node("big.bin").content = fixture.big;
    va::RegisterRequest next;
    next.root = fixture.archive.root();
    next.new_revision = true;
    CHECK(fixture.register_root(next).has_value());
    outcome = fixture.verify(fixture.archive.root());
    CHECK(outcome && outcome->revision == 2 && outcome->result == "PASSED" && outcome->warnings.empty());
}

// R-104 with the other half (decision 0032): a file whose whole-file hash
// cannot be relied on is not checkpointed, so that the resume of a run
// interrupted after it (the device gone at the file after the next) reads it
// again, and only it and the file not reached (by bytes), and warns again.
void a_resumed_verification_warns_again_of_a_file_hash()
{
    const std::string last = "last (sample data)";
    Chunked fixture(test_chunk_size, {{"z.txt", last}});
    write_wrong_file_hashes(fixture, false, true);
    fixture.archive.node("z.txt").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.verify(fixture.archive.root());
    CHECK(interrupted && interrupted->result == "INCOMPLETE");
    if (!interrupted) {
        return;
    }
    va::Entry notes;
    notes.path.p = "notes.txt";
    const auto saved = fixture.store.progress(interrupted->run_id);
    CHECK(saved && *saved == std::set<std::string>{va::entry_key(notes)});

    fixture.archive.node("z.txt").open_error.reset();
    fixture.archive.bytes_read = 0;
    va::VerifyRequest request;
    request.root = fixture.archive.root();
    request.resume_run = interrupted->run_id;
    const auto resumed = fixture.verify(request);
    CHECK(resumed && resumed->result == "PASSED_WITH_WARNINGS" && resumed->resumed_from == interrupted->run_id);
    if (!resumed) {
        return;
    }
    CHECK(fixture.archive.bytes_read == fixture.big.size() + last.size());
    CHECK(resumed->warnings == std::vector<std::string>{unreliable_file_hash("SHA-256")});
}

// R-104 with decision 0032: a resumed FULL verify compares by chunk. A run
// interrupted at its last file (the device gone) checkpointed only what
// matched by file and by chunk: notes.txt, neither big.bin, whose chunk hashes
// cannot be relied on, nor mod.bin, whose content differs in chunk 2. The
// resume reads both again, and only them and the file not reached (by bytes):
// it warns for the first and names the chunk that differs in the second
// (sample data).
void a_resumed_verification_compares_by_chunk()
{
    const auto mod = test::patterned(2 * test_chunk_size + 100, 3);
    const std::string last = "last (sample data)";
    Chunked fixture(test_chunk_size, {{"mod.bin", mod}, {"z.txt", last}});
    write_wrong_chunk_hashes(fixture, {1});
    auto& changed = fixture.archive.node("mod.bin").content;
    changed[2 * test_chunk_size + 10] = static_cast<char>(changed[2 * test_chunk_size + 10] ^ 0x01);
    fixture.archive.node("z.txt").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.verify(fixture.archive.root());
    CHECK(interrupted && interrupted->result == "INCOMPLETE");
    if (!interrupted) {
        return;
    }
    va::Entry notes;
    notes.path.p = "notes.txt";
    const auto saved = fixture.store.progress(interrupted->run_id);
    CHECK(saved && *saved == std::set<std::string>{va::entry_key(notes)});

    fixture.archive.node("z.txt").open_error.reset();
    fixture.archive.bytes_read = 0;
    va::VerifyRequest request;
    request.root = fixture.archive.root();
    request.resume_run = interrupted->run_id;
    const auto resumed = fixture.verify(request);
    CHECK(resumed && resumed->result == "FAILED" && resumed->resumed_from == interrupted->run_id);
    if (!resumed) {
        return;
    }
    CHECK(fixture.archive.bytes_read == fixture.big.size() + mod.size() + last.size());
    const auto* item = item_of(*resumed, "mod.bin");
    CHECK(item && item->detail == "content differs: chunk 2 of 3 differs, from byte 131,072" &&
          resumed->items.size() == 1);
    CHECK(resumed->warnings == std::vector<std::string>{"the chunk hashes of big.bin cannot be relied on: chunk 1 of "
                                                        "3 does not match, while the whole file matches its hashes; "
                                                        "the file is fine, and a new revision fixes them"});
    CHECK(resumed->counts.at("Verified") == 3);
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
    CHECK(std::ranges::count(outcome.warnings, "identity: the filesystem id cannot be read") == 0);
    CHECK(std::ranges::count(outcome.warnings, "identity: filesystem-id-unavailable") == 0);
}

// What matching noted of the media is a warning in plain words, while the
// identity's details keep their codes (decision 0017, amended 2026-10-27).
void identity_details_warn_in_plain_words()
{
    Registered fixture;
    fixture.media.volume.volume_label = "RENAMED SAMPLE";
    const auto outcome = fixture.verify_ok();
    CHECK(outcome.result == "PASSED_WITH_WARNINGS");
    CHECK(std::ranges::count(outcome.identity.details, "volume-label-differs") == 1);
    CHECK(std::ranges::count(outcome.warnings, "identity: the volume label differs from the one recorded") == 1);
    CHECK(std::ranges::count(outcome.warnings, "identity: volume-label-differs") == 0);

    // The other notes, each compared with the identity that the verified
    // revision records (decision 0042); sample values.
    const std::vector<std::pair<std::function<void(va::VolumeInfo&)>, std::string>> changes{
        {[](va::VolumeInfo& volume) { volume.capacity_bytes = 2000000; },
            "the capacity differs from the one recorded"},
        {[](va::VolumeInfo& volume) { volume.filesystem_type = "refs"; },
            "the filesystem type differs from the one recorded"},
        {[](va::VolumeInfo& volume) { volume.devices.front().model = "Other Sample Disk"; },
            "the device model differs from the one recorded"},
    };
    for (const auto& [change, words] : changes) {
        Registered changed;
        change(changed.media.volume);
        const auto noted = changed.verify_ok();
        CHECK(std::ranges::count(noted.warnings, "identity: " + words) == 1);
    }
}

void verify_engine_tests()
{
    unchanged_archive_passes();
    progress_counts_bytes();
    content_changes_fail();
    a_type_change_to_or_from_a_folder_with_contents_is_reported();
    mount_points_below_the_root_are_reported();
    items_are_in_canonical_order();
    unreadable_files_fail_with_their_reason();
    a_read_the_system_aborted_fails_the_run();
    a_cancelled_error_is_never_left_out();
    metadata_changes_warn();
    mode_and_attributes_count_only_under_strict_metadata();
    a_root_that_cannot_be_scanned_fails_with_a_filesystem_access_error();
    hardlinks_and_sparse_files_are_hashed_by_content();
    known_unreadable_entries_warn();
    tampered_manifest_fails();
    identity_outcomes();
    unreadable_filesystem_id_warns();
    identity_details_warn_in_plain_words();
    verify_by_archive_uuid();
    an_empty_folder_asks_whether_its_device_is_attached();
    unrepresentable_names_warn();
    a_name_registered_on_this_system_that_is_gone_is_missing();
    recorded_portability_warnings_make_the_run_pass_with_warnings();
    the_mount_driver_is_only_noted();
    link_targets_across_systems();
    interrupted_run_resumes_without_rehashing();
    a_device_that_goes_away_leaves_the_run_incomplete();
    a_device_that_goes_away_at_the_root_or_a_link_leaves_the_run_incomplete();
    a_device_that_stops_answering_leaves_the_run_incomplete();
    io_errors_stop_a_run_only_ten_in_a_row();
    a_watch_tells_a_device_that_stopped_answering();
    a_volume_is_the_same_while_its_ids_and_devices_are();
    the_rotational_override_applies_to_a_verification();
    the_io_option_wins_in_a_verification();
    resume_refused_after_new_revision();
    a_resume_by_uuid_stops_where_several_volumes_hold_the_archive();
    clones_registered_as_two_archives_say_to_detach_the_other_volumes();
    run_record_says_where_and_how_much();
    archive_found_by_uuid_is_read_at_its_folder();
    an_archive_whose_folder_is_gone_is_not_found();
    an_archive_on_a_volume_known_by_its_guid_path_verifies_by_uuid();
    an_index_that_differs_from_its_manifest_is_never_taken();
    a_manifest_that_is_not_the_one_recorded_stops_every_use_by_root();
    a_known_archive_whose_manifest_cannot_be_used_fails_unread();
    an_archive_at_a_revision_the_index_has_no_record_of();
    an_unfinished_first_registration_is_no_manifest_that_cannot_be_used();
    a_rolled_back_revision_looks_for_the_media();
    a_resume_whose_manifest_cannot_be_used_fails_and_keeps_the_run();
    a_named_archive_is_never_the_one_that_may_be_at_a_root();
    a_named_archive_reads_no_other_manifest();
    a_damaged_archive_stops_only_a_root_that_could_be_its();
    a_first_line_that_no_longer_parses_has_changed();
    entries_read_otherwise_than_validated_fail();
    a_modified_file_names_the_chunks_that_differ();
    the_chunks_that_differ_are_named_in_runs();
    whole_files_are_compared_as_before();
    chunk_hashes_that_do_not_match_a_file_that_matches_warn();
    a_file_hash_that_does_not_match_a_file_whose_chunks_match_warns();
    a_resumed_verification_warns_again_of_a_file_hash();
    a_resumed_verification_compares_by_chunk();
    item_rules();
}
