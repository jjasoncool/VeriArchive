#include "test_support.h"

#include "app/unfinished_operations.h"
#include "common/time.h"
#include "common/utf8.h"
#include "core/engine/relocate_engine.h"
#include "core/manifest/manifest_reader.h"
#include "engine_fixture.h"
#include "test_repository.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
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
// its details, in either language.
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

    std::filesystem::remove(registered->manifest);
    const auto gone = read_older();
    CHECK(gone && gone->files_total == 0 && gone->root.empty() &&
          gone->totals_failure == "the manifest of revision 1 (manifests/" + uuid +
                                      "/r0001.vam) is missing; restore it from a backup of the repository");
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
    CHECK(fixture.store.upsert_file_rows(archive_uuid, 1, {entry}));
    CHECK(poll(65s) == OperationState::Running);
    CHECK(describe_operation(english, watches.front(), start + 65s, now + 65s).state == "running");
    CHECK(!watches.front().operation().recorded_stopped && !watches.front().operation().updated);
    // Then quiet minutes say only that it may have stopped, as for any
    // operation of an older version.
    CHECK(poll(65s + 3min) == OperationState::Quiet);

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
    CHECK(store.upsert_file_rows(archive_uuid, 1, {hashed}));
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

    // `runs_an_operation`: whether the reading window runs an operation.
    va::app::UnfinishedOperation read(va::IProcessTable* processes, std::function<bool()> runs_an_operation = {})
    {
        const va::app::AttemptCheck attempts{processes, "test-host", std::move(runs_an_operation)};
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
    // Whether the window runs an operation, and how often a read asked.
    bool window_runs = false;
    int asked = 0;
    const auto window = [&] {
        ++asked;
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
    CHECK(planned.read(&processes, window).attempt == va::AttemptState::Ended && asked == 1);
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

// `archive list` and `archive show` give an archive's unfinished registration
// and relocation one line each (decision 0026): the revision it creates or
// verifies, where it reads, what it has done, and its state judged from one
// read, as --json judges it, in the Archive List's words and in both
// languages; no speed or time left.
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
    follows_operations_between_polls();
    words_one_row();
    orders_the_rows();
    a_registration_whose_process_ended_reads_stopped_at_once();
    a_registration_whose_process_runs_is_held_up_not_stopped();
    a_registration_taken_up_after_its_process_ended_runs();
    lists_an_unfinished_relocation();
    a_relocation_whose_new_carrier_differs_waits_to_be_abandoned();
    an_operation_made_for_an_older_revision_waits_to_be_abandoned();
    reads_operations_made_for_an_older_revision();
    words_a_line_for_the_command_line();
}
