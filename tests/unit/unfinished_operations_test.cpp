#include "test_support.h"

#include "app/typed_path.h"
#include "app/unfinished_operations.h"
#include "common/time.h"
#include "common/utf8.h"
#include "core/engine/abandon_registration.h"
#include "core/engine/rebuild_index.h"
#include "core/engine/relocate_engine.h"
#include "core/engine/retire_archive.h"
#include "core/manifest/manifest_reader.h"
#include "engine_fixture.h"
#include "test_repository.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <tuple>
#include <utility>

namespace {

using namespace std::chrono_literals;
using va::app::OperationState;
using va::app::OperationWatch;
using va::app::UnfinishedOperation;
using Clock = OperationWatch::Clock;

constexpr std::uint64_t mib = 1024 * 1024;
constexpr std::uint64_t gib = 1024 * mib;

// A fixed wall clock, for the operations' own "updated" times.
const auto wall_now = std::chrono::sys_seconds(std::chrono::sys_days(std::chrono::year(2026) / 10 / 15) + 12h);

// A registration of a sample volume (Z:, never a real one).
UnfinishedOperation registration(std::uint64_t files_done, std::uint64_t bytes_done)
{
    UnfinishedOperation operation;
    operation.key = "registration:c0ffee00-0000-4000-8000-000000000001";
    operation.archive_uuid = "c0ffee00-0000-4000-8000-000000000001";
    operation.label = "ARCH-0001";
    operation.root = "Z:\\";
    operation.started = "2026-10-15T01:00:00Z";
    operation.files_done = files_done;
    operation.bytes_done = bytes_done;
    return operation;
}

// The repository as the engines leave it: totals come from the plan, from the
// volume's used space (only for a whole volume), or not at all.
void reads_operations_with_approximate_totals()
{
    test::TestRepository repository;
    auto store = repository.store();
    CHECK(store.insert_archive({"aaaaaaaa-0000-4000-8000-000000000001", "ARCH-0001", "2026-10-15T01:00:00Z", 0,
        "REGISTRATION_INCOMPLETE", "", ""}));
    CHECK(store.insert_archive({"aaaaaaaa-0000-4000-8000-000000000002", "ARCH-0002", "2026-10-15T02:00:00Z", 0,
        "REGISTRATION_INCOMPLETE", "範例/Photos", ""}));
    CHECK(store.insert_archive({"aaaaaaaa-0000-4000-8000-000000000003", "ARCH-0003", "2026-10-15T03:00:00Z", 0,
        "REGISTRATION_INCOMPLETE", "", ""}));
    CHECK(store.set_setting("registration:aaaaaaaa-0000-4000-8000-000000000001",
        R"({"archive_uuid":"aaaaaaaa-0000-4000-8000-000000000001","created":"2026-10-15T01:00:00Z",)"
        R"("label":"ARCH-0001","revision":1,"root":"Z:\\"})"));
    CHECK(store.set_setting("registration:aaaaaaaa-0000-4000-8000-000000000002",
        R"({"archive_uuid":"aaaaaaaa-0000-4000-8000-000000000002","created":"2026-10-15T02:00:00Z",)"
        R"("label":"ARCH-0002","revision":1,"root":"Y:\\範例\\Photos"})"));
    CHECK(store.set_setting("registration:aaaaaaaa-0000-4000-8000-000000000003",
        R"({"archive_uuid":"aaaaaaaa-0000-4000-8000-000000000003","created":"2026-10-15T03:00:00Z",)"
        R"("label":"ARCH-0003","revision":1,"root":"X:\\","files_total":7,"bytes_total":700,)"
        R"("updated":"2026-10-15T03:30:00Z","stopped":"2026-10-15T03:30:00Z"})"));

    std::vector<std::string> asked;
    const auto read = va::app::read_unfinished_operations(repository.context.repo, {},
        [&](const std::filesystem::path& root) -> std::optional<std::uint64_t> {
            asked.push_back(va::path_to_utf8(root));
            return 9 * gib;
        });
    CHECK(read && read->operations.size() == 3);
    if (!read || read->operations.size() != 3) {
        return;
    }
    const auto& operations = read->operations;
    // Newest first.
    const auto& planned = operations[0];
    CHECK(planned.label == "ARCH-0003");
    CHECK(planned.bytes_total == 700 && planned.files_total == 7 && !planned.total_is_estimate);
    CHECK(planned.updated == va::parse_timestamp("2026-10-15T03:30:00Z"));
    CHECK(planned.recorded_stopped);  // its plan says the attempt stopped
    CHECK(planned.done_counted);
    const auto& folder = operations[1];
    CHECK(folder.bytes_total == 0 && !folder.total_is_estimate);  // not a whole volume: no total
    CHECK(!folder.recorded_stopped);
    const auto& whole = operations[2];
    CHECK(whole.kind == va::app::OperationKind::Registration && whole.label == "ARCH-0001");
    CHECK(whole.bytes_total == 9 * gib && whole.total_is_estimate && whole.files_total == 0);
    // Only the whole volume without totals asked for its used space.
    CHECK(asked.size() == 1 && asked.front() == "Z:\\");
}

// A verification of a version before decision 0022 records neither its
// totals nor its root: the header of its revision's manifest has them (its
// counts and root_hint), never the index's copy of that header (R-14). When
// the manifest cannot be used (the index's copy differs, or the manifest is
// gone; sample data), the totals stay unknown and the row says why, last in
// its details, in either language. Once its archive is retired, its manifest
// being gone, its latest manifest is lost, and the run is not listed at all
// (decision 0022, point 5); put back, the run is listed again, and where only
// the index's copy differs, the row says to rebuild the index, as before.
void an_older_verification_takes_its_totals_from_the_manifest()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a.txt", "a (sample data)");
    fixture.archive.add_file("b.txt", std::string(1000, 'b'));
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    const auto& uuid = registered->archive_uuid;
    const auto reader = va::ManifestReader::open(*fixture.repository.files, registered->manifest);
    CHECK(reader.has_value());
    if (!reader) {
        return;
    }
    const auto& counts = reader->header().counts;
    CHECK(fixture.store.set_setting(
        "run:older", R"({"archive_uuid":")" + uuid + R"(","revision":1,"started":"2026-10-14T01:00:00Z"})"));
    const auto read_older = [&]() -> std::optional<UnfinishedOperation> {
        const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
            [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; });
        CHECK(read && read->operations.size() == 1);
        if (!read || read->operations.size() != 1) {
            return std::nullopt;
        }
        return read->operations.front();
    };

    const auto older = read_older();
    CHECK(older && older->kind == va::app::OperationKind::Verification && older->run_id == "older");
    CHECK(older && older->files_total == counts.files + counts.streams && older->files_total == 2 &&
          older->bytes_total == counts.bytes && older->root == reader->header().root_hint &&
          older->totals_failure.empty());

    fixture.repository.execute("UPDATE archive_revisions SET header_json = replace(header_json, "
                               "'\"volume_label\":\"ARCHIVE\"', '\"volume_label\":\"OTHER\"')");
    const auto index_differs = read_older();
    const auto differs = "the repository's index differs from the manifest of revision 1 (manifests/" + uuid +
                         "/r0001.vam); rebuild the index with `repo rebuild-index`";
    CHECK(index_differs && index_differs->files_total == 0 && index_differs->bytes_total == 0 &&
          index_differs->root.empty() && index_differs->totals_failure == differs);
    if (index_differs) {
        const va::app::Text english;
        const va::app::Text chinese(va::app::Language::TraditionalChinese);
        OperationWatch watch;
        watch.update(*index_differs, Clock::time_point{}, wall_now);
        const auto row = describe_operation(english, watch, Clock::time_point{}, wall_now);
        CHECK(row.progress == "0 B done" && row.details == "0 files · total not known: " + differs && !row.fraction);
        CHECK(describe_operation(chinese, watch, Clock::time_point{}, wall_now).details ==
              "0 個檔案・總量不明：儲存庫的索引與版次 1 的清單檔（manifests/" + uuid +
                  "/r0001.vam）不符；請以 `repo rebuild-index` 重建索引");
    }
    fixture.repository.execute("UPDATE archive_revisions SET header_json = replace(header_json, "
                               "'\"volume_label\":\"OTHER\"', '\"volume_label\":\"ARCHIVE\"')");

    const auto saved = fixture.repository.context.repo.root.parent_path() / "SAMPLE-r0001.vam";
    std::filesystem::copy_file(registered->manifest, saved);
    std::filesystem::remove(registered->manifest);
    const auto gone = read_older();
    CHECK(gone && gone->files_total == 0 && gone->root.empty() &&
          gone->totals_failure ==
              "the manifest of revision 1 (manifests/" + uuid + "/r0001.vam) is missing" + test::put_back(uuid, 1));
    fixture.repository.execute("UPDATE archives SET status = 'RETIRED'");
    const auto retired = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
        [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; });
    CHECK(retired && retired->operations.empty());

    std::filesystem::copy_file(saved, registered->manifest);
    const auto put_back = read_older();
    CHECK(put_back && put_back->files_total == 2 && put_back->totals_failure.empty());
    fixture.repository.execute("UPDATE archive_revisions SET header_json = replace(header_json, "
                               "'\"volume_label\":\"ARCHIVE\"', '\"volume_label\":\"OTHER\"')");
    const auto retired_index_differs = read_older();
    CHECK(retired_index_differs && retired_index_differs->totals_failure == differs);
}

// Watching reads a database, never creates one: a repository folder whose
// database is missing (moved away, being restored) reads as an error.
void reads_nothing_into_a_missing_database()
{
    test::TestRepository repository;
    auto missing = repository.context.repo;
    missing.root = missing.root.parent_path() / "moved-away";
    std::filesystem::create_directories(missing.root);
    const auto operations = va::app::read_unfinished_operations(missing);
    CHECK(!operations);
    CHECK(!std::filesystem::exists(missing.database()));
}

// A verification recorded INCOMPLETE never changes: a later read takes its
// counts from the read before instead of counting its checkpoints again.
void keeps_the_counts_of_a_stopped_verification()
{
    test::TestRepository repository;
    auto store = repository.store();
    const std::string archive_uuid = "28ff645e-a220-4aac-ae0b-6c328bf1a2b3";
    CHECK(store.insert_archive({archive_uuid, "ARCH-0007", "2026-10-08T00:00:00Z", 1, "REGISTERED", "", ""}));
    CHECK(store.set_setting("run:stopped", R"({"archive_uuid":")" + archive_uuid +
                                               R"(","revision":1,"started":"2026-10-15T01:00:00Z","root":"Z:\\範例",)"
                                               R"("files_total":5,"bytes_total":500})"));
    va::Entry entry;
    entry.path.p = "a.txt";
    CHECK(store.add_progress("stopped", {va::entry_key(entry)}));
    va::VerifyRunRecord run;
    run.run_id = "stopped";
    run.archive_uuid = archive_uuid;
    run.revision = 1;
    run.mode = "FULL";
    run.started = "2026-10-15T01:00:00Z";
    run.finished = "2026-10-15T02:00:00Z";
    run.result = "INCOMPLETE";
    CHECK(store.append_verify_run(run, repository.context.repository_uuid));

    const auto first = va::app::read_unfinished_operations(repository.context.repo);
    CHECK(first && first->operations.size() == 1 && first->operations.front().files_done == 1 &&
          first->operations.front().recorded_stopped);
    if (!first || first->operations.size() != 1) {
        return;
    }
    // Rows the read before did not see (they cannot appear for a stopped run;
    // here they show that nothing was counted again).
    entry.path.p = "b.txt";
    CHECK(store.add_progress("stopped", {va::entry_key(entry)}));
    const auto second = va::app::read_unfinished_operations(repository.context.repo, first->operations);
    CHECK(second && second->operations.size() == 1 && second->operations.front().files_done == 1);
    const auto fresh = va::app::read_unfinished_operations(repository.context.repo);
    CHECK(fresh && fresh->operations.size() == 1 && fresh->operations.front().files_done == 2);
}

void tells_running_from_stopped()
{
    const Clock::time_point start{};
    OperationWatch watch;
    watch.update(registration(10, 10 * mib), start, wall_now);
    CHECK(watch.state(start, wall_now) == OperationState::Waiting);  // first poll
    watch.update(registration(10, 10 * mib), start + 5s, wall_now);
    CHECK(watch.state(start + 5s, wall_now) == OperationState::Waiting);  // not seen to move yet
    watch.update(registration(1010, 70 * mib), start + 60s, wall_now);
    CHECK(watch.state(start + 60s, wall_now) == OperationState::Running);
    watch.update(registration(1010, 70 * mib), start + 60s + 2min, wall_now);
    CHECK(watch.state(start + 60s + 2min, wall_now) == OperationState::Running);
    watch.update(registration(1010, 70 * mib), start + 60s + 3min, wall_now);
    CHECK(watch.state(start + 60s + 3min, wall_now) == OperationState::Quiet);
    CHECK(watch.quiet_for(start + 60s + 3min) == 3min);
    watch.update(registration(2010, 130 * mib), start + 60s + 4min, wall_now);
    CHECK(watch.state(start + 60s + 4min, wall_now) == OperationState::Running);  // moving again

    // Never seen to move: quiet after the same few minutes.
    OperationWatch idle;
    idle.update(registration(5, 5), start, wall_now);
    idle.update(registration(5, 5), start + 3min, wall_now);
    CHECK(idle.state(start + 3min, wall_now) == OperationState::Quiet);

    // A verification recorded INCOMPLETE has stopped, from the first poll on.
    OperationWatch incomplete;
    auto run = registration(1, 1);
    run.kind = va::app::OperationKind::Verification;
    run.recorded_stopped = true;
    incomplete.update(run, start, wall_now);
    CHECK(incomplete.state(start, wall_now) == OperationState::Stopped);

    // A registration whose plan says it stopped has stopped, however fresh
    // its "updated" (written with the stop).
    OperationWatch cancelled;
    auto stopped_plan = registration(1, 1);
    stopped_plan.updated = wall_now;
    stopped_plan.recorded_stopped = true;
    cancelled.update(stopped_plan, start, wall_now);
    CHECK(cancelled.state(start, wall_now) == OperationState::Stopped);

    // An operation that says when it last worked is judged by that alone.
    OperationWatch alive;
    auto beating = registration(1, 1);
    beating.updated = wall_now - 1min;
    alive.update(beating, start, wall_now);
    CHECK(alive.state(start, wall_now) == OperationState::Running);
    beating.updated = wall_now - 4min;
    alive.update(beating, start + 5s, wall_now);
    CHECK(alive.state(start + 5s, wall_now) == OperationState::Stopped);
}

// Cancels at the first hashing report, as Ctrl-C or the Cancel button would.
class CancelWhileHashing final : public va::IProgressSink {
public:
    explicit CancelWhileHashing(va::CancelToken& cancel) : cancel_(cancel) {}
    void on_progress(const va::Progress& progress) override
    {
        if (progress.items_total > 0) {
            cancel_.cancel();
        }
    }

private:
    va::CancelToken& cancel_;
};

