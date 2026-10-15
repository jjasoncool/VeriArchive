#include "test_support.h"

#include "engine_fixture.h"

#include "common/time.h"
#include "core/engine/heartbeat.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_reader.h"

#include <nlohmann/json.hpp>

#include <algorithm>

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
        fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host};
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
    media_copy_is_written_and_checked();
    copied_identity_file_fails();
    unidentifiable_volume_fails();
    plan_records_totals_and_updated();
    heartbeat_updates_only_its_record();
}
