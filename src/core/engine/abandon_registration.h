#pragma once
// Abandoning an unfinished registration on purpose (decision 0025), for
// example when its media failed and it can never be finished. Its plan (the
// setting "registration:<archive_uuid>") and its saved progress (the
// checkpoints, R-104) are removed in one transaction, so it can never be
// resumed; nothing else changes: recorded revisions, their index and
// manifests, media evidence, the verification history, and the media itself
// (R-03). It is refused while an attempt of the registration still runs
// (decision 0024). An archive whose first registration is abandoned never had
// a revision: its record goes with the registration, but only while
// everything says so, never the index alone (R-14).
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace va {

// An unfinished registration that can be abandoned now, and what abandoning
// it removes.
struct AbandonableRegistration {
    Uuid archive_uuid;
    std::string label;
    std::string root;                 // UTF-8, as the registration was given it
    std::uint32_t revision = 0;       // the revision it creates
    Timestamp created;                // when its plan was made (its first attempt)
    std::uint64_t files = 0;          // its saved progress: files hashed and checkpointed so far
    std::uint64_t bytes = 0;          // and their bytes
    // The archive's first registration: its plan creates revision 1 of a new
    // archive, and the archive's record holds nothing else (it never had a
    // revision, and no revision, verification or media evidence refers to it,
    // ArchiveStore::archive_without_revision). Its record goes too, in the
    // transaction that judged this, on the same condition.
    bool first_registration = false;
    bool media_copy = false;          // an attempt wrote .veriarchive/identity.json on the media
                                      // (register --write-media-copy), which stays there (R-03)
};

struct AbandonRequest {
    Uuid archive_uuid;
    // The registration the operator was shown (the GUI's row): abandoning is
    // refused when the archive's plan is another one by now. Without them,
    // whichever unfinished registration the archive has.
    std::optional<std::uint32_t> revision;
    std::optional<Timestamp> created;
};

struct AbandonServices {
    ArchiveStore& store;
    IProcessTable& processes;
    std::string host;  // this computer (HostInfo::host)
};

struct AbandonFailure {
    enum class Kind {
        Refused,     // nothing to abandon, another registration by now, or an attempt still runs (exit 12)
        Repository,  // the database could not be read or written (exit 11)
    };
    Kind kind;
    std::string message;
};

// The archive's unfinished registration, when it can be abandoned now: what
// abandoning it would remove, for asking the operator first. Reads only.
std::expected<AbandonableRegistration, AbandonFailure> abandonable_registration(
    AbandonServices& services, const AbandonRequest& request);

// Abandons the archive's unfinished registration, after the same checks, in
// one write transaction. Returns what was removed.
std::expected<AbandonableRegistration, AbandonFailure> abandon_registration(
    AbandonServices& services, const AbandonRequest& request);

} // namespace va
