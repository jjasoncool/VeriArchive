#include "app/unfinished_operations.h"

#include "common/time.h"
#include "common/utf8.h"
#include "core/repository/archive_store.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <system_error>

namespace va::app {

namespace {

// A gap between two steps longer than this starts the estimate afresh when it
// brought little work for its length (idle_share); for an operation that steps
// slowly (a volume of large videos checkpoints only when a file finishes,
// minutes apart), four times its usual gap, which it learns from the gaps
// seen so far.
constexpr auto long_gap_floor = std::chrono::minutes(10);
constexpr std::size_t steps_kept = 8;
// A long gap that brought less than this share of the bytes the operation
// reads in that time when it works (the fastest of its last few steps) was a
// stop, a sleep or a resumed scan, which bring almost nothing. A large file
// brings its bytes: between photos checkpointed 40 seconds apart, a video of
// 100 GB read in 11 minutes is work, not a stop.
constexpr double idle_share = 0.25;

// The bytes total the share is counted against: an approximate total that the
// done bytes already passed says nothing any more (a volume's used space can be
// smaller than the bytes of its files, e.g. with compressed files).
std::uint64_t usable_bytes_total(const UnfinishedOperation& operation)
{
    return operation.total_is_estimate && operation.bytes_done >= operation.bytes_total ? 0 : operation.bytes_total;
}

bool every_file_done(const UnfinishedOperation& operation)
{
    return operation.files_total > 0 && operation.files_done >= operation.files_total;
}

// The share as a whole percentage, rounded down. 100 % means done: only once
// every file is done, and never against an approximate total, which only the
// end of the operation can confirm.
int percent_of(const UnfinishedOperation& operation, double share)
{
    const auto percent = static_cast<int>(std::floor(share * 100));
    return every_file_done(operation) && !operation.total_is_estimate ? percent : std::min(percent, 99);
}

} // namespace

std::optional<std::uint64_t> volume_used_bytes(const std::filesystem::path& root)
{
    std::error_code error;
    const auto space = std::filesystem::space(root, error);
    if (error || space.capacity == 0 || space.free > space.capacity) {
        return std::nullopt;
    }
    return space.capacity - space.free;
}

std::expected<OperationsRead, IoError> read_unfinished_operations(const RepositoryLayout& layout,
    const std::vector<UnfinishedOperation>& previous,
    const std::function<std::optional<std::uint64_t>(const std::filesystem::path&)>& used_bytes,
    const AttemptCheck& attempts)
{
    // Read only: a database moved away while the list is shown reads as an
    // error instead of coming back empty, and watching never writes to it.
    auto store = ArchiveStore::open_read_only(layout);
    if (!store) {
        return std::unexpected(store.error());
    }
    // A verification recorded INCOMPLETE never changes: its counts of the
    // read before hold, and counting its checkpoints again would only cost.
    std::map<std::string, CheckpointCount> counted;
    for (const auto& operation : previous) {
        if (operation.kind == OperationKind::Verification && operation.recorded_stopped &&
            operation.done_counted) {
            counted[operation.run_id] = {operation.files_done, operation.bytes_done};
        }
    }
    // One read transaction: an operation that finishes meanwhile is either
    // still listed with the archives as they were, or gone with its result
    // in the mark.
    OperationsRead read;
    std::vector<PendingRegistration> registrations;
    std::vector<UnfinishedVerification> verifications;
    const auto done = store->read_transaction([&]() -> std::expected<void, IoError> {
        auto mark = store->archive_list_mark();
        if (!mark) {
            return std::unexpected(mark.error());
        }
        read.archives_mark = std::move(*mark);
        auto pending = store->pending_registrations();
        if (!pending) {
            return std::unexpected(pending.error());
        }
        registrations = std::move(*pending);
        // The time now decides which runs never recorded have been silent
        // long enough to have stopped, and so may be superseded.
        auto unfinished = store->unfinished_verifications(
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()), counted);
        if (!unfinished) {
            return std::unexpected(unfinished.error());
        }
        verifications = std::move(*unfinished);
        return {};
    });
    if (!done) {
        return std::unexpected(done.error());
    }