// A cancelled registration keeps its plan for `register --resume`, and the
// plan says that it stopped: the list shows it stopped at once, as it does a
// cancelled verification, not running until its "updated" is minutes old.
void a_cancelled_registration_reads_stopped()
{
    test::EngineFixture fixture;
    for (int index = 0; index < 20; ++index) {
        fixture.archive.add_file("f" + std::to_string(index), std::string(1000, 'x'));
    }
    CancelWhileHashing cancelling(fixture.cancel);
    va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    va::RegisterRequest request;
    request.root = fixture.archive.root();
    const auto outcome = va::register_archive(services, request, fixture.cancel, cancelling);
    CHECK(!outcome && outcome.error().kind == va::RegisterFailure::Kind::Incomplete);

    const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
        [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; });
    CHECK(read && read->operations.size() == 1);
    if (!read || read->operations.size() != 1) {
        return;
    }
    const auto& operation = read->operations.front();
    CHECK(operation.recorded_stopped);
    const va::app::Text english;
    const Clock::time_point start{};
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    OperationWatch watch;
    watch.update(operation, start, now);
    watch.update(operation, start + 5s, now + 5s);
    CHECK(watch.state(start + 5s, now + 5s) == OperationState::Stopped);
    CHECK(describe_operation(english, watch, start + 5s, now + 5s).state == "stopped; it can be resumed");
}

// A registration this version stopped, then resumed by an older version: an
// older `register --resume` checkpoints but never writes the plan, which keeps
// this version's "stopped" and the "updated" written with it. Once the
// checkpoints move after the first read that saw the mark, the registration
// runs again, judged by its counts as an operation of an older version.
void a_registration_resumed_by_an_older_version_runs()
{
    test::EngineFixture fixture;
    for (int index = 0; index < 20; ++index) {
        fixture.archive.add_file("f" + std::to_string(index), std::string(1000, 'x'));
    }
    CancelWhileHashing cancelling(fixture.cancel);
    va::RegisterServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host,
        fixture.processes};
    va::RegisterRequest request;
    request.root = fixture.archive.root();
    const auto outcome = va::register_archive(services, request, fixture.cancel, cancelling);
    CHECK(!outcome && outcome.error().kind == va::RegisterFailure::Kind::Incomplete);

    const va::app::Text english;
    const auto no_volume = [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; };
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const Clock::time_point start{};
    std::vector<OperationWatch> watches;
    const auto poll = [&](std::chrono::seconds after) {
        const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {}, no_volume);
        CHECK(read && read->operations.size() == 1);
        if (read) {
            va::app::follow(watches, read->operations, start + after, now + after);
        }
        return watches.size() == 1 ? watches.front().state(start + after, now + after) : OperationState::Waiting;
    };
    CHECK(poll(0s) == OperationState::Stopped);
    CHECK(poll(5s) == OperationState::Stopped);
    // Stopped by its mark: Resume resumes at once.
    const auto stopped_row = describe_operation(english, watches.front(), start + 5s, now + 5s);
    CHECK(stopped_row.offers_resume && !stopped_row.resume_asks_first);

    // The older version's checkpoint, the plan untouched.
    const auto plans = fixture.store.settings_with_prefix("registration:");
    CHECK(plans && plans->size() == 1);
    if (!plans || plans->size() != 1) {
        return;
    }
    const auto plan = nlohmann::json::parse(plans->front().second, nullptr, false);
    const auto archive_uuid = plan.value("archive_uuid", std::string{});
    va::Entry entry;
    entry.path.p = "older-version";
    entry.size = 5000;
    entry.b3 = "b3";
    CHECK(fixture.store.upsert_file_rows(archive_uuid, 1, {entry}, 0));
    CHECK(poll(65s) == OperationState::Running);
    CHECK(describe_operation(english, watches.front(), start + 65s, now + 65s).state == "running");
    CHECK(!watches.front().operation().recorded_stopped && !watches.front().operation().updated);
    // Then quiet minutes say only that it may have stopped, as for any
    // operation of an older version, and its row offers Resume and Abandon...
    // as an older version's registration's, its Resume asking first, as the
    // older version may still run it (decision 0022, point 7).
    CHECK(poll(65s + 3min) == OperationState::Quiet);
    const auto quiet_row = describe_operation(english, watches.front(), start + 65s + 3min, now + 65s + 3min);
    CHECK(quiet_row.offers_resume && quiet_row.abandonable && quiet_row.resume_asks_first);

    // This version stops it again: a new mark, with its own "updated".
    auto stopped = watches.front().operation();
    stopped.recorded_stopped = true;
    stopped.updated = now + 10min;
    stopped.files_done += 1;
    watches.front().update(stopped, start + 10min, now + 10min);
    CHECK(watches.front().state(start + 10min, now + 10min) == OperationState::Stopped);
    watches.front().update(stopped, start + 10min + 5s, now + 10min + 5s);
    CHECK(watches.front().state(start + 10min + 5s, now + 10min + 5s) == OperationState::Stopped);

    // A verification recorded INCOMPLETE is not taken up this way: a resume
    // is another run.
    OperationWatch run;
    auto incomplete = registration(1, 1);
    incomplete.kind = va::app::OperationKind::Verification;
    incomplete.recorded_stopped = true;
    run.update(incomplete, start, now);
    incomplete.files_done = 2;
    run.update(incomplete, start + 5s, now + 5s);
    CHECK(run.state(start + 5s, now + 5s) == OperationState::Stopped);
}

// Not in WAL mode the checkpoints are not counted (decision 0022): a row shows
// only the state its record gives, and says that the progress is not counted.
void shows_only_the_record_state_when_not_counted()
{
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    const Clock::time_point start{};
    auto operation = registration(0, 0);
    operation.done_counted = false;
    operation.files_total = 5000;
    operation.bytes_total = 8 * gib;

    // It says it works.
    OperationWatch working;
    operation.updated = wall_now;
    working.update(operation, start, wall_now);
    working.update(operation, start + 5min, wall_now + 1min);
    auto row = describe_operation(english, working, start + 5min, wall_now + 1min);
    CHECK(row.state == "running" && row.progress == "not counted" && row.details.empty() && !row.fraction);
    CHECK(describe_operation(chinese, working, start + 5min, wall_now + 1min).progress == "未計算");

    // Its plan says it stopped.
    OperationWatch stopped;
    operation.recorded_stopped = true;
    stopped.update(operation, start, wall_now);
    CHECK(describe_operation(english, stopped, start, wall_now).state == "stopped; it can be resumed");

    // An older version's record says nothing of its state: not known, also
    // after minutes (no counts to see moving or quiet).
    OperationWatch older;
    operation.recorded_stopped = false;
    operation.updated.reset();
    older.update(operation, start, wall_now);
    older.update(operation, start + 10min, wall_now + 10min);
    row = describe_operation(english, older, start + 10min, wall_now + 10min);
    CHECK(row.state_kind == OperationState::Unknown && row.state == "not known");
    CHECK(describe_operation(chinese, older, start + 10min, wall_now + 10min).state == "不明");
    CHECK(!va::app::share_done(operation));

    // A repository in the rollback-journal mode reads that way (sample data,
    // a sample root).
    test::TestRepository repository;
    repository.execute("PRAGMA journal_mode=DELETE;");
    auto store = repository.store();
    const std::string archive_uuid = "c0ffee00-0000-4000-8000-000000000001";
    CHECK(store.insert_archive({archive_uuid, "ARCH-0001", "2026-10-15T01:00:00Z", 0, "REGISTRATION_INCOMPLETE",
        "範例", ""}));
    CHECK(store.set_setting("registration:" + archive_uuid, R"({"archive_uuid":")" + archive_uuid +
                                                                 R"(","revision":1,"created":"2026-10-15T01:00:00Z",)"
                                                                 R"("root":"Z:\\範例","updated":"2026-10-15T01:00:00Z"})"));
    va::Entry hashed;
    hashed.path.p = "a.txt";
    hashed.size = 100;
    hashed.b3 = "b3";
    CHECK(store.upsert_file_rows(archive_uuid, 1, {hashed}, 0));
    const auto read = va::app::read_unfinished_operations(repository.context.repo);
    CHECK(read && read->operations.size() == 1 && !read->archives_mark.empty());
    if (read && read->operations.size() == 1) {
        CHECK(!read->operations.front().done_counted && read->operations.front().files_done == 0);
    }
}

// A read carries a mark of the archives (ArchiveStore::archive_list_mark), read
// with the operations: an operation that started and finished between two
// reads, never listed, still changes it, and the Archive List draws its table
// again. Sample data only.
void a_read_marks_the_archives()
{
    test::TestRepository repository;
    const auto first = va::app::read_unfinished_operations(repository.context.repo);
    CHECK(first && first->operations.empty() && !first->archives_mark.empty());
    const auto again = va::app::read_unfinished_operations(repository.context.repo);
    CHECK(again && first && again->archives_mark == first->archives_mark);

    // A small folder registered between two reads.
    auto store = repository.store();
    CHECK(store.insert_archive({"c0ffee00-0000-4000-8000-000000000003", "ARCH-0003", "2026-10-15T01:00:00Z", 1,
        "REGISTERED", "範例/Quick", ""}));
    const auto registered = va::app::read_unfinished_operations(repository.context.repo);
    CHECK(registered && registered->operations.empty() && first && registered->archives_mark != first->archives_mark);
    const auto mark = store.archive_list_mark();
    CHECK(mark && registered && *mark == registered->archives_mark);
}

// Checkpoints arrive once a minute while the polls come every 5 seconds. The
// speed must be that of the work, not of a minute's step seen in 5 seconds.
void estimates_from_checkpoint_steps()
{
    const Clock::time_point start{};
    OperationWatch watch;
    auto operation = registration(0, 0);
    operation.bytes_total = 100 * gib;
    operation.total_is_estimate = true;
    constexpr std::uint64_t per_second = 100 * mib;
    std::uint64_t checkpointed = 0;
    for (int second = 0; second <= 15 * 60; second += 5) {
        // The engine checkpoints at 3 s past each minute; the poll sees it later.
        if (second % 60 == 5) {
            checkpointed = static_cast<std::uint64_t>(second - 2) * per_second;
        }
        operation.files_done = checkpointed / gib;
        operation.bytes_done = checkpointed;
        watch.update(operation, start + std::chrono::seconds(second), wall_now);
    }
    const auto speed = watch.estimate().bytes_per_second();
    CHECK(speed && std::abs(*speed / static_cast<double>(per_second) - 1.0) < 0.1);
    const auto left = watch.estimate().time_left();
    CHECK(left.has_value());
    if (left) {
        // 100 GiB at 100 MiB/s takes 1024 s; about 15 minutes of it are done.
        const double expected = (100.0 * 1024 - static_cast<double>(checkpointed) / static_cast<double>(mib)) / 100.0;
        CHECK(std::abs(static_cast<double>(left->likely.count()) - expected) < expected * 0.15);
    }
}

// Large files: one finishes, and the checkpoints move, every 2 or every 12
// minutes, at 100 MiB/s, while the record says the operation works. Such a
// pace is learned: the estimate does not start afresh at every step, and its
// window holds enough steps to give a time left. A minute after a step the row
// shows the speed and a time left close to the truth.
void estimates_from_steps_minutes_apart()
{
    const va::app::Text english;
    constexpr std::uint64_t per_second = 100 * mib;
    constexpr std::uint64_t files = 100;
    for (const int step_minutes : {2, 12}) {
        const int step = step_minutes * 60;
        const auto file_size = static_cast<std::uint64_t>(step) * per_second;
        const Clock::time_point start{};
        OperationWatch watch;
        auto operation = registration(0, 0);
        operation.files_total = files;
        operation.bytes_total = files * file_size;
        int rows = 0;
        int with_speed = 0;
        int with_left = 0;
        double worst = 0;
        for (int second = 0; second <= 3 * 3600; second += 5) {
            const auto done = static_cast<std::uint64_t>(second / step);
            operation.files_done = done;
            operation.bytes_done = done * file_size;
            operation.updated = wall_now + std::chrono::seconds(second - second % 30);
            const auto now = start + std::chrono::seconds(second);
            const auto wall = wall_now + std::chrono::seconds(second);
            watch.update(operation, now, wall);
            // A minute after each step from the sixth on.
            if (second % step != 60 || second < 6 * step) {
                continue;
            }
            ++rows;
            const auto row = describe_operation(english, watch, now, wall);
            with_speed += row.details.find(" MiB/s") != std::string::npos;
            with_left += row.details.find("hours left") != std::string::npos ||
                         row.details.find("minutes left") != std::string::npos;
            if (const auto left = watch.estimate().time_left()) {
                // Made at the step, a minute ago: the files left at the pace.
                const double truth = static_cast<double>((files - done) * static_cast<std::uint64_t>(step));
                worst = std::max(worst, std::abs(static_cast<double>(left->likely.count()) / truth - 1));
            }
            const auto speed = watch.estimate().bytes_per_second();
            CHECK(speed && std::abs(*speed / static_cast<double>(per_second) - 1.0) < 0.05);
        }
        CHECK(rows > 0 && with_speed == rows && with_left == rows);
        CHECK(worst < 0.05);
    }
}

