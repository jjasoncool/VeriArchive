#include "core/engine/abandon_verification.h"

#include "common/time.h"
#include "core/engine/add_chunk_hashes.h"
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

// The archive's unfinished verification that adds chunk hashes and the checks
// before abandoning it, on the database as `store` reads it now (in the
// caller's transaction).
std::expected<AbandonableVerification, AbandonFailure> examine(
    AbandonServices& services, const VerificationAbandonRequest& request, const std::string& context)
{
    auto& store = services.store;
    const auto& archive_uuid = request.archive_uuid;
    const auto adding = unfinished_adding_verification(store, archive_uuid);
    if (!adding) {
        return repository(context, adding.error());
    }
    if (!*adding) {
        return fail(AbandonFailure::Kind::Refused,
            "archive " + archive_uuid + " has no unfinished verification that adds chunk hashes");
    }
    const auto& verification = **adding;

    // The operator decided on the run shown; by now the archive may have
    // another (it was resumed, under a run of its own).
    if (request.run_id && *request.run_id != verification.run_id) {
        return fail(AbandonFailure::Kind::Refused,
            "the unfinished verification of archive " + archive_uuid +
                " that adds chunk hashes is no longer the one shown (it was resumed, finished or abandoned "
                "meanwhile); nothing was changed");
    }

    // Never under an attempt that still runs (decision 0024): it would go on
    // writing checkpoints, and record a revision. A run recorded INCOMPLETE
    // has stopped, whether its process still runs or not (the GUI's goes on
    // after a cancel).
    const auto recorded = store.verify_run(verification.run_id);
    if (!recorded) {
        return repository(context, recorded.error());
    }
    if (!*recorded || (*recorded)->result != "INCOMPLETE") {
        const auto judged = judge_attempt(verification.record, services.processes, services.host);
        if (judged.state == AttemptState::Running) {
            const auto& process = judged.attempt->process;
            return fail(AbandonFailure::Kind::Refused,
                "the verification of archive " + archive_uuid + " that adds chunk hashes is still running in process " +
                    std::to_string(process.id) + " (" + (process.program.empty() ? std::string("?") : process.program) +
                    ") on this computer; abandon it once that process has ended");
        }
        if (judged.state == AttemptState::Unknown) {
            // No process this computer can check (another computer's, or one
            // that could not be read): an attempt that said it works a moment
            // ago may still run, as the Archive List shows it (decision 0022).
            const auto found = verification.record.find("updated");
            const auto updated = found != verification.record.end() && found->is_string()
                                     ? parse_timestamp(found->get<std::string>())
                                     : std::nullopt;
            const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            if (updated && now - *updated <= attempt_silent_after) {
                return fail(AbandonFailure::Kind::Refused,
                    "an attempt of the verification of archive " + archive_uuid +
                        " that adds chunk hashes said it was working less than 3 minutes ago; abandon it once it has "
                        "stopped");
            }
        }
    }

    AbandonableVerification abandonable;
    abandonable.archive_uuid = archive_uuid;
    abandonable.root = verification.root;
    abandonable.revision = verification.revision;
    abandonable.run_id = verification.run_id;
    abandonable.started = verification.started;
    // Its saved progress: the entries checkpointed under its run.
    const auto saved = store.progress_count(verification.run_id, archive_uuid, verification.revision);
    if (!saved) {
        return repository(context, saved.error());
    }
    abandonable.files = saved->files;
    abandonable.bytes = saved->bytes;
    if (const auto archive = store.find_archive(archive_uuid); archive && *archive) {
        abandonable.label = (*archive)->label;
    }
    return abandonable;
}

} // namespace

std::expected<AbandonableVerification, AbandonFailure> abandonable_verification(
    AbandonServices& services, const VerificationAbandonRequest& request)
{
    std::expected<AbandonableVerification, AbandonFailure> examined =
        fail(AbandonFailure::Kind::Repository, "cannot read the unfinished verification");
    const auto read = services.store.read_transaction([&]() -> std::expected<void, IoError> {
        examined = examine(services, request, "cannot read the unfinished verification: ");
        return {};
    });
    if (!read) {
        return repository("cannot read the unfinished verification: ", read.error());
    }
    return examined;
}

std::expected<AbandonableVerification, AbandonFailure> abandon_verification(
    AbandonServices& services, const VerificationAbandonRequest& request)
{
    auto& store = services.store;
    const std::string context = "cannot abandon the verification: ";
    std::optional<AbandonFailure> stopped; // a refusal, or a failure the checks reported
    AbandonableVerification removed;
    const auto written = store.write_transaction([&]() -> std::expected<void, IoError> {
        auto examined = examine(services, request, context);
        if (!examined) {
            stopped = examined.error();
            return std::unexpected(IoError{IoErrorKind::Other, 0, examined.error().message});
        }
        removed = std::move(*examined);
        // The record, the checkpoints and the chunk hashes kept with them;
        // nothing else.
        if (auto gone = store.remove_setting("run:" + removed.run_id); !gone) {
            return gone;
        }
        return store.clear_progress(removed.run_id);
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