    auto& operations = read.operations;
    for (const auto& registration : registrations) {
        UnfinishedOperation operation;
        operation.kind = OperationKind::Registration;
        operation.key = "registration:" + registration.archive_uuid;
        operation.archive_uuid = registration.archive_uuid;
        operation.label = registration.label;
        operation.root = registration.root;
        operation.started = registration.started;
        operation.files_done = registration.files_done;
        operation.bytes_done = registration.bytes_done;
        operation.files_total = registration.files_total.value_or(0);
        operation.bytes_total = registration.bytes_total.value_or(0);
        if (!registration.bytes_total && registration.volume_root && !registration.root.empty()) {
            if (const auto used = used_bytes(path_from_utf8(registration.root))) {
                operation.bytes_total = *used;
                operation.total_is_estimate = true;
            }
        }
        operation.updated = parse_timestamp(registration.updated);
        // An attempt that ended without finishing says so in its plan.
        operation.recorded_stopped = !registration.stopped.empty();
        operation.done_counted = registration.done_counted;
        operation.revision = registration.revision;
        // Otherwise the process its plan names tells whether it still runs
        // (decision 0024): one that has ended has stopped at once, not when
        // its plan has said nothing for minutes.
        const auto plan = nlohmann::json::parse(registration.plan, nullptr, false);
        operation.process = recorded_attempt(plan);
        if (attempts.processes && !operation.recorded_stopped) {
            operation.attempt =
                judge_attempt(plan, *attempts.processes, attempts.host, attempts.runs_an_operation).state;
        }
        operations.push_back(std::move(operation));
    }
    for (const auto& verification : verifications) {
        UnfinishedOperation operation;
        operation.kind = OperationKind::Verification;
        operation.key = "run:" + verification.run_id;
        operation.archive_uuid = verification.archive_uuid;
        operation.label = verification.label;
        operation.root = verification.root;
        operation.run_id = verification.run_id;
        operation.revision = verification.revision;
        operation.started = verification.started;
        operation.files_done = verification.files_done;
        operation.bytes_done = verification.bytes_done;
        operation.files_total = verification.files_total;
        operation.bytes_total = verification.bytes_total;
        operation.recorded_stopped = verification.recorded_incomplete;
        operation.resumable = verification.resumable;
        operation.updated = parse_timestamp(verification.updated);
        operation.done_counted = verification.done_counted;
        operations.push_back(std::move(operation));
    }
    // Newest first. Timestamps sort as text; a stable sort keeps registrations
    // before verifications that started in the same second.
    std::ranges::stable_sort(operations, std::ranges::greater{}, &UnfinishedOperation::started);
    return read;
}

void OperationWatch::update(UnfinishedOperation operation, Clock::time_point now, std::chrono::sys_seconds wall_now)
{
    const bool stepped = polls_ > 0 && (operation.files_done != operation_.files_done ||
                                           operation.bytes_done != operation_.bytes_done);
    if (polls_ == 0) {
        first_seen_ = now;
    }
    follow_stop_mark(operation);
    if (stepped) {
        // After a stop (`register --resume`, a computer that slept) the first
        // step would count the whole stop, and a resumed registration's scan,
        // as one stretch of very slow work: the time left would read days for
        // many minutes. So the estimate starts afresh when a poll saw the
        // operation stopped since the last step, or the polls were held up
        // meanwhile (polls_held_up), or after a long gap that brought little
        // work. That costs a few minutes of "estimating time left". A large
        // file also holds a step back for minutes, but it brings its bytes;
        // and "long" goes by this operation's own pace, not by a fixed time.
        // A stop in the middle of a large file that no poll saw, and no
        // hold-up told, brings that file's bytes as well: one shorter than
        // about three times the file's read passes for work (idle).
        bool afresh = stopped_since_step_;
        if (last_step_) {
            const auto gap = now - *last_step_;
            // operation_ is still the poll before this one: the counts of the last step.
            const std::uint64_t brought =
                operation.bytes_done > operation_.bytes_done ? operation.bytes_done - operation_.bytes_done : 0;
            afresh = afresh || (gap > long_gap() && idle(gap, brought));
            // Every gap teaches the pace, a long one too: steps evenly 12
            // minutes apart (a large file each) would otherwise start the
            // estimate afresh at every step. Only a gap in which a poll saw
            // the operation stopped, or the polls were held up, says nothing
            // about its pace.
            if (!stopped_since_step_) {
                steps_.push_back({gap, brought});
                if (steps_.size() > steps_kept) {
                    steps_.pop_front();
                }
            }
        }
        if (afresh) {
            estimate_ = ProgressEstimate{};
        }
        last_step_ = now;
        stopped_since_step_ = false;
        // Only polls that saw a step feed the estimate, starting with the first
        // step seen. A poll between steps would count one step's work against
        // too short a time (the work of a minute, seen five seconds after the
        // poll before), and the speed would come out many times too high.
        // Files count only when their total is known: with the files left
        // unknown, a time per file would go missing from the time left, so the
        // estimate then goes by bytes alone.
        const auto files_done = operation.files_total > 0 ? operation.files_done : 0;
        estimate_.update(
            Progress{operation.bytes_done, usable_bytes_total(operation), files_done, operation.files_total, {}},
            now);
    }
    operation_ = std::move(operation);
    ++polls_;
    if (state(now, wall_now) == OperationState::Stopped) {
        stopped_since_step_ = true;
    }
}

