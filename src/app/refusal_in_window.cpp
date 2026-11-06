#include "app/refusal_in_window.h"

#include "core/engine/volume_watch.h"

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

// The ways on that the operation's row under Unfinished operations in Jobs
// (decision 0049) offers once it has stopped: Resume, which continues a stopped
// registration, verification or relocation exactly as its command-line
// resume does (decision 0022, point 7; decision 0046), and Abandon...
// (decisions 0025, 0026, 0033). The windows name them first, then the refused
// view's own way to finish it.
constexpr std::string_view on_its_row =
    "with Resume, or abandon it with Abandon..., on its row under Unfinished operations in Jobs";

// Where Abandon... is for an operation that can only be abandoned.
constexpr std::string_view abandon_it = "abandon it with Abandon... under Unfinished operations in Jobs";

// Register Archive (decision 0046) offers only Resume the interrupted
// registration for a root whose registration is unfinished, once that root is
// chosen in it.
constexpr std::string_view in_register_archive = "whose options then resume it";

// A registration of the archive (decision 0025): finished with Resume on its
// row under Unfinished operations, or in Register Archive, the registration's
// root chosen there, whose options then resume it; made for an older revision
// (decision 0030), it can only be abandoned. Register Archive and Archives
// name the same ways on.
std::string registration_refusal(const RefusingOperation& refusing, bool runs)
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
                          "; or choose this folder or volume again in Register Archive, " +
                          std::string(in_register_archive);
        return runs ? "a registration of this root (archive " + uuid +
                          ") is still running; wait for it to finish; if it stops, " + ways
                    : "an interrupted registration of this root exists (archive " + uuid + "); " + ways;
    }
    // Of another root of the archive, such as a second mount point of its volume.
    const auto creating = "an unfinished registration of archive " + uuid + " (root \"" + refusing.root +
                          "\") is creating revision " + creates;
    const auto ways = std::string(on_its_row) + "; or finish it by choosing its root in Register Archive, " +
                      std::string(in_register_archive) + ")";
    return runs ? creating + " and is still running; nothing was changed (wait for it to finish; if it stops, "
                             "finish it " +
                      ways
                : creating + "; nothing was changed (finish it first " + ways;
}

// A relocation of the archive (decision 0026): finished with Resume on its row
// under Unfinished operations, or in the archive's details in Archives, where
// its stopped relocation shows (decision 0046), or abandoned there. One whose
// new carrier differs, or made for an older revision (decision 0030), can
// never finish, and is only abandoned.
std::string relocation_refusal(const RefusingOperation& refusing, bool runs)
{
    const auto& uuid = refusing.archive_uuid;
    const auto to = " (to \"" + refusing.root + "\")";
    if (refusing.differs) {
        const auto found = "the unfinished relocation of archive " + uuid + to +
                           " found that the new carrier does not hold the recorded content";
        return runs ? found + " and is still running; wait for it to end; if it is then still listed under "
                              "Unfinished operations in Jobs, abandon it there with Abandon..., then relocate again"
                    : found + " and cannot be resumed; " + std::string(abandon_it) + ", then relocate again";
    }
    if (refusing.later_revision) {
        const auto made = "the unfinished relocation of archive " + uuid + to + " verifies revision " +
                          std::to_string(refusing.revision) + ", but the archive now has a later revision (" +
                          std::to_string(*refusing.later_revision) + "); the relocation can no longer finish";
        return runs ? made + ", and is still running: wait for it to stop, then " + std::string(abandon_it) +
                          ", then relocate again"
                    : made + ": " + std::string(abandon_it) + ", then relocate again";
    }
    const auto ways = "resume it " + std::string(on_its_row) + "; or resume it in the archive's details";
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
                                                           : "with Resume in the archive's details in Archives";
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
        return registration_refusal(refusing, runs);
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

bool runs_as_the_window_judges(ArchiveStore& store, const RefusingOperation& refusing, IProcessTable& processes,
    const std::string& host, std::chrono::sys_seconds now)
{
    const auto runs = refusing_operation_runs(store, refusing, AttemptCheck{&processes, host, {}}, now);
    return runs && *runs;
}

std::string relocation_message_in_window(const Text& text, const RelocateFailure& failure, bool runs)
{
    // A resume at a root where its new carrier is not attached now, as when
    // a drive letter changed: the relocation dialog resumes it at the root the
    // operator gives (decision 0046), as `archive relocate <uuid> <root>
    // --resume` does.
    if (failure.elsewhere) {
        if (failure.refusing) {
            return text.format("the volume or folder of this root is not the one the unfinished relocation of "
                               "archive {} started on (\"{}\"); where the new carrier is attached at another drive "
                               "letter or mount point now, choose Relocate to a new carrier... in the archive's "
                               "details and give its root there, which resumes the relocation; otherwise attach it "
                               "as it was and choose Resume again, or abandon the relocation with Abandon... under "
                               "Unfinished operations in Jobs",
                failure.refusing->archive_uuid, failure.refusing->root);
        }
        return text.format("{}; where the new carrier is attached at another drive letter or mount point now, choose "
                           "Relocate to a new carrier... in the archive's details and give its root there, which "
                           "resumes the relocation; otherwise attach it as it was and choose Resume again, or abandon "
                           "the relocation with Abandon... under Unfinished operations in Jobs",
            text(failure.cause));
    }
    if (failure.refusing) {
        return text(refusal_in_window(*failure.refusing, RefusedIn::Archives, runs));
    }
    // Another operation recorded a later revision before it began: it is
    // started again, in the window as on the command line.
    if (failure.overtaken) {
        return text.format("{}; nothing was changed: relocate again", text(failure.cause));
    }
    if (!failure.keeps_progress) {
        return text(failure.message);
    }
    // Interrupted, or stopped by a failure: the record and the saved
    // progress stay, and Resume in the window continues it (decision 0046).
    if (failure.kind == RelocateFailure::Kind::Incomplete) {
        if (failure.not_answering) {
            return text.format("relocation was interrupted: the device of the new carrier stopped answering ({} "
                               "entries in a row could not be read); unplug it and attach it again, then continue "
                               "it with Resume under Unfinished operations in Jobs; if it stops again at the "
                               "same place, the device may be damaged",
                io_errors_that_stop);
        }
        return text("relocation was interrupted; continue it with Resume under Unfinished operations in Jobs, or "
                    "in the archive's details, without reading again what matched");
    }
    return text.format("{}; the relocation keeps its saved progress: continue it with Resume under Unfinished "
                       "operations in Jobs once that is dealt with",
        text(failure.cause));
}

std::string retirement_message_in_window(const Text& text, const RetireFailure& failure, bool runs)
{
    return failure.refusing ? text(refusal_in_window(*failure.refusing, RefusedIn::Archives, runs))
                            : text(failure.message);
}

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