// Photos, checkpointed every 1000 files 40 seconds apart, between videos of
// 100 GiB that take over 11 minutes each, at 150 MiB/s. The pace learned from
// the photos makes every video step "long" (over 10 minutes), but a video
// brings its bytes: it is work, not a stop, and the estimate goes on. Polled
// every 5 seconds, with the operation saying it works ("updated", every 30
// seconds) and without (an older version). Then a stop of two hours: the
// first step after it brings little for its length and starts the estimate
// afresh.
void estimates_between_small_and_large_files()
{
    const va::app::Text english;
    constexpr double speed = 150;  // MiB per second
    struct Step {
        double time;
        std::uint64_t files;
        std::uint64_t mib;
    };
    // One cycle: two checkpoints of 1000 photos of 6 MiB, then the last 400
    // photos and a video of 100 GiB in the checkpoint when the video finishes.
    constexpr int cycles = 12;
    constexpr double photos = 2400 * 6 / speed;
    constexpr double cycle = photos + 102400 / speed;
    std::vector<Step> steps;
    for (int index = 0; index < cycles; ++index) {
        const double begin = index * cycle;
        const auto files = static_cast<std::uint64_t>(index) * 2401;
        const auto done = static_cast<std::uint64_t>(index) * (2400 * 6 + 102400);
        steps.push_back({begin + 40, files + 1000, done + 6000});
        steps.push_back({begin + 80, files + 2000, done + 12000});
        steps.push_back({begin + cycle, files + 2401, done + 2400 * 6 + 102400});
    }
    const auto& last = steps.back();
    for (const bool says_it_works : {true, false}) {
        auto operation = registration(0, 0);
        operation.files_total = last.files;
        operation.bytes_total = last.mib * mib;
        OperationWatch watch;
        std::size_t seen = 0;
        int rows = 0;
        int with_left = 0;
        double worst = 0;
        for (int second = 0; second <= static_cast<int>(last.time) - 30; second += 5) {
            while (seen < steps.size() && steps[seen].time <= second) {
                operation.files_done = steps[seen].files;
                operation.bytes_done = steps[seen].mib * mib;
                ++seen;
            }
            if (says_it_works) {
                operation.updated = wall_now + std::chrono::seconds(second - second % 30);
            }
            const auto now = Clock::time_point{} + std::chrono::seconds(second);
            const auto wall = wall_now + std::chrono::seconds(second);
            watch.update(operation, now, wall);
            // 20 seconds after each step seen, from the third cycle on.
            if (seen <= 6 || second != static_cast<int>(std::ceil(steps[seen - 1].time / 5)) * 5 + 20) {
                continue;
            }
            ++rows;
            const auto row = describe_operation(english, watch, now, wall);
            CHECK(row.state == "running");
            with_left += row.details.find("minutes left") != std::string::npos ||
                         row.details.find("hours left") != std::string::npos;
            if (const auto left = watch.estimate().time_left()) {
                // Made at the step, from what was done then.
                const double truth = static_cast<double>(last.mib - steps[seen - 1].mib) / speed;
                worst = std::max(worst, std::abs(static_cast<double>(left->likely.count()) / truth - 1));
            }
        }
        CHECK(rows == 3 * (cycles - 2) - 1);  // every step from the third cycle on but the last
        CHECK(with_left == rows);
        CHECK(worst < 0.15);
        const OperationWatch learned = watch;
        const auto at = [&](int second) {
            return std::pair{Clock::time_point{} + std::chrono::seconds(second), wall_now + std::chrono::seconds(second)};
        };

        // Stopped for two hours, then a checkpoint of photos: a stop, not slow
        // work. The estimate starts afresh from that step.
        CHECK(watch.estimate().bytes_per_second().has_value());
        auto resumed = operation;
        resumed.files_done += 1000;
        resumed.bytes_done += 6000 * mib;
        const int back = static_cast<int>(last.time) + 2 * 3600;
        if (says_it_works) {
            resumed.updated = wall_now + std::chrono::seconds(back);
        }
        watch.update(resumed, at(back).first, at(back).second);
        CHECK(!watch.estimate().bytes_per_second().has_value());  // one sample: a new estimate

        if (says_it_works) {
            continue;
        }
        // An older version cancelled while reading a video: its last
        // checkpoint, written as it stopped, holds only the photos before it
        // (little work for its 6 minutes), and the video is read again from
        // its start when it is resumed two hours later, after a scan of 10
        // minutes. That first step brings the video's many bytes, but over
        // the whole stop: still a stop, though the cancel's slow checkpoint
        // pulled the estimate's speed down.
        OperationWatch cancelled = learned;
        auto stopping = operation;
        const int last_seen = static_cast<int>(std::ceil(steps[steps.size() - 2].time / 5)) * 5;
        const int cancel = last_seen + 360;
        stopping.files_done += 400;
        stopping.bytes_done += 2400 * mib;
        for (int second = last_seen + 5; second <= cancel + 2 * 3600 + 600 + 685; second += 5) {
            auto seen_now = second < cancel ? operation : stopping;
            if (second >= cancel + 2 * 3600 + 600 + 685) {
                seen_now.files_done += 1;
                seen_now.bytes_done += 102400 * mib;
            }
            cancelled.update(seen_now, at(second).first, at(second).second);
        }
        CHECK(!cancelled.estimate().bytes_per_second().has_value());  // one sample: a new estimate
    }
}

// A registration hashes for 20 minutes at 100 MiB/s, is stopped for two hours,
// then resumed (`register --resume`): it scans for ten minutes and hashes at
// its old speed again, checkpointing once a minute, with about 5.3 hours left.
// The first step after the stop must not count the stop as slow work: within
// minutes of the resume the time left is about right, not days.
void restarts_the_estimate_after_a_stop()
{
    const Clock::time_point start{};
    constexpr std::uint64_t per_second = 100 * mib;
    constexpr int resumed = 20 * 60 + 2 * 3600 + 10 * 60;  // hashing again from here
    auto operation = registration(0, 0);
    operation.bytes_total = per_second * (20 * 60 + 5 * 3600 + 24 * 60);
    OperationWatch watch;
    const auto done_at = [&](int second) -> std::uint64_t {
        if (second < resumed) {
            return static_cast<std::uint64_t>(std::min(second, 20 * 60) / 60 * 60) * per_second;
        }
        return static_cast<std::uint64_t>(20 * 60 + (second - resumed) / 60 * 60) * per_second;
    };
    const auto poll_until = [&](int last) {
        for (int second = 0; second <= last; second += 5) {
            operation.bytes_done = done_at(second);
            watch.update(operation, start + std::chrono::seconds(second), wall_now);
        }
    };
    const auto true_left = [&](int second) {
        return static_cast<double>(operation.bytes_total - done_at(second)) / static_cast<double>(per_second);
    };

    // Three minutes after hashing resumed: still estimating, or about right.
    poll_until(resumed + 180);
    const auto early = watch.estimate().time_left();
    CHECK(!early || std::abs(static_cast<double>(early->likely.count()) / true_left(resumed + 180) - 1) < 0.5);

    // Five minutes after: about right, at the old speed.
    watch = OperationWatch{};
    poll_until(resumed + 300);
    const auto left = watch.estimate().time_left();
    CHECK(left.has_value());
    if (left) {
        CHECK(std::abs(static_cast<double>(left->likely.count()) / true_left(resumed + 300) - 1) < 0.25);
        CHECK(static_cast<double>(left->high.count()) < 2 * true_left(resumed + 300));
    }
    const auto speed = watch.estimate().bytes_per_second();
    CHECK(speed && std::abs(*speed / static_cast<double>(per_second) - 1.0) < 0.1);

    // A short stop that the operation's own record shows ("updated" silent for
    // more than three minutes) starts the estimate afresh as well.
    OperationWatch beating;
    auto working = registration(0, 0);
    working.bytes_total = 100 * gib;
    for (int second = 0; second <= 600; second += 5) {
        working.bytes_done = static_cast<std::uint64_t>(second / 60 * 60) * per_second;
        working.updated = wall_now + std::chrono::seconds(second);
        beating.update(working, start + std::chrono::seconds(second), wall_now + std::chrono::seconds(second));
    }
    CHECK(beating.estimate().bytes_per_second().has_value());
    // Four minutes without a sign, then a minute of scanning that says it works.
    for (int second = 605; second <= 900; second += 5) {
        if (second > 840) {
            working.updated = wall_now + std::chrono::seconds(second);
        }
        beating.update(working, start + std::chrono::seconds(second), wall_now + std::chrono::seconds(second));
    }
    working.bytes_done += 60 * per_second;
    beating.update(working, start + 905s, wall_now + 905s);
    CHECK(!beating.estimate().bytes_per_second().has_value());  // one sample: a new estimate

    // An operation that does not say when it works: one step a minute, a stop
    // of two hours right after the first steps, one step, then a stop of an
    // hour. The first stop is not taken for the pace: the second one starts
    // the estimate afresh as well.
    OperationWatch quiet;
    auto silent = registration(0, 0);
    silent.bytes_total = 100 * gib;
    const auto poll = [&](int second, int steps) {
        silent.bytes_done = static_cast<std::uint64_t>(steps) * 60 * per_second;
        quiet.update(silent, start + std::chrono::seconds(second), wall_now + std::chrono::seconds(second));
    };
    poll(0, 0);
    poll(60, 1);
    poll(120, 2);
    poll(120 + 7200, 3);
    poll(120 + 7200 + 3600, 4);
    CHECK(!quiet.estimate().bytes_per_second().has_value());  // one sample: a new estimate
}

// The photos and videos of estimates_between_small_and_large_files, and the
// computer sleeps for 20 minutes, five minutes into a video, with the list on
// screen. Its clock goes on while it sleeps (Windows), so the video's step
// spans the sleep, and it brought the video's bytes: more than a quarter of
// what the fastest step reads in that time, so on its own it passes for work.
// An operation that says it works said so again on waking, before the list
// read again, so no poll saw it stopped either. The window's timer was held
// up, though (archive_list_view.cpp): told so, the watch starts the estimate
// afresh at that step instead of taking the sleep for slow work, and the next
// video's step gives a time left about right.
void restarts_the_estimate_after_a_sleep()
{
    const va::app::Text english;
    constexpr double speed = 150;  // MiB per second
    struct Step {
        double time;  // working time: the sleep left out
        std::uint64_t files;
        std::uint64_t mib;
    };
    constexpr int cycles = 8;
    constexpr double photos = 2400 * 6 / speed;
    constexpr double cycle = photos + 102400 / speed;
    std::vector<Step> steps;
    for (int index = 0; index < cycles; ++index) {
        const double begin = index * cycle;
        const auto files = static_cast<std::uint64_t>(index) * 2401;
        const auto done = static_cast<std::uint64_t>(index) * (2400 * 6 + 102400);
        steps.push_back({begin + 40, files + 1000, done + 6000});
        steps.push_back({begin + 80, files + 2000, done + 12000});
        steps.push_back({begin + cycle, files + 2401, done + 2400 * 6 + 102400});
    }
    const auto& last = steps.back();
    // Asleep from five minutes into the video of the fifth cycle.
    constexpr int asleep = static_cast<int>(4 * cycle + photos) + 300;
    constexpr int awake = asleep + 20 * 60;
    const auto wall_time = [&](double work) { return work > asleep ? work + (awake - asleep) : work; };
    constexpr std::size_t spanning = 4 * 3 + 2;  // that video's step
    constexpr std::size_t next_video = spanning + 3;
    const int checked = static_cast<int>(std::ceil(wall_time(steps[next_video].time) / 5)) * 5 + 20;
    for (const bool says_it_works : {true, false}) {
        for (const bool held_up : {true, false}) {
            auto operation = registration(0, 0);
            operation.files_total = last.files;
            operation.bytes_total = last.mib * mib;
            OperationWatch watch;
            std::size_t seen = 0;
            int polled = 0;
            for (int second = 5; second <= checked; second += 5) {
                if (second > asleep && second < awake) {
                    continue;  // nothing runs, nothing reads
                }
                const std::size_t before = seen;
                while (seen < steps.size() && wall_time(steps[seen].time) <= second) {
                    operation.files_done = steps[seen].files;
                    operation.bytes_done = steps[seen].mib * mib;
                    ++seen;
                }
                if (says_it_works) {
                    // Every 30 seconds while awake, the first time on waking.
                    const int beat = second >= awake ? awake + (second - awake) / 30 * 30 : second / 30 * 30;
                    operation.updated = wall_now + std::chrono::seconds(beat);
                }
                // The window's timer ticks every 5 seconds; this tick came 20 minutes late.
                if (held_up && second - polled > 60) {
                    watch.polls_held_up();
                }
                polled = second;
                const auto now = Clock::time_point{} + std::chrono::seconds(second);
                const auto wall = wall_now + std::chrono::seconds(second);
                watch.update(operation, now, wall);
                if (before <= spanning && seen > spanning) {
                    // No poll saw it stopped.
                    CHECK(watch.state(now, wall) == OperationState::Running);
                    // Told: a new estimate, of one sample. Not told: work.
                    CHECK(watch.estimate().bytes_per_second().has_value() == !held_up);
                }
                if (second != checked || !held_up) {
                    continue;
                }
                const auto row = describe_operation(english, watch, now, wall);
                CHECK(row.details.find(" left") != std::string::npos &&
                      row.details.find("estimating") == std::string::npos);
                const auto left = watch.estimate().time_left();
                CHECK(left.has_value());
                if (left) {
                    // Made at the step, from what was done then.
                    const double truth = static_cast<double>(last.mib - steps[next_video].mib) / speed;
                    CHECK(std::abs(static_cast<double>(left->likely.count()) / truth - 1) < 0.15);
                }
            }
        }
    }
}

// An operation that says it works but has not moved for a while: its speed
// and time left would be those of half an hour ago, so they are left out.
void leaves_out_a_stale_speed()
{
    const va::app::Text english;
    const Clock::time_point start{};
    OperationWatch watch;
    auto operation = registration(0, 0);
    operation.bytes_total = 1000 * gib;
    for (int second = 0; second <= 600; second += 5) {
        operation.bytes_done = static_cast<std::uint64_t>(second / 60 * 60) * 100 * mib;
        operation.updated = wall_now + std::chrono::seconds(second);
        watch.update(operation, start + std::chrono::seconds(second), wall_now + std::chrono::seconds(second));
    }
    auto row = describe_operation(english, watch, start + 600s, wall_now + 600s);
    CHECK(row.state == "running" && row.details.find("/s") != std::string::npos);
    // Thirty minutes on one large file: the record says it works, the
    // checkpoints do not move.
    for (int second = 605; second <= 2400; second += 5) {
        operation.updated = wall_now + std::chrono::seconds(second);
        watch.update(operation, start + std::chrono::seconds(second), wall_now + std::chrono::seconds(second));
    }
    row = describe_operation(english, watch, start + 2400s, wall_now + 2400s);
    CHECK(row.state == "running");
    CHECK(row.details == "0 files");
}

// 100 % means done: all the bytes can be counted while files remain.
void shows_100_percent_only_when_done()
{
    const va::app::Text english;
    const Clock::time_point start{};
    OperationWatch watch;
    auto operation = registration(4990, 8 * gib);
    operation.files_total = 5000;
    operation.bytes_total = 8 * gib;
    watch.update(operation, start, wall_now);
    auto row = describe_operation(english, watch, start, wall_now);
    CHECK(row.progress == "8.00 GiB of 8.00 GiB (99%)");
    CHECK(row.fraction && std::abs(*row.fraction - 0.99) < 1e-9);
    operation.files_done = 5000;
    watch.update(operation, start + 5s, wall_now);
    row = describe_operation(english, watch, start + 5s, wall_now);
    CHECK(row.progress == "8.00 GiB of 8.00 GiB (100%)");
    CHECK(row.fraction == std::optional<double>(1.0));
    // Only the files are known: the same rule.
    UnfinishedOperation counted = registration(1000, 0);
    counted.files_total = 1000;
    CHECK(va::app::share_done(counted) == std::optional<double>(1.0));
    counted.files_done = 999;
    CHECK(va::app::share_done(counted) == std::optional<double>(0.99));
}

void follows_operations_between_polls()
{
    const Clock::time_point start{};
    auto first = registration(0, 0);
    auto second = registration(0, 0);
    second.key = "run:abc";
    std::vector<OperationWatch> watches;
    va::app::follow(watches, {first, second}, start, wall_now);
    CHECK(watches.size() == 2 && watches[0].operation().key == first.key);
    first.bytes_done = 5 * mib;
    va::app::follow(watches, {first, second}, start + 60s, wall_now);
    CHECK(watches[0].state(start + 60s, wall_now) == OperationState::Running);  // kept its history
    CHECK(watches[1].state(start + 60s, wall_now) == OperationState::Waiting);
    // The registration finished: its watch goes, the verification's stays.
    va::app::follow(watches, {second}, start + 65s, wall_now);
    CHECK(watches.size() == 1 && watches[0].operation().key == "run:abc");
}