OperationWatch::Clock::duration OperationWatch::long_gap() const
{
    Clock::duration gap = long_gap_floor;
    if (!steps_.empty()) {
        // The lower median: of two gaps, the shorter one. A stop among the
        // first few gaps (of an operation that does not say when it works)
        // then cannot stretch "long" to hours.
        std::vector<Clock::duration> gaps;
        gaps.reserve(steps_.size());
        for (const auto& step : steps_) {
            gaps.push_back(step.gap);
        }
        const auto middle = gaps.begin() + static_cast<std::ptrdiff_t>((gaps.size() - 1) / 2);
        std::ranges::nth_element(gaps, middle);
        gap = std::max(gap, 4 * *middle);
    }
    return gap;
}

bool OperationWatch::idle(Clock::duration gap, std::uint64_t brought) const
{
    // What the operation reads when it works: the fastest of its last few
    // steps. A stop, a sleep, or the last checkpoint of a cancelled attempt
    // (written early, without the large file it was reading) only ever make a
    // step slower, so they cannot lower it, as they would lower the speed of
    // the estimate, which spans them. Without a step yet, nothing tells a
    // long gap of work from a stop, and it counts as a stop.
    double fastest = 0;
    for (const auto& step : steps_) {
        if (const double seconds = std::chrono::duration<double>(step.gap).count(); seconds > 0) {
            fastest = std::max(fastest, static_cast<double>(step.bytes) / seconds);
        }
    }
    if (fastest <= 0) {
        return true;
    }
    return static_cast<double>(brought) < idle_share * fastest * std::chrono::duration<double>(gap).count();
}

void OperationWatch::follow_stop_mark(UnfinishedOperation& operation)
{
    // The plan says its attempt ended: by its stop mark, or by naming a
    // process that has ended (decision 0024).
    const bool ended = operation.recorded_stopped || operation.attempt == AttemptState::Ended;
    if (operation.kind != OperationKind::Registration || !ended) {
        stop_mark_.reset();
        return;
    }
    if (!stop_mark_ || stop_mark_->updated != operation.updated) {
        // A mark this watch has not seen: the attempt that wrote it flushed
        // its last checkpoint first, so these counts are its last.
        stop_mark_ = StopMark{operation.updated, operation.files_done, operation.bytes_done};
    } else if (operation.files_done != stop_mark_->files_done || operation.bytes_done != stop_mark_->bytes_done) {
        // The counts moved under the same mark: an attempt that does not
        // write the plan works on it (an older version's resume, decision
        // 0022). This version's resume writes the plan without the mark, and
        // with its own process.
        stop_mark_->taken_up = true;
    }
    if (stop_mark_->taken_up) {
        // The mark, the process and the "updated" with them are the attempt's before.
        operation.recorded_stopped = false;
        operation.attempt = AttemptState::Unknown;
        operation.updated.reset();
    }
}

