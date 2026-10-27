#include "test_support.h"

#include "engine_fixture.h"

#include "common/time.h"
#include "core/engine/abandon_registration.h"
#include "core/engine/archive_status.h"
#include "core/engine/heartbeat.h"
#include "core/engine/rebuild_index.h"
#include "core/engine/relocate_engine.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <semaphore>
#include <set>
#include <thread>

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

// A malformed value is a usage error (exit 10), not an operator decision.
void malformed_values_are_invalid()
{
    Fixture fixture;
    fixture.archive.add_file("a.txt", "a");
    auto request = at("/vol");
    request.label = "OS00222-TEST";
    const auto label = fixture.run(request);
    CHECK(!label && label.error().kind == va::RegisterFailure::Kind::Invalid);

    request = at("/vol");
    request.copy_of = "not-a-uuid";
    const auto copy_of = fixture.run(request);
    CHECK(!copy_of && copy_of.error().kind == va::RegisterFailure::Kind::Invalid);
    CHECK(!fixture.store.archives() || fixture.store.archives()->empty());
}

// R-97 still holds where the filesystem id cannot be read (decision 0018):
// the device and the volume size recognise the registered volume.
void registered_root_without_filesystem_id_needs_a_flag()
{
    Fixture fixture;
    fixture.media.volume.filesystem_uuid.clear();
    fixture.media.volume.filesystem_uuid_short.clear();
    fixture.archive.add_file("a.txt", "a");
    CHECK(fixture.run(at("/vol")).has_value());
    const auto again = fixture.run(at("/vol"));
    CHECK(!again && again.error().kind == va::RegisterFailure::Kind::Refused);
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

// Decision 0005: a copy stays a copy. A new revision of an archive registered
// with --copy-of names the same original, as the latest revision's manifest
// names it (R-14), also when the new registration does not say so again.
void a_new_revision_of_a_copy_stays_a_copy()
{
    Fixture fixture;
    fixture.archive.add_file("a.txt", "a (sample data)");
    const va::Uuid original = "c0ffee00-0000-4000-8000-000000000005"; // sample data
    auto request = at("/vol");
    request.copy_of = original;
    CHECK(fixture.run(request).has_value());
    fixture.archive.add_file("b.txt", "b (sample data)");
    auto next = at("/vol");
    next.new_revision = true;
    const auto second = fixture.run(next);
    CHECK(second && second->revision == 2);
    if (second) {
        auto reader = va::ManifestReader::open(*fixture.repository.files, second->manifest);
        CHECK(reader && reader->header().copy_of == std::optional<va::Uuid>(original));
    }
}

// Passes everything to `inner`, except the `nth` opening of a file named
// `name`, which fails as a file that cannot be read.
class FailingOpen final : public va::IFileSystemAccess {
public:
    FailingOpen(va::IFileSystemAccess& inner, std::string name, int nth)
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
        if (va::path_to_utf8(file.filename()) == name_ && ++opened_ == nth_) {
            return std::unexpected(va::IoError{va::IoErrorKind::PermissionDenied, 5, "denied (sample)"});
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

// A new revision takes the original it is a copy of from the latest
// revision's manifest. When that manifest cannot be read then (here at its
// second opening, after the candidates were read from it), the registration
// stops rather than drop the original, or take it from the index (R-14).
void a_new_revision_reads_the_latest_header_from_its_manifest()
{
    Fixture fixture;
    fixture.archive.add_file("a.txt", "a (sample data)");
    const auto first = fixture.run(at("/vol"));
    CHECK(first.has_value());
    if (!first) {
        return;
    }
    FailingOpen repository_files(*fixture.repository.files, "r0001.vam", 2);
    va::RegisterServices services{fixture.archive, repository_files, *fixture.repository.writer,
        fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    auto next = at("/vol");
    next.new_revision = true;
    const auto registered = va::register_archive(services, next, fixture.cancel, fixture.progress);
    CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Repository &&
          registered.error().message == "the manifest of revision 1 (manifests/" + first->archive_uuid +
                                            "/r0001.vam) cannot be read: denied (sample)" +
                                            test::put_back(first->archive_uuid, 1));
    const auto plans = fixture.store.settings_with_prefix("registration:");
    CHECK(plans && plans->empty());
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

// Decision 0034: a device that goes away during a registration interrupts it
// (REGISTRATION_INCOMPLETE, resumable), also where its reads fail only with
// I/O errors, as Linux gives for a USB disk pulled out: the volume is looked
// up again, and it is gone, so no file is recorded unreadable for it and no
// revision is recorded. The same while the folders are scanned. Attached
// again, the resume reads only the files not hashed before, and records
// every file with its hashes. Sample data.
void a_device_that_goes_away_interrupts_the_registration()
{
    for (const bool while_scanning : {false, true}) {
        Fixture fixture;
        for (int index = 1; index <= 6; ++index) {
            fixture.archive.add_file(std::format("SAMPLE-dir/SAMPLE-{}.bin", index),
                std::string(1000, static_cast<char>('a' + index)));
        }
        test::RemovableDevice device(fixture.archive);
        if (while_scanning) {
            device.directory = "SAMPLE-dir";
        } else {
            device.file = "SAMPLE-4.bin";
            device.at_offset = 500;
        }
        device.on_removed = [&fixture] {
            fixture.media.error = va::IoError{va::IoErrorKind::NotFound, 2, "no volume at the root (sample)"};
        };
        fixture.media_files = &device;
        const auto interrupted = fixture.run(at("/vol"));
        CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
        const auto archives = fixture.store.archives();
        CHECK(archives && archives->size() == 1 && archives->front().status == "REGISTRATION_INCOMPLETE" &&
              archives->front().current_revision == 0);

        device.attach_again();
        fixture.media.error.reset();
        fixture.archive.bytes_read = 0;
        auto request = at("/vol");
        request.resume = true;
        const auto resumed = fixture.run(request);
        CHECK(resumed && resumed->revision == 1 && resumed->counts.files == 6 && resumed->unreadable.empty());
        // Only what was not hashed before the device went away is read again.
        CHECK(fixture.archive.bytes_read == (while_scanning ? 6000u : 3000u));
        const auto verified = fixture.verify(fixture.archive.root());
        CHECK(verified && verified->result == "PASSED");
    }
}

// A root typed with separators at its end, as shells complete a folder name,
// is the folder before them; a root keeps its own. A backslash separates only
// on Windows (R-40).
void trailing_separators_leave_the_folder()
{
    const auto folder = [](const char* path) {
        return va::path_to_utf8(va::without_trailing_separators(va::path_from_utf8(path)));
    };
    CHECK(folder("/vol/photos/") == "/vol/photos");
    CHECK(folder("/vol/photos//") == "/vol/photos");
    CHECK(folder("/vol/photos") == "/vol/photos");
    CHECK(folder("photos/") == "photos");
    CHECK(folder("/") == "/");
    CHECK(folder("").empty());
    // A volume's GUID path (decision 0029, point 3) is the root folder of its
    // volume, as "D:\" is: its own separator stays, in the \\?\ form only.
    // (Sample GUIDs.)
    const std::string guid_root = R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\)";
    if (std::filesystem::path::preferred_separator == '\\') {
        CHECK(folder(R"(D:\Photos\)") == R"(D:\Photos)");
        CHECK(folder("D:/Photos/") == "D:/Photos");
        CHECK(folder(R"(D:\)") == R"(D:\)");
        CHECK(folder("D:/") == "D:/");
        CHECK(folder("D:") == "D:");
        CHECK(folder(R"(\\server\share\dir\)") == R"(\\server\share\dir)");
        CHECK(folder(guid_root.c_str()) == guid_root);
        CHECK(folder((guid_root + R"(\)").c_str()) == guid_root);
        CHECK(folder(R"(\\?\VOLUME{c0ffee00-0000-4000-8000-000000000001}\)") ==
              R"(\\?\VOLUME{c0ffee00-0000-4000-8000-000000000001}\)");
        CHECK(folder((guid_root + R"(Photos\)").c_str()) == guid_root + "Photos");
        CHECK(folder(R"(\\.\Volume{c0ffee00-0000-4000-8000-000000000001}\)") ==
              R"(\\.\Volume{c0ffee00-0000-4000-8000-000000000001})");
    } else {
        CHECK(folder(R"(/vol/name\)") == R"(/vol/name\)");
        CHECK(folder(guid_root.c_str()) == guid_root);  // a name, as '\' separates nothing
    }
}

// A plan that an older version recorded for a root typed with a trailing
// separator is the plan of that folder: the root without the separator finds
// it, so the registration is resumed rather than started again beside it.
void a_plan_recorded_with_a_trailing_separator_is_found()
{
    Fixture fixture;
    fixture.archive.add_file("a.txt", "a (sample data)");
    fixture.archive.add_file("b.txt", "b (sample data)");
    fixture.archive.node("b.txt").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.run(at("/vol"));
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("b.txt").open_error.reset();
    const auto archives = fixture.store.archives();
    CHECK(archives && archives->size() == 1);
    if (!archives || archives->size() != 1) {
        return;
    }
    const auto archive_uuid = archives->front().archive_uuid;
    const auto key = "registration:" + archive_uuid;
    auto plan = nlohmann::json::parse(fixture.store.setting(key).value_or(""), nullptr, false);
    CHECK(plan.is_object() && plan.value("root", std::string{}) == "/vol");
    plan["root"] = "/vol/";
    CHECK(fixture.store.set_setting(key, plan.dump()));

    const auto again = fixture.run(at("/vol"));
    CHECK(!again && again.error().kind == va::RegisterFailure::Kind::Refused &&
          again.error().message.find(archive_uuid) != std::string::npos);
    auto request = at("/vol");
    request.resume = true;
    const auto resumed = fixture.run(request);
    CHECK(resumed && resumed->archive_uuid == archive_uuid && resumed->revision == 1 && resumed->counts.files == 2);
    const auto after = fixture.store.archives();
    CHECK(after && after->size() == 1 && after->front().status == "REGISTERED");
}

// R-89: the optional media copy, and what verification makes of it.
void media_copy_is_written_and_checked()
{
    Fixture fixture;
    fixture.archive.add_file("a.txt", "a");
    auto request = at("/vol");
    request.write_media_copy = true;
    const auto registered = fixture.run(request);
    CHECK(registered && !registered->media_copy.empty());
    if (!registered) {
        return;
    }
    CHECK(fixture.archive.contains(".veriarchive/identity.json"));
    CHECK(fixture.archive.contains(".veriarchive/r0001.vam"));
    CHECK(registered->counts.files == 1); // .veriarchive/ is excluded (R-54)

    auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED");

    // A differing copy of the same revision is a warning.
    auto& copy = fixture.archive.node(".veriarchive/r0001.vam");
    const auto original = copy.content;
    copy.content += "x";
    verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED_WITH_WARNINGS");
    copy.content = original;

    // A newer valid revision on the media means the repository was rolled back.
    auto rebaseline = at("/vol");
    rebaseline.new_revision = true;
    rebaseline.write_media_copy = true;
    CHECK(fixture.run(rebaseline).has_value());
    auto archive = fixture.store.find_archive(registered->archive_uuid);
    if (archive && *archive) {
        (*archive)->current_revision = 1;
        CHECK(fixture.store.update_archive(**archive));
    }
    fixture.repository.execute("DELETE FROM archive_revisions WHERE revision = 2");
    verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "FAILED");
    CHECK(verified && std::find(verified->failure_classes.begin(), verified->failure_classes.end(),
                          "ManifestRevisionRollback") != verified->failure_classes.end());
}

// §B10 last row: an identity file naming another archive was copied.
void copied_identity_file_fails()
{
    Fixture fixture;
    fixture.archive.add_file("a.txt", "a");
    CHECK(fixture.run(at("/vol")).has_value());
    fixture.archive.add_file(".veriarchive/identity.json",
        "{\"archive_uuid\":\"00000000-0000-4000-8000-000000000000\"}");
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "FAILED");
    CHECK(verified && verified->identity.result == va::MatchResult::Mismatch);
}

void unidentifiable_volume_fails()
{
    Fixture fixture;
    fixture.media.error = va::IoError{va::IoErrorKind::NotSupported, 0, "no volume"};
    const auto outcome = fixture.run(at("/vol"));
    CHECK(!outcome && outcome.error().kind == va::RegisterFailure::Kind::Media);
}

// Reads the registration's plan whenever hashing reports progress, as the
// GUI's Archive List would read it from another process.
class PlanWatcher final : public va::IProgressSink {
public:
    explicit PlanWatcher(va::ArchiveStore& store) : store_(store) {}
    void on_progress(const va::Progress& progress) override
    {
        if (progress.items_total == 0) {
            return; // scanning
        }
        if (const auto plans = store_.settings_with_prefix("registration:"); plans && plans->size() == 1) {
            plans_seen.push_back(nlohmann::json::parse(plans->front().second, nullptr, false));
        }
    }
    std::vector<nlohmann::json> plans_seen;

private:
    va::ArchiveStore& store_;
};

// Decision 0022: once the scan has counted them, the plan says how many files
// and bytes there are to hash, those an interrupted attempt hashed included
// (they count as done), and "updated" says when it last worked.
void plan_records_totals_and_updated()
{
    Fixture fixture;
    for (int index = 1; index <= 6; ++index) {
        fixture.archive.add_file("f" + std::to_string(index), std::string(1000, static_cast<char>('a' + index)));
    }
    fixture.archive.add_file("empty", "");
    fixture.archive.node("f4").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.run(at("/vol"));
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    const auto stored = fixture.store.settings_with_prefix("registration:");
    CHECK(stored && stored->size() == 1);
    if (!stored || stored->size() != 1) {
        return;
    }
    const auto plan = nlohmann::json::parse(stored->front().second, nullptr, false);
    CHECK(plan.value("files_total", 0) == 7 && plan.value("bytes_total", 0) == 6000);
    CHECK(va::parse_timestamp(plan.value("updated", std::string{})).has_value());
    // The attempt ended without finishing, and its plan says so last.
    CHECK(va::parse_timestamp(plan.value("stopped", std::string{})).has_value());

    // The resumed attempt hashes only what is left, and its plan still counts all.
    fixture.archive.node("f4").open_error.reset();
    PlanWatcher watcher(fixture.store);
    va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    auto request = at("/vol");
    request.resume = true;
    const auto resumed = va::register_archive(services, request, fixture.cancel, watcher);
    CHECK(resumed.has_value());
    CHECK(!watcher.plans_seen.empty());
    for (const auto& seen : watcher.plans_seen) {
        CHECK(seen.value("files_total", 0) == 7 && seen.value("bytes_total", 0) == 6000);
        CHECK(seen.value("root", std::string{}) == va::path_to_utf8(std::filesystem::path("/vol")));
        CHECK(!seen.contains("stopped"));  // working again
    }
    // Finished: the plan is gone.
    const auto left = fixture.store.settings_with_prefix("registration:");
    CHECK(left && left->empty());
}

// Reads the counts of the registration being created whenever hashing reports
// progress, as the GUI's Archive List would from another process.
class CountWatcher final : public va::IProgressSink {
public:
    explicit CountWatcher(va::ArchiveStore& store) : store_(store) {}
    void on_progress(const va::Progress& progress) override
    {
        if (progress.items_total == 0) {
            return; // scanning
        }
        if (const auto pending = store_.pending_registrations(); pending && pending->size() == 1) {
            seen.push_back(pending->front());
        }
    }
    std::vector<va::PendingRegistration> seen;

private:
    va::ArchiveStore& store_;
};

// Decision 0022: a resumed registration counts as done only the checkpoints
// it reuses. Those of files gone (a folder moved) or changed since the
// interrupted attempt hashed them go after the scan: counted, they made the
// done figures pass the totals, and the list show 100 % with files left.
void a_resumed_registration_counts_what_it_reuses()
{
    Fixture fixture;
    for (const char* name : {"edit/e1", "keep/k1", "keep/k2", "move/m1", "move/m2", "move/m3"}) {
        fixture.archive.add_file(name, std::string(1000, name[5]));
    }
    fixture.archive.add_file("stop/s", std::string(1000, 's')).open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    fixture.archive.add_file("tail/t", std::string(1000, 't'));
    const auto interrupted = fixture.run(at("/vol"));
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    const auto before = fixture.store.pending_registrations();
    CHECK(before && before->size() == 1 && before->front().files_done == 6);

    // While the device was away: the folder move/ became moved/, and edit/e1 grew.
    fixture.archive.remove("move");
    for (const char* name : {"moved/m1", "moved/m2", "moved/m3"}) {
        fixture.archive.add_file(name, std::string(1000, name[6]));
    }
    fixture.archive.node("edit/e1").content = std::string(2500, 'E');
    fixture.archive.node("stop/s").open_error.reset();

    CountWatcher watcher(fixture.store);
    va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    auto request = at("/vol");
    request.resume = true;
    fixture.archive.bytes_read = 0;
    const auto resumed = va::register_archive(services, request, fixture.cancel, watcher);
    CHECK(resumed && resumed->counts.files == 8);
    // Only keep/k1 and keep/k2 were reused: everything else was read.
    CHECK(fixture.archive.bytes_read == 2500 + 3000 + 1000 + 1000);
    CHECK(!watcher.seen.empty());
    for (const auto& seen : watcher.seen) {
        CHECK(seen.files_total == std::optional<std::uint64_t>(8) &&
              seen.bytes_total == std::optional<std::uint64_t>(9500));
        CHECK(seen.files_done <= 8 && seen.bytes_done <= 9500);
    }
    if (!watcher.seen.empty()) {
        // Hashing reports before its first checkpoint: the two reused files.
        CHECK(watcher.seen.front().files_done == 2 && watcher.seen.front().bytes_done == 2000);
    }
    if (resumed) {
        const auto verified = fixture.verify(fixture.archive.root());
        CHECK(verified && verified->result == "PASSED");
    }
}

// Writes on the archive media like FakeMediaWriter, and runs `before_manifest`
// when the manifest copy is about to be written there (R-89): after the
// registration was recorded, before it returns.
class HookedMediaWriter final : public va::IFileWriter {
public:
    HookedMediaWriter(va::IFileWriter& inner, std::function<void()> before_manifest)
        : inner_(inner), before_manifest_(std::move(before_manifest))
    {
    }
    std::expected<std::unique_ptr<va::IOutputFile>, va::IoError> create(
        const std::filesystem::path& destination) override
    {
        if (destination.extension() == ".vam" && before_manifest_) {
            before_manifest_();
        }
        return inner_.create(destination);
    }
    std::expected<void, va::IoError> remove(const std::filesystem::path& path) override { return inner_.remove(path); }
    std::expected<void, va::IoError> create_directories(const std::filesystem::path& path) override
    {
        return inner_.create_directories(path);
    }

private:
    va::IFileWriter& inner_;
    std::function<void()> before_manifest_;
};

// The plan's key is the archive's ("registration:<uuid>"). Once a
// registration is recorded, its plan is gone and the stop mark is off: the
// plan of the archive's next registration, which another process may have
// written by the time this one returns, keeps what that process wrote.
void a_recorded_registration_leaves_the_next_plan_alone()
{
    Fixture fixture;
    fixture.archive.add_file("a.txt", "a");
    std::string next_plan;
    HookedMediaWriter media(fixture.media_writer, [&] {
        const auto plans = fixture.store.settings_with_prefix("registration:");
        CHECK(plans && plans->empty());  // recorded: the plan is gone
        const auto archives = fixture.store.archives();
        if (!archives || archives->size() != 1) {
            CHECK(false);
            return;
        }
        // Another process starts `register --new-revision` of the archive.
        const auto& archive_uuid = archives->front().archive_uuid;
        next_plan = R"({"archive_uuid":")" + archive_uuid +
                    R"(","revision":2,"label":"ARCH-0001","created":"2026-10-15T01:00:00Z","new_archive":false,)"
                    R"("root":"/vol"})";
        auto other = fixture.repository.store();
        CHECK(other.set_setting("registration:" + archive_uuid, next_plan));
    });
    va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer, media,
        fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    auto request = at("/vol");
    request.write_media_copy = true;
    const auto registered = va::register_archive(services, request, fixture.cancel, fixture.progress);
    CHECK(registered && !registered->media_copy.empty());
    CHECK(!next_plan.empty());
    const auto plans = fixture.store.settings_with_prefix("registration:");
    CHECK(plans && plans->size() == 1 && plans->front().second == next_plan);
}