void words_one_row()
{
    const va::app::Text english;
    const Clock::time_point start{};

    // A whole volume, the total approximated from its used space.
    OperationWatch whole;
    auto operation = registration(78895, 3 * gib);
    operation.bytes_total = 10 * gib;
    operation.total_is_estimate = true;
    whole.update(operation, start, wall_now);
    auto row = describe_operation(english, whole, start, wall_now);
    CHECK(row.label == "ARCH-0001" && row.root == "Z:\\" && row.kind == "Registration");
    CHECK(row.progress == "3.00 GiB of about 10.0 GiB (30%)");
    CHECK(row.details == "78895 files");  // no speed nor time left before it is seen to run
    CHECK(row.state == "waiting for progress..." && row.state_kind == OperationState::Waiting);
    CHECK(row.fraction && std::abs(*row.fraction - 0.3) < 1e-9);
    CHECK(row.archive_uuid == operation.archive_uuid && row.run_id.empty());
    operation.bytes_done = 4 * gib;
    whole.update(operation, start + 60s, wall_now);
    row = describe_operation(english, whole, start + 60s, wall_now);
    CHECK(row.state == "running");
    CHECK(row.details == "78895 files · estimating time left");  // one step: no speed yet

    // An approximate total is never 100 %, and once passed it is no total.
    operation.bytes_done = 10 * gib - 1;
    whole.update(operation, start + 120s, wall_now);
    CHECK(describe_operation(english, whole, start + 120s, wall_now).progress ==
          "10.0 GiB of about 10.0 GiB (99%)");
    operation.bytes_done = 11 * gib;
    whole.update(operation, start + 180s, wall_now);
    row = describe_operation(english, whole, start + 180s, wall_now);
    CHECK(row.progress == "11.0 GiB done" && !row.fraction);
    // Still running at 7 GiB in two minutes, but no time left without a total.
    CHECK(row.details == "78895 files · 59.7 MiB/s");

    // Exact totals from the plan.
    OperationWatch planned;
    auto exact = registration(1200, 2 * gib);
    exact.files_total = 5000;
    exact.bytes_total = 8 * gib;
    planned.update(exact, start, wall_now);
    row = describe_operation(english, planned, start, wall_now);
    CHECK(row.progress == "2.00 GiB of 8.00 GiB (25%)");
    CHECK(row.details == "1200 of 5000 files");

    // Only the files are known: the share counts them.
    OperationWatch files_only;
    auto counted = registration(250, 0);
    counted.files_total = 1000;
    files_only.update(counted, start, wall_now);
    row = describe_operation(english, files_only, start, wall_now);
    CHECK(row.progress == "250 of 1000 files (25%)" && row.details.empty());

    // A verification recorded INCOMPLETE; one whose archive has a newer
    // revision now; a registration quiet for hours.
    OperationWatch stopped;
    auto run = registration(3, 300);
    run.kind = va::app::OperationKind::Verification;
    run.label.clear();
    run.run_id = "run-1";
    run.recorded_stopped = true;
    stopped.update(run, start, wall_now);
    row = describe_operation(english, stopped, start, wall_now);
    CHECK(row.label == "c0ffee00" && row.kind == "Verification" && row.run_id == "run-1");
    CHECK(row.state == "stopped; it can be resumed" && row.state_kind == OperationState::Stopped);
    CHECK(row.progress == "300 B done" && row.details == "3 files");
    OperationWatch outdated;
    run.recorded_stopped = false;
    run.resumable = false;
    run.updated = wall_now - 10min;
    outdated.update(run, start, wall_now);
    CHECK(describe_operation(english, outdated, start, wall_now).state ==
          "stopped; it cannot be resumed (the archive has a newer revision)");

    // The minutes count from when this window first looked.
    OperationWatch quiet;
    quiet.update(registration(7, 7), start, wall_now);
    quiet.update(registration(7, 7), start + 5min, wall_now);
    CHECK(describe_operation(english, quiet, start + 5min, wall_now).state ==
          "no progress seen for 5 minutes; it may have stopped");
    CHECK(describe_operation(english, quiet, start + 3h, wall_now).state ==
          "no progress seen for 3 hours; it may have stopped");

    // Chinese words every part through the catalog; the numbers stay.
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    row = describe_operation(chinese, planned, start, wall_now);
    CHECK(row.progress == "2.00 GiB / 8.00 GiB（25%）");
    CHECK(row.kind == "註冊" && row.state == "等待進度更新……");
    CHECK(describe_operation(chinese, quiet, start + 5min, wall_now).state == "已 5 分鐘未見進度，可能已停止");
    CHECK(describe_operation(chinese, stopped, start, wall_now).state == "已停止；可以繼續");
    CHECK(describe_operation(chinese, outdated, start, wall_now).state == "已停止；無法繼續（封存已有較新的版次）");
}

// Rows in the order shown: what runs (or may) first, then what stopped; the
// newest first within each.
void orders_the_rows()
{
    const va::app::Text english;
    const Clock::time_point start{};
    const auto operation = [](std::string key, std::string started, bool stopped) {
        auto made = registration(1, 1);
        made.key = key;
        made.label = key;
        made.started = std::move(started);
        if (stopped) {
            made.kind = va::app::OperationKind::Verification;
            made.recorded_stopped = true;
        }
        return made;
    };
    std::vector<OperationWatch> watches;
    va::app::follow(watches,
        {operation("stopped-old", "2026-10-10T00:00:00Z", true), operation("running-new", "2026-10-15T00:00:00Z", false),
            operation("stopped-new", "2026-10-12T00:00:00Z", true),
            operation("running-old", "2026-10-14T00:00:00Z", false)},
        start, wall_now);
    std::vector<std::string> labels;
    for (const auto& row : va::app::describe_operations(english, watches, start, wall_now)) {
        labels.push_back(row.label);
    }
    CHECK(labels == (std::vector<std::string>{"running-new", "running-old", "stopped-new", "stopped-old"}));
}


// ---- Which process runs a registration (decision 0024) ---------------------

// A repository with one registration of a sample volume (Z:, never a real
// one) whose plan names process 1000 of this computer and says it worked
// `updated`; no stop mark. Returns the repository's layout.
struct PlannedRegistration {
    test::TestRepository repository;
    const std::string archive_uuid = "c0ffee00-0000-4000-8000-000000000024";

    explicit PlannedRegistration(const std::string& updated, const std::string& host = "test-host")
    {
        auto store = repository.store();
        CHECK(store.insert_archive({archive_uuid, "ARCH-0024", "2026-10-15T01:00:00Z", 0, "REGISTRATION_INCOMPLETE",
            "", ""}));
        nlohmann::json plan{{"archive_uuid", archive_uuid}, {"revision", 1}, {"label", "ARCH-0024"},
            {"created", "2026-10-15T01:00:00Z"}, {"new_archive", true}, {"root", "Z:\\"}, {"files_total", 10},
            {"bytes_total", 1000}, {"updated", updated}};
        va::record_attempt(plan, {{1000, "boot-1:1000", "veriarchive-cli.exe"}, host});
        CHECK(store.set_setting("registration:" + archive_uuid, plan.dump()));
    }

    // `runs_this_operation`: whether the reading window runs the operation of a plan.
    va::app::UnfinishedOperation read(
        va::IProcessTable* processes, std::function<bool(const nlohmann::json&)> runs_this_operation = {})
    {
        const va::app::AttemptCheck attempts{processes, "test-host", std::move(runs_this_operation)};
        const auto read = va::app::read_unfinished_operations(repository.context.repo, {},
            [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; }, attempts);
        CHECK(read && read->operations.size() == 1);
        return read && read->operations.size() == 1 ? read->operations.front() : UnfinishedOperation{};
    }
};

std::string timestamp(std::chrono::sys_seconds time)
{
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", time);
}

// A registration whose process has ended (killed) reads stopped at once,
// though its plan said a moment ago that it worked: it can be resumed, and
// abandoned. So does one whose process id now belongs to a later process.
void a_registration_whose_process_ended_reads_stopped_at_once()
{
    const va::app::Text english;
    const Clock::time_point start{};
    PlannedRegistration planned(timestamp(wall_now - 20s));
    test::FakeProcessTable processes;
    processes.self = {4242, "boot-1:4242", "veriarchive-gui.exe"};
    for (const bool reused : {false, true}) {
        processes.others.clear();
        if (reused) {
            processes.others.push_back({1000, "boot-1:31337", "notepad.exe"});
        }
        const auto operation = planned.read(&processes);
        CHECK(operation.attempt == va::AttemptState::Ended && operation.revision == 1);
        CHECK(operation.process && operation.process->process.id == 1000);
        OperationWatch watch;
        watch.update(operation, start, wall_now);
        CHECK(watch.state(start, wall_now) == OperationState::Stopped);
        const auto row = describe_operation(english, watch, start, wall_now);
        CHECK(row.state == "stopped; it can be resumed" && row.abandonable);
        CHECK(row.process == "Process: 1000 (veriarchive-cli.exe)" && row.key == operation.key);
    }

    // Judged by its plan alone, as before decision 0024, without a process
    // table, or for a plan of another computer: running while its plan is fresh.
    for (const bool other_computer : {false, true}) {
        PlannedRegistration elsewhere(timestamp(wall_now - 20s), other_computer ? "another-computer" : "test-host");
        const auto operation = elsewhere.read(other_computer ? &processes : nullptr);
        CHECK(operation.attempt == va::AttemptState::Unknown);
        OperationWatch watch;
        watch.update(operation, start, wall_now);
        CHECK(watch.state(start, wall_now) == OperationState::Running);
        CHECK(!describe_operation(english, watch, start, wall_now).abandonable);
        watch.update(operation, start + 4min, wall_now + 4min);
        CHECK(watch.state(start + 4min, wall_now + 4min) == OperationState::Stopped);
    }
}

// While its process runs, a registration runs: while its plan is fresh it is
// running, and once its plan has said nothing for minutes it is held up, not
// stopped, as a resume would be refused (a read from a failing device can take
// that long). Neither may be abandoned. This window's own process counts as
// ended only while the window runs no operation.
void a_registration_whose_process_runs_is_held_up_not_stopped()
{
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    const Clock::time_point start{};
    PlannedRegistration planned(timestamp(wall_now - 20s));
    test::FakeProcessTable processes;
    processes.self = {4242, "boot-1:4242", "veriarchive-gui.exe"};
    processes.others = {{1000, "boot-1:1000", "veriarchive-cli.exe"}};
    // Whether the window runs the operation of the plan, how often a read
    // asked, and of which root (decision 0045: a job of the window is told by
    // its root and archive).
    bool window_runs = false;
    int asked = 0;
    std::string asked_of;
    const auto window = [&](const nlohmann::json& plan) {
        ++asked;
        asked_of = plan.value("root", std::string{});
        return window_runs;
    };
    const auto operation = planned.read(&processes, window);
    CHECK(operation.attempt == va::AttemptState::Running);
    CHECK(asked == 0);  // asked only of a plan that names the window's process
    OperationWatch watch;
    watch.update(operation, start, wall_now);
    CHECK(watch.state(start, wall_now) == OperationState::Running);
    auto row = describe_operation(english, watch, start, wall_now);
    CHECK(row.state == "running" && !row.abandonable);

    watch.update(operation, start + 10min, wall_now + 10min);
    CHECK(watch.state(start + 10min, wall_now + 10min) == OperationState::Silent);
    row = describe_operation(english, watch, start + 10min, wall_now + 10min);
    CHECK(row.state == "no progress reported for 10 minutes; its process is still running" && !row.abandonable);
    CHECK(describe_operation(chinese, watch, start + 10min, wall_now + 10min).state ==
          "已 10 分鐘未回報進度；其處理程序仍在執行");
    CHECK(describe_operation(chinese, watch, start + 10min, wall_now + 10min).process ==
          "處理程序：1000（veriarchive-cli.exe）");
    watch.update(operation, start + 3h, wall_now + 3h);
    CHECK(describe_operation(english, watch, start + 3h, wall_now + 3h).state ==
          "no progress reported for 3 hours; its process is still running");
    // Held up, it is listed with what runs, ahead of what stopped.
    CHECK(watch.state(start + 3h, wall_now + 3h) != OperationState::Stopped);

    // The plan names this window's process: an attempt that this window
    // cancelled, whose stop mark could not be written, while it runs no
    // operation; possibly the registration it runs, while it runs one. The
    // window is asked as the plan is judged, after the read, so a
    // registration it started while a read ran shows running, not stopped.
    processes.self = {1000, "boot-1:1000", "veriarchive-gui.exe"};
    processes.others.clear();
    CHECK(planned.read(&processes, window).attempt == va::AttemptState::Ended && asked == 1 && asked_of == "Z:\\");
    window_runs = true;
    CHECK(planned.read(&processes, window).attempt == va::AttemptState::Running && asked == 2);
    CHECK(planned.read(&processes).attempt == va::AttemptState::Ended);  // without the question: it runs none
}

// A registration whose process has ended, taken up by an older version's
// `register --resume`, which writes no plan: once its checkpoints move under
// the plan the watch first saw, it runs again, judged by its counts, as one
// taken up after a stop mark (decision 0022).
void a_registration_taken_up_after_its_process_ended_runs()
{
    const Clock::time_point start{};
    auto operation = registration(10, 10 * mib);
    operation.attempt = va::AttemptState::Ended;
    operation.updated = wall_now - 1min;
    OperationWatch watch;
    watch.update(operation, start, wall_now);
    CHECK(watch.state(start, wall_now) == OperationState::Stopped);
    watch.update(operation, start + 5s, wall_now + 5s);
    CHECK(watch.state(start + 5s, wall_now + 5s) == OperationState::Stopped);
    operation.files_done = 1010;
    operation.bytes_done = 70 * mib;
    watch.update(operation, start + 65s, wall_now + 65s);
    CHECK(watch.state(start + 65s, wall_now + 65s) == OperationState::Running);
    CHECK(watch.operation().attempt == va::AttemptState::Unknown && !watch.operation().updated);
}

// ---- A relocation (decision 0026) ------------------------------------------

