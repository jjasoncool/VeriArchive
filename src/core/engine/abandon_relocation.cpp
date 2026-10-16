#include "core/engine/abandon_relocation.h"

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

// Members of the record read without exceptions: another type (a damaged
// record) reads as missing.
std::string text_member(const nlohmann::json& record, const char* name)
{
    const auto found = record.find(name);
    return found != record.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// The archive's unfinished relocation and the checks before abandoning it, on
// the database as `store` reads it now (in the caller's transaction).
std::expected<AbandonableRelocation, AbandonFailure> examine(
    AbandonServices& services, const RelocationAbandonRequest& request, const std::string& context)
{
    auto& store = services.store;
    const auto& archive_uuid = request.archive_uuid;
    const auto stored = store.setting(relocation_key(archive_uuid));
    if (!stored) {
        return repository(context, stored.error());
    }
    const auto record = nlohmann::json::parse(*stored, nullptr, false);
    const auto revision = record.is_object() ? record.find("revision") : record.end();
    if (stored->empty() || !record.is_object() || revision == record.end() || !revision->is_number_unsigned() ||
        text_member(record, "run_id").empty()) {
        return fail(AbandonFailure::Kind::Refused, "archive " + archive_uuid + " has no unfinished relocation");
    }

    AbandonableRelocation relocation;
    relocation.archive_uuid = archive_uuid;
    relocation.label = text_member(record, "label");
    relocation.root = text_member(record, "root");
    relocation.revision = revision->get<std::uint32_t>();
    relocation.run_id = text_member(record, "run_id");
    relocation.started = text_member(record, "started");

    // The operator decided on the relocation shown; by now the archive may
    // have another (it was resumed and recorded, and the next one started).
    if (request.run_id && *request.run_id != relocation.run_id) {
        return fail(AbandonFailure::Kind::Refused,
            "the unfinished relocation of archive " + archive_uuid +
                " is no longer the one shown (it was resumed, finished or abandoned meanwhile); nothing was changed");
    }

    // Never under an attempt that still runs (decision 0024): it would go on
    // writing checkpoints, and record a revision.
    const auto judged = judge_attempt(record, services.processes, services.host);
    if (judged.state == AttemptState::Running) {
        const auto& process = judged.attempt->process;
        return fail(AbandonFailure::Kind::Refused,
            "the relocation of archive " + archive_uuid + " is still running in process " +
                std::to_string(process.id) + " (" + (process.program.empty() ? std::string("?") : process.program) +
                ") on this computer; abandon it once that process has ended");
    }
    if (judged.state == AttemptState::Unknown) {
        // No process this computer can check (another computer's, or one
        // that could not be read): an attempt that said it works a moment
        // ago may still run, as the Archive List shows it (decision 0022).
        const auto updated = parse_timestamp(text_member(record, "updated"));
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        if (updated && now - *updated <= attempt_silent_after) {
            return fail(AbandonFailure::Kind::Refused,
                "an attempt of the relocation of archive " + archive_uuid +
                    " said it was working less than 3 minutes ago; abandon it once it has stopped");
        }
    }

    // Its saved progress: the entries checkpointed under its run.
    const auto saved = store.progress_count(relocation.run_id, archive_uuid, relocation.revision);
    if (!saved) {
        return repository(context, saved.error());
    }
    relocation.files = saved->files;
    relocation.bytes = saved->bytes;
    if (relocation.label.empty()) {
        if (const auto archive = store.find_archive(archive_uuid); archive && *archive) {
            relocation.label = (*archive)->label;
        }
    }
    return relocation;
}

} // namespace

std::expected<AbandonableRelocation, AbandonFailure> abandonable_relocation(
    AbandonServices& services, const RelocationAbandonRequest& request)
{
    std::expected<AbandonableRelocation, AbandonFailure> examined =
        fail(AbandonFailure::Kind::Repository, "cannot read the unfinished relocation");
    const auto read = services.store.read_transaction([&]() -> std::expected<void, IoError> {
        examined = examine(services, request, "cannot read the unfinished relocation: ");
        return {};
    });
    if (!read) {
        return repository("cannot read the unfinished relocation: ", read.error());
    }
    return examined;
}

std::expected<AbandonableRelocation, AbandonFailure> abandon_relocation(
    AbandonServices& services, const RelocationAbandonRequest& request)
{
    auto& store = services.store;
    const std::string context = "cannot abandon the relocation: ";
    std::optional<AbandonFailure> stopped; // a refusal, or a failure the checks reported
    AbandonableRelocation removed;
    const auto written = store.write_transaction([&]() -> std::expected<void, IoError> {
        auto examined = examine(services, request, context);
        if (!examined) {
            stopped = examined.error();
            return std::unexpected(IoError{IoErrorKind::Other, 0, examined.error().message});
        }
        removed = std::move(*examined);
        // The record and the checkpoints; nothing else.
        return store.remove_relocation(removed.archive_uuid, removed.run_id);
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