// The heartbeat repeats the record with the time, at most once per interval,
// and never brings back a record that its operation removed on finishing.
void heartbeat_updates_only_its_record()
{
    test::TestRepository repository;
    auto store = repository.store();
    test::NullProgress forward;
    CHECK(store.set_setting("registration:x", R"({"archive_uuid":"x"})"));
    va::Heartbeat heartbeat(store, "registration:x", nlohmann::json{{"archive_uuid", "x"}}, forward,
        std::chrono::seconds(0));
    heartbeat.set("files_total", 3);
    heartbeat.on_progress({});
    const auto written = store.setting("registration:x");
    const auto record = nlohmann::json::parse(written.value_or(""), nullptr, false);
    CHECK(record.value("files_total", 0) == 3 && record.value("archive_uuid", std::string{}) == "x");
    CHECK(va::parse_timestamp(record.value("updated", std::string{})).has_value());

    // An attempt that ends without finishing writes when it stopped.
    heartbeat.stop();
    const auto stopped = nlohmann::json::parse(store.setting("registration:x").value_or(""), nullptr, false);
    CHECK(va::parse_timestamp(stopped.value("stopped", std::string{})).has_value());
    CHECK(stopped.value("files_total", 0) == 3);

    CHECK(store.remove_setting("registration:x"));
    heartbeat.beat();
    heartbeat.stop();
    CHECK(store.setting("registration:x") == std::string{});

    // With the usual interval, reports in quick succession write nothing.
    CHECK(store.set_setting("run:y", "{}"));
    va::Heartbeat slow(store, "run:y", nlohmann::json::object(), forward);
    slow.on_progress({});
    CHECK(store.setting("run:y") == std::string("{}"));
}

// Decision 0023: a registration's plan is written only while the revision it
// creates is not recorded. Once another attempt of the registration recorded
// it, the key may hold the plan of the archive's next registration, which
// neither a beat nor the stop mark of this attempt may replace. Decision 0025:
// nor once the registration was abandoned and a later one took its key.
void heartbeat_of_a_plan_stops_once_its_revision_is_recorded()
{
    test::TestRepository repository;
    auto store = repository.store();
    test::NullProgress forward;
    const std::string key = "registration:u";
    CHECK(store.set_setting(key, R"({"archive_uuid":"u","revision":1,"created":"2026-10-15T01:00:00Z"})"));
    va::Heartbeat heartbeat(store, key,
        nlohmann::json{{"archive_uuid", "u"}, {"revision", 1}, {"created", "2026-10-15T01:00:00Z"}}, forward,
        std::chrono::seconds(0));
    heartbeat.only_while_planned("u", 1, "2026-10-15T01:00:00Z");
    heartbeat.beat();
    const auto beaten = nlohmann::json::parse(store.setting(key).value_or(""), nullptr, false);
    CHECK(va::parse_timestamp(beaten.value("updated", std::string{})).has_value());

    // Another attempt records revision 1, and the next registration writes its plan.
    CHECK(store.insert_revision({"u", 1, "manifests/u/r0001.vam", "{}", "h", "b", "", "2026-10-15T01:00:00Z"}));
    const std::string next_plan = R"({"archive_uuid":"u","revision":2,"created":"2026-10-15T02:00:00Z"})";
    CHECK(store.set_setting(key, next_plan));
    heartbeat.on_progress({});
    heartbeat.beat();
    heartbeat.stop();
    CHECK(store.setting(key) == next_plan);

    // The plan of another revision of another archive is not held back by it.
    const std::string other_plan = R"({"archive_uuid":"w","revision":1,"created":"2026-10-15T01:00:00Z"})";
    CHECK(store.set_setting("registration:w", other_plan));
    va::Heartbeat other(store, "registration:w", nlohmann::json::parse(other_plan), forward, std::chrono::seconds(0));
    other.only_while_planned("w", 1, "2026-10-15T01:00:00Z");
    other.beat();
    const auto beaten_other = nlohmann::json::parse(store.setting("registration:w").value_or(""), nullptr, false);
    CHECK(va::parse_timestamp(beaten_other.value("updated", std::string{})).has_value());

    // Decision 0025: once the registration is abandoned and the archive's next
    // registration of the same revision begins, the key holds that plan, made
    // later: neither a beat nor the stop mark of the abandoned attempt replaces it.
    CHECK(store.remove_setting("registration:w"));
    const std::string later_plan = R"({"archive_uuid":"w","revision":1,"created":"2026-10-16T01:00:00Z"})";
    CHECK(store.set_setting("registration:w", later_plan));
    other.beat();
    other.stop();
    CHECK(store.setting("registration:w") == later_plan);
    // A damaged plan is no registration's plan, and is not written either.
    CHECK(store.set_setting("registration:w", "not a plan"));
    other.beat();
    CHECK(store.setting("registration:w") == std::string("not a plan"));
}

// Sample data for the tests of attempts that run at once: 40 files in two
// folders of a fake volume.
void fill_sample(test::FakeFileSystem& files)
{
    for (int index = 0; index < 40; ++index) {
        files.add_file((index < 20 ? "photos/p" : "videos/v") + std::to_string(100 + index) + ".bin",
            std::string(500 + index, static_cast<char>('a' + index % 26)));
    }
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

// An interrupted registration of the sample at /vol (its plan and the
// checkpoints of the files before the one that went away); its archive uuid.
va::Uuid interrupted_sample_registration(Fixture& fixture)
{
    fill_sample(fixture.archive);
    fixture.archive.node("videos/v130.bin").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.run(at("/vol"));
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("videos/v130.bin").open_error.reset();
    const auto archives = fixture.store.archives();
    CHECK(archives && archives->size() == 1);
    return archives && archives->size() == 1 ? archives->front().archive_uuid : va::Uuid{};
}

// The host named in the manifest that revision 1 is recorded with, after
// checking that the record is that file's bytes (R-14): its header line, the
// header's BLAKE3 and the body's.
std::string recorded_host(Fixture& fixture, const va::Uuid& archive_uuid)
{
    const auto recorded = fixture.store.revision(archive_uuid, 1);
    if (!recorded || !*recorded) {
        CHECK(false);
        return {};
    }
    const auto bytes =
        read_bytes(fixture.repository.context.repo.root / va::path_from_utf8((*recorded)->manifest_relpath));
    const auto newline = bytes.find('\n');
    const auto header_line = bytes.substr(0, newline);
    CHECK(newline != std::string::npos);
    CHECK((*recorded)->header_json == header_line);
    CHECK((*recorded)->header_blake3 == va::blake3_hex(header_line));
    CHECK(newline != std::string::npos && (*recorded)->body_blake3 == va::blake3_hex(bytes.substr(newline + 1)));
    const auto header = nlohmann::json::parse(header_line, nullptr, false);
    return header.is_object() && header.contains("created_by") ? header["created_by"].value("host", std::string{})
                                                               : std::string{};
}

// The index rows of revision 1 are its manifest's entries (R-80), hashes included.
void check_index_is_the_manifest(Fixture& fixture, const va::Uuid& archive_uuid)
{
    const auto manifest = va::manifest_path(fixture.repository.context.repo.manifests(), archive_uuid, 1);
    auto reader = va::ManifestReader::open(*fixture.repository.files, manifest);
    CHECK(reader.has_value());
    std::map<std::string, std::string> recorded;
    while (reader) {
        auto next = reader->next();
        if (!next || !*next) {
            CHECK(next.has_value());
            break;
        }
        recorded[(**next).path.p] = (**next).b3;
    }
    std::map<std::string, std::string> indexed;
    if (const auto rows = fixture.store.file_rows(archive_uuid, 1)) {
        for (const auto& row : *rows) {
            indexed[row.path.p] = row.b3;
        }
    }
    CHECK(!recorded.empty() && indexed == recorded);
}

// One attempt of a registration in a thread of its own, as in a process of
// its own: its own database connection, view of the media and process, and a
// host name of its own, so that two attempts write different manifests. Two
// attempts that run at once are then on two computers sharing the repository,
// where neither can see the other's process (decision 0024), as attempts of an
// older version cannot either. Once its manifest is written and flushed, it
// says so (`staged`) and waits (`go`); `on_naming` runs when it gives its
// manifest its name, inside the transaction that records the revision.
// `lock_wait`: how long its statements wait for another connection's write
// lock.
struct Attempt {
    Attempt(Fixture& fixture, std::string host_name,
        std::chrono::milliseconds lock_wait = va::statement_lock_wait)
        : store(fixture.repository.store(lock_wait)),
          writer(
              *fixture.repository.writer,
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
        fill_sample(files);
        host = fixture.host;
        host.host = std::move(host_name);
        static std::uint64_t next_process = 5000;
        processes.self = {next_process, "boot-1:" + std::to_string(next_process), "veriarchive-cli.exe"};
        ++next_process;
    }

    void start(Fixture& fixture, va::RegisterRequest request)
    {
        thread = std::thread([this, &fixture, request] {
            va::RegisterServices services{files, *fixture.repository.files, writer, media_writer, fixture.media,
                store, fixture.repository.context.repo, host, processes};
            outcome = va::register_archive(services, request, cancel, progress);
        });
    }

    test::FakeFileSystem files{"/vol"};
    test::FakeMediaWriter media_writer{files};
    va::ArchiveStore store;
    std::binary_semaphore staged{0};
    std::binary_semaphore go{0};
    test::StagingHookWriter writer;
    std::function<void()> on_naming;
    va::HostInfo host;
    test::FakeProcessTable processes;
    va::CancelToken cancel;
    test::NullProgress progress;
    std::expected<va::RegisterOutcome, va::RegisterFailure> outcome = std::unexpected(va::RegisterFailure{});
    std::thread thread;
};

// Decision 0023: two attempts of one registration at once (`register --resume`
// while an attempt still runs, or two resumes), interleaved at the manifest:
// both have written and flushed theirs when one is let through to record the
// revision, then the other. The second finds the revision recorded and stops:
// the recorded manifest keeps the first one's bytes and verifies, the media
// copy is the first one's only, neither leaves a file behind, and the
// attempts after them are not blocked.
void attempts_of_one_registration_record_it_once(bool first_started_records)
{
    Fixture fixture;
    const auto archive_uuid = interrupted_sample_registration(fixture);
    auto request = at("/vol");
    request.resume = true;
    request.write_media_copy = true;
    Attempt first(fixture, "host-first");
    Attempt second(fixture, "host-second");
    first.start(fixture, request);
    second.start(fixture, request);
    const bool first_staged = first.staged.try_acquire_for(std::chrono::seconds(60));
    const bool second_staged = second.staged.try_acquire_for(std::chrono::seconds(60));
    CHECK(first_staged && second_staged);
    auto& winner = first_started_records ? first : second;
    auto& loser = first_started_records ? second : first;
    winner.go.release();
    winner.thread.join();
    loser.go.release();
    loser.thread.join();

    CHECK(winner.outcome && winner.outcome->archive_uuid == archive_uuid && winner.outcome->revision == 1);
    CHECK(!loser.outcome && loser.outcome.error().kind == va::RegisterFailure::Kind::Refused &&
          loser.outcome.error().message == test::already_recorded_message(archive_uuid, 1));
    CHECK(recorded_host(fixture, archive_uuid) == winner.host.host);
    check_index_is_the_manifest(fixture, archive_uuid);
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED");
    const auto directory = fixture.repository.context.repo.manifests() / archive_uuid;
    CHECK(names_in(directory) == std::set<std::string>{"r0001.vam"});
    const auto bytes = read_bytes(directory / "r0001.vam");
    CHECK(winner.files.content_at("/vol/.veriarchive/r0001.vam") == bytes);
    CHECK(!loser.files.contains(".veriarchive/r0001.vam"));
    const auto plans = fixture.store.settings_with_prefix("registration:");
    CHECK(plans && plans->empty());

    // A third attempt after both finds nothing to resume and changes nothing.
    const auto third = fixture.run(request);
    CHECK(!third && third.error().kind == va::RegisterFailure::Kind::Refused);
    CHECK(read_bytes(directory / "r0001.vam") == bytes);
    // Nothing either attempt left blocks the archive's next registration.
    auto next = at("/vol");
    next.new_revision = true;
    const auto rebaselined = fixture.run(next);
    CHECK(rebaselined && rebaselined->revision == 2);
    CHECK(names_in(directory) == (std::set<std::string>{"r0001.vam", "r0002.vam"}));
    const auto again = fixture.verify(fixture.archive.root());
    CHECK(again && again->result == "PASSED" && again->revision == 2);
}

// Decision 0023: the record of a large archive holds the write lock for
// seconds, longer than a statement waits for it. An attempt that reaches its
// record meanwhile waits for the lock and then finds the revision recorded
// (exit 12), rather than failing because the database is locked (exit 11).
// Scaled down: the loser's statements wait 100 ms for the lock, and the
// winner holds it 400 ms, in its record.
void an_attempt_waits_for_the_record_of_another()
{
    Fixture fixture;
    const auto archive_uuid = interrupted_sample_registration(fixture);
    auto request = at("/vol");
    request.resume = true;
    Attempt winner(fixture, "host-winner");
    Attempt loser(fixture, "host-loser", std::chrono::milliseconds(100));
    std::binary_semaphore recording{0};
    winner.on_naming = [&] {
        recording.release();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    };
    winner.start(fixture, request);
    loser.start(fixture, request);
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
    const bool lost = !loser.outcome && loser.outcome.error().kind == va::RegisterFailure::Kind::Refused &&
                      loser.outcome.error().message == test::already_recorded_message(archive_uuid, 1);
    CHECK(lost);
    if (!lost && !loser.outcome) {
        std::cerr << "  loser: " << loser.outcome.error().message << '\n';
    }
    CHECK(recorded_host(fixture, archive_uuid) == winner.host.host);
    check_index_is_the_manifest(fixture, archive_uuid);
    CHECK(names_in(fixture.repository.context.repo.manifests() / archive_uuid) ==
          std::set<std::string>{"r0001.vam"});
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED");
}

// Decision 0023: an attempt whose record waits for the write lock stops
// waiting when it is cancelled (Ctrl+C): it ends as interrupted (exit 4) and
// records nothing, and a resume records later. Scaled down as above.
void a_record_waiting_for_the_lock_can_be_cancelled()
{
    Fixture fixture;
    const auto archive_uuid = interrupted_sample_registration(fixture);
    auto request = at("/vol");
    request.resume = true;
    std::thread holder;
    std::binary_semaphore held{0};
    va::CancelToken cancel;
    // Once its manifest is written, another process takes the lock, and the
    // operator cancels.
    test::StagingHookWriter hooked(*fixture.repository.writer, [&] {
        holder = std::thread([&] {
            auto store = fixture.repository.store();
            CHECK(store.begin());
            held.release();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            CHECK(store.commit());
        });
        CHECK(held.try_acquire_for(std::chrono::seconds(60)));
        cancel.cancel();
    });
    auto store = fixture.repository.store(std::chrono::milliseconds(100));
    va::RegisterServices services{fixture.archive, *fixture.repository.files, hooked, fixture.media_writer,
        fixture.media, store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    const auto cancelled = va::register_archive(services, request, cancel, fixture.progress);
    if (holder.joinable()) {
        holder.join();
    }
    CHECK(!cancelled && cancelled.error().kind == va::RegisterFailure::Kind::Incomplete &&
          cancelled.error().message == "registration was cancelled; continue with `register <root_path> --resume`");
    const auto revision = fixture.store.revision(archive_uuid, 1);
    CHECK(revision && !*revision);
    CHECK(names_in(fixture.repository.context.repo.manifests() / archive_uuid).empty());
    const auto resumed = fixture.run(request);
    CHECK(resumed && resumed->revision == 1);
}

// Decision 0023: an attempt still hashing when another attempt records the
// revision stops at its next checkpoint. It changes nothing of the recorded
// revision, not even the index row of a file it read differently, and leaves
// alone the plan of the archive's next registration, started meanwhile.
void an_attempt_that_loses_while_hashing_changes_nothing()
{
    Fixture fixture;
    const auto archive_uuid = interrupted_sample_registration(fixture);
    auto request = at("/vol");
    request.resume = true;

    // This attempt's view of the media: one file it has yet to hash reads
    // otherwise than the other attempt's (sample data).
    test::FakeFileSystem view{"/vol"};
    fill_sample(view);
    view.node("videos/v135.bin").content = std::string(535, 'X');
    std::expected<va::RegisterOutcome, va::RegisterFailure> other = std::unexpected(va::RegisterFailure{});
    std::string next_plan;
    test::HookedFiles hooked(view, "v133.bin", [&] {
        // The other attempt runs to its end, as another process would...
        auto store = fixture.repository.store();
        va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
            fixture.media_writer, fixture.media, store, fixture.repository.context.repo, fixture.host,
            fixture.processes};
        test::NullProgress quiet;
        va::CancelToken cancel;
        other = va::register_archive(services, request, cancel, quiet);
        // ...and a third process starts the archive's next registration.
        next_plan = R"({"archive_uuid":")" + archive_uuid +
                    R"(","revision":2,"label":"ARCH-0001","created":"2026-10-15T01:00:00Z","new_archive":false,)"
                    R"("root":"/vol"})";
        CHECK(store.set_setting("registration:" + archive_uuid, next_plan));
    });
    auto store = fixture.repository.store();
    va::RegisterServices services{hooked, *fixture.repository.files, *fixture.repository.writer,
        fixture.media_writer, fixture.media, store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    const auto late = va::register_archive(services, request, fixture.cancel, fixture.progress);

    CHECK(other && other->revision == 1);
    CHECK(!late && late.error().kind == va::RegisterFailure::Kind::Refused &&
          late.error().message == test::already_recorded_message(archive_uuid, 1));
    CHECK(recorded_host(fixture, archive_uuid) == fixture.host.host);
    check_index_is_the_manifest(fixture, archive_uuid);
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED");
    CHECK(names_in(fixture.repository.context.repo.manifests() / archive_uuid) ==
          std::set<std::string>{"r0001.vam"});
    CHECK(fixture.store.setting("registration:" + archive_uuid) == next_plan);
}

