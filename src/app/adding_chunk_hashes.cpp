#include "app/adding_chunk_hashes.h"

#include "core/engine/add_chunk_hashes.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace va::app {

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
