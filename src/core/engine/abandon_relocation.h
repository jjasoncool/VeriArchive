#pragma once
// Abandoning an unfinished relocation on purpose (decision 0026), for example
// when the new carrier failed, or the operator copies the archive elsewhere.
// Its record (the setting relocation_key) and its saved progress (the
// checkpoints in verify_progress, R-104) are removed in one transaction, so it
// can never be resumed; nothing else changes: the archive, its revisions,
// their index and manifests, media evidence, the verification history, and
// the media itself (R-03). A relocation never removes an archive: the archive
// it moves has a revision. It is refused while an attempt of the relocation
// still runs (decision 0024).
#include "core/engine/abandon_registration.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace va {

// An unfinished relocation that can be abandoned now, and what abandoning it
// removes.
struct AbandonableRelocation {
    Uuid archive_uuid;
    std::string label;
    std::string root;                 // UTF-8: the new root it reads
    std::uint32_t revision = 0;       // the revision it verifies against; it creates the next one
    std::string run_id;               // its checkpoints (verify_progress)
    Timestamp started;                // when its first attempt started
    std::uint64_t files = 0;          // its saved progress: files (and streams) matched so far
    std::uint64_t bytes = 0;          // and their bytes
};

struct RelocationAbandonRequest {
    Uuid archive_uuid;
    // The relocation the operator was shown (the GUI's row): abandoning is
    // refused when the archive's record is another relocation's by now.
    // Without it, whichever unfinished relocation the archive has.
    std::optional<std::string> run_id;
};

// The archive's unfinished relocation, when it can be abandoned now: what
// abandoning it would remove, for asking the operator first. Reads only.
std::expected<AbandonableRelocation, AbandonFailure> abandonable_relocation(
    AbandonServices& services, const RelocationAbandonRequest& request);

// Abandons the archive's unfinished relocation, after the same checks, in one
// write transaction. Returns what was removed.
std::expected<AbandonableRelocation, AbandonFailure> abandon_relocation(
    AbandonServices& services, const RelocationAbandonRequest& request);

} // namespace va