// Decision 0023: a resume that finds the revision of its plan recorded stops
// before it reads the media, and records nothing. Here the plan is still
// there: another operation recorded the revision and left it (one of an
// older version; the other attempt of a registration, a relocation and an
// import remove it when they record, decision 0030). Such a plan can never
// be finished, so the resume removes it, and the root takes another
// registration.
void a_resume_of_a_recorded_revision_reads_nothing()
{
    Fixture fixture;
    const auto archive_uuid = interrupted_sample_registration(fixture);
    // Recorded by another operation, which left the plan (sample data).
    CHECK(fixture.store.insert_revision(
        {archive_uuid, 1, "manifests/" + archive_uuid + "/r0001.vam", "{}", "h", "b", "", "2026-10-15T01:00:00Z"}));
    CHECK(!fixture.store.setting("registration:" + archive_uuid).value_or("").empty());
    fixture.archive.bytes_read = 0;
    auto request = at("/vol");
    request.resume = true;
    const auto resumed = fixture.run(request);
    CHECK(!resumed && resumed.error().kind == va::RegisterFailure::Kind::Refused &&
          resumed.error().message == test::already_recorded_message(archive_uuid, 1));
    CHECK(fixture.archive.bytes_read == 0);
    CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("x").empty());
    const auto again = fixture.run(request);
    CHECK(!again && again.error().message == "there is no interrupted registration of this root to resume");
}

// Decision 0023: what crashed attempts leave in the archive's manifest folder
// (a manifest never recorded, temporary and body files, also the body file of
// an older version) never blocks `register --resume`. The resume records its
// own manifest in place of the one never recorded and leaves the rest alone:
// an attempt in another process may still be writing it.
void crashed_attempts_leave_nothing_that_blocks_a_resume()
{
    Fixture fixture;
    const auto archive_uuid = interrupted_sample_registration(fixture);
    const auto directory = fixture.repository.context.repo.manifests() / archive_uuid;
    std::filesystem::create_directories(directory);
    const std::set<std::string> leftovers{"r0001.vam.tmp-99999-0",
        "r0001.vam.body-00000000-0000-4000-8000-000000000000",
        "r0001.vam.body-00000000-0000-4000-8000-000000000000.tmp-99999-1", "r0001.vam.body"};
    for (const auto& name : leftovers) {
        std::ofstream(directory / name, std::ios::binary) << "left by a crashed attempt (sample data)\n";
    }
    std::ofstream(directory / "r0001.vam", std::ios::binary)
        << R"({"format":"vam","version":1})" << "\nleft by a crashed attempt (sample data)\n";

    auto request = at("/vol");
    request.resume = true;
    const auto resumed = fixture.run(request);
    CHECK(resumed && resumed->revision == 1);
    CHECK(recorded_host(fixture, archive_uuid) == fixture.host.host);
    check_index_is_the_manifest(fixture, archive_uuid);
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED");
    auto expected = leftovers;
    expected.insert("r0001.vam");
    CHECK(names_in(directory) == expected);
}

// Writes on the archive media like FakeMediaWriter, and lets commits of
// identity.json fail as on Windows when another attempt replaces the file at
// the same moment (MoveFileExW, access denied): the first `failing` of them
// fail, each with the other attempt's bytes on the media when
// `other_attempt_wrote` (attempts of one registration write the same bytes).
class RacingMediaWriter final : public va::IFileWriter {
public:
    RacingMediaWriter(test::FakeFileSystem& files, int failing, bool other_attempt_wrote)
        : files_(files), failing_(failing), other_attempt_wrote_(other_attempt_wrote)
    {
    }

    std::expected<std::unique_ptr<va::IOutputFile>, va::IoError> create(
        const std::filesystem::path& destination) override
    {
        return std::make_unique<Output>(*this, destination);
    }
    std::expected<void, va::IoError> remove(const std::filesystem::path&) override { return {}; }
    std::expected<void, va::IoError> create_directories(const std::filesystem::path&) override { return {}; }

    int identity_commits = 0;   // tried, failed or not
    int identity_failures = 0;

private:
    class Output final : public va::IOutputFile {
    public:
        Output(RacingMediaWriter& writer, std::filesystem::path destination)
            : writer_(writer), destination_(std::move(destination))
        {
        }
        std::expected<void, va::IoError> write(std::span<const std::byte> data) override
        {
            content_.append(reinterpret_cast<const char*>(data.data()), data.size());
            return {};
        }
        std::expected<void, va::IoError> flush() override { return {}; }
        std::expected<void, va::IoError> commit() override
        {
            if (destination_.filename() == "identity.json") {
                ++writer_.identity_commits;
                if (writer_.failing_ > 0) {
                    --writer_.failing_;
                    ++writer_.identity_failures;
                    if (writer_.other_attempt_wrote_) {
                        writer_.files_.put(destination_, content_);
                    }
                    return std::unexpected(
                        va::IoError{va::IoErrorKind::PermissionDenied, 5, "MoveFileExW failed (Win32 error 5)"});
                }
            }
            writer_.files_.put(destination_, content_);
            return {};
        }

    private:
        RacingMediaWriter& writer_;
        std::filesystem::path destination_;
        std::string content_;
    };

    test::FakeFileSystem& files_;
    int failing_;
    bool other_attempt_wrote_;
};

// Decision 0023: the attempts of one registration write the same
// identity.json before their scan (R-89). A resume that finds those bytes on
// the media leaves the file as it is, so attempts that start at once do not
// replace it at the same moment, which fails on Windows.
void a_resume_leaves_the_identity_file_it_finds()
{
    Fixture fixture;
    fill_sample(fixture.archive);
    fixture.archive.node("videos/v130.bin").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    auto request = at("/vol");
    request.write_media_copy = true;
    const auto interrupted = fixture.run(request);
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("videos/v130.bin").open_error.reset();
    const auto identity = fixture.archive.content_at("/vol/.veriarchive/identity.json");
    CHECK(!identity.empty());

    RacingMediaWriter media(fixture.archive, 0, false);
    va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer, media,
        fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    request.resume = true;
    const auto resumed = va::register_archive(services, request, fixture.cancel, fixture.progress);
    CHECK(resumed && resumed->revision == 1 && !resumed->media_copy.empty());
    CHECK(media.identity_commits == 0);
    CHECK(fixture.archive.content_at("/vol/.veriarchive/identity.json") == identity);
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED");
}

// Decision 0023: a write of identity.json that fails because another attempt
// replaced the file at the same moment counts when, a moment later, the media
// holds the bytes this attempt wrote: the registration goes on. A write that
// keeps failing without them stops it, after three tries (exit 11).
void an_identity_file_another_attempt_wrote_counts()
{
    {
        Fixture fixture;
        fixture.archive.add_file("a.txt", "a");
        RacingMediaWriter media(fixture.archive, 1, true);
        va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer, media,
            fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
            fixture.processes};
        auto request = at("/vol");
        request.write_media_copy = true;
        const auto registered = va::register_archive(services, request, fixture.cancel, fixture.progress);
        CHECK(registered && !registered->media_copy.empty());
        CHECK(media.identity_commits == 1 && media.identity_failures == 1);
        const auto identity = fixture.archive.content_at("/vol/.veriarchive/identity.json");
        CHECK(registered && identity.find(registered->archive_uuid) != std::string::npos);
    }
    {
        Fixture fixture;
        fixture.archive.add_file("a.txt", "a");
        RacingMediaWriter media(fixture.archive, 100, false);
        va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer, media,
            fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
            fixture.processes};
        auto request = at("/vol");
        request.write_media_copy = true;
        const auto registered = va::register_archive(services, request, fixture.cancel, fixture.progress);
        CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Media &&
              registered.error().message == "cannot write the media copy: MoveFileExW failed (Win32 error 5)");
        CHECK(media.identity_failures == 3);
        CHECK(!fixture.archive.contains(".veriarchive/identity.json"));
    }
}


// ---- Which process runs an attempt (decision 0024) -------------------------

nlohmann::json plan_of(va::ArchiveStore& store, const va::Uuid& archive_uuid)
{
    return nlohmann::json::parse(store.setting("registration:" + archive_uuid).value_or(""), nullptr, false);
}

// An interrupted registration of the sample at /vol whose attempt ran in
// `process` on this computer. `killed`: the process ended without writing
// the stop mark; otherwise the attempt wrote it (cancelled, its device gone).
va::Uuid interrupted_in(Fixture& fixture, const va::ProcessIdentity& process, bool killed)
{
    const auto runs_in = fixture.processes.self;
    fixture.processes.self = process;
    const auto archive_uuid = interrupted_sample_registration(fixture);
    fixture.processes.self = runs_in;
    if (killed) {
        auto plan = plan_of(fixture.store, archive_uuid);
        plan.erase("stopped");
        CHECK(fixture.store.set_setting("registration:" + archive_uuid, plan.dump()));
    }
    return archive_uuid;
}

std::string still_running_message(const va::ProcessIdentity& process)
{
    return "this registration is still running in process " + std::to_string(process.id) + " (" + process.program +
           ") on this computer; resume it once that process has ended";
}

// Every attempt records in its plan which process runs it: its id, its start
// (which tells it from a later process given the same id), the computer and
// the program. `register --resume` refuses while that process still runs,
// names it, reads nothing of the media and leaves the plan as it is; once the
// process has ended, it goes ahead at once, without waiting for the plan to
// go quiet, and the plan names the resuming process while it works.
void a_resume_waits_while_the_attempt_before_runs()
{
    Fixture fixture;
    const va::ProcessIdentity first{1000, "boot-1:1000", "veriarchive-gui.exe"};
    const auto archive_uuid = interrupted_in(fixture, first, true);
    const auto plan = plan_of(fixture.store, archive_uuid);
    CHECK(plan.contains("process") && plan["process"] == (nlohmann::json{{"id", 1000}, {"start", "boot-1:1000"},
                                                             {"host", "test-host"}, {"program", "veriarchive-gui.exe"}}));
    CHECK(!plan.contains("stopped"));

    // A resume from the command line while the window's attempt hangs.
    fixture.processes.self = {2000, "boot-1:2000", "veriarchive-cli.exe"};
    fixture.processes.others = {first};
    fixture.archive.bytes_read = 0;
    auto request = at("/vol");
    request.resume = true;
    const auto refused = fixture.run(request);
    CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Refused &&
          refused.error().message == still_running_message(first));
    CHECK(fixture.archive.bytes_read == 0);
    CHECK(plan_of(fixture.store, archive_uuid) == plan);

    // The window's process ends: the resume goes ahead at once.
    fixture.processes.others.clear();
    PlanWatcher watcher(fixture.store);
    va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    const auto resumed = va::register_archive(services, request, fixture.cancel, watcher);
    CHECK(resumed && resumed->archive_uuid == archive_uuid && resumed->revision == 1);
    CHECK(!watcher.plans_seen.empty());
    for (const auto& seen : watcher.plans_seen) {
        CHECK(seen.contains("process") && seen["process"].value("id", 0) == 2000 &&
              seen["process"].value("program", std::string{}) == "veriarchive-cli.exe");
        CHECK(!seen.contains("stopped"));
    }
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED");
}

// A process id is given to a later process once its process has ended (also
// after a reboot, when the clock ticks start again): a process with the
// recorded id but another start is not the attempt's, and the resume goes on.
void a_resume_goes_ahead_when_the_process_id_is_reused()
{
    for (const auto* later : {"boot-1:98765", "boot-2:1000"}) {
        Fixture fixture;
        const va::ProcessIdentity first{1000, "boot-1:1000", "veriarchive-cli.exe"};
        const auto archive_uuid = interrupted_in(fixture, first, true);
        fixture.processes.self = {2000, "boot-1:2000", "veriarchive-cli.exe"};
        fixture.processes.others = {{1000, later, "notepad.exe"}};
        auto request = at("/vol");
        request.resume = true;
        const auto resumed = fixture.run(request);
        CHECK(resumed && resumed->archive_uuid == archive_uuid && resumed->revision == 1);
    }
}

// Where nothing tells whether the attempt before still runs, the resume goes
// ahead as before (decision 0023: the first attempt to record the revision
// wins): a plan of a version before decision 0024 names no process (it says
// when it worked; one that never does is resumed only once confirmed, below),
// a repository shared by computers holds plans whose process id means
// something only on the other computer, and the process table may refuse to
// answer. A plan that says the
// attempt stopped (its last write) needs no process: the GUI's window goes on
// after a cancel. And a plan that names this very process is of an attempt
// that ended: a process runs one registration at a time.
void a_resume_goes_ahead_where_the_attempt_before_has_ended_or_cannot_be_seen()
{
    enum class Case { OlderVersion, OtherComputer, TableRefuses, StopMark, ThisProcess };
    for (const auto kind :
        {Case::OlderVersion, Case::OtherComputer, Case::TableRefuses, Case::StopMark, Case::ThisProcess}) {
        Fixture fixture;
        const va::ProcessIdentity first{1000, "boot-1:1000", "veriarchive-gui.exe"};
        const auto archive_uuid = interrupted_in(fixture, first, kind != Case::StopMark);
        fixture.processes.self = kind == Case::ThisProcess ? first : va::ProcessIdentity{2000, "boot-1:2000", "x"};
        fixture.processes.others = {first};
        auto plan = plan_of(fixture.store, archive_uuid);
        if (kind == Case::OlderVersion) {
            plan.erase("process");
        } else if (kind == Case::OtherComputer) {
            plan["process"]["host"] = "another-computer";
        } else if (kind == Case::TableRefuses) {
            fixture.processes.error = va::IoError{va::IoErrorKind::PermissionDenied, 5, "OpenProcess failed"};
        }
        CHECK(fixture.store.set_setting("registration:" + archive_uuid, plan.dump()));
        auto request = at("/vol");
        request.resume = true;
        const auto resumed = fixture.run(request);
        CHECK(resumed && resumed->archive_uuid == archive_uuid && resumed->revision == 1);
        if (!resumed) {
            std::cerr << "  case " << static_cast<int>(kind) << ": " << resumed.error().message << '\n';
        }
    }
}

// The bytes of `rows`, the saved progress of a registration.
std::uint64_t bytes_of(const std::vector<va::Entry>& rows)
{
    std::uint64_t bytes = 0;
    for (const auto& row : rows) {
        bytes += row.size.value_or(0);
    }
    return bytes;
}

