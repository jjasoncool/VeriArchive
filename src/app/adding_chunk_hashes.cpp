#include "app/adding_chunk_hashes.h"

#include "app/chunk_size.h"
#include "core/engine/add_chunk_hashes.h"

#include <utility>

namespace va::app {

namespace {

nlohmann::json optional_text(const std::string& value)
{
    return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
}

} // namespace

std::string chunk_hashes_line(const Text& text, const VerifyOutcome& outcome, bool sentence)
{
    if (outcome.added_revision > 0) {
        const auto chunk_size = chunk_size_text(outcome.adding_chunk_size);
        // Two calls: the compiler checks each literal template against its values.
        return sentence ? text.format("Chunk hashes added: revision {} records them (chunk size {}, {} chunks, "
                                      "Merkle root {})",
                              outcome.added_revision, chunk_size, outcome.added_chunks, outcome.added_merkle_root)
                        : text.format("chunk hashes added: revision {} records them (chunk size {}, {} chunks, "
                                      "Merkle root {})",
                              outcome.added_revision, chunk_size, outcome.added_chunks, outcome.added_merkle_root);
    }
    if (!outcome.not_added.empty()) {
        const auto why = text(outcome.not_added);
        return sentence ? text.format("Chunk hashes not added: {}", why)
                        : text.format("chunk hashes not added: {}", why);
    }
    return {};
}

nlohmann::json chunk_hashes_member(const VerifyOutcome& outcome)
{
    if (outcome.adding_chunk_size == 0) {
        return nlohmann::json::object();
    }
    const bool added = outcome.added_revision > 0;
    return {{"chunk_hashes",
        {
            {"chunk_size", outcome.adding_chunk_size},
            {"added_revision", added ? nlohmann::json(outcome.added_revision) : nlohmann::json(nullptr)},
            {"chunks", added ? nlohmann::json(outcome.added_chunks) : nlohmann::json(nullptr)},
            {"merkle_root", optional_text(outcome.added_merkle_root)},
            {"not_added", optional_text(outcome.not_added)},
        }}};
}

nlohmann::json unfinished_verification_member(const UnfinishedVerification& verification,
    const UnfinishedOperation& operation, std::chrono::sys_seconds wall_now)
{
    nlohmann::json json{
        {"root", verification.root},
        {"revision", verification.revision},
        {"run_id", verification.run_id},
        {"chunk_size", verification.adds_chunk_hashes},
        {"started", verification.started},
        {"updated", optional_text(verification.updated)},
        {"state", state_code(state_of_one_read(operation, wall_now))},
        {"resumable", verification.resumable},
        {"files_done", verification.done_counted ? nlohmann::json(verification.files_done) : nullptr},
        {"bytes_done", verification.done_counted ? nlohmann::json(verification.bytes_done) : nullptr},
        {"files_total", verification.files_total},
        {"bytes_total", verification.bytes_total},
    };
    if (operation.process) {
        json["process"] = {{"id", operation.process->process.id}, {"program", operation.process->process.program},
            {"host", operation.process->host}};
    }
    return {{"unfinished_verification", std::move(json)}};
}

nlohmann::json abandonable_verification_json(const AbandonableVerification& verification)
{
    return {
        {"archive_uuid", verification.archive_uuid},
        {"label", verification.label},
        {"revision", verification.revision},
        {"root", verification.root},
        {"run_id", verification.run_id},
        {"started", verification.started},
        {"saved_progress", {{"files", verification.files}, {"bytes", verification.bytes}}},
    };
}

nlohmann::json abandonable_verification_member(const AbandonableVerification& verification)
{
    return {{"verification", abandonable_verification_json(verification)}};
}

std::string stop_message_in_window(const VerifyStop& stop)
{
    if (!stop.adding) {
        return stop.message;
    }
    const auto& adding = *stop.adding;
    return "an unfinished verification of archive " + adding.archive_uuid + " that adds chunk hashes (run " +
           adding.run_id + ") is creating revision " + std::to_string(adding.revision + 1) +
           "; nothing was changed (finish it first with Resume interrupted run once the archive is chosen above, or "
           "abandon it with Abandon... in the Archive List)";
}

std::expected<std::optional<RunToResume>, IoError> resumable_adding_verification(
    ArchiveStore& store, const Uuid& archive_uuid, const AttemptCheck& attempts, std::chrono::sys_seconds wall_now)
{
    std::optional<AddingVerification> adding;
    bool recorded_incomplete = false;
    const auto read = store.read_transaction([&]() -> std::expected<void, IoError> {
        auto found = unfinished_adding_verification(store, archive_uuid);
        if (!found) {
            return std::unexpected(found.error());
        }
        adding = std::move(*found);
        if (!adding) {
            return {};
        }
        const auto recorded = store.verify_run(adding->run_id);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
        recorded_incomplete = *recorded && (*recorded)->result == "INCOMPLETE";
        return {};
    });
    if (!read) {
        return std::unexpected(read.error());
    }
    if (!adding) {
        return std::optional<RunToResume>{};
    }
    // Judged as the Archive List judges its row, from what its record and
    // the history say; its progress plays no part.
    UnfinishedVerification verification;
    verification.run_id = adding->run_id;
    verification.archive_uuid = archive_uuid;
    verification.root = adding->root;
    verification.revision = adding->revision;
    verification.started = adding->started;
    verification.recorded_incomplete = recorded_incomplete;
    const auto updated = adding->record.find("updated");
    if (updated != adding->record.end() && updated->is_string()) {
        verification.updated = updated->get<std::string>();
    }
    verification.adds_chunk_hashes = adding->chunk_size;
    verification.record = adding->record.dump();
    if (!has_stopped(state_of_one_read(verification_operation(verification, attempts), wall_now))) {
        return std::optional<RunToResume>{};
    }
    return std::optional<RunToResume>(RunToResume{adding->run_id, std::nullopt, archive_uuid});
}

std::optional<RunToResume> run_to_resume(
    const std::optional<RunToResume>& interrupted_here, const std::optional<RunToResume>& chosen)
{
    if (chosen && !(interrupted_here && interrupted_here->archive_uuid == chosen->archive_uuid)) {
        return chosen;
    }
    return interrupted_here;
}

} // namespace va::app