OperationState OperationWatch::state(Clock::time_point now, std::chrono::sys_seconds wall_now) const
{
    // Its record says it stopped, or the process that ran its attempt has
    // ended (decision 0024).
    if (operation_.recorded_stopped || operation_.attempt == AttemptState::Ended) {
        return OperationState::Stopped;
    }
    if (operation_.updated) {
        // The operation says itself when it last worked.
        if (wall_now - *operation_.updated <= quiet_after) {
            return OperationState::Running;
        }
        // Silent for minutes. While the process that runs its attempt still
        // runs, it has not stopped (and a resume would be refused): something
        // holds it up, such as a read from a failing device.
        return operation_.attempt == AttemptState::Running ? OperationState::Silent : OperationState::Stopped;
    }
    if (operation_.attempt == AttemptState::Running) {
        return OperationState::Running;
    }
    if (!operation_.done_counted) {
        // An older version's record says nothing of its state, and its counts
        // are not read: nothing tells whether it works.
        return OperationState::Unknown;
    }
    if (polls_ <= 1) {
        return OperationState::Waiting;
    }
    if (quiet_for(now) >= quiet_after) {
        return OperationState::Quiet;
    }
    return last_step_ ? OperationState::Running : OperationState::Waiting;
}

std::chrono::seconds OperationWatch::quiet_for(Clock::time_point now) const
{
    return std::chrono::floor<std::chrono::seconds>(now - last_step_.value_or(first_seen_));
}

void follow(std::vector<OperationWatch>& watches, std::vector<UnfinishedOperation> operations,
    OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now)
{
    std::vector<OperationWatch> followed;
    followed.reserve(operations.size());
    for (auto& operation : operations) {
        const auto seen = std::ranges::find(watches, operation.key,
            [](const OperationWatch& watch) -> const std::string& { return watch.operation().key; });
        OperationWatch watch = seen != watches.end() ? std::move(*seen) : OperationWatch{};
        watch.update(std::move(operation), now, wall_now);
        followed.push_back(std::move(watch));
    }
    watches = std::move(followed);
}

std::optional<double> share_done(const UnfinishedOperation& operation)
{
    std::optional<double> share;
    if (!operation.done_counted) {
        return share;
    }
    if (const auto total = usable_bytes_total(operation); total > 0) {
        share = std::min(1.0, static_cast<double>(operation.bytes_done) / static_cast<double>(total));
    } else if (operation.files_total > 0) {
        share = std::min(1.0, static_cast<double>(operation.files_done) / static_cast<double>(operation.files_total));
    }
    // All bytes can be counted while files remain (empty files, or a file that
    // grew since the scan): the bar still waits for the last file.
    if (share && !every_file_done(operation)) {
        share = std::min(*share, 0.99);
    }
    return share;
}

