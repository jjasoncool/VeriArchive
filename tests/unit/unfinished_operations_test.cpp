#include "test_support.h"

#include "app/unfinished_operations.h"
#include "common/time.h"
#include "common/utf8.h"
#include "engine_fixture.h"
#include "test_repository.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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
    const auto operations = va::app::read_unfinished_operations(repository.context.repo, {},
        [&](const std::filesystem::path& root) -> std::optional<std::uint64_t> {
            asked.push_back(va::path_to_utf8(root));
            return 9 * gib;
        });
    CHECK(operations && operations->size() == 3);
    if (!operations || operations->size() != 3) {
        return;
    }
    // Newest first.
    const auto& planned = (*operations)[0];
    CHECK(planned.label == "ARCH-0003");
    CHECK(planned.bytes_total == 700 && planned.files_total == 7 && !planned.total_is_estimate);
    CHECK(planned.updated == va::parse_timestamp("2026-10-15T03:30:00Z"));
    CHECK(planned.recorded_stopped);  // its plan says the attempt stopped
    const auto& folder = (*operations)[1];
    CHECK(folder.bytes_total == 0 && !folder.total_is_estimate);  // not a whole volume: no total
    CHECK(!folder.recorded_stopped);
    const auto& whole = (*operations)[2];
    CHECK(whole.kind == va::app::OperationKind::Registration && whole.label == "ARCH-0001");
    CHECK(whole.bytes_total == 9 * gib && whole.total_is_estimate && whole.files_total == 0);
    // Only the whole volume without totals asked for its used space.
    CHECK(asked.size() == 1 && asked.front() == "Z:\\");
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
    CHECK(first && first->size() == 1 && first->front().files_done == 1 && first->front().recorded_stopped);
    if (!first || first->size() != 1) {
        return;
    }
    // Rows the read before did not see (they cannot appear for a stopped run;
    // here they show that nothing was counted again).
    entry.path.p = "b.txt";
    CHECK(store.add_progress("stopped", {va::entry_key(entry)}));
    const auto second = va::app::read_unfinished_operations(repository.context.repo, *first);
    CHECK(second && second->size() == 1 && second->front().files_done == 1);
    const auto fresh = va::app::read_unfinished_operations(repository.context.repo);
    CHECK(fresh && fresh->size() == 1 && fresh->front().files_done == 2);
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
        fixture.media_writer, fixture.media, fixture.store, fixture.repository.context.repo, fixture.host};
    va::RegisterRequest request;
    request.root = fixture.archive.root();
    const auto outcome = va::register_archive(services, request, fixture.cancel, cancelling);
    CHECK(!outcome && outcome.error().kind == va::RegisterFailure::Kind::Incomplete);

    const auto operations = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
        [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; });
    CHECK(operations && operations->size() == 1);
    if (!operations || operations->size() != 1) {
        return;
    }
    CHECK(operations->front().recorded_stopped);
    const va::app::Text english;
    const Clock::time_point start{};
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    OperationWatch watch;
    watch.update(operations->front(), start, now);
    watch.update(operations->front(), start + 5s, now + 5s);
    CHECK(watch.state(start + 5s, now + 5s) == OperationState::Stopped);
    CHECK(describe_operation(english, watch, start + 5s, now + 5s).state == "stopped; it can be resumed");
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

} // namespace

void unfinished_operations_tests()
{
    reads_operations_with_approximate_totals();
    reads_nothing_into_a_missing_database();
    keeps_the_counts_of_a_stopped_verification();
    tells_running_from_stopped();
    a_cancelled_registration_reads_stopped();
    estimates_from_checkpoint_steps();
    estimates_from_steps_minutes_apart();
    restarts_the_estimate_after_a_stop();
    leaves_out_a_stale_speed();
    shows_100_percent_only_when_done();
    follows_operations_between_polls();
    words_one_row();
    orders_the_rows();
}