// An unfinished relocation is listed with the registrations and
// verifications: its new root, the revision it verifies against, its
// progress from its checkpoints against the totals of its record, and its
// state, which its record and its process tell as a registration's do
// (decision 0024). A stopped one may be abandoned; one whose process runs
// may not. The command line's --json judges it from one read.
void lists_an_unfinished_relocation()
{
    test::EngineFixture fixture;
    auto second = test::fake_volume(fixture.archive.root() / "new");
    second.filesystem_uuid = "5555666677778888";
    second.devices.front().serial = "NEW-DISK";
    fixture.media.extra_volumes.push_back(second);
    fixture.archive.add_file("old/a.txt", "aaaa"); // sample data
    fixture.archive.add_file("old/b.txt", "bbbbbb");
    const auto registered = fixture.register_root(fixture.archive.root() / "old");
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    fixture.archive.add_file("new/a.txt", "aaaa");
    fixture.archive.add_file("new/b.txt", "bbbbbb").open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media, fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid,
        fixture.host, fixture.processes};
    const auto interrupted = va::relocate_archive(services,
        {registered->archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    CHECK(!interrupted && interrupted.error().kind == va::RelocateFailure::Kind::Incomplete);

    test::FakeProcessTable window;
    window.self = {9999, "boot-1:9999", "veriarchive-gui.exe"};
    const va::app::AttemptCheck attempts{&window, "test-host", {}};
    const auto read_one = [&] {
        const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
            [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; }, attempts);
        CHECK(read && read->operations.size() == 1);
        return read && read->operations.size() == 1 ? read->operations.front() : UnfinishedOperation{};
    };
    const auto stopped = read_one();
    CHECK(stopped.kind == va::app::OperationKind::Relocation &&
          stopped.key == "relocation:" + registered->archive_uuid && stopped.label == "ARCH-0001");
    CHECK(stopped.root == va::path_to_utf8(fixture.archive.root() / "new") && stopped.revision == 1 &&
          !stopped.run_id.empty());
    CHECK(stopped.files_done == 1 && stopped.bytes_done == 4 && stopped.files_total == 2 && stopped.bytes_total == 10);
    CHECK(stopped.recorded_stopped && stopped.resumable && stopped.done_counted);
    CHECK(stopped.process && stopped.process->process.id == 4242);

    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    const Clock::time_point start{};
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    OperationWatch watch;
    watch.update(stopped, start, now);
    auto row = describe_operation(english, watch, start, now);
    CHECK(row.kind == "Relocation" && row.state == "stopped; it can be resumed" && row.abandonable);
    CHECK(row.progress == "4 B of 10 B (40%)" && row.details == "1 of 2 files");
    CHECK(row.process == "Process: 4242 (veriarchive-cli.exe)" && row.run_id.empty());
    CHECK(describe_operation(chinese, watch, start, now).kind == "搬移");
    CHECK(va::app::state_of_one_read(stopped, now) == OperationState::Stopped);
    CHECK(va::app::state_code(OperationState::Stopped) == "stopped");

    // Its process runs again (a resume that has not checkpointed yet): it
    // runs, and is not offered for abandoning.
    auto record = nlohmann::json::parse(fixture.store.setting(stopped.key).value_or(""), nullptr, false);
    record.erase("stopped");
    record["updated"] = va::current_timestamp();
    CHECK(fixture.store.set_setting(stopped.key, record.dump()));
    window.others = {fixture.processes.self};
    const auto running = read_one();
    CHECK(running.attempt == va::AttemptState::Running && !running.recorded_stopped);
    OperationWatch alive;
    alive.update(running, start, now);
    row = describe_operation(english, alive, start, now);
    CHECK(row.state == "running" && !row.abandonable);
    CHECK(va::app::state_code(va::app::state_of_one_read(running, now)) == "running");
    CHECK(va::app::state_code(va::app::state_of_one_read(running, now + 10min)) == "held_up");

    // One whose archive is at a newer revision than the one it verifies
    // cannot be resumed (R-104), and says so.
    record["revision"] = 0;
    record["stopped"] = va::current_timestamp();
    CHECK(fixture.store.set_setting(stopped.key, record.dump()));
    const auto outdated = read_one();
    CHECK(!outdated.resumable && outdated.revision == 0);
    OperationWatch dead;
    dead.update(outdated, start, now);
    row = describe_operation(english, dead, start, now);
    CHECK(row.state == "stopped; it cannot be resumed (the archive has a newer revision)" && row.abandonable);
}

// A relocation whose record says that its new carrier does not hold the
// recorded content (decision 0026) can never finish. While its attempt still
// runs (it reads on to list every entry that differs, then ends the
// relocation itself) it runs as any attempt; once the attempt has stopped
// (its stop mark, its process ended, or silent for minutes) it is shown as a
// relocation whose new carrier differs, which is not resumed but abandoned,
// in both languages and as --json says it, and is ordered with the stopped
// operations.
void a_relocation_whose_new_carrier_differs_waits_to_be_abandoned()
{
    UnfinishedOperation operation;
    operation.kind = va::app::OperationKind::Relocation;
    operation.key = "relocation:c0ffee00-0000-4000-8000-000000000026";
    operation.archive_uuid = "c0ffee00-0000-4000-8000-000000000026";
    operation.label = "ARCH-0026";
    operation.root = "Z:\\";
    operation.started = "2026-10-15T09:00:00Z";
    operation.files_done = 20;
    operation.bytes_done = 10 * mib;
    operation.files_total = 40;
    operation.bytes_total = 20 * mib;
    operation.differs = true;
    operation.resumable = false;
    operation.updated = wall_now - 20s;
    operation.attempt = va::AttemptState::Running;

    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    const Clock::time_point start{};
    OperationWatch working;
    working.update(operation, start, wall_now);
    CHECK(working.state(start, wall_now) == OperationState::Running);
    CHECK(!describe_operation(english, working, start, wall_now).abandonable);

    for (const auto& [stopped, attempt, updated] :
        {std::tuple{true, va::AttemptState::Running, wall_now - 20s},
            std::tuple{false, va::AttemptState::Ended, wall_now - 20s},
            std::tuple{false, va::AttemptState::Unknown, wall_now - 10min}}) {
        auto ended = operation;
        ended.recorded_stopped = stopped;
        ended.attempt = attempt;
        ended.updated = updated;
        OperationWatch watch;
        watch.update(ended, start, wall_now);
        CHECK(watch.state(start, wall_now) == OperationState::Differs);
        const auto row = describe_operation(english, watch, start, wall_now);
        CHECK(row.state_kind == OperationState::Differs && row.abandonable &&
              row.state == "new carrier differs; abandon it");
        CHECK(describe_operation(chinese, watch, start, wall_now).state == "新載體上的內容不符；請放棄它");
        CHECK(va::app::state_code(va::app::state_of_one_read(ended, wall_now)) == "differs");
    }

    // After a registration that runs, though it started later.
    auto ended = operation;
    ended.recorded_stopped = true;
    auto running = registration(1, mib);
    running.updated = wall_now;
    std::vector<OperationWatch> watches;
    va::app::follow(watches, {ended, running}, start, wall_now);
    const auto rows = va::app::describe_operations(english, watches, start, wall_now);
    CHECK(rows.size() == 2 && rows[0].kind == "Registration" && rows[1].kind == "Relocation" &&
          rows[1].state_kind == OperationState::Differs);
}

// ---- Made for an older revision (decision 0030) ----------------------------

// A registration or relocation made for an older revision than its archive
// has now never records. While its attempt still runs (it stops at its next
// checkpoint) it runs as any attempt, and is not offered for abandoning; once
// the attempt has stopped (its stop mark, its process ended, or silent for
// minutes) it is shown in a state of its own, to be abandoned, in both
// languages, as --json says it and on the command line's line, ordered with
// the stopped operations. A relocation whose new carrier differs keeps its
// own state when it was made for an older revision as well.
void an_operation_made_for_an_older_revision_waits_to_be_abandoned()
{
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    const Clock::time_point start{};
    for (const auto kind : {va::app::OperationKind::Registration, va::app::OperationKind::Relocation}) {
        auto operation = registration(20, 10 * mib);
        operation.kind = kind;
        operation.revision = 2;
        operation.files_total = 40;
        operation.bytes_total = 20 * mib;
        operation.later_revision = true;
        operation.resumable = false;
        operation.updated = wall_now - 20s;
        operation.attempt = va::AttemptState::Running;
        OperationWatch working;
        working.update(operation, start, wall_now);
        CHECK(working.state(start, wall_now) == OperationState::Running);
        CHECK(!describe_operation(english, working, start, wall_now).abandonable);

        for (const auto& [stopped, attempt, updated] :
            {std::tuple{true, va::AttemptState::Running, wall_now - 20s},
                std::tuple{false, va::AttemptState::Ended, wall_now - 20s},
                std::tuple{false, va::AttemptState::Unknown, wall_now - 10min}}) {
            auto ended = operation;
            ended.recorded_stopped = stopped;
            ended.attempt = attempt;
            ended.updated = updated;
            OperationWatch watch;
            watch.update(ended, start, wall_now);
            CHECK(watch.state(start, wall_now) == OperationState::LaterRevision);
            const auto row = describe_operation(english, watch, start, wall_now);
            CHECK(row.state_kind == OperationState::LaterRevision && row.abandonable &&
                  row.state == "archive has a later revision; abandon it");
            CHECK(describe_operation(chinese, watch, start, wall_now).state == "封存已有較新的版次；請放棄它");
            CHECK(va::app::state_code(va::app::state_of_one_read(ended, wall_now)) == "later_revision");
        }

        // After a registration that runs, though it started later.
        auto ended = operation;
        ended.key = std::string(kind == va::app::OperationKind::Registration ? "registration:" : "relocation:") +
                    "c0ffee00-0000-4000-8000-000000000030";
        ended.recorded_stopped = true;
        auto running = registration(1, mib);
        running.started = "2026-10-15T02:00:00Z";
        running.updated = wall_now;
        std::vector<OperationWatch> watches;
        va::app::follow(watches, {ended, running}, start, wall_now);
        const auto rows = va::app::describe_operations(english, watches, start, wall_now);
        CHECK(rows.size() == 2 && rows[0].state == "running" && rows[1].state_kind == OperationState::LaterRevision);
    }

    // Both hold for a stopped relocation (decision 0030, point 7): its new
    // carrier differs, and its archive has a later revision. It keeps the
    // state of a new carrier that differs, as the next relocation needs
    // another copy as well, and is offered for abandoning.
    auto differing = registration(20, 10 * mib);
    differing.kind = va::app::OperationKind::Relocation;
    differing.key = "relocation:c0ffee00-0000-4000-8000-000000000030";
    differing.revision = 1;
    differing.files_total = 40;
    differing.bytes_total = 20 * mib;
    differing.differs = true;
    differing.later_revision = true;
    differing.resumable = false;
    differing.recorded_stopped = true;
    differing.updated = wall_now - 20s;
    OperationWatch both;
    both.update(differing, start, wall_now);
    CHECK(both.state(start, wall_now) == OperationState::Differs);
    const auto row = describe_operation(english, both, start, wall_now);
    CHECK(row.state_kind == OperationState::Differs && row.abandonable &&
          row.state == "new carrier differs; abandon it");
    CHECK(describe_operation(chinese, both, start, wall_now).state == "新載體上的內容不符；請放棄它");
    CHECK(va::app::state_code(va::app::state_of_one_read(differing, wall_now)) == "differs");

    // The command line's line under the archive.
    auto registering = registration(1200, 3 * gib);
    registering.revision = 2;
    registering.files_total = 3000;
    registering.bytes_total = 8 * gib;
    registering.recorded_stopped = true;
    registering.later_revision = true;
    CHECK(va::app::describe_for_archive(english, registering, wall_now) ==
          "Unfinished registration of revision 2 from \"Z:\\\": 3.00 GiB of 8.00 GiB (37%) · 1200 of 3000 files; "
          "archive has a later revision; abandon it");
    CHECK(va::app::describe_for_archive(chinese, registering, wall_now) ==
          "未完成的註冊（版次 2，根目錄「Z:\\」）：3.00 GiB / 8.00 GiB（37%）・1200 / 3000 個檔案；封存已有較新的版次；請放棄它");
}

// Decision 0028, point 9: a registration over the lost manifest of an
// archive retired since, as a version before that point let one begin,
// never records: stopped (by its mark, its process ended, or silent for
// minutes), it is shown in a state of its own, to be abandoned, without
// Resume, in both languages, as --json says it and on the command line's
// line, ordered with the stopped operations. While its attempt runs, it is
// shown as any that runs. One made for an older revision as well keeps the
// state of a later revision.
void a_registration_of_an_archive_retired_since_waits_to_be_abandoned()
{
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    const Clock::time_point start{};
    auto operation = registration(20, 10 * mib);
    operation.revision = 3;
    operation.files_total = 40;
    operation.bytes_total = 20 * mib;
    operation.archive_retired = true;
    operation.updated = wall_now - 20s;
    operation.attempt = va::AttemptState::Running;
    OperationWatch working;
    working.update(operation, start, wall_now);
    CHECK(working.state(start, wall_now) == OperationState::Running);
    CHECK(!describe_operation(english, working, start, wall_now).abandonable);

    for (const auto& [stopped, attempt, updated] :
        {std::tuple{true, va::AttemptState::Running, wall_now - 20s},
            std::tuple{false, va::AttemptState::Ended, wall_now - 20s},
            std::tuple{false, va::AttemptState::Unknown, wall_now - 10min}}) {
        auto ended = operation;
        ended.recorded_stopped = stopped;
        ended.attempt = attempt;
        ended.updated = updated;
        OperationWatch watch;
        watch.update(ended, start, wall_now);
        CHECK(watch.state(start, wall_now) == OperationState::ArchiveRetired);
        const auto row = describe_operation(english, watch, start, wall_now);
        CHECK(row.state_kind == OperationState::ArchiveRetired && row.abandonable && !row.offers_resume &&
              row.state == "archive is retired; abandon it");
        CHECK(describe_operation(chinese, watch, start, wall_now).state == "封存已停用；請放棄它");
        CHECK(va::app::state_code(va::app::state_of_one_read(ended, wall_now)) == "archive_retired");
    }

    // After a registration that runs, though it started later.
    auto ended = operation;
    ended.key = "registration:c0ffee00-0000-4000-8000-000000000028";
    ended.recorded_stopped = true;
    auto running = registration(1, mib);
    running.started = "2026-10-15T02:00:00Z";
    running.updated = wall_now;
    std::vector<OperationWatch> watches;
    va::app::follow(watches, {ended, running}, start, wall_now);
    const auto rows = va::app::describe_operations(english, watches, start, wall_now);
    CHECK(rows.size() == 2 && rows[0].state == "running" && rows[1].state_kind == OperationState::ArchiveRetired);

    // Made for an older revision as well: that state, as its resume says.
    auto both = ended;
    both.later_revision = true;
    OperationWatch later;
    later.update(both, start, wall_now);
    CHECK(later.state(start, wall_now) == OperationState::LaterRevision);

    // The command line's line under the archive.
    CHECK(va::app::describe_for_archive(english, ended, wall_now) ==
          "Unfinished registration of revision 3 from \"Z:\\\": 10.0 MiB of 20.0 MiB (50%) · 20 of 40 files; "
          "archive is retired; abandon it");
    CHECK(va::app::describe_for_archive(chinese, ended, wall_now) ==
          "未完成的註冊（版次 3，根目錄「Z:\\」）：10.0 MiB / 20.0 MiB（50%）・20 / 40 個檔案；封存已停用；請放棄它");
}