OperationRow describe_operation(const Text& text, const OperationWatch& watch, OperationWatch::Clock::time_point now,
    std::chrono::sys_seconds wall_now)
{
    const auto& operation = watch.operation();
    OperationRow row;
    row.label = operation.label.empty() ? operation.archive_uuid.substr(0, 8) : operation.label;
    row.root = operation.root;
    row.kind = operation.kind == OperationKind::Registration ? text("Registration") : text("Verification");
    row.state_kind = watch.state(now, wall_now);
    row.fraction = share_done(operation);
    row.archive_uuid = operation.archive_uuid;
    row.run_id = operation.run_id;
    row.key = operation.key;
    if (operation.kind == OperationKind::Registration && operation.process) {
        const auto& program = operation.process->process.program;
        row.process = text.format(
            "Process: {} ({})", operation.process->process.id, program.empty() ? std::string("?") : program);
    }
    // A stopped registration may be given up on purpose (decision 0025); one
    // that runs, or may, may not.
    row.abandonable = operation.kind == OperationKind::Registration && row.state_kind == OperationState::Stopped;

    // What is done, against the total when it is known.
    const bool by_bytes = usable_bytes_total(operation) > 0;
    const bool by_files = !by_bytes && operation.files_total > 0;
    if (!operation.done_counted) {
        // Only the state, from the record (decision 0022).
        row.progress = text("not counted");
    } else if (row.fraction && by_bytes) {
        const auto done = format_size(operation.bytes_done);
        const auto total = format_size(operation.bytes_total);
        const auto percent = percent_of(operation, *row.fraction);
        // Two calls: the compiler checks each literal template against its values.
        row.progress = operation.total_is_estimate ? text.format("{} of about {} ({}%)", done, total, percent)
                                                   : text.format("{} of {} ({}%)", done, total, percent);
    } else if (row.fraction && by_files) {
        row.progress = text.format("{} of {} files ({}%)", operation.files_done, operation.files_total,
            percent_of(operation, *row.fraction));
    } else {
        row.progress = text.format("{} done", format_size(operation.bytes_done));
    }

    // Files (unless the progress counted them), then speed and time left while
    // it moves. An operation that says it works can still go minutes without
    // a step (a large file): its speed and time left would then be those of
    // minutes ago, so they are left out until it moves again.
    std::string details;
    if (operation.done_counted && !by_files) {
        details = operation.files_total > 0
                      ? text.format("{} of {} files", operation.files_done, operation.files_total)
                      : text.format("{} files", operation.files_done);
    }
    const auto add = [&](const std::string& part) {
        details = details.empty() ? part : text.format("{} · {}", details, part);
    };
    if (row.state_kind == OperationState::Running && watch.quiet_for(now) < quiet_after) {
        const auto& estimate = watch.estimate();
        if (const auto speed = estimate.bytes_per_second(); speed && *speed > 0) {
            add(text.format("{}/s", format_size(static_cast<std::uint64_t>(std::llround(*speed)))));
        }
        // Without a total there is nothing to count the time left against.
        if (row.fraction) {
            add(format_time_left(text, estimate.time_left()));
        }
    }
    row.details = details;

    switch (row.state_kind) {
    case OperationState::Waiting:
        row.state = text("waiting for progress...");
        break;
    case OperationState::Unknown:
        row.state = text("not known");
        break;
    case OperationState::Running:
        row.state = text("running");
        break;
    case OperationState::Quiet: {
        // Counted from when this window first looked, which may be long after
        // the operation last moved: "seen" says so.
        const auto minutes = std::chrono::floor<std::chrono::minutes>(watch.quiet_for(now)).count();
        // Two calls, as above.
        row.state = minutes < 120
                        ? text.format("no progress seen for {} minutes; it may have stopped", minutes)
                        : text.format("no progress seen for {} hours; it may have stopped", minutes / 60);
        break;
    }
    case OperationState::Silent: {
        // Counted from the attempt's own last word ("updated").
        const auto minutes =
            operation.updated ? std::chrono::floor<std::chrono::minutes>(wall_now - *operation.updated).count() : 0;
        // Two calls, as above.
        row.state = minutes < 120
                        ? text.format("no progress reported for {} minutes; its process is still running", minutes)
                        : text.format("no progress reported for {} hours; its process is still running", minutes / 60);
        break;
    }
    case OperationState::Stopped:
        row.state = operation.resumable ? text("stopped; it can be resumed")
                                        : text("stopped; it cannot be resumed (the archive has a newer revision)");
        break;
    }
    return row;
}

std::vector<OperationRow> describe_operations(const Text& text, const std::vector<OperationWatch>& watches,
    OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now)
{
    struct Shown {
        OperationRow row;
        const Timestamp* started;
    };
    std::vector<Shown> shown;
    shown.reserve(watches.size());
    for (const auto& watch : watches) {
        shown.push_back({describe_operation(text, watch, now, wall_now), &watch.operation().started});
    }
    // What runs is what the operator follows; stopped operations, which wait
    // for a resume, come after it.
    std::ranges::stable_sort(shown, [](const Shown& left, const Shown& right) {
        const bool left_stopped = left.row.state_kind == OperationState::Stopped;
        const bool right_stopped = right.row.state_kind == OperationState::Stopped;
        if (left_stopped != right_stopped) {
            return right_stopped;
        }
        return *left.started > *right.started;
    });
    std::vector<OperationRow> rows;
    rows.reserve(shown.size());
    for (auto& entry : shown) {
        rows.push_back(std::move(entry.row));
    }
    return rows;
}

} // namespace va::app
