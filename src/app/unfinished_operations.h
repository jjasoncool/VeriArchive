#pragma once
// Unfinished registrations and verifications, for showing their progress while
// they run, whoever runs them: this window, another one, or the command line.
// The GUI's Archive List polls the repository every few seconds
// (read_unfinished_operations), follows each operation from poll to poll
// (OperationWatch) and shows one row per operation (describe_operations).
//
// The repository only knows what the operations checkpoint (R-104): the files
// hashed or matched so far, written every 1000 files or 60 seconds, but only
// when a file finishes. So the numbers move in steps, and a large file or the
// scan before hashing can hold a step back for minutes. While it works, an
// operation also writes the time into its record ("updated", decision 0022),
// which tells a working operation from a stopped one. For an operation that
// does not (one started by an older version), a few quiet minutes only say
// that it *may* have stopped.
#include "app/progress_estimate.h"
#include "app/text.h"
#include "common/error.h"
#include "common/repository_layout.h"
#include "common/types.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace va::app {

enum class OperationKind { Registration, Verification };

// One unfinished operation as one poll of the repository saw it.
struct UnfinishedOperation {
    OperationKind kind = OperationKind::Registration;
    std::string key;                   // its setting ("registration:<uuid>", "run:<id>"): the same in every poll
    Uuid archive_uuid;
    std::string label;
    std::string root;                  // UTF-8; may be empty for a verification
    std::string run_id;                // verifications only
    Timestamp started;
    std::uint64_t files_done = 0;
    std::uint64_t bytes_done = 0;
    std::uint64_t files_total = 0;     // 0: not known
    std::uint64_t bytes_total = 0;     // 0: not known
    bool total_is_estimate = false;    // bytes_total is the volume's used space, not the bytes to hash
    bool recorded_stopped = false;     // its record says it stopped: a verification recorded INCOMPLETE,
                                       // a registration whose plan says "stopped"
    bool resumable = true;             // false: a verification whose archive has a newer revision (R-104)
    std::optional<std::chrono::sys_seconds> updated;  // when the operation last said it was working, if it does
};

// The space in use on the volume whose root is `root` (capacity less free
// space); nothing when it cannot be read, e.g. the media is not attached.
std::optional<std::uint64_t> volume_used_bytes(const std::filesystem::path& root);

// Reads the unfinished operations of the repository, newest first, without
// the verifications that nothing can finish any more (judged at the time now,
// ArchiveStore::unfinished_verifications). A registration whose plan has no
// totals yet, and whose root is a whole volume, gets that volume's used space
// (`used_bytes`) as its approximate total: every file on it is registered, so
// the files to hash fill about that much. `previous`: the operations of the
// read before; a verification recorded INCOMPLETE never changes, so its
// counts are taken from there. Opens the database read only, so watching
// never creates or changes it; meant to run on a thread of its own, as a read
// can take a moment on large repositories.
std::expected<std::vector<UnfinishedOperation>, IoError> read_unfinished_operations(const RepositoryLayout& layout,
    const std::vector<UnfinishedOperation>& previous = {},
    const std::function<std::optional<std::uint64_t>(const std::filesystem::path&)>& used_bytes =
        volume_used_bytes);

enum class OperationState {
    Waiting,   // first seen, and not yet seen to move
    Running,   // moved in the last few minutes, or its record says it works
    Quiet,     // no progress seen for a few minutes: stopped, or busy with a large file or the scan
    Stopped,   // certainly stopped (its record says so, or has been silent for minutes)
};

// After this long without progress an operation is no longer called running,
// and one whose record has said nothing for this long has stopped.
inline constexpr std::chrono::minutes quiet_after{3};

// Follows one operation from poll to poll: when its numbers last moved, and
// its speed and time left (ProgressEstimate).
class OperationWatch {
public:
    using Clock = std::chrono::steady_clock;

    // Takes the operation as a poll at `now` saw it. `wall_now` is the system
    // clock, for the operation's own "updated" time.
    void update(UnfinishedOperation operation, Clock::time_point now, std::chrono::sys_seconds wall_now);

    const UnfinishedOperation& operation() const { return operation_; }
    OperationState state(Clock::time_point now, std::chrono::sys_seconds wall_now) const;
    // How long no progress was seen: since the last step, or since the first
    // poll. This window cannot know what happened before it first looked.
    std::chrono::seconds quiet_for(Clock::time_point now) const;
    const ProgressEstimate& estimate() const { return estimate_; }

private:
    // A gap between two steps longer than this means the operation stopped
    // in between, or the computer slept.
    Clock::duration long_gap() const;

    UnfinishedOperation operation_;
    std::size_t polls_ = 0;
    Clock::time_point first_seen_{};
    std::optional<Clock::time_point> last_step_;
    std::deque<Clock::duration> step_gaps_;  // the last few gaps between steps: this operation's pace
    bool stopped_since_step_ = false;        // a poll since the last step saw it Stopped
    ProgressEstimate estimate_;
};

// Takes a poll's operations: one watch per operation, in the poll's order. An
// operation seen before keeps its watch (and so its speed); a finished one,
// no longer in the poll, is dropped.
void follow(std::vector<OperationWatch>& watches, std::vector<UnfinishedOperation> operations,
    OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now);

// The share done, from 0 to 1: by bytes when their total is known, else by
// files; nothing when neither is known, or when the done bytes already passed
// an approximate total. It stays below 1 (at most 0.99) until every file is
// done, so 100 % means done, as in ProgressEstimate.
std::optional<double> share_done(const UnfinishedOperation& operation);

// One row of the Archive List's unfinished operations, worded in `text`'s language.
struct OperationRow {
    std::string label;
    std::string root;
    std::string kind;                // "Registration", "Verification"
    std::string progress;            // "3.45 TiB of about 8.55 TiB (40%)"
    std::string details;             // "78895 files · 120 MiB/s · about 11-13 hours left"
    std::string state;               // "running", "no progress seen for 5 minutes; it may have stopped"
    OperationState state_kind = OperationState::Waiting;
    std::optional<double> fraction;  // for a progress bar
    Uuid archive_uuid;               // for a tooltip
    std::string run_id;              // verifications only, for a tooltip
};

OperationRow describe_operation(const Text& text, const OperationWatch& watch, OperationWatch::Clock::time_point now,
    std::chrono::sys_seconds wall_now);

// The rows in the order shown: the operations that run, or may, first, then
// those that stopped; the newest first within each.
std::vector<OperationRow> describe_operations(const Text& text, const std::vector<OperationWatch>& watches,
    OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now);

} // namespace va::app