// Decision 0022, point 7 (operator, 2026-10-20): a plan that never says when
// its attempt last worked, as a version before decision 0022 wrote it (sample
// damage of the sample plan: no "updated", no stop mark, no process, no
// totals), may be run still by that version, which would put its own manifest
// in place of the one the resume records. Without confirmation the resume
// reads nothing, changes nothing (the plan, its saved progress) and says what
// it would take up; confirmed, as `register <root> --resume --confirm`, it
// goes ahead as before, reading only the rest, and records the revision. Such
// a plan whose revision is recorded goes as before, without a question
// (decision 0023). So is one whose "updated" is no time (sample damage). A
// plan of this version, which says when it worked, resumes without
// confirmation (the tests above), and takes one too: a script may always pass
// --confirm. So does a plan that names its process but never says
// when it worked, as a version since decision 0024 leaves one whose writes of
// "updated" all failed: that version names its manifest in the transaction
// that records it.
void a_resume_of_an_older_versions_plan_asks_for_confirmation()
{
    const auto as_an_older_version_wrote_it = [](Fixture& fixture, const va::Uuid& archive_uuid) {
        auto plan = plan_of(fixture.store, archive_uuid);
        CHECK(plan.contains("updated") && plan.contains("stopped") && plan.contains("process"));
        for (const auto* member : {"updated", "stopped", "process", "files_total", "bytes_total", "media_copy"}) {
            plan.erase(member);
        }
        CHECK(fixture.store.set_setting("registration:" + archive_uuid, plan.dump()));
        return plan;
    };
    {
        Fixture fixture;
        const auto archive_uuid = interrupted_sample_registration(fixture);
        const auto plan = as_an_older_version_wrote_it(fixture, archive_uuid);
        const auto saved = fixture.store.file_rows(archive_uuid, 1);
        CHECK(saved && !saved->empty());
        if (!saved) {
            return;
        }

        fixture.archive.bytes_read = 0;
        auto request = at("/vol");
        request.resume = true;
        const auto asked = fixture.run(request);
        CHECK(!asked && asked.error().kind == va::RegisterFailure::Kind::Unconfirmed &&
              asked.error().message == "run the command again with --confirm to resume it" &&
              asked.error().older_version && !asked.error().lost_manifest);
        if (asked || !asked.error().older_version) {
            return;
        }
        const auto& older = *asked.error().older_version;
        CHECK(older.archive_uuid == archive_uuid && older.revision == 1 && older.root == "/vol" &&
              older.label == plan.value("label", std::string{}) && !older.label.empty() &&
              older.started == plan.value("created", std::string{}) && !older.started.empty());
        CHECK(fixture.archive.bytes_read == 0);
        CHECK(plan_of(fixture.store, archive_uuid) == plan);
        const auto kept = fixture.store.file_rows(archive_uuid, 1);
        CHECK(kept && kept->size() == saved->size());

        request.confirmed = true;
        const auto resumed = fixture.run(request);
        CHECK(resumed && resumed->archive_uuid == archive_uuid && resumed->revision == 1 &&
              resumed->counts.files == 40);
        CHECK(resumed && fixture.archive.bytes_read == resumed->counts.bytes - bytes_of(*saved));
        check_index_is_the_manifest(fixture, archive_uuid);
        CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("x").empty());
    }
    {
        // Its revision recorded by another operation, which left the plan
        // (sample data): the resume removes the plan and records nothing, as
        // for any plan (decision 0023), without asking first.
        Fixture fixture;
        const auto archive_uuid = interrupted_sample_registration(fixture);
        as_an_older_version_wrote_it(fixture, archive_uuid);
        CHECK(fixture.store.insert_revision({archive_uuid, 1, "manifests/" + archive_uuid + "/r0001.vam", "{}", "h",
            "b", "", "2026-10-15T01:00:00Z"}));
        auto request = at("/vol");
        request.resume = true;
        const auto resumed = fixture.run(request);
        CHECK(!resumed && resumed.error().kind == va::RegisterFailure::Kind::Refused &&
              resumed.error().message == test::already_recorded_message(archive_uuid, 1));
        CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("x").empty());
    }
    {
        // A plan whose "updated" is no time (sample damage), and that names
        // no process, says nothing of when it worked: it is asked about too.
        Fixture fixture;
        const auto archive_uuid = interrupted_sample_registration(fixture);
        auto plan = as_an_older_version_wrote_it(fixture, archive_uuid);
        plan["updated"] = "not a time (sample damage)";
        CHECK(fixture.store.set_setting("registration:" + archive_uuid, plan.dump()));
        fixture.archive.bytes_read = 0;
        auto request = at("/vol");
        request.resume = true;
        const auto asked = fixture.run(request);
        CHECK(!asked && asked.error().kind == va::RegisterFailure::Kind::Unconfirmed && asked.error().older_version);
        CHECK(fixture.archive.bytes_read == 0);
        CHECK(plan_of(fixture.store, archive_uuid) == plan);
    }
    {
        // A plan of this version, confirmed although it needs no confirmation.
        Fixture fixture;
        const auto archive_uuid = interrupted_sample_registration(fixture);
        auto request = at("/vol");
        request.resume = true;
        request.confirmed = true;
        const auto resumed = fixture.run(request);
        CHECK(resumed && resumed->archive_uuid == archive_uuid && resumed->revision == 1);
    }
    {
        // A plan that names the process of its attempt, which has ended, and
        // never says when it worked (sample damage: no "updated", no stop
        // mark): unconfirmed, it resumes as before.
        Fixture fixture;
        const va::ProcessIdentity first{1000, "boot-1:1000", "veriarchive-cli.exe"};
        const auto archive_uuid = interrupted_in(fixture, first, true);
        auto plan = plan_of(fixture.store, archive_uuid);
        plan.erase("updated");
        CHECK(plan.contains("process") && !plan.contains("stopped"));
        CHECK(fixture.store.set_setting("registration:" + archive_uuid, plan.dump()));
        fixture.processes.self = {2000, "boot-1:2000", "veriarchive-cli.exe"};
        auto request = at("/vol");
        request.resume = true;
        const auto resumed = fixture.run(request);
        CHECK(resumed && resumed->archive_uuid == archive_uuid && resumed->revision == 1);
        if (!resumed) {
            std::cerr << "  a plan that names its process: " << resumed.error().message << '\n';
        }
    }
}

// Decision 0022, point 6: a plan says when its attempt worked from the moment
// it is made, in the transaction that writes it, where only the heartbeat's
// writes, which are best effort, said so. A registration whose every later
// write of its plan fails (a trigger, sample failure, standing in for another
// connection that holds the write lock for longer than a statement waits),
// interrupted as a file's device goes away, leaves its plan as it made it:
// saying when it worked and naming its process, without totals or the stop
// mark. Its resume, as a script resumes this version's registrations, needs
// no confirmation (point 7).
void a_plan_says_when_it_worked_from_the_moment_it_is_made()
{
    Fixture fixture;
    fixture.processes.self = {1000, "boot-1:1000", "veriarchive-cli.exe"};
    fixture.repository.execute("CREATE TRIGGER sample_failure BEFORE UPDATE ON settings "
                               "WHEN NEW.key LIKE 'registration:%' BEGIN SELECT RAISE(ABORT, 'sample failure'); END;");
    const auto archive_uuid = interrupted_sample_registration(fixture);
    fixture.repository.execute("DROP TRIGGER sample_failure;");
    const auto plan = plan_of(fixture.store, archive_uuid);
    CHECK(plan.is_object() && va::parse_timestamp(plan.value("updated", std::string{})).has_value() &&
          plan.contains("process") && !plan.contains("files_total") && !plan.contains("stopped"));
    fixture.processes.self = {2000, "boot-1:2000", "veriarchive-cli.exe"};
    auto request = at("/vol");
    request.resume = true;
    const auto resumed = fixture.run(request);
    CHECK(resumed && resumed->archive_uuid == archive_uuid && resumed->revision == 1);
    if (!resumed) {
        std::cerr << "  a plan whose later writes failed: " << resumed.error().message << '\n';
    }
}

// Two resumes started at once on one computer: the first takes the
// registration over in the transaction that finds the attempt before ended,
// so the second, a moment later, finds the first's process running and stops
// before it reads anything. Only one attempt hashes and records.
void resumes_started_at_once_take_turns()
{
    Fixture fixture;
    const auto archive_uuid = interrupted_sample_registration(fixture);
    auto request = at("/vol");
    request.resume = true;
    Attempt first(fixture, fixture.host.host);
    Attempt second(fixture, fixture.host.host);
    second.processes.others = {first.processes.self};
    first.start(fixture, request);
    CHECK(first.staged.try_acquire_for(std::chrono::seconds(60)));
    const auto plan = plan_of(fixture.store, archive_uuid);
    CHECK(plan.contains("process") && plan["process"].value("id", std::uint64_t{0}) == first.processes.self.id);

    second.start(fixture, request);
    second.thread.join();
    CHECK(!second.outcome && second.outcome.error().kind == va::RegisterFailure::Kind::Refused &&
          second.outcome.error().message == still_running_message(first.processes.self));
    CHECK(second.files.bytes_read == 0);
    const auto unchanged = plan_of(fixture.store, archive_uuid);
    CHECK(unchanged.contains("process") && unchanged["process"] == plan["process"]);

    first.go.release();
    first.thread.join();
    CHECK(first.outcome && first.outcome->revision == 1);
    check_index_is_the_manifest(fixture, archive_uuid);
}

// ---- An attempt whose registration is abandoned (decision 0025) ------------

// The operator abandons a registration whose attempt runs where this computer
// cannot see it (another computer sharing the repository) and whose plan has
// said nothing for minutes. The attempt stops at its next checkpoint, writes
// none of its saved progress, and records nothing: the archive of an
// abandoned first registration is gone. The root takes a new registration.
void an_attempt_stops_when_its_first_registration_is_abandoned()
{
    Fixture fixture;
    fill_sample(fixture.archive);
    std::expected<va::AbandonableRegistration, va::AbandonFailure> abandoned =
        std::unexpected(va::AbandonFailure{});
    test::HookedFiles hooked(fixture.archive, "v133.bin", [&] {
        auto store = fixture.repository.store();
        const auto archives = store.archives();
        if (!archives || archives->size() != 1) {
            CHECK(false);
            return;
        }
        const auto archive_uuid = archives->front().archive_uuid;
        auto plan = plan_of(store, archive_uuid);
        plan["updated"] = "2026-01-01T00:00:00Z";  // silent for long
        CHECK(store.set_setting("registration:" + archive_uuid, plan.dump()));
        va::AbandonServices services{store, fixture.processes, fixture.host.host};
        abandoned = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
    });
    va::HostInfo elsewhere = fixture.host;
    elsewhere.host = "another-computer";
    va::RegisterServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media_writer,
        fixture.media, fixture.store, fixture.repository.context.repo, elsewhere, fixture.processes};
    const auto registered = va::register_archive(services, at("/vol"), fixture.cancel, fixture.progress);
    CHECK(abandoned && abandoned->first_registration);
    if (!abandoned) {
        return;
    }
    const auto& archive_uuid = abandoned->archive_uuid;
    CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Refused &&
          registered.error().message == "the unfinished registration of archive " + archive_uuid +
                                            " was abandoned while this attempt ran; this attempt recorded nothing");
    const auto archives = fixture.store.archives();
    CHECK(archives && archives->empty());
    const auto rows = fixture.store.file_rows(archive_uuid, 1);
    CHECK(rows && rows->empty());
    CHECK(fixture.store.setting("registration:" + archive_uuid) == std::string{});
    CHECK(names_in(fixture.repository.context.repo.manifests() / archive_uuid).empty());
    const auto again = fixture.run(at("/vol"));
    CHECK(again && again->revision == 1 && again->archive_uuid != archive_uuid);
}

// As above, but abandoned after the attempt's last checkpoint, while it
// writes its manifest: its record finds the registration abandoned, and
// records nothing, so no revision is left without its archive's record.
void an_attempt_abandoned_while_it_writes_its_manifest_records_nothing()
{
    Fixture fixture;
    fill_sample(fixture.archive);
    std::expected<va::AbandonableRegistration, va::AbandonFailure> abandoned =
        std::unexpected(va::AbandonFailure{});
    test::StagingHookWriter writer(*fixture.repository.writer, [&] {
        auto store = fixture.repository.store();
        const auto archives = store.archives();
        if (!archives || archives->size() != 1) {
            CHECK(false);
            return;
        }
        const auto archive_uuid = archives->front().archive_uuid;
        auto plan = plan_of(store, archive_uuid);
        plan["updated"] = "2026-01-01T00:00:00Z";  // silent for long
        CHECK(store.set_setting("registration:" + archive_uuid, plan.dump()));
        va::AbandonServices services{store, fixture.processes, fixture.host.host};
        abandoned = va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt});
    });
    va::HostInfo elsewhere = fixture.host;
    elsewhere.host = "another-computer";
    va::RegisterServices services{fixture.archive, *fixture.repository.files, writer, fixture.media_writer,
        fixture.media, fixture.store, fixture.repository.context.repo, elsewhere, fixture.processes};
    const auto registered = va::register_archive(services, at("/vol"), fixture.cancel, fixture.progress);
    CHECK(abandoned && abandoned->first_registration && abandoned->files == 40);
    if (!abandoned) {
        return;
    }
    const auto& archive_uuid = abandoned->archive_uuid;
    CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Refused &&
          registered.error().message == "the unfinished registration of archive " + archive_uuid +
                                            " was abandoned while this attempt ran; this attempt recorded nothing");
    const auto revision = fixture.store.revision(archive_uuid, 1);
    CHECK(revision && !*revision);
    const auto archives = fixture.store.archives();
    CHECK(archives && archives->empty());
    const auto rows = fixture.store.file_rows(archive_uuid, 1);
    CHECK(rows && rows->empty());
    CHECK(names_in(fixture.repository.context.repo.manifests() / archive_uuid).empty());
}

// As above for `register --new-revision`, and meanwhile the archive's next
// registration of the same revision began (its plan made later). The
// abandoned attempt stops, and neither its checkpoints, its heartbeat nor its
// stop mark touch that registration's plan; revision 1 stays.
void an_abandoned_attempt_leaves_the_next_registration_alone()
{
    Fixture fixture;
    fixture.archive.add_file("a.txt", "a (sample data)");
    const auto first = fixture.run(at("/vol"));
    CHECK(first.has_value());
    if (!first) {
        return;
    }
    const auto& archive_uuid = first->archive_uuid;
    fill_sample(fixture.archive);
    std::string next_plan;
    test::HookedFiles hooked(fixture.archive, "v133.bin", [&] {
        auto store = fixture.repository.store();
        auto plan = plan_of(store, archive_uuid);
        plan["updated"] = "2026-01-01T00:00:00Z";
        CHECK(store.set_setting("registration:" + archive_uuid, plan.dump()));
        va::AbandonServices services{store, fixture.processes, fixture.host.host};
        CHECK(va::abandon_registration(services, {archive_uuid, std::nullopt, std::nullopt}).has_value());
        // The next registration's plan: of the same revision, made at another time.
        auto next = plan;
        next["created"] = "1999-12-31T23:59:59Z";
        next.erase("process");
        next.erase("updated");
        next_plan = next.dump();
        CHECK(store.set_setting("registration:" + archive_uuid, next_plan));
    });
    va::HostInfo elsewhere = fixture.host;
    elsewhere.host = "another-computer";
    va::RegisterServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media_writer,
        fixture.media, fixture.store, fixture.repository.context.repo, elsewhere, fixture.processes};
    auto request = at("/vol");
    request.new_revision = true;
    const auto registered = va::register_archive(services, request, fixture.cancel, fixture.progress);
    CHECK(!next_plan.empty());
    CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Refused &&
          registered.error().message == "the unfinished registration of archive " + archive_uuid +
                                            " was abandoned while this attempt ran; this attempt recorded nothing");
    CHECK(fixture.store.setting("registration:" + archive_uuid) == next_plan);
    CHECK(!fixture.store.has_file_rows(archive_uuid, 2).value_or(true));
    const auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 1);
}

// Decision 0025: an archive has one unfinished registration at a time. Its
// volume is also reached through a second path, a second mount point (sample
// data): a `register --new-revision` through that path does not find the
// running registration by its root, and would replace its plan, so that the
// running attempt stopped as if abandoned. It is refused instead, before it
// reads anything, naming that registration's root and both ways out; the
// running attempt records its revision. Once a registration has stopped, a
// registration through either path is refused too: through its own root as
// before, now also naming the abandon; and once it is abandoned, the other
// path takes the archive's next revision.
void a_registration_never_replaces_another_unfinished_one_of_its_archive()
{
    Fixture fixture;
    for (int index = 0; index < 40; ++index) {
        const auto name = "f" + std::to_string(100 + index) + ".bin";
        const auto content = std::string(500 + index, static_cast<char>('a' + index % 26));
        fixture.archive.add_file("data/" + name, content);
        fixture.archive.add_file("alias/data/" + name, content);
    }
    auto alias = fixture.media.volume;
    alias.mount_point = fixture.archive.root() / "alias";
    fixture.media.extra_volumes.push_back(alias);
    const auto data = fixture.archive.root() / "data";
    const auto registered = fixture.run(at(data));
    CHECK(registered && registered->revision == 1);
    if (!registered) {
        return;
    }
    const auto& archive_uuid = registered->archive_uuid;
    const auto another = [&](std::uint32_t revision) {
        return "an unfinished registration of archive " + archive_uuid + " (root \"" + va::path_to_utf8(data) +
               "\") is creating revision " + std::to_string(revision) +
               "; nothing was changed (finish it with `register --resume` first, or abandon it with "
               "`archive abandon-registration " +
               archive_uuid + "`)";
    };
    auto through_alias = at(fixture.archive.root() / "alias" / "data");
    through_alias.new_revision = true;
    auto through_data = at(data);
    through_data.new_revision = true;

    // While the first runs.
    fixture.archive.add_file("data/new.bin", "new (sample data)");
    fixture.archive.add_file("alias/data/new.bin", "new (sample data)");
    std::expected<va::RegisterOutcome, va::RegisterFailure> second = std::unexpected(va::RegisterFailure{});
    std::string plan_meanwhile;
    test::HookedFiles hooked(fixture.archive, "f120.bin", [&] {
        // A second command, in a process of its own, which sees the first's.
        auto store = fixture.repository.store();
        plan_meanwhile = store.setting("registration:" + archive_uuid).value_or("");
        test::FakeProcessTable processes;
        processes.self = {7000, "boot-1:7000", "veriarchive-cli.exe"};
        processes.others = {fixture.processes.self};
        va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
            fixture.media_writer, fixture.media, store, fixture.repository.context.repo, fixture.host, processes};
        va::CancelToken cancel;
        test::NullProgress quiet;
        second = va::register_archive(services, through_alias, cancel, quiet);
        CHECK(store.setting("registration:" + archive_uuid).value_or("") == plan_meanwhile);
    });
    va::RegisterServices services{hooked, *fixture.repository.files, *fixture.repository.writer, fixture.media_writer,
        fixture.media, fixture.store, fixture.repository.context.repo, fixture.host, fixture.processes};
    const auto first = va::register_archive(services, through_data, fixture.cancel, fixture.progress);
    CHECK(!plan_meanwhile.empty());
    CHECK(!second && second.error().kind == va::RegisterFailure::Kind::Refused &&
          second.error().message == another(2));
    if (second || second.error().message != another(2)) {
        std::cerr << "  second: " << (second ? std::string("recorded") : second.error().message) << '\n';
    }
    CHECK(first && first->revision == 2);
    const auto verified = fixture.verify(data);
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);

    // The next registration stops: its device went away (sample data).
    fixture.archive.add_file("data/newer.bin", "newer (sample data)");
    fixture.archive.node("data/f130.bin").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto stopped = fixture.run(through_data);
    CHECK(!stopped && stopped.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("data/f130.bin").open_error.reset();
    const auto plan = fixture.store.setting("registration:" + archive_uuid).value_or("");
    const auto refused = fixture.run(through_alias);
    CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Refused &&
          refused.error().message == another(3));
    const auto same_root = fixture.run(through_data);
    CHECK(!same_root && same_root.error().kind == va::RegisterFailure::Kind::Refused &&
          same_root.error().message == "an interrupted registration of this root exists (archive " + archive_uuid +
                                           "); finish it with --resume, or abandon it with `archive "
                                           "abandon-registration " +
                                           archive_uuid + "`");
    CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("") == plan);

    va::AbandonServices abandoning{fixture.store, fixture.processes, fixture.host.host};
    const auto abandoned = va::abandon_registration(abandoning, {archive_uuid, std::nullopt, std::nullopt});
    CHECK(abandoned && abandoned->revision == 3 && !abandoned->first_registration);
    const auto next = fixture.run(through_alias);
    CHECK(next && next->archive_uuid == archive_uuid && next->revision == 3);

    // A plan whose revision is recorded is done with (decision 0023), as one
    // an older version leaves when another operation records its revision:
    // it is replaced, and the archive takes its next revision.
    const nlohmann::json dead_plan{{"archive_uuid", archive_uuid}, {"revision", 3}, {"label", "ARCH-0001"},
        {"created", "2026-10-15T01:00:00Z"}, {"new_archive", false},
        {"root", va::path_to_utf8(fixture.archive.root() / "alias" / "data")}};
    CHECK(fixture.store.set_setting("registration:" + archive_uuid, dead_plan.dump()));
    const auto after_dead = fixture.run(through_data);
    CHECK(after_dead && after_dead->revision == 4);
    if (!after_dead) {
        std::cerr << "  after a dead plan: " << after_dead.error().message << '\n';
    }
}

