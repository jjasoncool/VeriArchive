#pragma once
// Unfinished registrations, verifications and relocations, for showing their
// progress while they run, whoever runs them: this window, another one, or the
// command line. The GUI's Archive List polls the repository every few seconds
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
// that it *may* have stopped. A registration's plan, and a relocation's record
// (decision 0026), also name the process that runs its attempt (decision
// 0024): on this computer, whether that process still runs tells at once
// whether the attempt does.
#include "app/progress_estimate.h"
#include "app/text.h"
#include "common/config.h"
#include "common/error.h"
#include "common/repository_layout.h"
#include "common/types.h"
#include "core/engine/register_engine.h"
#include "core/engine/registration_attempt.h"
#include "core/engine/verify_engine.h"
#include "core/repository/archive_store.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace va::app {

enum class OperationKind { Registration, Verification, Relocation };

// One unfinished operation as one poll of the repository saw it.
struct UnfinishedOperation {
    OperationKind kind = OperationKind::Registration;
    std::string key;                   // its setting ("registration:<uuid>", "run:<id>", "relocation:<uuid>"):
                                       // the same in every poll
    Uuid archive_uuid;
    std::string label;
    std::string root;                  // UTF-8; may be empty for a verification; a relocation's new root
    std::string run_id;                // verifications and relocations (its checkpoints, decision 0026)
    std::uint32_t revision = 0;        // the revision a registration creates, or a verification verifies, or a
                                       // relocation verifies against (it creates the next one)
    Timestamp started;                 // for a registration, when its plan was made (its "created")
    std::uint64_t files_done = 0;
    std::uint64_t bytes_done = 0;
    std::uint64_t files_total = 0;     // 0: not known
    std::uint64_t bytes_total = 0;     // 0: not known
    std::string totals_failure;        // why the totals are not known: a verification of a version before
                                       // decision 0022, whose revision's manifest holds them but cannot be
                                       // used (an engine sentence)
    bool total_is_estimate = false;    // bytes_total is the volume's used space, not the bytes to hash
    bool recorded_stopped = false;     // its record says it stopped: a verification recorded INCOMPLETE,
                                       // a registration whose plan says "stopped", a relocation likewise
    bool resumable = true;             // false: a verification or relocation whose archive has a newer
                                       // revision (R-104), or a relocation whose new carrier differs
    bool differs = false;              // a relocation whose record says that its new carrier does not hold
                                       // the recorded content (decision 0026): it can never finish
    bool later_revision = false;       // a registration or relocation made for an older revision: the archive
                                       // has a later one now, so it can never finish (decision 0030)
    bool archive_retired = false;      // a registration over a lost manifest of an archive retired since: a
                                       // retired archive takes no new revision, so it can never finish
                                       // (decision 0028, point 9)
    std::optional<std::chrono::sys_seconds> updated;  // when the operation last said it was working, if it does
    bool done_counted = true;          // false: files_done and bytes_done are not known, as the checkpoints
                                       // are not counted where that would hold up the operation (a database
                                       // not in WAL mode, decision 0022); only the record's state is shown
    // A registration or relocation whose record does not say it stopped:
    // whether the process that runs its attempt runs (decision 0024). Unknown
    // for a record that names no process, or one on another computer, and when
    // no process table was read; and for verifications.
    AttemptState attempt = AttemptState::Unknown;
    std::optional<AttemptProcess> process;  // the process the record names, if it names one
};

// How a read judges each registration's attempt (decision 0024): through this
// computer's process table. Without one, by the plan's "updated" alone
// (decision 0022).
struct AttemptCheck {
    IProcessTable* processes = nullptr;
    std::string host;  // this computer (HostInfo::host)
    // Whether this process runs an operation now (judge_attempt): asked only
    // for a plan that names this process, as it is judged after the read, so
    // a registration that this window started while the read ran is never
    // taken for one that has ended. Without it, this process runs none.
    std::function<bool()> runs_an_operation;
};

// What one read of the repository found.
struct OperationsRead {
    std::vector<UnfinishedOperation> operations;  // newest first
    // Changes whenever what the Archive List shows of the archives may have
    // changed: an archive added, a revision or a status recorded, a
    // verification recorded (ArchiveStore::archive_list_mark). Read in the
    // same read transaction as the operations, so the two agree.
    std::string archives_mark;
};

// The space in use on the volume whose root is `root` (capacity less free
// space); nothing when it cannot be read, e.g. the media is not attached.
std::optional<std::uint64_t> volume_used_bytes(const std::filesystem::path& root);