// Decision 0028, point 9: the repository tells which unfinished registration
// is one over the lost manifest of an archive retired since. Archive A
// registered twice (sample data), its manifest of revision 2 gone, and a
// registration over that lost manifest interrupted (its device removed);
// then A is retired, as a version before that point retired it without
// looking at its registrations (sample damage of the archive's record). The
// registration reads as stopped and resumable until A is retired, then as
// one to abandon, without Resume; an interrupted registration of a new
// revision of another archive retired since, which is no registration over
// a lost manifest, is not told so.
void reads_a_registration_of_an_archive_retired_since()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a/SAMPLE-one.txt", "one (sample data)");
    fixture.archive.add_file("b/SAMPLE-three.txt", "three (sample data)");
    const auto first = fixture.register_root(fixture.archive.root() / "a");
    fixture.archive.add_file("a/SAMPLE-two.txt", "two (sample data)");
    va::RegisterRequest again;
    again.root = fixture.archive.root() / "a";
    again.new_revision = true;
    const auto second = fixture.register_root(again);
    const auto b = fixture.register_root(fixture.archive.root() / "b");
    CHECK(first && second && b);
    if (!first || !second || !b) {
        return;
    }
    const auto& archive_uuid = first->archive_uuid;
    std::filesystem::remove(va::manifest_path(fixture.repository.context.repo.manifests(), archive_uuid, 2));
    fixture.archive.node("a/SAMPLE-two.txt").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    va::RegisterRequest over;
    over.root = fixture.archive.root() / "a";
    over.lost_manifest_of = archive_uuid;
    over.confirmed = true;
    const auto interrupted = fixture.register_root(over);
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.add_file("b/SAMPLE-four.txt", "four (sample data)");
    fixture.archive.node("b/SAMPLE-four.txt").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    va::RegisterRequest next;
    next.root = fixture.archive.root() / "b";
    next.new_revision = true;
    const auto other = fixture.register_root(next);
    CHECK(!other && other.error().kind == va::RegisterFailure::Kind::Incomplete);

    const va::app::Text english;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    // Each registration's state and whether it offers Resume, as one read gives them.
    const auto states = [&] {
        const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
            [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; });
        CHECK(read && read->operations.size() == 2);
        std::map<va::Uuid, std::pair<std::string, bool>> found;
        for (const auto& operation : read ? read->operations : std::vector<UnfinishedOperation>{}) {
            OperationWatch watch;
            watch.update(operation, Clock::time_point{}, now);
            const auto row = describe_operation(english, watch, Clock::time_point{}, now);
            found[operation.archive_uuid] = {row.state, row.offers_resume};
        }
        return found;
    };
    auto before = states();
    CHECK((before[archive_uuid] == std::pair<std::string, bool>{"stopped; it can be resumed", true}));
    CHECK((before[b->archive_uuid] == std::pair<std::string, bool>{"stopped; it can be resumed", true}));

    fixture.repository.execute("UPDATE archives SET status = 'RETIRED'");
    auto after = states();
    CHECK((after[archive_uuid] == std::pair<std::string, bool>{"archive is retired; abandon it", false}));
    // Not a registration over a lost manifest: not told so.
    CHECK(after[b->archive_uuid].first != "archive is retired; abandon it");
}

// The repository tells which unfinished registration and relocation were
// made for an older revision (decision 0030). An archive at revision 1 has a
// relocation against revision 1 and a `register --new-revision` creating
// revision 2, both interrupted (sample data). They read as stopped and
// resumable until an import gives the archive revision 3; then both read as
// made for an older revision, to be abandoned.
void reads_operations_made_for_an_older_revision()
{
    test::EngineFixture fixture;
    auto second = test::fake_volume(fixture.archive.root() / "new");
    second.filesystem_uuid = "5555666677778888";
    second.devices.front().serial = "NEW-DISK";
    fixture.media.extra_volumes.push_back(second);
    fixture.archive.add_file("old/a.txt", "aaaa"); // sample data
    fixture.archive.add_file("old/b.txt", "bbbbbb");
    const auto registered = fixture.register_root(fixture.archive.root() / "old");
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    const auto& archive_uuid = registered->archive_uuid;
    fixture.archive.add_file("new/a.txt", "aaaa");
    fixture.archive.add_file("new/b.txt", "bbbbbb").open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media, fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid,
        fixture.host, fixture.processes};
    const auto relocated = va::relocate_archive(
        services, {archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Incomplete);
    fixture.archive.node("old/b.txt").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    va::RegisterRequest request;
    request.root = fixture.archive.root() / "old";
    request.new_revision = true;
    const auto interrupted = fixture.register_root(request);
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);

    test::FakeProcessTable window;
    window.self = {9999, "boot-1:9999", "veriarchive-gui.exe"};
    const va::app::AttemptCheck attempts{&window, "test-host", {}};
    const va::app::Text english;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    // The registration's and the relocation's state, as one read gives them.
    const auto states = [&] {
        const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
            [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; }, attempts);
        CHECK(read && read->operations.size() == 2);
        std::map<va::app::OperationKind, std::pair<bool, std::string>> found;
        for (const auto& operation : read ? read->operations : std::vector<UnfinishedOperation>{}) {
            OperationWatch watch;
            watch.update(operation, Clock::time_point{}, now);
            found[operation.kind] = {operation.later_revision,
                describe_operation(english, watch, Clock::time_point{}, now).state};
        }
        return found;
    };
    const auto before = states();
    CHECK(before.size() == 2);
    for (const auto& [kind, state] : before) {
        CHECK(!state.first && state.second == "stopped; it can be resumed");
    }

    test::import_later_revision(fixture, fixture.store, archive_uuid, 3);
    const auto after = states();
    CHECK(after.size() == 2);
    for (const auto& [kind, state] : after) {
        CHECK(state.first && state.second == "archive has a later revision; abandon it");
    }
}

// `archive list` and `archive show` give an archive's unfinished registration,
// relocation and verification that adds chunk hashes one line each (decisions
// 0026, 0033): the revision it creates or verifies, where it reads, what it
// has done, and its state judged from one read, as --json judges it, in the
// Archive List's words and in both languages; no speed or time left.
void words_a_line_for_the_command_line()
{
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    auto registering = registration(1200, 3 * gib);
    registering.revision = 2;
    registering.files_total = 3000;
    registering.bytes_total = 8 * gib;
    registering.recorded_stopped = true;
    CHECK(va::app::describe_for_archive(english, registering, wall_now) ==
          "Unfinished registration of revision 2 from \"Z:\\\": 3.00 GiB of 8.00 GiB (37%) · 1200 of 3000 files; "
          "stopped; it can be resumed");
    CHECK(va::app::describe_for_archive(chinese, registering, wall_now) ==
          "未完成的註冊（版次 2，根目錄「Z:\\」）：3.00 GiB / 8.00 GiB（37%）・1200 / 3000 個檔案；已停止；可以繼續");
    // A scan that has not counted the files yet, working.
    auto scanning = registration(0, 0);
    scanning.revision = 1;
    scanning.updated = wall_now - 10s;
    CHECK(va::app::describe_for_archive(english, scanning, wall_now) ==
          "Unfinished registration of revision 1 from \"Z:\\\": 0 B done · 0 files; running");
    // A plan of an older version, which says nothing of its state: as --json
    // judges it from one read ("waiting"), not as a window that has watched
    // it for minutes.
    auto older = registration(5, 5 * mib);
    older.revision = 1;
    CHECK(va::app::state_code(va::app::state_of_one_read(older, wall_now)) == "waiting");
    CHECK(va::app::describe_for_archive(english, older, wall_now) ==
          "Unfinished registration of revision 1 from \"Z:\\\": 5.00 MiB done · 5 files; waiting for progress...");

    UnfinishedOperation relocating;
    relocating.kind = va::app::OperationKind::Relocation;
    relocating.key = "relocation:c0ffee00-0000-4000-8000-000000000003";
    relocating.archive_uuid = "c0ffee00-0000-4000-8000-000000000003";
    relocating.label = "ARCH-0003";
    relocating.root = "Y:\\範例";
    relocating.revision = 1;
    relocating.started = "2026-10-15T03:00:00Z";
    relocating.files_done = 20;
    relocating.bytes_done = 10 * mib;
    relocating.files_total = 40;
    relocating.bytes_total = 20 * mib;
    relocating.updated = wall_now - 20s;
    relocating.attempt = va::AttemptState::Running;
    CHECK(va::app::describe_for_archive(english, relocating, wall_now) ==
          "Unfinished relocation of revision 1 to \"Y:\\範例\": 10.0 MiB of 20.0 MiB (50%) · 20 of 40 files; running");
    CHECK(va::app::describe_for_archive(chinese, relocating, wall_now) ==
          "未完成的搬移（版次 1，至「Y:\\範例」）：10.0 MiB / 20.0 MiB（50%）・20 / 40 個檔案；執行中");
    // Not counted (the rollback-journal mode, decision 0022): only the state
    // its record gives, here that its new carrier differs.
    relocating.done_counted = false;
    relocating.recorded_stopped = true;
    relocating.differs = true;
    CHECK(va::app::describe_for_archive(english, relocating, wall_now) ==
          "Unfinished relocation of revision 1 to \"Y:\\範例\": not counted; new carrier differs; abandon it");

    // A verification that adds chunk hashes, stopped (sample data).
    UnfinishedOperation adding;
    adding.kind = va::app::OperationKind::Verification;
    adding.key = "run:c0ffee00-0000-4000-8000-0000000000a1";
    adding.archive_uuid = "c0ffee00-0000-4000-8000-000000000033";
    adding.label = "ARCH-0033";
    adding.root = "X:\\範例";
    adding.run_id = "c0ffee00-0000-4000-8000-0000000000a1";
    adding.revision = 1;
    adding.started = "2026-10-15T03:00:00Z";
    adding.files_done = 20;
    adding.bytes_done = 10 * mib;
    adding.files_total = 40;
    adding.bytes_total = 20 * mib;
    adding.adds_chunk_hashes = true;
    adding.recorded_stopped = true;
    CHECK(va::app::describe_for_archive(english, adding, wall_now) ==
          "Unfinished verification of revision 1 adding chunk hashes, from \"X:\\範例\": 10.0 MiB of 20.0 MiB "
          "(50%) · 20 of 40 files; stopped; it can be resumed");
    CHECK(va::app::describe_for_archive(chinese, adding, wall_now) ==
          "未完成的驗證（加入區塊雜湊，版次 1，根目錄「X:\\範例」）：10.0 MiB / 20.0 MiB（50%）・20 / 40 個檔案；"
          "已停止；可以繼續");
}

// ---- A verification that adds chunk hashes (decision 0033) -----------------

// A stopped verification that adds chunk hashes is listed until it finishes or
// is abandoned: a later verification of the archive, which supersedes a
// stopped one that adds none, does not supersede it, as no other operation
// creates the archive's next revision while it is unfinished. Its row says
// what it does, and offers Abandon once stopped, in both languages; a stopped
// verification that adds none is not offered.
void a_verification_that_adds_chunk_hashes_is_listed_until_abandoned()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a.bin", std::string(200000, 'a')); // sample data
    fixture.archive.add_file("b.txt", "b (sample data)");
    va::RegisterRequest request;
    request.root = fixture.archive.root();
    request.chunk_size = 0; // as V1 registered it
    const auto registered = fixture.register_root(request);
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    fixture.archive.node("b.txt").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    va::VerifyRequest adding;
    adding.archive_uuid = registered->archive_uuid;
    adding.add_chunk_hashes = true;
    adding.chunk_size = 64 * 1024;
    const auto interrupted = fixture.verify(adding);
    CHECK(interrupted && interrupted->result == "INCOMPLETE");
    fixture.archive.node("b.txt").open_error.reset();
    // A later verification of the archive, which adds none, passes.
    const auto later = fixture.verify(fixture.archive.root());
    CHECK(later && later->result == "PASSED");

    const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
        [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; });
    CHECK(read && read->operations.size() == 1);
    if (!read || read->operations.size() != 1 || !interrupted) {
        return;
    }
    const auto& operation = read->operations.front();
    CHECK(operation.kind == va::app::OperationKind::Verification && operation.adds_chunk_hashes &&
          operation.run_id == interrupted->run_id && operation.recorded_stopped && operation.resumable);

    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    const Clock::time_point start{};
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    OperationWatch watch;
    watch.update(operation, start, now);
    const auto row = describe_operation(english, watch, start, now);
    CHECK(row.kind == "Verification adding chunk hashes" && row.state == "stopped; it can be resumed" &&
          row.abandonable && row.run_id == interrupted->run_id);
    CHECK(describe_operation(chinese, watch, start, now).kind == "驗證（加入區塊雜湊）");

    auto plain = operation;
    plain.adds_chunk_hashes = false;
    OperationWatch other;
    other.update(plain, start, now);
    const auto plain_row = describe_operation(english, other, start, now);
    CHECK(plain_row.kind == "Verification" && !plain_row.abandonable);
}

// The process that the record of a verification that adds chunk hashes names
// tells whether it still runs (decisions 0024, 0033), as a registration's
// plan does: while that process runs, one that has said nothing for minutes
// is held up, not stopped, and is not offered for abandoning; once that
// process has ended, it has stopped at once, and is. Its row names the
// process. The command line's `archive list` reads only the verifications
// that add chunk hashes. The records are written by hand, as the engine
// writes them (sample data).
void a_verification_that_adds_chunk_hashes_is_judged_by_its_process()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("b.txt", "b (sample data)");
    va::RegisterRequest request;
    request.root = fixture.archive.root();
    request.chunk_size = 0; // as V1 registered it
    const auto registered = fixture.register_root(request);
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    nlohmann::json record{{"archive_uuid", registered->archive_uuid}, {"revision", 1},
        {"root", va::path_to_utf8(fixture.archive.root())}, {"started", timestamp(now - 1h)},
        {"updated", timestamp(now - 10min)}, {"files_total", 1}, {"bytes_total", 15}, {"add_chunk_hashes", 65536}};
    va::record_attempt(record, {{1000, "boot-1:1000", "veriarchive-cli.exe"}, "test-host"});
    const std::string run_id = "c0ffee00-0000-4000-8000-0000000000a2";
    CHECK(fixture.store.set_setting("run:" + run_id, record.dump()));

    test::FakeProcessTable processes;
    processes.self = {4242, "boot-1:4242", "veriarchive-gui.exe"};
    processes.others = {{1000, "boot-1:1000", "veriarchive-cli.exe"}};
    const va::app::AttemptCheck attempts{&processes, "test-host", {}};
    const auto read_one = [&] {
        const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
            [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; }, attempts);
        CHECK(read && read->operations.size() == 1);
        return read && read->operations.size() == 1 ? read->operations.front() : UnfinishedOperation{};
    };
    const va::app::Text english;
    const Clock::time_point start{};

    const auto running = read_one();
    CHECK(running.kind == va::app::OperationKind::Verification && running.adds_chunk_hashes &&
          running.run_id == run_id && !running.recorded_stopped);
    CHECK(running.attempt == va::AttemptState::Running && running.process &&
          running.process->process.id == 1000);
    OperationWatch held;
    held.update(running, start, now);
    auto row = describe_operation(english, held, start, now);
    CHECK(row.kind == "Verification adding chunk hashes" &&
          row.state == "no progress reported for 10 minutes; its process is still running" && !row.abandonable);
    CHECK(row.process == "Process: 1000 (veriarchive-cli.exe)");
    CHECK(va::app::state_code(va::app::state_of_one_read(running, now)) == "held_up");

    processes.others.clear();
    const auto ended = read_one();
    CHECK(ended.attempt == va::AttemptState::Ended);
    OperationWatch stopped;
    stopped.update(ended, start, now);
    row = describe_operation(english, stopped, start, now);
    CHECK(row.state == "stopped; it can be resumed" && row.abandonable);

    // A verification that adds none, which works now: listed, but not read
    // for the command line.
    const nlohmann::json plain{{"archive_uuid", registered->archive_uuid}, {"revision", 1},
        {"root", va::path_to_utf8(fixture.archive.root())}, {"started", timestamp(now)}, {"updated", timestamp(now)},
        {"files_total", 1}, {"bytes_total", 15}};
    CHECK(fixture.store.set_setting("run:c0ffee00-0000-4000-8000-0000000000a3", plain.dump()));
    const auto every = fixture.store.unfinished_verifications(now);
    CHECK(every && every->size() == 2);
    const auto adding = fixture.store.unfinished_verifications(now, {}, true);
    CHECK(adding && adding->size() == 1 && adding->front().run_id == run_id);
}

