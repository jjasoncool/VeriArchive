#pragma once
// FULL verification (HANDOFF §B11, M3): identify the media, validate the
// manifest against the repository record, re-read and re-hash every entry, and
// record a hash-chained result. Only "FULL VERIFY PASSED" means every registered
// byte was read and matched (§A6).
//
// Adding chunk hashes (decision 0033): a FULL verify of an archive whose
// current revision has no chunk size, as V1 registered it, can also hash the
// chunks of its files in the same read, and records the next revision with
// the same entries and their chunk hashes when every entry matched.
//
// A changed enclosure (decision 0047): a FULL verify of an archive whose
// filesystem is seen behind another enclosure (MIGRATED, enclosure_changed)
// goes on only with a carrier confirmation, records the confirmation in its
// run whatever its result, and, when it passed with every entry matched as a
// relocation requires, records the next revision with the new carrier, as a
// relocation does (decision 0004).
#include "core/engine/add_chunk_hashes.h"
#include "core/engine/candidates.h"
#include "core/engine/carrier_confirmation.h"
#include "core/engine/device_alerts.h"
#include "core/engine/read_speed.h"
#include "core/identity/matcher.h"
#include "core/interfaces.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"
#include "core/smart/smart_data.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace va {

struct VerifyRequest {
    std::optional<std::filesystem::path> root;  // verify the archive at this path, or
    std::optional<Uuid> archive_uuid;           // find this archive on the attached volumes
    bool strict_metadata = false;               // R-51: metadata differences fail the run
    std::optional<bool> rotational;             // --io hdd|ssd
    std::map<std::string, bool> rotational_override = {}; // io.rotational_override of the config, by serial (R-111)
    std::optional<std::string> resume_run;      // R-104: continue an INCOMPLETE run
    // Decision 0033: also add chunk hashes, to an archive whose current
    // revision has none (chunk size 0). A resumed run adds them as the run it
    // continues did, at its chunk size, also without this; with it, a run
    // that adds none is not resumed.
    bool add_chunk_hashes = false;
    // The chunk size at which they are added: 64 MiB, as a registration's
    // (decision 0032); tests use 64 KiB.
    std::uint64_t chunk_size = default_chunk_size;
    // Decision 0044: told of the devices' health before the archive is read.
    DevicesObserved devices_observed = {};
    // The clock that times the read of each hundredth of the content
    // (decision 0044); unset, the steady clock. Tests give their own.
    ReadSpeedMeter::Clock read_clock = {};
    // Decision 0047: the confirmation that the carrier found behind another
    // enclosure holds the archive's device. Used only by a verification of
    // the archive named by `archive_uuid` (a root alone matches every archive,
    // and a confirmation names enclosures, not an archive), only where
    // identity matching gives MIGRATED and enclosure_changed holds, for the
    // enclosures it names (as recorded, as seen now); there, without it, the
    // verification stops as every MIGRATED does (exit 12, naming relocation).
    // Ignored where the carrier matches as recorded. A resumed run goes on
    // with the confirmation its record carries, and with no other. Never
    // together with add_chunk_hashes.
    std::optional<CarrierConfirmation> carrier_confirmation = std::nullopt;
    // Decision 0052: the SMART reads that the window made as the operator
    // started this verification (Settings: read SMART when a FULL
    // verification starts); those of the devices of the volume it reads are
    // recorded with the run (VerifyOutcome::smart). A resumed run records
    // those its record carries, and no other.
    std::vector<SmartRead> smart_reads = {};
};

struct VerifyServices {
    IFileSystemAccess& archive_files;
    IFileSystemAccess& repository_files;
    IMediaEnumerator& media;
    ArchiveStore& store;
    RepositoryLayout layout;
    std::string repository_uuid;
    HostInfo host;
    IFileWriter& writer;                        // the manifest of a revision with chunk hashes added
    IProcessTable& processes;                   // which process runs a verification that adds chunk hashes
};