// Reads the unfinished operations of the repository, newest first, without
// the verifications that cannot be resumed or that a later verification of the
// same archive supersedes (judged at the time now,
// ArchiveStore::unfinished_verifications), nor those of a retired archive
// whose latest manifest is lost, which can never be resumed (judged as the
// archive lists judge it, retired_with_manifest_lost; decision 0022, point
// 5), and the mark of its archives. A registration whose plan has no totals
// yet, and whose root is a whole volume, gets that volume's used space
// (`used_bytes`) as its approximate total: every file on it is registered, so
// the files to hash fill about that much. A
// verification of a version before decision 0022, whose record has neither
// totals nor root, gets them from the header of its revision's manifest (its
// counts and root_hint, load_recorded_header), never from the index's copy of
// that header (R-14); when that manifest cannot be used, its totals stay
// unknown, and totals_failure says why.
// `previous`: the operations of the read before; a verification recorded
// INCOMPLETE never changes, so its counts are taken from there. Opens the
// database read only, so watching never creates or changes it; meant to run
// on a thread of its own, as a read can take a moment on large repositories.
// `attempts`: how each registration's attempt is judged (decision 0024).
std::expected<OperationsRead, IoError> read_unfinished_operations(const RepositoryLayout& layout,
    const std::vector<UnfinishedOperation>& previous = {},
    const std::function<std::optional<std::uint64_t>(const std::filesystem::path&)>& used_bytes =
        volume_used_bytes,
    const AttemptCheck& attempts = {});

// An unfinished registration as an operation of the list, its attempt judged
// as `attempts` says (decision 0024), its totals those of its plan, once its
// scan has counted them. read_unfinished_operations uses it, and adds the
// approximate total of a whole volume; so does the command line for
// `archive list|show`, without it.
UnfinishedOperation registration_operation(const PendingRegistration& registration, const AttemptCheck& attempts);

// An unfinished relocation (decision 0026) as an operation of the list, its
// attempt judged as `attempts` says. read_unfinished_operations uses it, and
// so does the command line for `archive list|show`.
UnfinishedOperation relocation_operation(const UnfinishedRelocation& relocation, const AttemptCheck& attempts);

enum class OperationState {
    Waiting,   // first seen, and not yet seen to move
    Running,   // moved in the last few minutes, or its record says it works
    Quiet,     // no progress seen for a few minutes: stopped, or busy with a large file or the scan
    Silent,    // a registration whose plan has said nothing for a few minutes, while the process that
               // runs its attempt still runs (decision 0024): held up, not stopped
    Stopped,   // certainly stopped (its record says so, has been silent for minutes, or the process
               // that ran its attempt has ended)
    Differs,   // a relocation stopped as Stopped says, whose record says that its new carrier does not
               // hold the recorded content (decision 0026): it is never resumed, and waits to be abandoned
    LaterRevision,  // a registration or relocation stopped as Stopped says, made for an older revision
                    // than the archive has now (decision 0030): it never records, and waits to be abandoned
    ArchiveRetired, // a registration over a lost manifest stopped as Stopped says, whose archive is retired
                    // by now (decision 0028, point 9): it never records, and waits to be abandoned
    Unknown,   // its progress is not counted and its record says nothing of its state (an older version)
};

// Whether an operation in `state` has stopped: Stopped, Differs, LaterRevision or
// ArchiveRetired.
bool has_stopped(OperationState state);

// After this long without progress an operation is no longer called running,
// and one whose record has said nothing for this long has stopped.
inline constexpr std::chrono::minutes quiet_after{3};

// The state in --json, in English: "waiting", "running", "quiet", "held_up"
// (Silent), "stopped", "differs", "later_revision", "archive_retired",
// "unknown".
std::string_view state_code(OperationState state);

// Follows one operation from poll to poll: when its numbers last moved, and
// its speed and time left (ProgressEstimate).
class OperationWatch {
public:
    using Clock = std::chrono::steady_clock;

    // Takes the operation as a poll at `now` saw it. `wall_now` is the system
    // clock, for the operation's own "updated" time.
    void update(UnfinishedOperation operation, Clock::time_point now, std::chrono::sys_seconds wall_now);

    // The polls were held up for a while, as when the computer slept: where
    // the clock goes on while it sleeps (Windows), the operation's next step
    // spans the sleep, in which nothing was done, though no poll saw the
    // operation stopped. That step is taken as one after a stop: the
    // estimate starts afresh, and its gap says nothing of the pace.
    void polls_held_up() { stopped_since_step_ = true; }

