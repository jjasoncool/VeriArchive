#pragma once
// Abandoning an unfinished FULL verify on purpose: one that adds chunk hashes
// (decision 0033), as when its media failed, since while it is unfinished no
// registration or relocation of the archive begins (one operation creating an
// archive's next revision at a time); and, named by its run, one that adds
// none and has stopped (decision 0050), which a later verification of the
// archive would supersede (decision 0022), so that it no longer waits to be
// resumed where none will be. Its record (the setting "run:<run_id>") and its
// saved progress (its checkpoints in verify_progress and the chunk hashes kept
// with them) are removed in one transaction, so it can never be resumed;
// nothing else changes: the archive, its revisions, their index and
// manifests, media evidence, the verification history (its INCOMPLETE runs
// included), and the media itself (R-03). It is refused while an attempt of it
// still runs (decision 0024).
#include "core/engine/abandon_registration.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace va {

// An unfinished verification that can be abandoned now, and what abandoning
// it removes.
struct AbandonableVerification {
    Uuid archive_uuid;
    std::string label;
    std::string root;                 // UTF-8: where it reads
    std::uint32_t revision = 0;       // the revision it verifies; it records the next one
    std::string run_id;               // its record and checkpoints
    Timestamp started;                // when its attempt started
    std::uint64_t files = 0;          // its saved progress: files (and streams) matched so far
    std::uint64_t bytes = 0;          // and their bytes
    bool adds_chunk_hashes = true;    // false: a verification that adds none (decision 0050)
};

struct VerificationAbandonRequest {
    Uuid archive_uuid;
    // The run the operator was shown (the GUI's row), whether it adds chunk
    // hashes or not: abandoning is refused when it is no longer unfinished,
    // or, for one that adds them, when the archive's unfinished verification
    // that adds them is another by now. Without it, the archive's unfinished
    // verification that adds chunk hashes (`archive abandon-verification`).
    std::optional<std::string> run_id;
};

// The verification the request names, when it can be abandoned now: what
// abandoning it would remove, for asking the operator first. Reads only.
std::expected<AbandonableVerification, AbandonFailure> abandonable_verification(
    AbandonServices& services, const VerificationAbandonRequest& request);

// Abandons it, after the same checks, in one write transaction. Returns what
// was removed.
std::expected<AbandonableVerification, AbandonFailure> abandon_verification(
    AbandonServices& services, const VerificationAbandonRequest& request);

} // namespace va