// ---- Resume from the list (decision 0022, point 7) -------------------------

// The row of an operation in `state`, as the list shows it at once.
va::app::OperationRow row_of(const UnfinishedOperation& operation)
{
    OperationWatch watch;
    watch.update(operation, Clock::time_point{}, wall_now);
    return describe_operation(va::app::Text{}, watch, Clock::time_point{}, wall_now);
}

// The list offers Resume for a verification that has stopped and can be
// resumed (R-104), and for a registration that has stopped, or whose plan
// never says when it worked and that has been quiet for minutes: not for one
// that runs, or may still say so (waiting, quiet with a plan that says when
// it worked or of a verification, not known, held up), not for one that can
// no longer finish (an archive with a newer or later revision, or retired),
// and never for a relocation, which the GUI does not run. Abandon... goes
// with a registration's and a relocation's Resume, also where it alone is
// offered (decision 0025). Resume asks first only on the quiet registration
// whose plan names no process, which the older version may still run
// (operator, 2026-10-20).
void offers_resume_only_where_it_continues()
{
    auto verification = registration(3, 300);
    verification.kind = va::app::OperationKind::Verification;
    verification.key = "run:c0ffee00-0000-4000-8000-0000000000aa";
    verification.run_id = "c0ffee00-0000-4000-8000-0000000000aa";
    verification.recorded_stopped = true;  // recorded INCOMPLETE
    auto row = row_of(verification);
    CHECK(row.state == "stopped; it can be resumed" && row.offers_resume && !row.abandonable &&
          !row.resume_asks_first);
    // Killed, its record silent for minutes: stopped as well.
    verification.recorded_stopped = false;
    verification.updated = wall_now - 10min;
    CHECK(row_of(verification).offers_resume);
    // Its archive has a newer revision: it cannot be resumed (R-104).
    verification.resumable = false;
    row = row_of(verification);
    CHECK(row.state == "stopped; it cannot be resumed (the archive has a newer revision)" && !row.offers_resume);
    verification.resumable = true;
    // It runs, or may: it works now, it was just seen (an older version's
    // run, which says nothing), or its checkpoints are not counted.
    verification.updated = wall_now - 20s;
    row = row_of(verification);
    CHECK(row.state == "running" && !row.offers_resume);
    verification.updated.reset();
    CHECK(row_of(verification).state_kind == OperationState::Waiting && !row_of(verification).offers_resume);
    OperationWatch quiet;
    quiet.update(verification, Clock::time_point{}, wall_now);
    quiet.update(verification, Clock::time_point{} + 5min, wall_now);
    CHECK(quiet.state(Clock::time_point{} + 5min, wall_now) == OperationState::Quiet);
    row = describe_operation(va::app::Text{}, quiet, Clock::time_point{} + 5min, wall_now);
    CHECK(!row.offers_resume && !row.abandonable);
    verification.done_counted = false;
    CHECK(row_of(verification).state_kind == OperationState::Unknown && !row_of(verification).offers_resume);

    // A registration that stopped: its stop mark, or its process ended. Its
    // attempt has ended, so Resume resumes at once.
    auto registering = registration(10, 10 * mib);
    registering.recorded_stopped = true;
    row = row_of(registering);
    CHECK(row.state == "stopped; it can be resumed" && row.offers_resume && row.abandonable &&
          !row.resume_asks_first);
    registering.recorded_stopped = false;
    registering.attempt = va::AttemptState::Ended;
    registering.updated = wall_now - 20s;
    CHECK(row_of(registering).offers_resume && !row_of(registering).resume_asks_first);
    // Running, or held up while its process runs.
    registering.attempt = va::AttemptState::Running;
    CHECK(row_of(registering).state == "running" && !row_of(registering).offers_resume);
    registering.updated = wall_now - 10min;
    CHECK(row_of(registering).state_kind == OperationState::Silent && !row_of(registering).offers_resume);
    // Made for an older revision (decision 0030): only abandoned.
    registering.attempt = va::AttemptState::Unknown;
    registering.recorded_stopped = true;
    registering.later_revision = true;
    row = row_of(registering);
    CHECK(row.state_kind == OperationState::LaterRevision && !row.offers_resume && row.abandonable);

    // A registration whose plan never says when it worked (an older
    // version's): waiting at first, running while its checkpoints move, and
    // once they have stood still for 3 minutes, quiet: it may have stopped,
    // and nothing tells more, so it is offered as a stopped registration
    // (decision 0022, point 7; decision 0025), its Resume asking first, as
    // the older version may still run it, but where it can no longer finish,
    // where it is only abandoned.
    const auto offered = [](const UnfinishedOperation& older, OperationState state, bool resume, bool abandon,
                             bool asks) {
        const Clock::time_point start{};
        OperationWatch watch;
        watch.update(older, start, wall_now);
        auto moved = older;
        moved.files_done += 10;
        moved.bytes_done += 10 * mib;
        watch.update(moved, start + 1min, wall_now);
        const auto running = describe_operation(va::app::Text{}, watch, start + 1min, wall_now);
        watch.update(moved, start + 1min + 2min, wall_now);
        const auto later = describe_operation(va::app::Text{}, watch, start + 1min + 2min, wall_now);
        watch.update(moved, start + 1min + 3min, wall_now);
        const auto quiet_row = describe_operation(va::app::Text{}, watch, start + 1min + 3min, wall_now);
        return running.state_kind == OperationState::Running && !running.offers_resume && !running.abandonable &&
               !running.resume_asks_first && later.state_kind == OperationState::Running && !later.offers_resume &&
               !later.abandonable && !later.resume_asks_first && quiet_row.state_kind == state &&
               quiet_row.offers_resume == resume && quiet_row.abandonable == abandon &&
               quiet_row.resume_asks_first == asks;
    };
    auto older = registration(10, 10 * mib);
    row = row_of(older);
    CHECK(row.state_kind == OperationState::Waiting && !row.offers_resume && !row.abandonable &&
          !row.resume_asks_first);
    CHECK(offered(older, OperationState::Quiet, true, true, true));
    auto older_made_for_older = older;
    older_made_for_older.later_revision = true;
    CHECK(offered(older_made_for_older, OperationState::Quiet, false, true, false));
    auto older_retired = older;
    older_retired.archive_retired = true;
    CHECK(offered(older_retired, OperationState::Quiet, false, true, false));
    // A plan that names its process but never says when it worked, as a
    // version since decision 0024 leaves one whose writes of "updated" all
    // failed, here of another computer: quiet as well, with both, but its
    // Resume resumes at once, as the resume itself asks nothing of it (its
    // version names its manifest in the transaction that records it).
    auto named = older;
    named.process = va::AttemptProcess{{1000, "boot-1:1000", "veriarchive-cli.exe"}, "another-computer"};
    CHECK(offered(named, OperationState::Quiet, true, true, false));
    // Its checkpoints not counted (the rollback-journal mode): not known.
    older.done_counted = false;
    row = row_of(older);
    CHECK(row.state_kind == OperationState::Unknown && !row.offers_resume && !row.abandonable);
    // A plan that says when it worked is judged by that: running, not quiet.
    auto beating = registration(10, 10 * mib);
    beating.updated = wall_now - 20s;
    CHECK(offered(beating, OperationState::Running, false, false, false));

    // A relocation: abandoned in the list, resumed on the command line.
    auto relocating = registration(20, 10 * mib);
    relocating.kind = va::app::OperationKind::Relocation;
    relocating.key = "relocation:c0ffee00-0000-4000-8000-000000000001";
    relocating.recorded_stopped = true;
    row = row_of(relocating);
    CHECK(row.state == "stopped; it can be resumed" && !row.offers_resume && row.abandonable);
    relocating.differs = true;
    relocating.resumable = false;
    CHECK(row_of(relocating).state_kind == OperationState::Differs && !row_of(relocating).offers_resume);
}

// The rows of the repository's unfinished operations, read and judged at once
// as the list's first read does (sample data; no volume's used space asked).
std::vector<UnfinishedOperation> read_operations(test::EngineFixture& fixture)
{
    const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
        [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; });
    CHECK(read.has_value());
    return read ? read->operations : std::vector<UnfinishedOperation>{};
}

// Resume on a stopped verification continues it as `verify --full
// <archive_uuid> --resume <run_id>` does: the archive named by its uuid, so
// looked for among the attached volumes rather than at the root the run read,
// and the run, whose verified entries are not read again; strict metadata
// only where the policy sets it, as without --strict-metadata, for no check
// box of the GUI takes part. Then nothing is left to resume. (A sample
// volume, sample files.)
void a_stopped_verification_resumes_from_the_list_as_on_the_command_line()
{
    test::EngineFixture fixture;
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

    const auto operations = read_operations(fixture);
    CHECK(operations.size() == 1);
    if (operations.size() != 1) {
        return;
    }
    const auto& operation = operations.front();
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    OperationWatch watch;
    watch.update(operation, Clock::time_point{}, now);
    const auto row = describe_operation(va::app::Text{}, watch, Clock::time_point{}, now);
    CHECK(row.kind == "Verification" && row.state == "stopped; it can be resumed" && row.offers_resume);

    const auto request = va::app::verification_resume(operation, va::Config{});
    CHECK(request.archive_uuid == registered->archive_uuid && request.resume_run == interrupted->run_id);
    CHECK(!request.root && !request.strict_metadata && !request.rotational && request.rotational_override.empty());
    // As on the command line given no option: strict metadata where the
    // policy sets it, and the readers as io.rotational_override says (R-111,
    // a sample serial).
    va::Config config;
    config.policy.strict_metadata = true;
    config.rotational_override["SAMPLE-SERIAL"] = true;
    const auto strict = va::app::verification_resume(operation, config);
    CHECK(strict.strict_metadata && strict.archive_uuid == request.archive_uuid &&
          strict.resume_run == request.resume_run && !strict.root && !strict.rotational &&
          strict.rotational_override == config.rotational_override);
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.verify(request);
    CHECK(resumed && resumed->result == "PASSED" && resumed->resumed_from == interrupted->run_id);
    CHECK(resumed && resumed->root == fixture.archive.root() && resumed->counts.at("Verified") == 9);
    CHECK(interrupted->coverage_bytes >= 4000 && fixture.archive.bytes_read == 9000 - interrupted->coverage_bytes);
    CHECK(read_operations(fixture).empty());
}

// A root of a sample volume that the command line takes as it is typed: the
// sample letter K: on Windows, a folder below the root on Linux.
std::filesystem::path sample_root()
{
    return std::filesystem::path::preferred_separator == '\\' ? std::filesystem::path(R"(K:\SAMPLE-resume)")
                                                               : std::filesystem::path("/SAMPLE-resume");
}

// Resume on a stopped registration continues it as `register <root> --resume`
// does: the root its plan names, made as the command line makes a root typed
// there (here an older version's plan names it with a separator at its end),
// without a label or the options of a new registration, and without a media
// copy, for no check box of the GUI takes part: nothing is written on the
// media (R-03). The files it hashed are not read again, and the registration
// is recorded. Then nothing is left to resume. (A sample volume, sample files.)
void a_stopped_registration_resumes_from_the_list_as_on_the_command_line()
{
    test::EngineFixture fixture(sample_root());
    for (int index = 1; index <= 6; ++index) {
        fixture.archive.add_file("f" + std::to_string(index), std::string(1000, static_cast<char>('a' + index)));
    }
    fixture.archive.node("f4").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.register_root(fixture.archive.root());
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("f4").open_error.reset();
    const auto archives = fixture.store.archives();
    CHECK(archives && archives->size() == 1);
    if (!archives || archives->size() != 1) {
        return;
    }
    const auto archive_uuid = archives->front().archive_uuid;
    const auto key = "registration:" + archive_uuid;
    auto plan = nlohmann::json::parse(fixture.store.setting(key).value_or(""), nullptr, false);
    CHECK(plan.is_object() && plan.value("root", std::string{}) == va::path_to_utf8(sample_root()));
    plan["root"] = va::path_to_utf8(sample_root()) + (std::filesystem::path::preferred_separator == '\\' ? "\\" : "/");
    CHECK(fixture.store.set_setting(key, plan.dump()));
    const auto saved = fixture.store.file_rows(archive_uuid, 1);
    CHECK(saved && saved->size() >= 3);  // f1..f3 were checkpointed

    const auto operations = read_operations(fixture);
    CHECK(operations.size() == 1);
    if (operations.size() != 1) {
        return;
    }
    const auto& operation = operations.front();
    const auto row = row_of(operation);
    CHECK(row.kind == "Registration" && row.state == "stopped; it can be resumed" && row.offers_resume &&
          !row.resume_asks_first);

    va::Config config;
    config.rotational_override["SAMPLE-SERIAL"] = false; // R-111, a sample serial
    const auto request = va::app::registration_resume(operation, config);
    CHECK(request.resume && request.root == sample_root() && request.root == va::app::typed_root(operation.root));
    CHECK(!request.label && !request.copy_of && !request.new_revision && !request.force_new_archive &&
          !request.write_media_copy && !request.rotational && !request.confirmed &&
          request.rotational_override == config.rotational_override);
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.register_root(request);
    CHECK(resumed && resumed->archive_uuid == archive_uuid && resumed->revision == 1 && resumed->counts.files == 6);
    CHECK(saved && fixture.archive.bytes_read == 6000 - 1000 * saved->size());
    CHECK(read_operations(fixture).empty());
}