    // The operation as the latest poll saw it. A registration whose plan says
    // it stopped, or names a process that has ended, but whose checkpoints
    // moved since a poll first saw that, has been taken up again by an
    // attempt that does not write the plan (an older version's `register
    // --resume`): its plan's "stopped" and "updated", and what its process
    // tells, belong to the attempt before, so they are left out here, and it
    // is judged by its counts (the process its plan names stays named).
    const UnfinishedOperation& operation() const { return operation_; }
    // Whether a poll saw such a registration taken up again (see operation()):
    // an attempt that does not write the plan, an older version's, runs it,
    // whatever process its plan names.
    bool taken_up() const { return stop_mark_ && stop_mark_->taken_up; }
    OperationState state(Clock::time_point now, std::chrono::sys_seconds wall_now) const;
    // How long no progress was seen: since the last step, or since the first
    // poll. This window cannot know what happened before it first looked.
    std::chrono::seconds quiet_for(Clock::time_point now) const;
    const ProgressEstimate& estimate() const { return estimate_; }

private:
    // A gap between two steps longer than this, which brought little work for
    // its length (idle: `brought` bytes), means the operation stopped in
    // between, or the computer slept.
    Clock::duration long_gap() const;
    bool idle(Clock::duration gap, std::uint64_t brought) const;
    // The state as the record, the process and the counts tell it, before a
    // relocation whose new carrier differs, and an operation made for an
    // older revision, are told apart (state()).
    OperationState judged_state(Clock::time_point now, std::chrono::sys_seconds wall_now) const;
    // Leaves out a plan's stop mark, or its ended process, once another
    // attempt has taken the registration up again (see operation()).
    void follow_stop_mark(UnfinishedOperation& operation);

    // One gap between two steps, and the bytes the later step brought.
    struct Step {
        Clock::duration gap{};
        std::uint64_t bytes = 0;
    };
    // A registration's stop mark, or its ended process, as a poll first saw
    // it: the plan's "updated" written with it (a later attempt writes its
    // own), and the counts then.
    struct StopMark {
        std::optional<std::chrono::sys_seconds> updated;
        std::uint64_t files_done = 0;
        std::uint64_t bytes_done = 0;
        bool taken_up = false;  // the counts moved since: another attempt works on it
    };

    UnfinishedOperation operation_;
    std::size_t polls_ = 0;
    Clock::time_point first_seen_{};
    std::optional<Clock::time_point> last_step_;
    std::deque<Step> steps_;                 // the last few steps: this operation's pace and speed
    bool stopped_since_step_ = false;        // a poll since the last step saw it Stopped, or the
                                             // polls were held up (polls_held_up)
    std::optional<StopMark> stop_mark_;
    ProgressEstimate estimate_;
};

// Takes a poll's operations: one watch per operation, in the poll's order. An
// operation seen before keeps its watch (and so its speed); a finished one,
// no longer in the poll, is dropped.
void follow(std::vector<OperationWatch>& watches, std::vector<UnfinishedOperation> operations,
    OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now);

// The state of an operation from one read alone, as the command line shows it
// (--json), with nothing seen before: as the Archive List judges its first read.
OperationState state_of_one_read(const UnfinishedOperation& operation, std::chrono::sys_seconds wall_now);

// The share done, from 0 to 1: by bytes when their total is known, else by
// files; nothing when neither is known, when the done bytes already passed an
// approximate total, or when what is done was not counted. It stays below 1
// (at most 0.99) until every file is done, so 100 % means done, as in
// ProgressEstimate.
std::optional<double> share_done(const UnfinishedOperation& operation);

