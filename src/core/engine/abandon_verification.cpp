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

// The record of the run that the request names, where it is an unfinished
// verification of the archive that adds no chunk hashes (decision 0050), read
// into the fields that a verification that adds them has too; nothing where
// it is one that adds them, which the archive's unfinished verification that
// adds them stands for, and nothing where the record is gone while the archive
// has such a verification, which says that the one shown is no longer it.
std::expected<std::optional<AddingVerification>, AbandonFailure> plain_verification(
    ArchiveStore& store, const VerificationAbandonRequest& request, const std::string& context)
{
    const auto value = store.setting("run:" + *request.run_id);
    if (!value) {
        return repository(context, value.error());
    }
    auto record = nlohmann::json::parse(*value, nullptr, false);
    const auto member = [&](const char* name) {
        const auto found = record.is_object() ? record.find(name) : record.end();
        return found != record.end() && found->is_string() ? found->get<std::string>() : std::string{};
    };
    if (value->empty() || !record.is_object() || member("archive_uuid") != request.archive_uuid) {
        const auto adding = unfinished_adding_verification(store, request.archive_uuid);
        if (!adding) {
            return repository(context, adding.error());
        }
        if (*adding) {
            return std::optional<AddingVerification>{};
        }
        return fail(AbandonFailure::Kind::Refused,
            "the stopped verification of archive " + request.archive_uuid +
                " is no longer the one shown (it was resumed, finished or abandoned meanwhile); nothing was changed");
    }
    if (adding_chunk_size(record) != 0) {
        return std::optional<AddingVerification>{};
    }
    AddingVerification verification;
    verification.run_id = *request.run_id;
    verification.archive_uuid = request.archive_uuid;
    const auto revision = record.find("revision");
    verification.revision =
        revision != record.end() && revision->is_number_unsigned() ? revision->get<std::uint32_t>() : 0;
    verification.root = member("root");
    verification.started = member("started");
    verification.record = std::move(record);
    return std::optional<AddingVerification>(std::move(verification));
}

// The verification to abandon and the checks before abandoning it, on the
// database as `store` reads it now (in the caller's transaction).
std::expected<AbandonableVerification, AbandonFailure> examine(
    AbandonServices& services, const VerificationAbandonRequest& request, const std::string& context)
{
    auto& store = services.store;
    const auto& archive_uuid = request.archive_uuid;
    // A verification that adds no chunk hashes, named by its run (decision
    // 0050).
    std::optional<AddingVerification> plain;
    if (request.run_id) {
        auto found = plain_verification(store, request, context);
        if (!found) {
            return std::unexpected(found.error());
        }
        plain = std::move(*found);
    }
    std::optional<AddingVerification> adding;
    if (!plain) {
        auto found = unfinished_adding_verification(store, archive_uuid);
        if (!found) {
            return repository(context, found.error());
        }
        if (!*found) {
            return fail(AbandonFailure::Kind::Refused,
                "archive " + archive_uuid + " has no unfinished verification that adds chunk hashes");
        }
        adding = std::move(*found);
        // The operator decided on the run shown; by now the archive may have
        // another (it was resumed, under a run of its own).
        if (request.run_id && *request.run_id != adding->run_id) {
            return fail(AbandonFailure::Kind::Refused,
                "the unfinished verification of archive " + archive_uuid +
                    " that adds chunk hashes is no longer the one shown (it was resumed, finished or abandoned "
                    "meanwhile); nothing was changed");
        }
    }
    const auto& verification = plain ? *plain : *adding;
    const std::string what = plain ? "" : " that adds chunk hashes";

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
                "the verification of archive " + archive_uuid + what + " is still running in process " +
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
                    "an attempt of the verification of archive " + archive_uuid + what +
                        " said it was working less than 3 minutes ago; abandon it once it has stopped");
            }
        }
    }

    AbandonableVerification abandonable;
    abandonable.archive_uuid = archive_uuid;
    abandonable.root = verification.root;
    abandonable.revision = verification.revision;
    abandonable.run_id = verification.run_id;
    abandonable.started = verification.started;
    abandonable.adds_chunk_hashes = !plain;
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