// ---- A plan made for an older revision (decision 0030) ---------------------

// How a registration made for an older revision is refused.
std::string made_for_an_older_revision(
    const va::Uuid& archive_uuid, std::uint32_t creates, std::uint32_t archive_revision)
{
    return "the unfinished registration of archive " + archive_uuid + " (root \"" +
           va::path_to_utf8(std::filesystem::path("/vol")) + "\") would create revision " + std::to_string(creates) +
           ", but the archive now has a later revision (" + std::to_string(archive_revision) +
           "); the registration can no longer finish: abandon it with `archive abandon-registration " + archive_uuid +
           "`";
}

// The sample at /vol registered as revision 1 (sample data); its uuid.
va::Uuid registered_sample(Fixture& fixture)
{
    fill_sample(fixture.archive);
    const auto first = fixture.run(at("/vol"));
    CHECK(first && first->revision == 1);
    return first ? first->archive_uuid : va::Uuid{};
}

// Decision 0030: a `register --new-revision` that creates revision 2 is
// interrupted (its device went away), and then an import gives the archive
// revision 3 (another repository's export, sample data). The registration can
// no longer finish: revision 2 would set the archive back (R-15). Its resume
// is refused before it reads anything, saying so and naming the abandon, and
// so are a registration of its root without --resume and one of its volume
// through another path (a second mount point), which would otherwise be told
// to finish it. Nothing is recorded: its plan and saved progress stay, and the
// archive stays at revision 3, which verifies. A plan whose revision is
// recorded is done with instead (decision 0023), and keeps the words that
// name its resume, which removes it. Abandoned, the registration leaves the
// archive to its next one.
void a_registration_made_for_an_older_revision_never_records()
{
    Fixture fixture;
    const auto archive_uuid = registered_sample(fixture);
    fixture.archive.node("videos/v130.bin").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    auto again = at("/vol");
    again.new_revision = true;
    const auto interrupted = fixture.run(again);
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("videos/v130.bin").open_error.reset();
    const auto key = "registration:" + archive_uuid;
    const auto plan = fixture.store.setting(key).value_or("");
    const auto saved = fixture.store.file_row_count(archive_uuid, 2).value_or(va::CheckpointCount{});
    CHECK(!plan.empty() && saved.files > 0);

    test::import_later_revision(fixture, fixture.store, archive_uuid, 3);
    const auto refusal = made_for_an_older_revision(archive_uuid, 2, 3);
    fixture.archive.bytes_read = 0;
    auto resume = at("/vol");
    resume.resume = true;
    const auto resumed = fixture.run(resume);
    CHECK(!resumed && resumed.error().kind == va::RegisterFailure::Kind::Refused &&
          resumed.error().message == refusal);
    const auto restarted = fixture.run(again);
    CHECK(!restarted && restarted.error().kind == va::RegisterFailure::Kind::Refused &&
          restarted.error().message == refusal);
    auto alias = fixture.media.volume;
    alias.mount_point = fixture.archive.root() / "alias";
    fixture.media.extra_volumes.push_back(alias);
    auto through_alias = at(fixture.archive.root() / "alias");
    through_alias.new_revision = true;
    const auto other_path = fixture.run(through_alias);
    CHECK(!other_path && other_path.error().kind == va::RegisterFailure::Kind::Refused &&
          other_path.error().message == refusal);
    fixture.media.extra_volumes.clear();
    CHECK(fixture.archive.bytes_read == 0);
    CHECK(fixture.store.setting(key) == plan);
    const auto kept = fixture.store.file_row_count(archive_uuid, 2);
    CHECK(kept && kept->files == saved.files && kept->bytes == saved.bytes);
    const auto revision = fixture.store.revision(archive_uuid, 2);
    CHECK(revision && !*revision);
    const auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 3);
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED" && verified->revision == 3);

    // The plan of a revision that another operation recorded and left behind
    // (sample data): its root's registration says to resume it.
    auto dead = nlohmann::json::parse(plan, nullptr, false);
    dead["revision"] = 3;
    CHECK(fixture.store.set_setting(key, dead.dump()));
    const auto done_with = fixture.run(again);
    CHECK(!done_with && done_with.error().message == "an interrupted registration of this root exists (archive " +
                                                         archive_uuid +
                                                         "); finish it with --resume, or abandon it with `archive "
                                                         "abandon-registration " +
                                                         archive_uuid + "`");
    CHECK(fixture.store.set_setting(key, plan));

    va::AbandonServices abandoning{fixture.store, fixture.processes, fixture.host.host};
    const auto abandoned = va::abandon_registration(abandoning, {archive_uuid, std::nullopt, std::nullopt});
    CHECK(abandoned && abandoned->revision == 2 && abandoned->files == saved.files && !abandoned->first_registration);
    const auto next = fixture.run(again);
    CHECK(next && next->archive_uuid == archive_uuid && next->revision == 4);
    const auto latest = fixture.verify(fixture.archive.root());
    CHECK(latest && latest->result == "PASSED" && latest->revision == 4);
}

// Decision 0030: an import gives the archive revision 3 while a `register
// --new-revision` creating revision 2 writes its manifest. Its record finds
// the archive at a later revision and records nothing: no revision 2, no
// manifest under its name, the archive at revision 3. Its plan says that the
// attempt stopped, and keeps the files it saved.
void a_registration_meets_a_later_revision_at_its_record()
{
    Fixture fixture;
    const auto archive_uuid = registered_sample(fixture);
    test::StagingHookWriter writer(*fixture.repository.writer, [&] {
        auto store = fixture.repository.store(); // another process's connection
        test::import_later_revision(fixture, store, archive_uuid, 3);
    });
    va::RegisterServices services{fixture.archive, *fixture.repository.files, writer, fixture.media_writer,
        fixture.media, fixture.store, fixture.repository.context.repo, fixture.host, fixture.processes};
    auto request = at("/vol");
    request.new_revision = true;
    const auto registered = va::register_archive(services, request, fixture.cancel, fixture.progress);
    CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Refused &&
          registered.error().message == made_for_an_older_revision(archive_uuid, 2, 3));
    const auto revision = fixture.store.revision(archive_uuid, 2);
    CHECK(revision && !*revision);
    const auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 3);
    CHECK(names_in(fixture.repository.context.repo.manifests() / archive_uuid) ==
          (std::set<std::string>{"r0001.vam", "r0003.vam"}));
    const auto plan = plan_of(fixture.store, archive_uuid);
    CHECK(plan.is_object() && plan.value("revision", 0) == 2 && plan.contains("stopped"));
    const auto saved = fixture.store.file_row_count(archive_uuid, 2);
    CHECK(saved && saved->files == 40);
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->result == "PASSED" && verified->revision == 3);
}

// Decision 0030: a registration that can no longer finish stops reading at
// its next checkpoint, after at most 1000 more files, rather than reading the
// rest for nothing. While a `register --new-revision` of 2540 sample files
// reads its 101st, an import gives the archive revision 3: the registration
// stops with less than half of the rest read, and records nothing.
void a_registration_made_for_an_older_revision_stops_reading()
{
    constexpr int many = 2500;
    constexpr std::uint64_t many_file_bytes = 10;
    constexpr std::uint64_t sample_bytes = 20780; // fill_sample's 40 files
    Fixture fixture;
    for (int index = 0; index < many; ++index) {
        fixture.archive.add_file(std::format("many/m{:05}.txt", index), std::string(many_file_bytes, 'm'));
    }
    const auto archive_uuid = registered_sample(fixture);
    std::uint64_t read_before = 0;
    test::HookedFiles hooked(fixture.archive, "m00100.txt", [&] {
        auto store = fixture.repository.store(); // another process's connection
        test::import_later_revision(fixture, store, archive_uuid, 3);
        read_before = fixture.archive.bytes_read;
    });
    va::RegisterServices services{hooked, *fixture.repository.files, *fixture.repository.writer,
        fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    auto request = at("/vol");
    request.new_revision = true;
    const auto registered = va::register_archive(services, request, fixture.cancel, fixture.progress);
    CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Refused &&
          registered.error().message == made_for_an_older_revision(archive_uuid, 2, 3));
    // From the 101st file on, the volume holds 2400 more of them and the 40
    // others; it stops after about 900.
    const auto unread = (many - 100) * many_file_bytes + sample_bytes;
    const auto read_after = fixture.archive.bytes_read - read_before;
    CHECK(read_after < unread / 2);
    if (read_after >= unread / 2) {
        std::cerr << "  read " << read_after << " of " << unread
                  << " bytes after the archive had a later revision\n";
    }
    const auto revision = fixture.store.revision(archive_uuid, 2);
    CHECK(revision && !*revision);
}

// Decision 0030: an interrupted attempt says to go on with --resume, but not
// once its plan was made for an older revision, which a resume refuses. Here
// an import gives the archive revision 3 as a `register --new-revision`
// creating revision 2 opens its first file, where the device goes away: no
// checkpoint had files to save, so none looked. The attempt ends as one made
// for an older revision, not as interrupted, in the words of its resume,
// which reads nothing; its plan stays, saying that it stopped, and nothing is
// recorded.
void an_interrupted_registration_made_for_an_older_revision_says_to_abandon_it()
{
    Fixture fixture;
    const auto archive_uuid = registered_sample(fixture);
    test::HookedFiles hooked(fixture.archive, "p100.bin", [&] {
        auto store = fixture.repository.store(); // another process's connection
        test::import_later_revision(fixture, store, archive_uuid, 3);
        fixture.archive.node("photos/p100.bin").open_error =
            va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    });
    va::RegisterServices services{hooked, *fixture.repository.files, *fixture.repository.writer,
        fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    auto request = at("/vol");
    request.new_revision = true;
    const auto registered = va::register_archive(services, request, fixture.cancel, fixture.progress);
    const auto refusal = made_for_an_older_revision(archive_uuid, 2, 3);
    CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Refused &&
          registered.error().message == refusal);
    if (!registered && registered.error().message != refusal) {
        std::cerr << "  interrupted after an import: " << registered.error().message << '\n';
    }
    const auto plan = plan_of(fixture.store, archive_uuid);
    CHECK(plan.is_object() && plan.value("revision", 0) == 2 && plan.contains("stopped"));
    const auto revision = fixture.store.revision(archive_uuid, 2);
    CHECK(revision && !*revision);
    const auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 3);

    fixture.archive.node("photos/p100.bin").open_error.reset();
    fixture.archive.bytes_read = 0;
    auto resume = at("/vol");
    resume.resume = true;
    const auto resumed = fixture.run(resume);
    CHECK(!resumed && resumed.error().message == refusal && fixture.archive.bytes_read == 0);
}

// A resume cancelled while its takeover waits for the write lock (decision
// 0024, point 3) says to go on with --resume, but not for a plan made for an
// older revision, which a resume refuses (decision 0030). A `register
// --new-revision` creating revision 2 was interrupted; then another
// connection holds the write lock while a resume, already cancelled, waits
// for it (scaled down: its statements wait 100 ms). It names the resume, and
// once an import has given the archive revision 3, the abandon.
void a_cancelled_resume_of_a_plan_made_for_an_older_revision_says_to_abandon_it()
{
    Fixture fixture;
    const auto archive_uuid = registered_sample(fixture);
    fixture.archive.node("videos/v130.bin").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    auto again = at("/vol");
    again.new_revision = true;
    const auto interrupted = fixture.run(again);
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("videos/v130.bin").open_error.reset();
    const auto cancelled_resume = [&] {
        auto holder = fixture.repository.store(); // another process's write lock
        CHECK(holder.begin());
        auto store = fixture.repository.store(std::chrono::milliseconds(100));
        va::CancelToken cancel;
        cancel.cancel();
        va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
            fixture.media_writer, fixture.media, store, fixture.repository.context.repo, fixture.host,
            fixture.processes};
        auto resume = at("/vol");
        resume.resume = true;
        auto resumed = va::register_archive(services, resume, cancel, fixture.progress);
        holder.rollback();
        return resumed;
    };
    const auto before = cancelled_resume();
    CHECK(!before && before.error().kind == va::RegisterFailure::Kind::Incomplete &&
          before.error().message == "registration was cancelled; continue with `register <root_path> --resume`");

    test::import_later_revision(fixture, fixture.store, archive_uuid, 3);
    const auto after = cancelled_resume();
    CHECK(!after && after.error().kind == va::RegisterFailure::Kind::Refused &&
          after.error().message == made_for_an_older_revision(archive_uuid, 2, 3));
    const auto plan = plan_of(fixture.store, archive_uuid);
    CHECK(plan.is_object() && plan.value("revision", 0) == 2);
}

// Decision 0030, point 6: a `register --new-revision` is made for the
// revision the archive is at when it starts. Here another operation records a
// later revision after the registration read the archive at revision 1, as it
// reads the header of revision 1 (the second opening of r0001.vam), before it
// writes its plan: an import of revision 3, which would leave the
// registration made for an older revision, or of revision 2, the one it would
// create. The transaction that would write the plan finds the archive at that
// revision: it writes nothing, reads nothing of the media, and says to run it
// again, which then creates the revision after it.
void a_registration_overtaken_before_its_plan_writes_nothing()
{
    for (const std::uint32_t imported : {3u, 2u}) {
        Fixture fixture;
        const auto archive_uuid = registered_sample(fixture);
        test::HookedFiles hooked(
            *fixture.repository.files, "r0001.vam",
            [&] {
                auto store = fixture.repository.store(); // another process's connection
                test::import_later_revision(fixture, store, archive_uuid, imported);
            },
            2);
        va::RegisterServices services{fixture.archive, hooked, *fixture.repository.writer, fixture.media_writer,
            fixture.media, fixture.store, fixture.repository.context.repo, fixture.host, fixture.processes};
        auto request = at("/vol");
        request.new_revision = true;
        fixture.archive.bytes_read = 0;
        const auto registered = va::register_archive(services, request, fixture.cancel, fixture.progress);
        const auto refusal = "revision " + std::to_string(imported) + " of archive " + archive_uuid +
                             " was recorded by another operation before this registration could begin; nothing was "
                             "changed (run `register --new-revision` again)";
        CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Refused &&
              registered.error().message == refusal);
        if (!registered && registered.error().message != refusal) {
            std::cerr << "  overtaken by revision " << imported << ": " << registered.error().message << '\n';
        }
        CHECK(fixture.archive.bytes_read == 0);
        CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("x").empty());
        const auto archive = fixture.store.find_archive(archive_uuid);
        CHECK(archive && *archive && (*archive)->current_revision == imported);
        const auto again = fixture.run(request);
        CHECK(again && again->revision == imported + 1);
    }
}

// ---- A registration over a lost manifest (decision 0028, point 8)

// The rows of `table` in the fixture's database, as another process would read them.
std::int64_t rows_of(Fixture& fixture, const std::string& table)
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

// The match result and the note of the newest media evidence.
std::string newest_evidence(Fixture& fixture)
{
    sqlite3* database = nullptr;
    const auto name = fixture.repository.context.repo.database().u8string();
    std::string evidence;
    if (sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &database, SQLITE_OPEN_READONLY, nullptr) ==
        SQLITE_OK) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database,
                "SELECT match_result || ' / ' || operator_note FROM media_evidence ORDER BY id DESC LIMIT 1", -1,
                &statement, nullptr) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            evidence = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close(database);
    return evidence;
}

