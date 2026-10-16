#include "test_support.h"

#include "engine_fixture.h"

#include "common/time.h"
#include "core/engine/abandon_registration.h"
#include "core/engine/heartbeat.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
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
    if (std::filesystem::path::preferred_separator == '\\') {
        CHECK(folder(R"(D:\Photos\)") == R"(D:\Photos)");
        CHECK(folder("D:/Photos/") == "D:/Photos");
        CHECK(folder(R"(D:\)") == R"(D:\)");
        CHECK(folder("D:/") == "D:/");
        CHECK(folder("D:") == "D:");
        CHECK(folder(R"(\\server\share\dir\)") == R"(\\server\share\dir)");
    } else {
        CHECK(folder(R"(/vol/name\)") == R"(/vol/name\)");
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
// wins): a plan of an older version names no process, a repository shared by
// computers holds plans whose process id means something only on the other
// computer, and the process table may refuse to answer. A plan that says the
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

} // namespace

void register_engine_tests()
{
    first_registration_writes_manifest_and_index();
    malformed_values_are_invalid();
    registered_root_needs_a_flag();
    registered_root_without_filesystem_id_needs_a_flag();
    overlapping_roots_are_refused();
    unreadable_files_are_recorded();
    retired_archive_takes_no_revision();
    interrupted_registration_resumes();
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
    resumes_started_at_once_take_turns();
    an_attempt_stops_when_its_first_registration_is_abandoned();
    an_attempt_abandoned_while_it_writes_its_manifest_records_nothing();
    an_abandoned_attempt_leaves_the_next_registration_alone();
    a_registration_never_replaces_another_unfinished_one_of_its_archive();
}