// One row of the Archive List's unfinished operations, worded in `text`'s language.
struct OperationRow {
    std::string label;
    std::string root;
    std::string kind;                // "Registration", "Verification", "Relocation"
    std::string progress;            // "3.45 TiB of about 8.55 TiB (40%)", "not counted"
    std::string files;               // the files done where the progress does not count them: "78895 files",
                                     // "1 of 2 files"; empty where they are not counted
    std::string details;             // "78895 files · 120 MiB/s · about 11-13 hours left", and last, when a
                                     // manifest keeps the total from being known, "total not known: <cause>"
    std::string state;               // "running", "no progress seen for 5 minutes; it may have stopped"
    OperationState state_kind = OperationState::Waiting;
    std::optional<double> fraction;  // for a progress bar
    Uuid archive_uuid;               // for a tooltip
    std::string run_id;              // verifications only, for a tooltip
    std::string process;             // a registration's or relocation's: "Process: 4242 (veriarchive-cli.exe)",
                                     // for a tooltip
    std::string key;                 // the operation's key (UnfinishedOperation::key)
    bool abandonable = false;        // a registration (decision 0025) or relocation (decision 0026) shown
                                     // stopped (has_stopped), or a registration whose plan never says when it
                                     // worked (an older version's) shown quiet (OperationState::Quiet): the
                                     // operator may abandon it
    bool offers_resume = false;      // a verification shown "stopped; it can be resumed", or a registration
                                     // shown stopped (OperationState::Stopped) or, never saying when it worked,
                                     // quiet, unless it can no longer finish: the list offers Resume, which
                                     // continues it as its command-line resume does (decision 0022, point 7;
                                     // verification_resume, registration_resume)
    bool resume_asks_first = false;  // Resume is offered on a registration shown quiet, whose plan never says
                                     // when it worked and names no process (an older version's), or that an
                                     // older version took up (OperationWatch::taken_up): that version may still
                                     // run it, so Resume asks first, saying older_version_resume_warning, and
                                     // resumes only once the operator chose Resume there (decision 0022, point
                                     // 7), as the resume itself asks for such a plan. Not on one shown stopped,
                                     // nor on a quiet one whose plan names a process: their versions name their
                                     // manifest in the transaction that records it (decision 0023)
};

OperationRow describe_operation(const Text& text, const OperationWatch& watch, OperationWatch::Clock::time_point now,
    std::chrono::sys_seconds wall_now);

// The rows in the order shown: the operations that run, or may, first, then
// those that stopped (has_stopped); the newest first within each.
std::vector<OperationRow> describe_operations(const Text& text, const std::vector<OperationWatch>& watches,
    OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now);

// What Resume on a row of the Archive List runs (decision 0022, point 7): the
// operation continued exactly as its command-line resume continues it given
// no option, the whole request made here. The GUI runs it as it is: the check
// boxes of the views where operations start take no part, as they are not
// where Resume is clicked.
//
// A stopped verification, as `verify --full <archive_uuid> --resume <run_id>`:
// its archive named by its uuid, and so looked for among the attached volumes
// (never at the root the run read before), and its run; strict metadata only
// where `policy` sets it, as on the command line without --strict-metadata.
VerifyRequest verification_resume(const UnfinishedOperation& verification, const Policy& policy);
// A stopped registration, as `register <root> --resume`: the root its plan
// names, made as the command line makes a root typed there (typed_root), and
// no media copy (R-03), as without --write-media-copy. Before anything of the
// root is read, it is checked as that command checks it (root_not_taken, with
// the plan's root as typed). `confirmed`: the operator chose Resume in the
// question that a resume of a registration an older version left asks first
// (OperationRow::resume_asks_first, or the resume's own refusal,
// RegisterFailure::older_version), as `--confirm` does on the command line;
// a resume of any other registration needs none (decision 0022, point 7).
RegisterRequest registration_resume(const UnfinishedOperation& registration, bool confirmed = false);

// What a resume of a registration that an older version of VeriArchive left,
// and that may still run it, says before the operator confirms it: the
// Archive List's question (OperationRow::resume_asks_first), and `register
// <root> --resume` without --confirm (decision 0022, point 7, operator,
// 2026-10-20). That version never records when it last worked, so nothing
// tells whether it still runs the registration: it may be reading a large
// file, or scanning, for minutes without a checkpoint. And it names its
// manifest before it records it (decision 0023, consequences): still running,
// it puts its own manifest in place of the one that the resume records as it
// finishes, after which that manifest has changed (decision 0028): every
// verification of the archive fails on it, and the registration of a new
// folder or volume and every relocation stop on it (point 4) until the
// archive takes a new revision over it (point 8).
inline constexpr std::string_view older_version_resume_warning =
    "An older version of VeriArchive left this registration unfinished. That version does not record when it "
    "last worked, so VeriArchive cannot tell whether it has stopped or is still reading a large file or scanning "
    "the folder or volume. If it is still running, it will later record its own manifest over the one that this "
    "resume records: from then on every verification of the archive fails, and other registrations and "
    "relocations stop until the archive gets a new revision. Make sure that the older version's window or "
    "command has been closed before you resume.";

// One line about an archive's unfinished registration or relocation, for the
// command line's `archive list` and `archive show` (decision 0026): the
// revision it creates or verifies, where it reads, what it has done and its
// state, judged from one read as --json judges it (state_of_one_read), and
// worded in `text`'s language as the Archive List words its row; no speed or
// time left, which one read cannot tell.
std::string describe_for_archive(const Text& text, const UnfinishedOperation& operation,
    std::chrono::sys_seconds wall_now);

} // namespace va::app