// An archive at /vol/a (sample data) registered twice, and verified at
// revision 2 by its uuid; its archive uuid. `media_copy`: both registrations
// write a media copy on it (R-89).
va::Uuid archive_a_registered_twice(Fixture& fixture, bool media_copy = false)
{
    fixture.archive.add_file("a/SAMPLE-one.txt", "one (sample data)");
    auto first = at("/vol/a");
    first.write_media_copy = media_copy;
    const auto registered = fixture.run(first);
    fixture.archive.add_file("a/SAMPLE-two.txt", "two (sample data)");
    auto second = at("/vol/a");
    second.new_revision = true;
    second.write_media_copy = media_copy;
    const auto again = fixture.run(second);
    CHECK(registered && again && again->revision == 2);
    if (!registered) {
        return {};
    }
    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = registered->archive_uuid;
    const auto verified = fixture.verify(by_uuid);
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
    return registered->archive_uuid;
}

va::RegisterRequest over_lost_manifest(const std::filesystem::path& root, const va::Uuid& archive_uuid, bool confirmed)
{
    auto request = at(root);
    request.lost_manifest_of = archive_uuid;
    request.confirmed = confirmed;
    return request;
}

std::vector<std::uint32_t> revisions_of(Fixture& fixture, const va::Uuid& archive_uuid)
{
    std::vector<std::uint32_t> numbers;
    if (const auto revisions = fixture.store.revisions(archive_uuid)) {
        for (const auto& revision : *revisions) {
            numbers.push_back(revision.revision);
        }
    }
    return numbers;
}

// Decision 0028, point 8: the latest manifest of archive A (revision 2, at
// /vol/a, verified there) is lost (sample damage): it is gone, its first line
// changed, its body changed (which `repo rebuild-index` skipped, marking the
// archive), or it is gone with its record, so that the index has no record
// of it. Meanwhile a registration of a new folder of the volume stops on it,
// unless only the body changed, which matching does not read (line 1 alone).
// A registration over the lost manifest that is not confirmed says what it
// would register, and writes and reads nothing of it. Confirmed, it registers
// revision 3 from the root, which supersedes revision 2: the archive is at
// revision 3, never back at an earlier one; its history keeps its run and an
// intact hash chain; the record of revision 2 leaves the index with its rows
// where its manifest's first line cannot be used, and stays where it can (only
// its body changed); the rebuild's mark goes; the root's media evidence says
// that it matched nothing. The archive then verifies at revision 3, is listed
// without a problem, also with its earlier revisions read, and a
// registration of a new folder goes ahead.
void a_registration_over_a_lost_manifest_registers_the_next_revision()
{
    enum class Case { Missing, FirstLine, Body, NotIndexed };
    for (const auto what : {Case::Missing, Case::FirstLine, Case::Body, Case::NotIndexed}) {
        Fixture fixture;
        const auto archive_uuid = archive_a_registered_twice(fixture);
        const auto& layout = fixture.repository.context.repo;
        const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
        auto bytes = read_bytes(second);
        auto kind = va::UnusableManifest::Kind::Missing;
        if (what == Case::Missing || what == Case::NotIndexed) {
            std::filesystem::remove(second);
        } else if (what == Case::FirstLine) {
            bytes[bytes.find("\"volume_label\":\"ARCHIVE\"") + 16] ^= 0x01; // "ARCHIVE" -> "@RCHIVE"
            std::ofstream(second, std::ios::binary | std::ios::trunc) << bytes;
            kind = va::UnusableManifest::Kind::Changed;
        } else {
            bytes[bytes.size() - 5] ^= 0x01;
            std::ofstream(second, std::ios::binary | std::ios::trunc) << bytes;
            kind = va::UnusableManifest::Kind::Changed;
            const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
            CHECK(rebuilt && rebuilt->revisions_kept.size() == 1);
            CHECK(!fixture.store.setting(va::skipped_manifest_key(archive_uuid)).value_or("").empty());
        }
        if (what == Case::NotIndexed) {
            fixture.repository.execute("DELETE FROM archive_revisions WHERE revision = 2; "
                                       "DELETE FROM files WHERE revision = 2");
            kind = va::UnusableManifest::Kind::NotIndexed;
        }
        fixture.archive.add_file("b/SAMPLE-three.txt", "three (sample data)");
        fixture.archive.add_file("c/SAMPLE-four.txt", "four (sample data)");
        const auto stopped = fixture.run(at("/vol/b"));
        if (what == Case::Body) {
            CHECK(stopped.has_value());
        } else {
            CHECK(!stopped && stopped.error().kind == va::RegisterFailure::Kind::Repository &&
                  stopped.error().message.ends_with(test::put_back(archive_uuid, 2)));
        }

        // Not confirmed: what it would register, and nothing written.
        const auto runs_before = rows_of(fixture, "verify_runs");
        const auto evidence_before = rows_of(fixture, "media_evidence");
        const auto revisions_before = revisions_of(fixture, archive_uuid);
        fixture.archive.bytes_read = 0;
        const auto asked = fixture.run(over_lost_manifest("/vol/a", archive_uuid, false));
        CHECK(!asked && asked.error().kind == va::RegisterFailure::Kind::Unconfirmed &&
              asked.error().message == "run the command again with --confirm to register revision 3");
        CHECK(!asked && asked.error().lost_manifest && asked.error().lost_manifest->archive_uuid == archive_uuid &&
              asked.error().lost_manifest->label == "ARCH-0001" && asked.error().lost_manifest->lost_revision == 2 &&
              asked.error().lost_manifest->revision == 3 && asked.error().lost_manifest->root == "/vol/a" &&
              asked.error().lost_manifest->manifest.kind == kind &&
              va::plain_message(asked.error().lost_manifest->manifest).ends_with(test::put_back(archive_uuid, 2)));
        if (asked) {
            std::cerr << "  not confirmed: registered revision " << asked->revision << '\n';
        }
        CHECK(fixture.archive.bytes_read == 0 && rows_of(fixture, "media_evidence") == evidence_before &&
              revisions_of(fixture, archive_uuid) == revisions_before);
        CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("x").empty());
        auto archive = fixture.store.find_archive(archive_uuid);
        CHECK(archive && *archive && (*archive)->current_revision == 2);

        // Confirmed: revision 3 from what the root holds now.
        const auto registered = fixture.run(over_lost_manifest("/vol/a", archive_uuid, true));
        const bool left_out = what == Case::Missing || what == Case::FirstLine;
        CHECK(registered && registered->revision == 3 && registered->lost_revision == std::optional<std::uint32_t>(2) &&
              registered->lost_revision_left_out == left_out && registered->counts.files == 2);
        if (!registered) {
            std::cerr << "  over the lost manifest: " << registered.error().message << '\n';
            continue;
        }
        archive = fixture.store.find_archive(archive_uuid);
        CHECK(archive && *archive && (*archive)->current_revision == 3 && (*archive)->status == "REGISTERED" &&
              (*archive)->root_subpath == "a");
        const auto expected = what == Case::Body ? std::vector<std::uint32_t>{1, 2, 3} : std::vector<std::uint32_t>{1, 3};
        CHECK(revisions_of(fixture, archive_uuid) == expected);
        const auto rows = fixture.store.file_row_count(archive_uuid, 2);
        CHECK(rows && rows->files == (what == Case::Body ? 2u : 0u));
        auto reader = va::ManifestReader::open(*fixture.repository.files, registered->manifest);
        CHECK(reader && reader->header().revision == 3 && reader->header().supersedes == 2u &&
              reader->header().root_subpath == "a");
        CHECK(rows_of(fixture, "verify_runs") == runs_before);
        const auto chain = fixture.store.check_history(fixture.repository.context.repository_uuid);
        CHECK(chain && chain->runs == 1 && !chain->first_broken_run);
        CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("x").empty());
        CHECK(fixture.store.setting(va::skipped_manifest_key(archive_uuid)).value_or("x").empty());
        CHECK(newest_evidence(fixture) == "UNKNOWN / registered over the lost manifest of revision 2");

        va::VerifyRequest by_uuid;
        by_uuid.archive_uuid = archive_uuid;
        const auto verified = fixture.verify(by_uuid);
        CHECK(verified && verified->result == "PASSED" && verified->revision == 3 && verified->media_read);
        const auto statuses = va::archive_statuses(fixture.store, *fixture.repository.files, layout, va::Policy{},
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
            va::StatusSelection{archive_uuid, true, true});
        CHECK(statuses && statuses->size() == 1 && statuses->front().header &&
              !statuses->front().unusable_manifest && statuses->front().earlier_unusable.empty());
        const auto other = fixture.run(at("/vol/c"));
        CHECK(other && other->revision == 1 && other->archive_uuid != archive_uuid);
        if (!other) {
            std::cerr << "  the new folder after the lost manifest: " << other.error().message << '\n';
        }
    }
}

// Decision 0028, point 8: nothing is registered over a manifest that is not
// lost, and the request names the archive and its revision alone. Refused,
// writing nothing: an archive whose manifest can be used (register its next
// revision as usual), one whose index only differs from the intact manifest
// or has no record of a manifest that reads as its own (rebuild the index),
// one whose first registration never finished, and an archive that is not in
// the repository. A label, --new-revision, --force-new-archive and --resume
// are usage errors beside it, as is --confirm without it.
void a_registration_over_a_manifest_that_is_not_lost_is_refused()
{
    {
        Fixture fixture;
        const auto archive_uuid = archive_a_registered_twice(fixture);
        const auto usable = fixture.run(over_lost_manifest("/vol/a", archive_uuid, true));
        CHECK(!usable && usable.error().kind == va::RegisterFailure::Kind::Refused &&
              usable.error().message == "the manifest of revision 2 of archive " + archive_uuid +
                                            " can be used: register its next revision with `register <root_path> "
                                            "--new-revision`");

        fixture.repository.execute("UPDATE archive_revisions SET header_json = replace(header_json, "
                                   "'\"volume_label\":\"ARCHIVE\"', '\"volume_label\":\"OTHER\"') WHERE revision = 2");
        const auto differs = fixture.run(over_lost_manifest("/vol/a", archive_uuid, true));
        CHECK(!differs && differs.error().kind == va::RegisterFailure::Kind::Refused &&
              differs.error().message ==
                  "the manifest of revision 2 of archive " + archive_uuid +
                      " is not lost: the repository's index differs from the manifest of revision 2 (manifests/" +
                      archive_uuid + "/r0002.vam); rebuild the index with `repo rebuild-index`");
        fixture.repository.execute("DELETE FROM archive_revisions WHERE revision = 2; "
                                   "DELETE FROM files WHERE revision = 2");
        const auto not_indexed = fixture.run(over_lost_manifest("/vol/a", archive_uuid, true));
        CHECK(!not_indexed && not_indexed.error().kind == va::RegisterFailure::Kind::Refused &&
              not_indexed.error().message ==
                  "the manifest of revision 2 of archive " + archive_uuid +
                      " is not lost: the repository's index has no record of the manifest of revision 2 (manifests/" +
                      archive_uuid + "/r0002.vam); rebuild the index with `repo rebuild-index`");
        CHECK(revisions_of(fixture, archive_uuid) == std::vector<std::uint32_t>{1});
        const auto archive = fixture.store.find_archive(archive_uuid);
        CHECK(archive && *archive && (*archive)->current_revision == 2);
        CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("x").empty());

        const va::Uuid unknown = "c0ffee00-0000-4000-8000-0000000000aa"; // sample data
        const auto none = fixture.run(over_lost_manifest("/vol/a", unknown, true));
        CHECK(!none && none.error().kind == va::RegisterFailure::Kind::Refused &&
              none.error().message == "no archive " + unknown);

        auto labelled = over_lost_manifest("/vol/a", archive_uuid, true);
        labelled.label = "ARCH-0009";
        auto new_revision = over_lost_manifest("/vol/a", archive_uuid, true);
        new_revision.new_revision = true;
        auto separate = over_lost_manifest("/vol/a", archive_uuid, true);
        separate.force_new_archive = true;
        auto resumed = over_lost_manifest("/vol/a", archive_uuid, true);
        resumed.resume = true;
        for (const auto& request : {labelled, new_revision, separate, resumed}) {
            const auto refused = fixture.run(request);
            CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Invalid &&
                  refused.error().message == "--lost-manifest takes none of --label, --new-revision, "
                                             "--force-new-archive and --resume");
        }
        auto confirmed_alone = at("/vol/a");
        confirmed_alone.confirmed = true;
        const auto alone = fixture.run(confirmed_alone);
        CHECK(!alone && alone.error().kind == va::RegisterFailure::Kind::Invalid &&
              alone.error().message == "--confirm is used only with --lost-manifest or --resume");
    }
    {
        Fixture fixture;
        const auto interrupted_uuid = interrupted_sample_registration(fixture);
        const auto first = fixture.run(over_lost_manifest("/vol/photos", interrupted_uuid, true));
        CHECK(!first && first.error().kind == va::RegisterFailure::Kind::Refused &&
              first.error().message == "archive " + interrupted_uuid + " has no revision yet");
    }
}

// How a registration over a lost manifest refuses a root that overlaps the
// root of archive `other` on a volume that is plainly that archive's: its
// filesystem id and device, as that archive's latest manifest records them
// (a MATCH, decision 0028, point 8).
std::string overlaps_the_root_of(const va::Uuid& other)
{
    return "the root overlaps the root of archive " + other +
           ", whose latest manifest records this volume's filesystem id and device (decision 0001)";
}

// Decision 0028, point 8: the root of a registration over a lost manifest is
// the operator's word, as only the lost manifest recorded the archive's root,
// and it is refused where it is plainly not the archive's: where it contains
// or lies in the root of an archive whose manifest can be used on a volume
// that is plainly that archive's (here B at /vol/b, a MATCH of B), and where
// its identity file names another archive than one whose very root it is.
// B's very root is taken, naming B, as the archive named may be its twin,
// also where its identity file names B, but not where it names a third
// archive. Where the
// root holds the lost manifest as a media copy that `import` puts back, the
// registration says to import it instead; imported, the manifest can be used
// again. A media copy that is not the one recorded, as one with a byte of its
// body changed, puts nothing back, and does not stop the registration. Where
// the index has no record of the manifest either, a media copy that reads as
// it says the same. Sample data throughout.
void a_registration_over_a_lost_manifest_needs_the_archives_own_root()
{
    Fixture fixture;
    const auto archive_uuid = archive_a_registered_twice(fixture, true);
    fixture.archive.add_file("b/SAMPLE-three.txt", "three (sample data)");
    const auto b = fixture.run(at("/vol/b"));
    CHECK(b.has_value());
    if (!b) {
        return;
    }
    const auto& layout = fixture.repository.context.repo;
    const auto recorded = va::manifest_path(layout.manifests(), archive_uuid, 2);
    const auto kept = read_bytes(recorded);
    std::filesystem::remove(recorded);
    const auto refused = [&](const std::filesystem::path& root, const std::string& message) {
        const auto registered = fixture.run(over_lost_manifest(root, archive_uuid, false));
        CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Refused &&
              registered.error().message == message);
        if (!registered && registered.error().message != message) {
            std::cerr << "  at " << va::path_to_utf8(root) << ": " << registered.error().message << '\n';
        }
    };
    const auto asks = [&](const std::filesystem::path& root) {
        const auto registered = fixture.run(over_lost_manifest(root, archive_uuid, false));
        CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Unconfirmed);
        if (!registered && registered.error().kind != va::RegisterFailure::Kind::Unconfirmed) {
            std::cerr << "  at " << va::path_to_utf8(root) << ": " << registered.error().message << '\n';
        }
    };
    const auto overlaps_b = overlaps_the_root_of(b->archive_uuid);
    refused("/vol", overlaps_b);
    fixture.archive.add_directory("b/sub");
    refused("/vol/b/sub", overlaps_b);
    // B's very root: the same root as B's, named before the confirmation.
    const auto asks_naming_b = [&](const char* what) {
        const auto registered = fixture.run(over_lost_manifest("/vol/b", archive_uuid, false));
        CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Unconfirmed &&
              registered.error().lost_manifest && registered.error().lost_manifest->possible_clones.empty() &&
              registered.error().lost_manifest->same_root.size() == 1 &&
              registered.error().lost_manifest->same_root.front().archive_uuid == b->archive_uuid &&
              registered.error().lost_manifest->same_root.front().label == "ARCH-0002");
        if (!registered && registered.error().kind != va::RegisterFailure::Kind::Unconfirmed) {
            std::cerr << "  B's root, " << what << ": " << registered.error().message << '\n';
        }
    };
    asks_naming_b("no identity file");
    const auto b_identity = std::filesystem::path("/vol/b") / ".veriarchive" / "identity.json";
    fixture.archive.put(b_identity, "{\"archive_uuid\":\"" + b->archive_uuid + "\"}");
    asks_naming_b("its identity file naming B");
    const va::Uuid third = "c0ffee00-0000-4000-8000-0000000000bb"; // sample data
    fixture.archive.put(b_identity, "{\"archive_uuid\":\"" + third + "\"}");
    refused("/vol/b", "the identity file of this root (.veriarchive/identity.json) names archive " + third +
                          ", not archive " + archive_uuid);
    fixture.archive.remove("b/.veriarchive");

    // The media copy that the registration of revision 2 wrote on A.
    const auto copy = std::filesystem::path("/vol/a") / ".veriarchive" / "r0002.vam";
    const auto import_it = "the media copy at this root holds the manifest of revision 2 of archive " + archive_uuid +
                           ": put it back with `import " + va::path_to_utf8(copy) + "`";
    refused("/vol/a", import_it);
    const auto copy_bytes = fixture.archive.content_at(copy);
    auto damaged = copy_bytes;
    damaged[damaged.size() - 5] ^= 0x01;
    fixture.archive.put(copy, damaged);
    asks("/vol/a");
    // The identity file that the media copy wrote names A: another archive's stops it.
    const auto identity = std::filesystem::path("/vol/a") / ".veriarchive" / "identity.json";
    const auto identity_bytes = fixture.archive.content_at(identity);
    CHECK(identity_bytes.contains(archive_uuid));
    fixture.archive.put(identity, "{\"archive_uuid\":\"" + b->archive_uuid + "\"}");
    refused("/vol/a", "the identity file of this root (.veriarchive/identity.json) names archive " + b->archive_uuid +
                          ", not archive " + archive_uuid);
    fixture.archive.put(identity, identity_bytes);
    fixture.archive.add_directory("a/sub");
    asks("/vol/a/sub");

    // The index has no record of it either: the copy reads as its manifest.
    fixture.archive.put(copy, copy_bytes);
    fixture.repository.execute("DELETE FROM archive_revisions WHERE revision = 2; DELETE FROM files WHERE revision = 2");
    refused("/vol/a", import_it);
    const auto imported = va::import_manifest(fixture.store, fixture.archive, *fixture.repository.writer, layout, copy,
        false);
    CHECK(imported && imported->revision == 2 && !imported->already_present);
    refused("/vol/a", "the manifest of revision 2 of archive " + archive_uuid +
                          " can be used: register its next revision with `register <root_path> --new-revision`");
    CHECK(read_bytes(recorded) == kept);
    CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("x").empty());
}