// A finished (or interrupted) run, already recorded in the history chain.
struct VerifyOutcome {
    std::string run_id;
    std::string resumed_from;                   // R-104: the INCOMPLETE run this one continues
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 0;
    std::filesystem::path root;
    std::string result;                         // PASSED | PASSED_WITH_WARNINGS | FAILED | INCOMPLETE
    MatchOutcome identity;
    std::vector<std::string> failure_classes;   // §A6 run-level failure classes
    std::vector<std::string> warnings;
    // The devices of the volume read, with the health each reported when the
    // run started (decision 0044): a device alert is told beside the result
    // and never changes it. Empty where the run read no media.
    std::vector<PhysicalDeviceInfo> devices;
    // Decision 0044, once the verification reached a result: how fast it
    // read the content, with the runs it resumed, and how that compares with
    // the last FULL verification of the archive on this device whose speed
    // was as usual; reading markedly slower is a device alert. Nothing where
    // it read nothing, or the history holds nothing to compare with.
    std::optional<ReadSpeed> read_speed;
    std::optional<SpeedComparison> read_compared;
    // Informational, never part of the result (decision 0006): "driver:
    // ntfs-3g" where a FUSE program read the media instead of the kernel's
    // driver of its format.
    std::vector<std::string> notes;
    std::map<std::string, std::uint64_t> counts; // item status -> number of entries
    std::vector<VerifyItemRecord> items;        // every non-Verified item (D7)
    std::uint64_t coverage_bytes = 0;           // bytes read and hashed in this run
    std::uint64_t registered_bytes = 0;
    // False when the run FAILED before the media was looked for, as the
    // manifest of the archive's revision cannot be used (decision 0028):
    // nothing was read, no media evidence recorded, and `root` and
    // `identity` say nothing.
    bool media_read = true;
    // Decision 0033: the chunk size at which this run adds chunk hashes (0:
    // it adds none). Once it has added them, the revision that records them
    // (then also `revision`), with their number and its Merkle root; when it
    // finished without, why (an engine sentence). Neither while INCOMPLETE.
    std::uint64_t adding_chunk_size = 0;
    std::uint32_t added_revision = 0;
    std::uint64_t added_chunks = 0;
    std::string added_merkle_root;
    std::string not_added;
    // Decision 0047: the confirmation this run went on with past a changed
    // enclosure, as its counts record it (also of an INCOMPLETE or FAILED
    // run). Once the run has recorded the new carrier, the revision that
    // records it (then also `revision`); when it finished without (FAILED, or
    // passed and could not record it), why: the cause alone, an engine
    // sentence, as `not_added` holds one. Neither while INCOMPLETE.
    std::optional<RecordedConfirmation> carrier_confirmed;
    std::uint32_t carrier_revision = 0;
    std::string carrier_not_recorded;
    // Decision 0052: the SMART reads of the volume's devices that its counts
    // record (`smart`, smart_reads_json), hash-chained with it whatever its
    // result; none where none was read as it started.
    std::vector<SmartRead> smart;
};

// Verification could not start: the media needs an operator decision
// (MIGRATED, AMBIGUOUS, UNKNOWN, previous carrier) or something is missing,
// or it was refused (Refused), such as adding chunk hashes to an archive that
// has them (decision 0033). An archive named by its uuid, or by the run of it
// to resume, is known before the media is looked for: when the manifest of
// its current revision cannot be used, the verification does not stop but
// FAILS, and is recorded (decision 0028).
struct VerifyStop {
    enum class Kind { Decision, NotFound, Repository, Refused };
    Kind kind;
    std::string message;
    MatchOutcome identity;
    // An archive named by its uuid that more than one attached volume holds
    // (decision 0022, point 7): the archive's root on each, for the user
    // interfaces to word what to do. Empty for every other stop.
    std::vector<std::filesystem::path> roots_found = {};
    // Refused while another operation that creates the archive's next
    // revision is unfinished (decision 0033): a registration, a relocation,
    // or a verification that adds chunk hashes. `message` names the commands
    // that finish or abandon it, as the command line says them; the GUI names
    // its own ways (app::stop_message_in_window).
    std::optional<RefusingOperation> refusing = std::nullopt;
};

std::expected<VerifyOutcome, VerifyStop> verify_full(
    VerifyServices& services,
    const VerifyRequest& request,
    CancelToken& cancel,
    IProgressSink& progress);

} // namespace va
