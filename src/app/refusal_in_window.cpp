#include "app/refusal_in_window.h"

#include "common/shell_argument.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace va::app {
namespace {

using Kind = RefusingOperation::Kind;

// A member of a record read without exceptions: another type, or a record
// that is no object, reads as missing.
std::string text_member(const nlohmann::json& record, const char* name)
{
    if (!record.is_object()) {
        return {};
    }
    const auto found = record.find(name);
    return found != record.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// The ways on that the operation's row under Unfinished operations in
// Archives offers once it has stopped: Resume, which continues a stopped
// registration or verification exactly as its command-line resume does
// (decision 0022, point 7), and Abandon... (decisions 0025, 0033). The windows
// name them first, then the refused view's own way to finish it.
constexpr std::string_view on_its_row =
    "with Resume, or abandon it with Abandon..., on its row under Unfinished operations in Archives";

// Where Abandon... is for an operation that can only be abandoned.
constexpr std::string_view abandon_it = "abandon it with Abandon... under Unfinished operations in Archives";

// A registration of the archive (decision 0025): finished with Resume on its
// row under Unfinished operations, or with Register Archive's checkbox
// "Resume an interrupted registration of this folder or volume", the
// registration's root entered; made for an older revision (decision 0030), it
// can only be abandoned.
std::string registration_refusal(const RefusingOperation& refusing, RefusedIn view, bool runs)
{
    const auto& uuid = refusing.archive_uuid;
    const auto creates = std::to_string(refusing.revision);
    if (refusing.later_revision) {
        const auto made = "the unfinished registration of archive " + uuid + " (root \"" + refusing.root +
                          "\") would create revision " + creates + ", but the archive now has a later revision (" +
                          std::to_string(*refusing.later_revision) + "); the registration can no longer finish";
        return runs ? made + ", and is still running: wait for it to stop, then " + std::string(abandon_it)
                    : made + ": " + std::string(abandon_it);
    }
    // The registration of the very root entered: its plan is that root's.
    if (refusing.this_root) {
        const auto ways = "finish it " + std::string(on_its_row) +
                          "; or finish it by ticking Resume an interrupted registration of this folder or volume and "
                          "pressing Register again";
        return runs ? "a registration of this root (archive " + uuid +
                          ") is still running; wait for it to finish; if it stops, " + ways
                    : "an interrupted registration of this root exists (archive " + uuid + "); " + ways;
    }
    // Of another root of the archive, such as a second mount point of its volume.
    const auto creating = "an unfinished registration of archive " + uuid + " (root \"" + refusing.root +
                          "\") is creating revision " + creates;
    const std::string resume =
        view == RefusedIn::RegisterArchive
            ? "by entering its root above, ticking Resume an interrupted registration of this folder or volume and "
              "pressing Register"
            : "in Register Archive by entering its root, ticking Resume an interrupted registration of this folder or "
              "volume and pressing Register";
    const auto ways = std::string(on_its_row) + "; or finish it " + resume + ")";
    return runs ? creating + " and is still running; nothing was changed (wait for it to finish; if it stops, "
                             "finish it " +
                      ways
                : creating + "; nothing was changed (finish it first " + ways;
}

// A relocation of the archive (decision 0026), which has no window: it is
// continued on the command line, the new root written as it can be typed
// (decision 0022, point 9), or abandoned under Unfinished operations. One
// whose new carrier differs can never finish, and is only abandoned.
std::string relocation_refusal(const RefusingOperation& refusing, bool runs)
{
    const auto& uuid = refusing.archive_uuid;
    const auto to = " (to \"" + refusing.root + "\")";
    if (refusing.differs) {
        const auto found = "the unfinished relocation of archive " + uuid + to +
                           " found that the new carrier does not hold the recorded content";
        return runs ? found + " and is still running; wait for it to end; if it is then still listed under "
                              "Unfinished operations in Archives, abandon it there with Abandon..., then relocate again"
                    : found + " and cannot be resumed; " + std::string(abandon_it) + ", then relocate again";
    }
    const auto ways = "resume it on the command line with `archive relocate " + uuid + " " +
                      shell_argument(refusing.root) + " --resume`, or " + std::string(abandon_it);
    return runs ? "an unfinished relocation of archive " + uuid + to +
                      " exists and is still running; wait for it to finish; if it stops, " + ways
                : "an unfinished relocation of archive " + uuid + to + " exists; " + ways;
}

// A FULL verify that adds chunk hashes to the archive (decision 0033):
// finished with Resume on its row under Unfinished operations, or with Resume
// in the archive's details in Archives, where its stopped verification shows
// (decision 0046).
std::string verification_refusal(const RefusingOperation& refusing, RefusedIn view, bool runs)
{
    const auto refusal = "an unfinished verification of archive " + refusing.archive_uuid +
                         " that adds chunk hashes (run " + refusing.run_id + ") is creating revision " +
                         std::to_string(refusing.revision + 1);
    const std::string resume = view == RefusedIn::Archives ? "with Resume in the archive's details"
                                                           : "with Resume in the archive's details there";
    const auto ways = std::string(on_its_row) + "; or finish it " + resume + ")";
    return runs ? refusal + " and is still running; nothing was changed (wait for it to finish; if it stops, "
                            "finish it " +
                      ways
                : refusal + "; nothing was changed (finish it first " + ways;
}

} // namespace

std::string refusal_in_window(const RefusingOperation& refusing, RefusedIn view, bool runs)
{
    switch (refusing.kind) {
    case Kind::Registration:
        return registration_refusal(refusing, view, runs);
    case Kind::Relocation:
        return relocation_refusal(refusing, runs);
    case Kind::AddingVerification:
        return verification_refusal(refusing, view, runs);
    }
    return {};
}

std::string register_message_in_window(const RegisterFailure& failure, bool runs)
{
    return failure.refusing ? refusal_in_window(*failure.refusing, RefusedIn::RegisterArchive, runs)
                            : failure.message;
}

std::string stop_message_in_window(const VerifyStop& stop, bool runs)
{
    return stop.refusing ? refusal_in_window(*stop.refusing, RefusedIn::Archives, runs) : stop.message;
}

std::expected<bool, IoError> refusing_operation_runs(ArchiveStore& store, const RefusingOperation& refusing,
    const AttemptCheck& attempts, std::chrono::sys_seconds wall_now)
{
    const auto& record = refusing.record;
    UnfinishedOperation operation;
    switch (refusing.kind) {
    case Kind::Registration: {
        PendingRegistration registration;
        registration.archive_uuid = refusing.archive_uuid;
        registration.root = refusing.root;
        registration.revision = refusing.revision;
        registration.started = refusing.started;
        registration.updated = text_member(record, "updated");
        registration.stopped = text_member(record, "stopped");
        registration.plan = record.dump();
        registration.later_revision = refusing.later_revision.has_value();
        operation = registration_operation(registration, attempts);
        break;
    }
    case Kind::Relocation: {
        UnfinishedRelocation relocation;
        relocation.archive_uuid = refusing.archive_uuid;
        relocation.root = refusing.root;
        relocation.revision = refusing.revision;
        relocation.run_id = refusing.run_id;
        relocation.started = refusing.started;
        relocation.updated = text_member(record, "updated");
        relocation.stopped = text_member(record, "stopped");
        relocation.differs = text_member(record, "differs");
        relocation.later_revision = refusing.later_revision.has_value();
        relocation.record = record.dump();
        operation = relocation_operation(relocation, attempts);
        break;
    }
    case Kind::AddingVerification: {
        const auto recorded = store.verify_run(refusing.run_id);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
        UnfinishedVerification verification;
        verification.run_id = refusing.run_id;
        verification.archive_uuid = refusing.archive_uuid;
        verification.root = refusing.root;
        verification.revision = refusing.revision;
        verification.started = refusing.started;
        verification.recorded_incomplete = *recorded && (*recorded)->result == "INCOMPLETE";
        verification.updated = text_member(record, "updated");
        verification.adds_chunk_hashes = adding_chunk_size(record);
        verification.record = record.dump();
        operation = verification_operation(verification, attempts);
        break;
    }
    }
    // Running, or held up while its process runs. Where one read cannot tell,
    // as for a record that says nothing of its state (an older version's,
    // waiting for progress, and later quiet or not known), it is not taken
    // for one that runs: the window names its ways on (decision 0033).
    const auto state = state_of_one_read(operation, wall_now);
    return state == OperationState::Running || state == OperationState::Silent;
}

namespace {

// Whether the window says that `refusing` still runs: as
// refusing_operation_runs judges it, with this process taken for one that
// runs no operation (judge_attempt without `runs_an_operation`); not where it
// cannot be judged, where the window names its ways on.
bool runs_as_the_window_judges(ArchiveStore& store, const RefusingOperation& refusing, IProcessTable& processes,
    const std::string& host, std::chrono::sys_seconds now)
{
    const auto runs = refusing_operation_runs(store, refusing, AttemptCheck{&processes, host, {}}, now);
    return runs && *runs;
}

} // namespace

std::string register_message_in_window(const RegisterFailure& failure, ArchiveStore& store,
    IProcessTable& processes, const std::string& host, std::chrono::sys_seconds now)
{
    return register_message_in_window(
        failure, failure.refusing && runs_as_the_window_judges(store, *failure.refusing, processes, host, now));
}

std::string stop_message_in_window(const VerifyStop& stop, ArchiveStore& store, IProcessTable& processes,
    const std::string& host, std::chrono::sys_seconds now)
{
    return stop_message_in_window(
        stop, stop.refusing && runs_as_the_window_judges(store, *stop.refusing, processes, host, now));
}

} // namespace va::app