// A registration interrupted as the device of a file went away (sample data),
// whose plan a version before decision 0022 wrote: no "updated", no stop mark,
// no process, no totals (sample damage of the plan). The list shows it
// waiting, and once its checkpoints have stood still for 3 minutes, as one
// that may have stopped, with Resume, which asks first (operator,
// 2026-10-20), and Abandon... (decision 0022, point 7; decision 0025). The
// check that Abandon... runs first, the command line's, would abandon it. A
// request that Resume made without the operator's word is refused, changing
// nothing, as `register <root> --resume` is; the one it makes once the
// operator chose Resume in the question continues it as `register <root>
// --resume --confirm` does, reading only the rest, and then nothing is left.
void an_older_versions_quiet_registration_is_resumed_or_abandoned()
{
    test::EngineFixture fixture(sample_root());
    for (int index = 1; index <= 6; ++index) {
        fixture.archive.add_file("f" + std::to_string(index), std::string(1000, static_cast<char>('a' + index)));
    }
    fixture.archive.node("f4").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.register_root(fixture.archive.root());
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("f4").open_error.reset();
    const auto archives = fixture.store.archives();
    CHECK(archives && archives->size() == 1);
    if (!archives || archives->size() != 1) {
        return;
    }
    const auto archive_uuid = archives->front().archive_uuid;
    const auto key = "registration:" + archive_uuid;
    auto plan = nlohmann::json::parse(fixture.store.setting(key).value_or(""), nullptr, false);
    CHECK(plan.is_object() && plan.contains("updated") && plan.contains("stopped") && plan.contains("process"));
    for (const auto* member : {"updated", "stopped", "process", "files_total", "bytes_total", "media_copy"}) {
        plan.erase(member);
    }
    CHECK(fixture.store.set_setting(key, plan.dump()));

    // Two reads 3 minutes apart, the checkpoints still.
    const Clock::time_point start{};
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    OperationWatch watch;
    for (const auto after : {0min, 3min}) {
        const auto operations = read_operations(fixture);
        CHECK(operations.size() == 1);
        if (operations.size() != 1) {
            return;
        }
        watch.update(operations.front(), start + after, now + after);
        const auto row = describe_operation(va::app::Text{}, watch, start + after, now + after);
        if (after == 0min) {
            CHECK(row.state == "waiting for progress..." && !row.offers_resume && !row.abandonable &&
                  !row.resume_asks_first);
        } else {
            CHECK(row.state == "no progress seen for 3 minutes; it may have stopped" && row.offers_resume &&
                  row.abandonable && row.resume_asks_first);
        }
    }

    // Abandon... reads first what abandoning removes, with the command line's
    // check, which takes it: nothing in its plan says that it works.
    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto& operation = watch.operation();
    const auto abandonable =
        va::abandonable_registration(services, {archive_uuid, operation.revision, operation.started});
    CHECK(abandonable && abandonable->files >= 3 && abandonable->first_registration);

    // Resume asks first. Unconfirmed, the resume itself refuses, reading and
    // changing nothing, as `register <root> --resume` does on the command
    // line; the request it makes once the operator chose Resume in the
    // question continues it, as `register <root> --resume --confirm`.
    const auto unconfirmed = va::app::registration_resume(operation, va::Config{});
    CHECK(unconfirmed.resume && !unconfirmed.confirmed);
    fixture.archive.bytes_read = 0;
    const auto refused = fixture.register_root(unconfirmed);
    CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Unconfirmed &&
          refused.error().older_version && refused.error().older_version->archive_uuid == archive_uuid);
    CHECK(fixture.archive.bytes_read == 0 && read_operations(fixture).size() == 1);
    const auto request = va::app::registration_resume(operation, va::Config{}, true);
    CHECK(request.resume && request.confirmed && request.root == sample_root() && !request.write_media_copy);
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.register_root(request);
    CHECK(resumed && resumed->archive_uuid == archive_uuid && resumed->revision == 1 && resumed->counts.files == 6);
    CHECK(abandonable && fixture.archive.bytes_read == 6000 - 1000 * abandonable->files);
    CHECK(read_operations(fixture).empty());
}

// ---- A retired archive (decision 0022, point 5) ----------------------------

// A retired archive can still be verified, and its interrupted verifications
// resumed (decision 0003), so they are listed as any archive's; only where its
// latest manifest is lost (decision 0028, point 9) can they never be resumed,
// and they are left out, judged as the archive lists judge that manifest.
// Archives A and B at /vol/a and /vol/b (sample data), each verification
// interrupted as the device of its last file went away: both listed, stopped
// and resumable. A retired as any archive: both listed. B's manifest gone:
// both listed, as for any archive whose manifest cannot be used (decision
// 0028, point 3), also once retiring B asks for the confirmation; B retired
// with it, its device taken for gone as well: only A's run is listed. B's
// manifest put back: both again. One byte of the body of B's manifest changed
// (sample damage): both, as line 1, all that the lists read of it, is intact;
// once a rebuild has skipped that manifest, which the lists then read whole,
// only A's. Put back, with the index's copy of its header changed (sample
// damage): both, as a rebuild of the index gives that manifest back. Each
// run's record and checkpoints stay as they were. A's run, resumed from the
// list, reads only the rest of A and passes, warning that A is retired.
void leaves_out_the_verifications_of_a_retired_archive_whose_manifest_is_lost()
{
    test::EngineFixture fixture;
    for (int index = 1; index <= 5; ++index) {
        for (const auto* folder : {"a", "b"}) {
            fixture.archive.add_file(std::format("{}/SAMPLE-{}.txt", folder, index),
                std::string(1000, static_cast<char>('0' + index)));
        }
    }
    const auto a = fixture.register_root(fixture.archive.root() / "a");
    const auto b = fixture.register_root(fixture.archive.root() / "b");
    CHECK(a && b);
    if (!a || !b) {
        return;
    }
    const va::IoError unplugged{va::IoErrorKind::DeviceRemoved, 0, "unplugged (sample)"};
    fixture.archive.node("a/SAMPLE-5.txt").open_error = unplugged;
    fixture.archive.node("b/SAMPLE-5.txt").open_error = unplugged;
    const auto a_run = fixture.verify(fixture.archive.root() / "a");
    const auto b_run = fixture.verify(fixture.archive.root() / "b");
    CHECK(a_run && a_run->result == "INCOMPLETE" && b_run && b_run->result == "INCOMPLETE");
    if (!a_run || !b_run) {
        return;
    }

    // The archives whose verifications are listed, each stopped and resumable.
    const auto listed = [&] {
        std::vector<va::Uuid> archives;
        for (const auto& operation : read_operations(fixture)) {
            CHECK(operation.kind == va::app::OperationKind::Verification);
            CHECK(row_of(operation).state == "stopped; it can be resumed");
            archives.push_back(operation.archive_uuid);
        }
        std::ranges::sort(archives);
        return archives;
    };
    auto both = std::vector<va::Uuid>{a->archive_uuid, b->archive_uuid};
    std::ranges::sort(both);
    CHECK(listed() == both);
    // What each run left for a resume.
    const auto kept = [&](const std::string& run_id) {
        return std::pair{fixture.store.setting("run:" + run_id).value_or(""),
            fixture.store.progress(run_id).value_or(std::set<std::string>{})};
    };
    const auto a_kept = kept(a_run->run_id);
    const auto b_kept = kept(b_run->run_id);
    CHECK(!a_kept.first.empty() && !a_kept.second.empty() && !b_kept.first.empty() && !b_kept.second.empty());

    const auto retire = [&](const va::Uuid& archive_uuid, bool confirmed) {
        return va::retire_archive(fixture.store, *fixture.repository.files, fixture.repository.context.repo,
            va::RetireRequest{archive_uuid, confirmed});
    };
    const auto a_retired = retire(a->archive_uuid, false);
    CHECK(a_retired && !a_retired->lost_manifest);
    CHECK(listed() == both);
    CHECK(kept(a_run->run_id) == a_kept);

    const auto& layout = fixture.repository.context.repo;
    const auto saved = layout.root.parent_path() / "SAMPLE-b-r0001.vam";
    std::filesystem::copy_file(b->manifest, saved);
    std::filesystem::remove(b->manifest);
    CHECK(listed() == both);
    const auto asked = retire(b->archive_uuid, false);
    CHECK(!asked && asked.error().kind == va::RetireFailure::Kind::Unconfirmed);
    CHECK(listed() == both);
    const auto b_retired = retire(b->archive_uuid, true);
    CHECK(b_retired && b_retired->lost_manifest);
    CHECK(listed() == std::vector<va::Uuid>{a->archive_uuid});
    CHECK(kept(a_run->run_id) == a_kept && kept(b_run->run_id) == b_kept);

    std::filesystem::copy_file(saved, b->manifest);
    CHECK(listed() == both);

    std::string bytes;
    {
        std::ifstream input(saved, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    CHECK(bytes.size() > 5);
    if (bytes.size() > 5) {
        bytes[bytes.size() - 5] ^= 0x01;
    }
    std::ofstream(b->manifest, std::ios::binary | std::ios::trunc) << bytes;
    CHECK(listed() == both);
    const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, layout, test::not_cancelled(), test::no_progress());
    CHECK(rebuilt && rebuilt->revisions_kept.size() == 1);
    CHECK(listed() == std::vector<va::Uuid>{a->archive_uuid});

    std::filesystem::copy_file(saved, b->manifest, std::filesystem::copy_options::overwrite_existing);
    fixture.repository.execute(("UPDATE archive_revisions SET header_json = replace(header_json, "
                                "'\"volume_label\":\"ARCHIVE\"', '\"volume_label\":\"OTHER\"') WHERE archive_uuid = " +
                                test::sql_text(b->archive_uuid))
                                   .c_str());
    CHECK(listed() == both);
    CHECK(kept(a_run->run_id) == a_kept && kept(b_run->run_id) == b_kept);

    // A retired archive is verified as any: its run resumed from the list.
    fixture.archive.node("a/SAMPLE-5.txt").open_error.reset();
    const auto operations = read_operations(fixture);
    const auto a_operation = std::ranges::find(operations, a_run->run_id, &UnfinishedOperation::run_id);
    CHECK(a_operation != operations.end());
    if (a_operation == operations.end()) {
        return;
    }
    CHECK(row_of(*a_operation).offers_resume);
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.verify(va::app::verification_resume(*a_operation, va::Config{}));
    CHECK(resumed && resumed->archive_uuid == a->archive_uuid && resumed->resumed_from == a_run->run_id &&
          resumed->result == "PASSED_WITH_WARNINGS" &&
          std::ranges::find(resumed->warnings, "identity: the archive is retired") != resumed->warnings.end());
    CHECK(a_run->coverage_bytes >= 4000 && fixture.archive.bytes_read == 5000 - a_run->coverage_bytes);
    CHECK(listed() == std::vector<va::Uuid>{b->archive_uuid});
}

// A verification that adds chunk hashes and was recorded INCOMPLETE has
// stopped, whatever the process its record names does (decision 0033): the
// GUI's window goes on after it cancelled its run. Read by another process
// (another window) that sees that process still running, it is stopped, can
// be resumed and is offered for resuming and abandoning, in the Archive List
// and from one read, as --json judges it; Resume on its row then records the
// revision with chunk hashes. Its process is not judged, as a record that
// says that it stopped needs no judging (UnfinishedOperation::attempt).
// Sample data.
void a_verification_that_adds_chunk_hashes_recorded_incomplete_has_stopped()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a.bin", std::string(200000, 'a')); // sample data
    fixture.archive.add_file("b.txt", "b (sample data)");
    va::RegisterRequest request;
    request.root = fixture.archive.root();
    request.chunk_size = 0; // as V1 registered it
    const auto registered = fixture.register_root(request);
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    fixture.archive.node("b.txt").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    va::VerifyRequest adding;
    adding.archive_uuid = registered->archive_uuid;
    adding.add_chunk_hashes = true;
    adding.chunk_size = 64 * 1024;
    const auto interrupted = fixture.verify(adding);
    CHECK(interrupted && interrupted->result == "INCOMPLETE");

    test::FakeProcessTable window;
    window.self = {5151, "boot-1:2000", "veriarchive-gui.exe"};
    window.others = {fixture.processes.self};
    const va::app::AttemptCheck attempts{&window, "test-host", {}};
    const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
        [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; }, attempts);
    CHECK(read && read->operations.size() == 1);
    if (!read || read->operations.size() != 1 || !interrupted) {
        return;
    }
    const auto& operation = read->operations.front();
    CHECK(operation.adds_chunk_hashes && operation.run_id == interrupted->run_id && operation.recorded_stopped &&
          operation.resumable);
    CHECK(operation.attempt == va::AttemptState::Unknown && operation.process &&
          operation.process->process.id == fixture.processes.self.id);
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    CHECK(va::app::state_code(va::app::state_of_one_read(operation, now)) == "stopped");
    const va::app::Text english;
    const Clock::time_point start{};
    OperationWatch watch;
    watch.update(operation, start, now);
    const auto row = describe_operation(english, watch, start, now);
    CHECK(row.state == "stopped; it can be resumed" && row.abandonable && row.offers_resume &&
          row.process == "Process: 4242 (veriarchive-cli.exe)");

    // Resume on its row, which Register Archive and Verify Archive name first
    // when such a verification refuses them (decision 0033, point 7),
    // finishes it as `verify --full <uuid> --resume <run_id>` does: it goes on
    // adding chunk hashes, at the chunk size of the run it continues, and
    // records revision 2 with them.
    fixture.archive.node("b.txt").open_error.reset();
    const auto resumed = fixture.verify(va::app::verification_resume(operation, va::Config{}));
    CHECK(resumed && resumed->resumed_from == interrupted->run_id && resumed->adding_chunk_size == 64 * 1024 &&
          resumed->added_revision == 2 && resumed->not_added.empty());
    CHECK(read_operations(fixture).empty());
}

} // namespace

void unfinished_operations_tests()
{
    reads_operations_with_approximate_totals();
    an_older_verification_takes_its_totals_from_the_manifest();
    reads_nothing_into_a_missing_database();
    keeps_the_counts_of_a_stopped_verification();
    tells_running_from_stopped();
    a_cancelled_registration_reads_stopped();
    a_registration_resumed_by_an_older_version_runs();
    shows_only_the_record_state_when_not_counted();
    a_read_marks_the_archives();
    estimates_from_checkpoint_steps();
    estimates_from_steps_minutes_apart();
    estimates_between_small_and_large_files();
    restarts_the_estimate_after_a_stop();
    restarts_the_estimate_after_a_sleep();
    leaves_out_a_stale_speed();
    shows_100_percent_only_when_done();
    offers_resume_only_where_it_continues();
    a_stopped_verification_resumes_from_the_list_as_on_the_command_line();
    a_stopped_registration_resumes_from_the_list_as_on_the_command_line();
    an_older_versions_quiet_registration_is_resumed_or_abandoned();
    leaves_out_the_verifications_of_a_retired_archive_whose_manifest_is_lost();
    follows_operations_between_polls();
    words_one_row();
    orders_the_rows();
    a_registration_whose_process_ended_reads_stopped_at_once();
    a_registration_whose_process_runs_is_held_up_not_stopped();
    a_registration_taken_up_after_its_process_ended_runs();
    lists_an_unfinished_relocation();
    a_relocation_whose_new_carrier_differs_waits_to_be_abandoned();
    an_operation_made_for_an_older_revision_waits_to_be_abandoned();
    a_registration_of_an_archive_retired_since_waits_to_be_abandoned();
    reads_a_registration_of_an_archive_retired_since();
    reads_operations_made_for_an_older_revision();
    words_a_line_for_the_command_line();
    a_verification_that_adds_chunk_hashes_is_listed_until_abandoned();
    a_verification_that_adds_chunk_hashes_is_judged_by_its_process();
    a_verification_that_adds_chunk_hashes_recorded_incomplete_has_stopped();
}
