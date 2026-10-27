#include "app/refusal_in_window.h"

#include "common/shell_argument.h"

#include <nlohmann/json.hpp>

#include <string>

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

// A registration of the archive (decision 0025). Register Archive's own way to
// finish it is its checkbox "Resume an interrupted registration of this folder
// or volume", with the registration's root entered; made for an older
// revision (decision 0030), it can only be abandoned.
std::string registration_refusal(const RefusingOperation& refusing, RefusedIn view, bool stopped)
{
    const auto& uuid = refusing.archive_uuid;
    const auto creates = std::to_string(refusing.revision);
    if (refusing.later_revision) {
        const auto made = "the unfinished registration of archive " + uuid + " (root \"" + refusing.root +
                          "\") would create revision " + creates + ", but the archive now has a later revision (" +
                          std::to_string(*refusing.later_revision) + "); the registration can no longer finish";
        return stopped ? made + ": abandon it with Abandon... in the Archive List"
                       : made + ", and is still running: wait for it to stop, then abandon it with Abandon... in the "
                                "Archive List";
    }
    // The registration of the very root entered: its plan is that root's.
    if (refusing.this_root) {
        const std::string ways = "finish it by ticking Resume an interrupted registration of this folder or volume "
                                 "and pressing Register again, or abandon it with Abandon... in the Archive List";
        return stopped ? "an interrupted registration of this root exists (archive " + uuid + "); " + ways
                       : "a registration of this root (archive " + uuid +
                             ") is still running; wait for it to finish; if it stops, " + ways;
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
    return stopped ? creating + "; nothing was changed (finish it first " + resume +
                         ", or abandon it with Abandon... in the Archive List)"
                   : creating + " and is still running; nothing was changed (wait for it to finish; if it stops, "
                                "finish it " +
                         resume + ", or abandon it with Abandon... in the Archive List)";
}

// A relocation of the archive (decision 0026), which has no window: it is
// continued on the command line, the new root written as it can be typed
// (decision 0022, point 9). One whose new carrier differs can never finish,
// and is only abandoned.
std::string relocation_refusal(const RefusingOperation& refusing, bool stopped)
{
    const auto& uuid = refusing.archive_uuid;
    const auto to = " (to \"" + refusing.root + "\")";
    if (refusing.differs) {
        const auto found = "the unfinished relocation of archive " + uuid + to +
                           " found that the new carrier does not hold the recorded content";
        return stopped ? found + " and cannot be resumed; abandon it with Abandon... in the Archive List, then "
                                 "relocate again"
                       : found + " and is still running; wait for it to end; if it is then still listed in the "
                                 "Archive List, abandon it there with Abandon..., then relocate again";
    }
    const auto ways = "resume it on the command line with `archive relocate " + uuid + " " +
                      shell_argument(refusing.root) + " --resume`, or abandon it with Abandon... in the Archive List";
    return stopped ? "an unfinished relocation of archive " + uuid + to + " exists; " + ways
                   : "an unfinished relocation of archive " + uuid + to +
                         " exists and is still running; wait for it to finish; if it stops, " + ways;
}

// A FULL verify that adds chunk hashes to the archive (decision 0033): Verify
// Archive's Resume interrupted run continues it once its archive is chosen in
// the view's list.
std::string verification_refusal(const RefusingOperation& refusing, RefusedIn view, bool stopped)
{
    const auto refusal = "an unfinished verification of archive " + refusing.archive_uuid +
                         " that adds chunk hashes (run " + refusing.run_id + ") is creating revision " +
                         std::to_string(refusing.revision + 1);
    const std::string resume = view == RefusedIn::VerifyArchive
                                   ? "with Resume interrupted run once the archive is chosen above"
                                   : "with Resume interrupted run in Verify Archive once the archive is chosen there";
    return stopped ? refusal + "; nothing was changed (finish it first " + resume +
                         ", or abandon it with Abandon... in the Archive List)"
                   : refusal + " and is still running; nothing was changed (wait for it to finish; if it stops, "
                               "finish it " +
                         resume + ", or abandon it with Abandon... in the Archive List)";
}

} // namespace

std::string refusal_in_window(const RefusingOperation& refusing, RefusedIn view, bool stopped)
{
    switch (refusing.kind) {
    case Kind::Registration:
        return registration_refusal(refusing, view, stopped);
    case Kind::Relocation:
        return relocation_refusal(refusing, stopped);
    case Kind::AddingVerification:
        return verification_refusal(refusing, view, stopped);
    }
    return {};
}

std::string register_message_in_window(const RegisterFailure& failure, bool stopped)
{
    return failure.refusing ? refusal_in_window(*failure.refusing, RefusedIn::RegisterArchive, stopped)
                            : failure.message;
}

std::string stop_message_in_window(const VerifyStop& stop, bool stopped)
{
    return stop.refusing ? refusal_in_window(*stop.refusing, RefusedIn::VerifyArchive, stopped) : stop.message;
}

std::expected<bool, IoError> refusing_operation_stopped(ArchiveStore& store, const RefusingOperation& refusing,
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
    return has_stopped(state_of_one_read(operation, wall_now));
}

} // namespace va::app