// Decision 0028, point 8: a device and a sector-by-sector clone of it,
// registered as two archives (decision 0005): A on the device (sample volume
// /vol/SAMPLE-source), B on the clone (/vol/SAMPLE-cloned, registered with
// --force-new-archive), both volumes with the device's filesystem id, each
// device with a serial of its own (SAMPLE-SERIAL-1, SAMPLE-SERIAL-2) or both
// behind bridges that give none. With A's manifest lost, the registration over
// it takes A's own volume and a folder of it, the clone attached or not: that
// volume matches B's only as MIGRATED (another serial) or MATCH_WEAK (no
// serial to compare), which a clone reads as, so it is not plainly B's. Not
// confirmed, it names B, at the root of A's volume, which is B's very root on
// a volume that matches B's only so, as the archive whose root it is, with
// the match, and at a folder of that volume, which lies in B's root there, as
// possibly a clone; it changes nothing. Confirmed, it registers A's revision
// 2 from the device, and a new folder registers after it. B's own volume, a
// MATCH of B (its filesystem id and its serial), is B's very root, named
// with the match MATCH; without serials it reads as the device does,
// MATCH_WEAK, named so. Sample data.
void a_registration_over_a_lost_manifest_takes_its_own_volume_beside_a_registered_clone()
{
    for (const bool serials : {true, false}) {
        for (const bool clone_attached : {true, false}) {
            Fixture fixture;
            const auto device = fixture.archive.root() / "SAMPLE-source";
            const auto clone = fixture.archive.root() / "SAMPLE-cloned";
            auto device_volume = test::fake_volume(device);
            auto clone_volume = test::fake_volume(clone);
            device_volume.devices.front().serial = serials ? "SAMPLE-SERIAL-1" : "";
            clone_volume.devices.front().serial = serials ? "SAMPLE-SERIAL-2" : "";
            fixture.media.volume = device_volume;
            fixture.media.extra_volumes = {clone_volume};
            for (const auto* side : {"SAMPLE-source", "SAMPLE-cloned"}) {
                fixture.archive.add_file(std::string(side) + "/SAMPLE-one.txt", "one (sample data)");
                fixture.archive.add_file(std::string(side) + "/sub/SAMPLE-two.txt", "two (sample data)");
            }
            const auto a = fixture.run(at(device));
            auto separately = at(clone);
            separately.force_new_archive = true;
            const auto b = fixture.run(separately);
            CHECK(a && b && a->archive_uuid != b->archive_uuid);
            if (!a || !b) {
                return;
            }
            std::filesystem::remove(va::manifest_path(fixture.repository.context.repo.manifests(), a->archive_uuid, 1));
            if (!clone_attached) {
                fixture.media.extra_volumes.clear();
            }
            const auto clone_match = serials ? va::MatchResult::Migrated : va::MatchResult::MatchWeak;
            // `root` is B's very root (`very_root`), named as the archive whose
            // root it is, or lies in it, named as possibly a clone; each with
            // the match of the root's volume and B's.
            const auto asks_naming_b = [&](const std::filesystem::path& root, bool very_root, va::MatchResult match) {
                const auto asked = fixture.run(over_lost_manifest(root, a->archive_uuid, false));
                CHECK(!asked && asked.error().kind == va::RegisterFailure::Kind::Unconfirmed &&
                      asked.error().lost_manifest);
                if (asked || !asked.error().lost_manifest) {
                    if (!asked) {
                        std::cerr << "  at " << va::path_to_utf8(root) << ": " << asked.error().message << '\n';
                    }
                    return;
                }
                const auto& lost = *asked.error().lost_manifest;
                const auto named = very_root ? lost.same_root.size() : lost.possible_clones.size();
                const auto other = very_root ? lost.possible_clones.size() : lost.same_root.size();
                CHECK(named == 1 && other == 0);
                if (very_root && named == 1) {
                    CHECK(lost.same_root.front().archive_uuid == b->archive_uuid &&
                          lost.same_root.front().label == "ARCH-0002" && lost.same_root.front().match == match);
                } else if (named == 1) {
                    CHECK(lost.possible_clones.front().archive_uuid == b->archive_uuid &&
                          lost.possible_clones.front().label == "ARCH-0002" &&
                          lost.possible_clones.front().match == match);
                }
                CHECK(fixture.store.setting("registration:" + a->archive_uuid).value_or("x").empty());
            };
            asks_naming_b(device, true, clone_match);
            asks_naming_b(device / "sub", false, clone_match);
            if (clone_attached) {
                // B's own volume: a MATCH of B at its very root where each
                // device has its serial; without serials it reads as the
                // device does.
                asks_naming_b(clone, true, serials ? va::MatchResult::Match : va::MatchResult::MatchWeak);
            }
            CHECK(revisions_of(fixture, a->archive_uuid) == std::vector<std::uint32_t>{1});

            const auto registered = fixture.run(over_lost_manifest(device, a->archive_uuid, true));
            CHECK(registered && registered->archive_uuid == a->archive_uuid && registered->revision == 2 &&
                  registered->lost_revision == std::optional<std::uint32_t>(1) && registered->counts.files == 2);
            if (!registered) {
                std::cerr << "  over the lost manifest beside a clone: " << registered.error().message << '\n';
                continue;
            }
            const auto archive = fixture.store.find_archive(a->archive_uuid);
            CHECK(archive && *archive && (*archive)->current_revision == 2 && (*archive)->root_subpath.empty());
            // Another sample volume (its mount point no shorter than the
            // device's, as the fake enumerator wants it) registers after it.
            const auto another = fixture.archive.root() / "SAMPLE-another-volume";
            fixture.archive.add_file("SAMPLE-another-volume/SAMPLE-three.txt", "three (sample data)");
            fixture.media.extra_volumes.push_back(test::fake_volume(another));
            fixture.media.extra_volumes.back().filesystem_uuid = "5555666677778888";
            fixture.media.extra_volumes.back().filesystem_uuid_short = "77778888";
            fixture.media.extra_volumes.back().devices.front().serial = "SAMPLE-SERIAL-3";
            const auto other = fixture.run(at(another));
            CHECK(other && other->revision == 1 && other->archive_uuid != a->archive_uuid &&
                  other->archive_uuid != b->archive_uuid);
        }
    }
}

// Decision 0028, point 8 (operator, 2026-10-17): the same folder, /vol/a
// (sample data), registered on purpose as two archives, A and its twin B
// (R-97, --force-new-archive), each with a media copy, so that the identity
// file on it names B, the last one written. A's manifest is lost: the
// registration over it takes the folder, B's very root, naming B as the
// archive whose root it is, as A is named by its uuid and the operator
// confirms; a folder in it, or one that contains it, stays refused as
// overlapping B's root, and an identity file that names a third archive
// refuses the folder. Not confirmed, nothing is written; confirmed, A's
// revision 2 is registered from the folder, B is left as it was, and the
// folder, both archives' root again, matches both (AMBIGUOUS).
void a_registration_over_a_lost_manifest_takes_the_root_of_its_twin()
{
    Fixture fixture;
    fixture.archive.add_file("a/SAMPLE-one.txt", "one (sample data)");
    fixture.archive.add_file("a/sub/SAMPLE-two.txt", "two (sample data)");
    auto first = at("/vol/a");
    first.write_media_copy = true;
    const auto a = fixture.run(first);
    auto twice = at("/vol/a");
    twice.force_new_archive = true;
    twice.write_media_copy = true;
    const auto b = fixture.run(twice);
    CHECK(a && b && a->archive_uuid != b->archive_uuid);
    if (!a || !b) {
        return;
    }
    const auto identity = std::filesystem::path("/vol/a") / ".veriarchive" / "identity.json";
    CHECK(fixture.archive.content_at(identity).contains(b->archive_uuid));
    const auto b_manifest = read_bytes(va::manifest_path(fixture.repository.context.repo.manifests(), b->archive_uuid, 1));
    std::filesystem::remove(va::manifest_path(fixture.repository.context.repo.manifests(), a->archive_uuid, 1));

    const auto asked = fixture.run(over_lost_manifest("/vol/a", a->archive_uuid, false));
    CHECK(!asked && asked.error().kind == va::RegisterFailure::Kind::Unconfirmed && asked.error().lost_manifest &&
          asked.error().lost_manifest->possible_clones.empty() && asked.error().lost_manifest->same_root.size() == 1 &&
          asked.error().lost_manifest->same_root.front().archive_uuid == b->archive_uuid &&
          asked.error().lost_manifest->same_root.front().label == "ARCH-0002");
    if (!asked && asked.error().kind != va::RegisterFailure::Kind::Unconfirmed) {
        std::cerr << "  the root of its twin: " << asked.error().message << '\n';
    }
    CHECK(fixture.store.setting("registration:" + a->archive_uuid).value_or("x").empty());
    for (const auto* folder : {"/vol/a/sub", "/vol"}) {
        const auto refused = fixture.run(over_lost_manifest(folder, a->archive_uuid, false));
        CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Refused &&
              refused.error().message == overlaps_the_root_of(b->archive_uuid));
    }
    const auto b_identity = fixture.archive.content_at(identity);
    const va::Uuid third = "c0ffee00-0000-4000-8000-0000000000cc"; // sample data
    fixture.archive.put(identity, "{\"archive_uuid\":\"" + third + "\"}");
    const auto third_named = fixture.run(over_lost_manifest("/vol/a", a->archive_uuid, true));
    CHECK(!third_named && third_named.error().kind == va::RegisterFailure::Kind::Refused &&
          third_named.error().message == "the identity file of this root (.veriarchive/identity.json) names archive " +
                                             third + ", not archive " + a->archive_uuid);
    fixture.archive.put(identity, b_identity);
    CHECK(revisions_of(fixture, a->archive_uuid) == std::vector<std::uint32_t>{1});

    const auto registered = fixture.run(over_lost_manifest("/vol/a", a->archive_uuid, true));
    CHECK(registered && registered->archive_uuid == a->archive_uuid && registered->revision == 2 &&
          registered->lost_revision == std::optional<std::uint32_t>(1) && registered->lost_revision_left_out &&
          registered->counts.files == 2);
    if (!registered) {
        std::cerr << "  over the lost manifest at its twin's root: " << registered.error().message << '\n';
        return;
    }
    const auto archive = fixture.store.find_archive(a->archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2 && (*archive)->root_subpath == "a");
    CHECK(revisions_of(fixture, b->archive_uuid) == std::vector<std::uint32_t>{1});
    CHECK(read_bytes(va::manifest_path(fixture.repository.context.repo.manifests(), b->archive_uuid, 1)) == b_manifest);
    const auto both = fixture.verify("/vol/a");
    CHECK(!both && both.error().identity.result == va::MatchResult::Ambiguous &&
          both.error().identity.archives.size() == 2);
}

// Decision 0028, point 8: twins on a volume that matches them only weakly.
// The same folder, /vol/a (sample data), registered on purpose as A and its
// twin B (R-97, --force-new-archive), each with a media copy, so that the
// identity file on it names B; its volume shows no device serial
// (MATCH_WEAK, as behind a bridge that gives none), or another serial than
// when the twins were registered (MIGRATED, as in another enclosure; sample
// serials). A's manifest is lost: the registration over it takes the folder,
// naming B as the archive whose very root it is, with the match, and no
// possible clone, though the identity file names B; a folder in it is taken
// too, naming B as possibly a clone; an identity file that names a third
// archive refuses the folder. Confirmed, A's revision 2 is registered from
// the folder, and B is left as it was.
void a_registration_over_a_lost_manifest_takes_the_root_of_its_twin_on_a_volume_matched_weakly()
{
    for (const auto match : {va::MatchResult::MatchWeak, va::MatchResult::Migrated}) {
        Fixture fixture;
        if (match == va::MatchResult::MatchWeak) {
            fixture.media.volume.devices.front().serial.clear();
        }
        fixture.archive.add_file("a/SAMPLE-one.txt", "one (sample data)");
        fixture.archive.add_file("a/sub/SAMPLE-two.txt", "two (sample data)");
        auto first = at("/vol/a");
        first.write_media_copy = true;
        const auto a = fixture.run(first);
        auto twice = at("/vol/a");
        twice.force_new_archive = true;
        twice.write_media_copy = true;
        const auto b = fixture.run(twice);
        CHECK(a && b && a->archive_uuid != b->archive_uuid);
        if (!a || !b) {
            return;
        }
        if (match == va::MatchResult::Migrated) {
            fixture.media.volume.devices.front().serial = "SAMPLE-SERIAL-9";
        }
        const auto identity = std::filesystem::path("/vol/a") / ".veriarchive" / "identity.json";
        CHECK(fixture.archive.content_at(identity).contains(b->archive_uuid));
        std::filesystem::remove(va::manifest_path(fixture.repository.context.repo.manifests(), a->archive_uuid, 1));

        const auto asked = fixture.run(over_lost_manifest("/vol/a", a->archive_uuid, false));
        CHECK(!asked && asked.error().kind == va::RegisterFailure::Kind::Unconfirmed && asked.error().lost_manifest &&
              asked.error().lost_manifest->possible_clones.empty() && asked.error().lost_manifest->same_root.size() == 1 &&
              asked.error().lost_manifest->same_root.front().archive_uuid == b->archive_uuid &&
              asked.error().lost_manifest->same_root.front().label == "ARCH-0002" &&
              asked.error().lost_manifest->same_root.front().match == match);
        if (!asked && asked.error().kind != va::RegisterFailure::Kind::Unconfirmed) {
            std::cerr << "  the root of its twin on a volume matched only as " << va::to_string(match) << ": "
                      << asked.error().message << '\n';
        }
        CHECK(fixture.store.setting("registration:" + a->archive_uuid).value_or("x").empty());
        const auto in_it = fixture.run(over_lost_manifest("/vol/a/sub", a->archive_uuid, false));
        CHECK(!in_it && in_it.error().kind == va::RegisterFailure::Kind::Unconfirmed && in_it.error().lost_manifest &&
              in_it.error().lost_manifest->same_root.empty() &&
              in_it.error().lost_manifest->possible_clones.size() == 1 &&
              in_it.error().lost_manifest->possible_clones.front().archive_uuid == b->archive_uuid &&
              in_it.error().lost_manifest->possible_clones.front().match == match);
        const auto b_identity = fixture.archive.content_at(identity);
        const va::Uuid third = "c0ffee00-0000-4000-8000-0000000000dd"; // sample data
        fixture.archive.put(identity, "{\"archive_uuid\":\"" + third + "\"}");
        const auto third_named = fixture.run(over_lost_manifest("/vol/a", a->archive_uuid, true));
        CHECK(!third_named && third_named.error().kind == va::RegisterFailure::Kind::Refused &&
              third_named.error().message == "the identity file of this root (.veriarchive/identity.json) names archive " +
                                                 third + ", not archive " + a->archive_uuid);
        fixture.archive.put(identity, b_identity);
        CHECK(revisions_of(fixture, a->archive_uuid) == std::vector<std::uint32_t>{1});

        const auto registered = fixture.run(over_lost_manifest("/vol/a", a->archive_uuid, true));
        CHECK(registered && registered->archive_uuid == a->archive_uuid && registered->revision == 2 &&
              registered->lost_revision == std::optional<std::uint32_t>(1) && registered->counts.files == 2);
        if (!registered) {
            std::cerr << "  over the lost manifest at its twin's root: " << registered.error().message << '\n';
            continue;
        }
        CHECK(revisions_of(fixture, b->archive_uuid) == std::vector<std::uint32_t>{1});
    }
}

// Decision 0028, point 8: a registration over a lost manifest resumes as any
// registration (R-104): its plan names the lost revision, and the attempt
// that records the next revision leaves the lost revision's record out of
// the index as the first attempt would have. Where the manifest was put back
// meanwhile, the record of its revision stays, and the next revision is
// registered all the same, as the operator confirmed. The archive is
// registered at it, as by any registration of a new revision, and the new
// revision names the original that the archive is a copy of, as its newest
// manifest that can be used says (decision 0005). Sample data.
void a_registration_over_a_lost_manifest_resumes()
{
    for (const bool put_back_meanwhile : {false, true}) {
        Fixture fixture;
        const va::Uuid original = "c0ffee00-0000-4000-8000-000000000028"; // sample data
        fixture.archive.add_file("a/SAMPLE-one.txt", "one (sample data)");
        auto first = at("/vol/a");
        first.copy_of = original;
        const auto registered = fixture.run(first);
        fixture.archive.add_file("a/SAMPLE-two.txt", "two (sample data)");
        auto second = at("/vol/a");
        second.new_revision = true;
        CHECK(registered && fixture.run(second).has_value());
        if (!registered) {
            return;
        }
        const auto& archive_uuid = registered->archive_uuid;
        const auto recorded = va::manifest_path(fixture.repository.context.repo.manifests(), archive_uuid, 2);
        const auto kept = read_bytes(recorded);
        std::filesystem::remove(recorded);

        fixture.archive.node("a/SAMPLE-two.txt").open_error =
            va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged (sample)"};
        const auto interrupted = fixture.run(over_lost_manifest("/vol/a", archive_uuid, true));
        CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
        fixture.archive.node("a/SAMPLE-two.txt").open_error.reset();
        const auto plan = nlohmann::json::parse(fixture.store.setting("registration:" + archive_uuid).value_or(""),
            nullptr, false);
        CHECK(plan.is_object() && plan.value("lost_manifest", 0u) == 2u && plan.value("revision", 0u) == 3u);
        CHECK(revisions_of(fixture, archive_uuid) == (std::vector<std::uint32_t>{1, 2}));
        if (put_back_meanwhile) {
            std::ofstream(recorded, std::ios::binary | std::ios::trunc) << kept;
        }

        auto resume = at("/vol/a");
        resume.resume = true;
        const auto resumed = fixture.run(resume);
        CHECK(resumed && resumed->revision == 3 && resumed->lost_revision == std::optional<std::uint32_t>(2) &&
              resumed->lost_revision_left_out == !put_back_meanwhile);
        if (!resumed) {
            std::cerr << "  resumed over the lost manifest: " << resumed.error().message << '\n';
            continue;
        }
        CHECK(revisions_of(fixture, archive_uuid) ==
              (put_back_meanwhile ? std::vector<std::uint32_t>{1, 2, 3} : std::vector<std::uint32_t>{1, 3}));
        const auto archive = fixture.store.find_archive(archive_uuid);
        CHECK(archive && *archive && (*archive)->current_revision == 3 && (*archive)->status == "REGISTERED");
        auto reader = va::ManifestReader::open(*fixture.repository.files, resumed->manifest);
        CHECK(reader && reader->header().copy_of == std::optional<va::Uuid>(original) &&
              reader->header().supersedes == 2u);
    }
}

