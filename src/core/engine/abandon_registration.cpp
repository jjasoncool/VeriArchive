#include "core/engine/abandon_registration.h"

#include "common/time.h"
#include "core/engine/registration_attempt.h"

#include <chrono>
#include <nlohmann/json.hpp>

namespace va {
namespace {

std::unexpected<AbandonFailure> fail(AbandonFailure::Kind kind, std::string message)
{
    return std::unexpected(AbandonFailure{kind, std::move(message)});
}

std::unexpected<AbandonFailure> repository(const std::string& context, const IoError& error)
{
    return fail(AbandonFailure::Kind::Repository, context + error.message);
}

// Members of the plan read without exceptions: another type (a damaged plan)
// reads as missing.
std::string text_member(const nlohmann::json& plan, const char* name)
{
    const auto found = plan.find(name);
    return found != plan.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// The archive's unfinished registration and the checks before abandoning it,
// on the database as `store` reads it now (in the caller's transaction).
std::expected<AbandonableRegistration, AbandonFailure> examine(
    AbandonServices& services, const AbandonRequest& request, const std::string& context)
{
    auto& store = services.store;
    const auto& archive_uuid = request.archive_uuid;
    const auto stored = store.setting("registration:" + archive_uuid);
    if (!stored) {
        return repository(context, stored.error());
    }
    const auto plan = nlohmann::json::parse(*stored, nullptr, false);
    const auto revision = plan.is_object() ? plan.find("revision") : plan.end();
    if (stored->empty() || !plan.is_object() || revision == plan.end() || !revision->is_number_unsigned()) {
        return fail(AbandonFailure::Kind::Refused, "archive " + archive_uuid + " has no unfinished registration");
    }

    AbandonableRegistration registration;
    registration.archive_uuid = archive_uuid;
    registration.label = text_member(plan, "label");
    registration.root = text_member(plan, "root");
    registration.revision = revision->get<std::uint32_t>();
    registration.created = text_member(plan, "created");
    const auto media_copy = plan.find("media_copy");
    registration.media_copy = media_copy != plan.end() && media_copy->is_boolean() && media_copy->get<bool>();

    // The operator decided on the registration shown; by now the archive may
    // have another (it was resumed and recorded, and the next one started).
    if ((request.revision && *request.revision != registration.revision) ||
        (request.created && *request.created != registration.created)) {
        return fail(AbandonFailure::Kind::Refused,
            "the unfinished registration of archive " + archive_uuid +
                " is no longer the one shown (it was resumed, finished or abandoned meanwhile); nothing was changed");
    }

    // Never under an attempt that still runs (decision 0024): it would go on
    // writing checkpoints, and record a revision of an archive without a record.
    const auto judged = judge_attempt(plan, services.processes, services.host);
    if (judged.state == AttemptState::Running) {
        const auto& process = judged.attempt->process;
        return fail(AbandonFailure::Kind::Refused,
            "the registration of archive " + archive_uuid + " is still running in process " +
                std::to_string(process.id) + " (" + (process.program.empty() ? std::string("?") : process.program) +
                ") on this computer; abandon it once that process has ended");
    }
    if (judged.state == AttemptState::Unknown) {
        // No process this computer can check (an older version's plan, or
        // another computer's): an attempt that said it works a moment ago may
        // still run, as the Archive List shows it (decision 0022).
        const auto updated = parse_timestamp(text_member(plan, "updated"));
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        if (updated && now - *updated <= attempt_silent_after) {
            return fail(AbandonFailure::Kind::Refused,
                "an attempt of the registration of archive " + archive_uuid +
                    " said it was working less than 3 minutes ago; abandon it once it has stopped");
        }
    }

    // Its saved progress: the rows under the revision it creates. Once that
    // revision is recorded (by an operation that left the plan, decision
    // 0023), those rows are the recorded manifest's index, not progress.
    const auto recorded = store.revision_recorded(archive_uuid, registration.revision);
    if (!recorded) {
        return repository(context, recorded.error());
    }
    if (!*recorded) {
        const auto saved = store.file_row_count(archive_uuid, registration.revision);
        if (!saved) {
            return repository(context, saved.error());
        }
        registration.files = saved->files;
        registration.bytes = saved->bytes;
    }
    // A first registration: its plan creates revision 1 of a new archive,
    // and the archive's record holds nothing else. Never judged from the
    // index alone (R-14): an archive whose index lost its revisions (a
    // damaged database) still has its record, its history and its evidence,
    // and its registration of a new revision keeps them.
    const auto new_archive = plan.find("new_archive");
    if (registration.revision == 1 && new_archive != plan.end() && new_archive->is_boolean() &&
        new_archive->get<bool>()) {
        const auto without_revision = store.archive_without_revision(archive_uuid);
        if (!without_revision) {
            return repository(context, without_revision.error());
        }
        registration.first_registration = *without_revision;
    }
    if (registration.label.empty()) {
        if (const auto archive = store.find_archive(archive_uuid); archive && *archive) {
            registration.label = (*archive)->label;
        }
    }
    return registration;
}

} // namespace

std::expected<AbandonableRegistration, AbandonFailure> abandonable_registration(
    AbandonServices& services, const AbandonRequest& request)
{
    std::expected<AbandonableRegistration, AbandonFailure> examined =
        fail(AbandonFailure::Kind::Repository, "cannot read the unfinished registration");
    const auto read = services.store.read_transaction([&]() -> std::expected<void, IoError> {
        examined = examine(services, request, "cannot read the unfinished registration: ");
        return {};
    });
    if (!read) {
        return repository("cannot read the unfinished registration: ", read.error());
    }
    return examined;
}

std::expected<AbandonableRegistration, AbandonFailure> abandon_registration(
    AbandonServices& services, const AbandonRequest& request)
{
    auto& store = services.store;
    const std::string context = "cannot abandon the registration: ";
    std::optional<AbandonFailure> stopped; // a refusal, or a failure the checks reported
    AbandonableRegistration removed;
    const auto written = store.write_transaction([&]() -> std::expected<void, IoError> {
        auto examined = examine(services, request, context);
        if (!examined) {
            stopped = examined.error();
            return std::unexpected(IoError{IoErrorKind::Other, 0, examined.error().message});
        }
        removed = std::move(*examined);
        const auto& archive_uuid = removed.archive_uuid;
        const auto recorded = store.revision_recorded(archive_uuid, removed.revision);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
        // The saved progress, never a recorded revision's index (see examine).
        if (!*recorded) {
            if (auto deleted = store.delete_file_rows(archive_uuid, removed.revision); !deleted) {
                return deleted;
            }
        }
        if (auto plan = store.remove_setting("registration:" + archive_uuid); !plan) {
            return plan;
        }
        // An archive that never had a revision has nothing else: no manifest
        // to verify or compare, no history. Its record goes with its first
        // registration (decision 0025); its label is not given out again.
        // The store checks the condition that examine read once more, in the
        // statement that removes it, within this transaction.
        if (removed.first_registration) {
            if (auto archive = store.remove_archive_without_revision(archive_uuid); !archive) {
                return std::unexpected(archive.error());
            }
        }
        return {};
    });
    if (stopped) {
        return std::unexpected(*stopped);
    }
    if (!written) {
        return repository(context, written.error());
    }
    return removed;
}

} // namespace va