// Decision 0028, point 9: a retired archive takes no new revision (decision
// 0003), not even over its lost manifest. Archive A at /vol/a (sample data),
// registered twice, whose manifest of revision 2 is gone: retired, as an
// archive retired before its manifest was lost, the registration over its
// lost manifest is refused and writes nothing. Retired while that
// registration was named, before its plan is written (as the media copy is
// looked for at its root), it writes no plan either. And a registration over
// the lost manifest that a version before this point let begin (interrupted,
// the archive retired since) is refused when it is resumed, naming the
// abandon, which takes it away.
void a_retired_archive_takes_no_registration_over_its_lost_manifest()
{
    const auto retire = [](Fixture& fixture, const va::Uuid& archive_uuid) {
        auto archive = fixture.store.find_archive(archive_uuid);
        CHECK(archive && *archive);
        if (archive && *archive) {
            (*archive)->status = "RETIRED";
            CHECK(fixture.store.update_archive(**archive));
        }
    };
    const std::string retired_refusal = "a retired archive takes no new revisions (decision 0003)";
    {
        Fixture fixture;
        const auto archive_uuid = archive_a_registered_twice(fixture);
        std::filesystem::remove(va::manifest_path(fixture.repository.context.repo.manifests(), archive_uuid, 2));
        retire(fixture, archive_uuid);
        for (const bool confirmed : {false, true}) {
            const auto refused = fixture.run(over_lost_manifest("/vol/a", archive_uuid, confirmed));
            CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Refused &&
                  refused.error().message == retired_refusal);
        }
        CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("x").empty());
        CHECK(revisions_of(fixture, archive_uuid) == (std::vector<std::uint32_t>{1, 2}));
    }
    {
        // Retired by another process while the registration was named: the
        // transaction that writes its plan finds the archive retired.
        Fixture fixture;
        const auto archive_uuid = archive_a_registered_twice(fixture);
        std::filesystem::remove(va::manifest_path(fixture.repository.context.repo.manifests(), archive_uuid, 2));
        const std::string sql = "UPDATE archives SET status = 'RETIRED' WHERE archive_uuid = '" + archive_uuid + "'";
        test::HookedFiles hooked(fixture.archive, "r0002.vam", [&] { fixture.repository.execute(sql.c_str()); });
        va::RegisterServices services{hooked, *fixture.repository.files, *fixture.repository.writer,
            fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
            fixture.processes};
        const auto refused = va::register_archive(
            services, over_lost_manifest("/vol/a", archive_uuid, true), fixture.cancel, fixture.progress);
        CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Refused &&
              refused.error().message == retired_refusal);
        if (refused) {
            std::cerr << "  retired meanwhile: registered revision " << refused->revision << '\n';
        }
        CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("x").empty());
        CHECK(revisions_of(fixture, archive_uuid) == (std::vector<std::uint32_t>{1, 2}));
    }
    {
        Fixture fixture;
        const auto archive_uuid = archive_a_registered_twice(fixture);
        std::filesystem::remove(va::manifest_path(fixture.repository.context.repo.manifests(), archive_uuid, 2));
        fixture.archive.node("a/SAMPLE-two.txt").open_error =
            va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged (sample)"};
        const auto interrupted = fixture.run(over_lost_manifest("/vol/a", archive_uuid, true));
        CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
        fixture.archive.node("a/SAMPLE-two.txt").open_error.reset();
        retire(fixture, archive_uuid);
        auto resume = at("/vol/a");
        resume.resume = true;
        const auto refused = fixture.run(resume);
        CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Refused &&
              refused.error().message == "archive " + archive_uuid +
                                             " is retired and takes no new revisions (decision 0003): abandon its "
                                             "unfinished registration with `archive abandon-registration " +
                                             archive_uuid + "`");
        if (refused) {
            std::cerr << "  resumed for a retired archive: registered revision " << refused->revision << '\n';
        }
        const auto plan = nlohmann::json::parse(fixture.store.setting("registration:" + archive_uuid).value_or(""),
            nullptr, false);
        CHECK(plan.is_object() && plan.value("lost_manifest", 0u) == 2u);
        CHECK(revisions_of(fixture, archive_uuid) == (std::vector<std::uint32_t>{1, 2}));
        va::AbandonServices abandon_services{fixture.store, fixture.processes, fixture.host.host};
        const auto abandoned =
            va::abandon_registration(abandon_services, va::AbandonRequest{archive_uuid, std::nullopt, std::nullopt});
        CHECK(abandoned && abandoned->revision == 3 && !abandoned->first_registration);
        CHECK(fixture.store.setting("registration:" + archive_uuid).value_or("x").empty());
        const auto archive = fixture.store.find_archive(archive_uuid);
        CHECK(archive && *archive && (*archive)->current_revision == 2 && (*archive)->status == "RETIRED");
    }
}

// Decision 0022, point 7: a resume asks for the confirmation last, after
// every refusal, so that a plan that can no longer finish is refused as
// before, naming the abandon, also where it never says when it worked and
// names no process (sample damage, as a version before decision 0022 wrote
// it), rather than asked about first. A `register --new-revision` of the
// sample at /vol, interrupted as a file's device went away, made for an older
// revision once an import gives the archive revision 3 (decision 0030); and a
// registration over the lost manifest of archive A (sample data), interrupted
// likewise, whose archive is retired since (decision 0028, point 9). Each
// resume, unconfirmed, is refused so, reads nothing and leaves the plan as it
// was.
void an_older_versions_plan_that_can_no_longer_finish_is_refused_unasked()
{
    const auto as_an_older_version_wrote_it = [](Fixture& fixture, const va::Uuid& archive_uuid) {
        auto plan = plan_of(fixture.store, archive_uuid);
        CHECK(plan.contains("updated") && plan.contains("process"));
        for (const auto* member : {"updated", "stopped", "process", "files_total", "bytes_total", "media_copy"}) {
            plan.erase(member);
        }
        CHECK(fixture.store.set_setting("registration:" + archive_uuid, plan.dump()));
        return plan;
    };
    {
        Fixture fixture;
        const auto archive_uuid = registered_sample(fixture);
        fixture.archive.node("videos/v130.bin").open_error =
            va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
        auto again = at("/vol");
        again.new_revision = true;
        const auto interrupted = fixture.run(again);
        CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
        fixture.archive.node("videos/v130.bin").open_error.reset();
        const auto plan = as_an_older_version_wrote_it(fixture, archive_uuid);
        test::import_later_revision(fixture, fixture.store, archive_uuid, 3);
        fixture.archive.bytes_read = 0;
        auto resume = at("/vol");
        resume.resume = true;
        const auto refused = fixture.run(resume);
        CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Refused &&
              refused.error().message == made_for_an_older_revision(archive_uuid, 2, 3) &&
              !refused.error().older_version);
        if (refused) {
            std::cerr << "  made for an older revision: registered revision " << refused->revision << '\n';
        } else if (refused.error().kind != va::RegisterFailure::Kind::Refused) {
            std::cerr << "  made for an older revision: " << refused.error().message << '\n';
        }
        CHECK(fixture.archive.bytes_read == 0);
        CHECK(plan_of(fixture.store, archive_uuid) == plan);
    }
    {
        Fixture fixture;
        const auto archive_uuid = archive_a_registered_twice(fixture);
        std::filesystem::remove(va::manifest_path(fixture.repository.context.repo.manifests(), archive_uuid, 2));
        fixture.archive.node("a/SAMPLE-two.txt").open_error =
            va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged (sample)"};
        const auto interrupted = fixture.run(over_lost_manifest("/vol/a", archive_uuid, true));
        CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
        fixture.archive.node("a/SAMPLE-two.txt").open_error.reset();
        const auto plan = as_an_older_version_wrote_it(fixture, archive_uuid);
        auto archive = fixture.store.find_archive(archive_uuid);
        CHECK(archive && *archive);
        if (archive && *archive) {
            (*archive)->status = "RETIRED";
            CHECK(fixture.store.update_archive(**archive));
        }
        fixture.archive.bytes_read = 0;
        auto resume = at("/vol/a");
        resume.resume = true;
        const auto refused = fixture.run(resume);
        CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Refused &&
              refused.error().message == "archive " + archive_uuid +
                                             " is retired and takes no new revisions (decision 0003): abandon its "
                                             "unfinished registration with `archive abandon-registration " +
                                             archive_uuid + "`" &&
              !refused.error().older_version);
        if (refused) {
            std::cerr << "  for a retired archive: registered revision " << refused->revision << '\n';
        } else if (refused.error().kind != va::RegisterFailure::Kind::Refused) {
            std::cerr << "  for a retired archive: " << refused.error().message << '\n';
        }
        CHECK(fixture.archive.bytes_read == 0);
        CHECK(plan_of(fixture.store, archive_uuid) == plan);
    }
}

// Decision 0028, point 4: beside an archive whose latest manifest cannot be
// used, a registration of a new revision goes ahead where its root is plainly
// another archive's, by the rule that lets a verification by root go ahead.
// Two archives on one volume (sample data): A at /vol/a, whose manifest is
// gone, and B at /vol/b. A new revision of B's root, a MATCH at its folder, is
// registered as B's revision 2, which verifies by its root. Every other
// registration stops with A's plain message and writes nothing: a new
// revision of A's root, a new folder, B's root registered as a separate
// archive, and a new revision of B's root with no device serial to compare
// (MATCH_WEAK: a clone reads the same), on another device (MIGRATED), and
// with an identity file that names A. So does a relocation of B. Once A's
// manifest is put back, the new folder registers.
void a_new_revision_of_another_archives_root_goes_ahead_beside_a_damaged_archive()
{
    Fixture fixture;
    fixture.archive.add_file("a/SAMPLE-one.txt", "one (sample data)");
    fixture.archive.add_file("b/SAMPLE-two.txt", "two (sample data)");
    fixture.archive.add_file("c/SAMPLE-three.txt", "three (sample data)");
    fixture.archive.add_file("b-copy/SAMPLE-two.txt", "two (sample data)");
    const auto a = fixture.run(at("/vol/a"));
    const auto b = fixture.run(at("/vol/b"));
    CHECK(a && b);
    if (!a || !b) {
        return;
    }
    const auto& layout = fixture.repository.context.repo;
    const auto path = va::manifest_path(layout.manifests(), a->archive_uuid, 1);
    const auto kept = read_bytes(path);
    std::filesystem::remove(path);
    const auto message = "the manifest of revision 1 (manifests/" + a->archive_uuid + "/r0001.vam) is missing" +
                         test::put_back(a->archive_uuid, 1);
    const auto new_revision = [](const std::filesystem::path& root) {
        auto request = at(root);
        request.new_revision = true;
        return request;
    };
    const auto stops = [&](const va::RegisterRequest& request, const char* what) {
        const auto registered = fixture.run(request);
        CHECK(!registered && registered.error().kind == va::RegisterFailure::Kind::Repository &&
              registered.error().message == message);
        if (registered) {
            std::cerr << "  " << what << ": registered revision " << registered->revision << '\n';
        } else if (registered.error().message != message) {
            std::cerr << "  " << what << ": " << registered.error().message << '\n';
        }
        const auto plans = fixture.store.settings_with_prefix("registration:");
        CHECK(plans && plans->empty());
    };

    fixture.archive.add_file("b/SAMPLE-four.txt", "four (sample data)");
    const auto again = fixture.run(new_revision("/vol/b"));
    CHECK(again && again->archive_uuid == b->archive_uuid && again->revision == 2);
    if (!again) {
        std::cerr << "  a new revision of B's root: " << again.error().message << '\n';
    }
    const auto verified = fixture.verify("/vol/b");
    CHECK(verified && verified->archive_uuid == b->archive_uuid && verified->revision == 2 &&
          verified->result == "PASSED");

    stops(new_revision("/vol/a"), "a new revision of A's root");
    stops(at("/vol/c"), "a new folder");
    auto separate = at("/vol/b");
    separate.force_new_archive = true;
    stops(separate, "B's root as a separate archive");
    const auto volume = fixture.media.volume;
    fixture.media.volume.devices.front().serial.clear();
    stops(new_revision("/vol/b"), "no device serial");
    fixture.media.volume = volume;
    fixture.media.volume.devices.front().serial = "OTHER-SERIAL";
    stops(new_revision("/vol/b"), "another device");
    fixture.media.volume = volume;
    fixture.archive.add_file("b/.veriarchive/identity.json", "{\"archive_uuid\":\"" + a->archive_uuid + "\"}");
    stops(new_revision("/vol/b"), "an identity file naming A");
    fixture.archive.remove("b/.veriarchive");

    va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media, fixture.store, layout, fixture.repository.context.repository_uuid, fixture.host,
        fixture.processes};
    const auto relocated = va::relocate_archive(services,
        va::RelocateRequest{b->archive_uuid, fixture.archive.root() / "b-copy", std::nullopt, false}, fixture.cancel,
        fixture.progress);
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Repository &&
          relocated.error().message == message);
    CHECK(revisions_of(fixture, b->archive_uuid) == (std::vector<std::uint32_t>{1, 2}));

    std::ofstream(path, std::ios::binary | std::ios::trunc) << kept;
    const auto c = fixture.run(at("/vol/c"));
    CHECK(c && c->revision == 1);
}

} // namespace

void register_engine_tests()
{
    first_registration_writes_manifest_and_index();
    malformed_values_are_invalid();
    registered_root_needs_a_flag();
    registered_root_without_filesystem_id_needs_a_flag();
    a_new_revision_of_a_copy_stays_a_copy();
    a_new_revision_reads_the_latest_header_from_its_manifest();
    overlapping_roots_are_refused();
    unreadable_files_are_recorded();
    retired_archive_takes_no_revision();
    interrupted_registration_resumes();
    a_device_that_goes_away_interrupts_the_registration();
    trailing_separators_leave_the_folder();
    a_plan_recorded_with_a_trailing_separator_is_found();
    media_copy_is_written_and_checked();
    copied_identity_file_fails();
    unidentifiable_volume_fails();
    plan_records_totals_and_updated();
    a_resumed_registration_counts_what_it_reuses();
    a_recorded_registration_leaves_the_next_plan_alone();
    heartbeat_updates_only_its_record();
    heartbeat_of_a_plan_stops_once_its_revision_is_recorded();
    attempts_of_one_registration_record_it_once(true);
    attempts_of_one_registration_record_it_once(false);
    an_attempt_waits_for_the_record_of_another();
    a_record_waiting_for_the_lock_can_be_cancelled();
    an_attempt_that_loses_while_hashing_changes_nothing();
    a_resume_of_a_recorded_revision_reads_nothing();
    crashed_attempts_leave_nothing_that_blocks_a_resume();
    a_resume_leaves_the_identity_file_it_finds();
    an_identity_file_another_attempt_wrote_counts();
    a_resume_waits_while_the_attempt_before_runs();
    a_resume_goes_ahead_when_the_process_id_is_reused();
    a_resume_goes_ahead_where_the_attempt_before_has_ended_or_cannot_be_seen();
    a_resume_of_an_older_versions_plan_asks_for_confirmation();
    a_plan_says_when_it_worked_from_the_moment_it_is_made();
    resumes_started_at_once_take_turns();
    an_attempt_stops_when_its_first_registration_is_abandoned();
    an_attempt_abandoned_while_it_writes_its_manifest_records_nothing();
    an_abandoned_attempt_leaves_the_next_registration_alone();
    a_registration_never_replaces_another_unfinished_one_of_its_archive();
    a_registration_made_for_an_older_revision_never_records();
    a_registration_meets_a_later_revision_at_its_record();
    a_registration_made_for_an_older_revision_stops_reading();
    an_interrupted_registration_made_for_an_older_revision_says_to_abandon_it();
    a_cancelled_resume_of_a_plan_made_for_an_older_revision_says_to_abandon_it();
    a_registration_overtaken_before_its_plan_writes_nothing();
    a_registration_over_a_lost_manifest_registers_the_next_revision();
    a_registration_over_a_manifest_that_is_not_lost_is_refused();
    a_registration_over_a_lost_manifest_needs_the_archives_own_root();
    a_registration_over_a_lost_manifest_takes_its_own_volume_beside_a_registered_clone();
    a_registration_over_a_lost_manifest_takes_the_root_of_its_twin();
    a_registration_over_a_lost_manifest_takes_the_root_of_its_twin_on_a_volume_matched_weakly();
    a_registration_over_a_lost_manifest_resumes();
    a_retired_archive_takes_no_registration_over_its_lost_manifest();
    an_older_versions_plan_that_can_no_longer_finish_is_refused_unasked();
    a_new_revision_of_another_archives_root_goes_ahead_beside_a_damaged_archive();
}
