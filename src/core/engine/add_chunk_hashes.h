#pragma once
// Adding chunk hashes (decision 0033): a FULL verify of an archive whose
// current revision rN has no chunk size, as V1 registered it, that also hashes
// the chunks of its files in the read that hashes each file whole (R-101), and
// that records the next revision r(N+1) when every entry matched: rN's entry
// lines as they are, followed by the chunk lines of every file larger than the
// chunk size, with the Merkle root of §B6. Otherwise it records the
// verification alone, as any FULL verify does.
//
// While it has not finished, the run's record ("run:<run_id>", decision 0022)
// says that it adds chunk hashes and at which chunk size, so that `verify
// --resume` goes on adding them, and which process runs it (decision 0024).
// It is one of the operations that create an archive's next revision, with a
// registration and a relocation: one of them at a time is unfinished, and
// each refuses to begin while another is, naming how to finish or abandon it.
#include "core/engine/candidates.h"
#include "core/manifest/entry.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace va {

// The member of a run's record that says that the run adds chunk hashes: the
// chunk size at which it hashes them.
inline constexpr const char* adding_member = "add_chunk_hashes";

// Why chunk hashes cannot be added to `archive` now, or nothing when they can:
// a retired archive takes no new revisions (decision 0003), and an archive
// whose current revision has a chunk size, as its manifest says (R-14), has
// them. Reads the first line of that manifest alone (load_recorded_header);
// fails when it cannot. The rule by which a verification refuses to add them
// and the GUI offers to.
std::expected<std::optional<std::string>, IoError> why_chunk_hashes_cannot_be_added(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const ArchiveRecord& archive);

// An unfinished FULL verify that adds chunk hashes to an archive: its run's
// record.
struct AddingVerification {
    std::string run_id;
    Uuid archive_uuid;
    std::uint32_t revision = 0;     // the revision it verifies; it records the next one
    std::uint64_t chunk_size = 0;   // the chunk size at which it adds them
    std::string root;               // where it reads (UTF-8)
    Timestamp started;
    nlohmann::json record;
};

// The archive's unfinished verification that adds chunk hashes: the record of
// a run that adds them to the revision the archive is at now, as its record in
// `archives` says. One that adds them to an earlier revision can never finish
// (R-104), and is none. Inside a write transaction the answer holds until it
// ends.
std::expected<std::optional<AddingVerification>, IoError> unfinished_adding_verification(
    ArchiveStore& store, const Uuid& archive_uuid);

// The refusal of an operation that would create the archive's next revision
// while `verification` is unfinished (one operation creating a revision at a
// time), naming how to finish or abandon it.
std::string adding_verification_refusal(const AddingVerification& verification);

// The unfinished operation that keeps another from creating an archive's next
// revision, as the refusal found it (decisions 0025, 0026, 0033): what the
// windows need to name their own ways on in place of the command lines that
// the refusal names, and to judge whether it still runs, as the Archive List
// judges its row (app::refusal_in_window).
struct RefusingOperation {
    enum class Kind {
        Registration,        // a registration's plan, "registration:<archive_uuid>" (decision 0025)
        Relocation,          // a relocation's record, "relocation:<archive_uuid>" (decision 0026)
        AddingVerification,  // the record of a FULL verify that adds chunk hashes, "run:<run_id>" (decision 0033)
    };
    Kind kind = Kind::Registration;
    Uuid archive_uuid;
    std::string root;               // where it reads (UTF-8): a registration's root, a relocation's new root
    std::uint32_t revision = 0;     // the revision a registration creates; the one a relocation or a
                                    // verification verifies, whose next one it creates
    std::string run_id;             // a relocation's or a verification's
    Timestamp started;              // when its plan or record was made
    bool this_root = false;         // a registration of the very root of the refused registration (decision
                                    // 0025, point 1), not of another root of its archive
    bool differs = false;           // a relocation whose new carrier does not hold the recorded content
    std::optional<std::uint32_t> later_revision;  // made for an older revision than the archive has now
                                                  // (decision 0030): the archive's revision
    nlohmann::json record;          // its plan or record as the refusal read it
};

// The registration whose plan is `plan` (a stored plan, parsed), as the
// operation that refuses another.
RefusingOperation refusing_registration(const Uuid& archive_uuid, const nlohmann::json& plan);

// The relocation whose record is `record`, as the operation that refuses
// another; its record says whether its new carrier differs.
RefusingOperation refusing_relocation(const Uuid& archive_uuid, const nlohmann::json& record);

// The verification that adds chunk hashes `verification`, as the operation
// that refuses another.
RefusingOperation refusing_verification(const AddingVerification& verification);

// Why an operation that would create an archive's next revision does not
// begin while another one that creates it is unfinished: the refusal, which
// names the commands that finish or abandon the other one, and the other one,
// whose ways on the windows name in their own words (decision 0033).
struct NextRevisionRefusal {
    std::string message;
    RefusingOperation refusing;
};

// Why a FULL verify that adds chunk hashes to `archive` does not begin:
// another operation that creates its next revision is unfinished, a
// registration (decision 0025), a relocation (decision 0026) or another
// verification that adds chunk hashes, in the words of that operation's own
// refusals, which name how to finish or abandon it. Nothing when none is. A
// registration or relocation whose revision is recorded can never finish, and
// is none. Called in the transaction that begins the verification.
std::expected<std::optional<NextRevisionRefusal>, IoError> another_operation_creates_the_revision(
    ArchiveStore& store, const ArchiveRecord& archive);

// Removes the records and saved progress of the verifications that add chunk
// hashes to an earlier revision of `archive` than the one it is at: they can
// never finish (R-104). Their runs in the history stay.
std::expected<void, IoError> remove_adding_verifications_of_earlier_revisions(
    ArchiveStore& store, const ArchiveRecord& archive);

// The chunk size at which the run whose record is `record` adds chunk hashes;
// 0 when it adds none (a FULL verify that only verifies, or a record of a
// version before decision 0033), or when the value is no chunk size.
std::uint64_t adding_chunk_size(const nlohmann::json& record);

// The entries of r(N+1): rN's entries `recorded` as they are, in their order,
// each file that has chunk lines at `chunk_size` (has_chunk_lines) with the
// chunk hashes that `chunks` holds under its entry_key(). Fails, naming the
// file, when such a file has none there, or not as many as its size gives.
std::expected<std::vector<Entry>, IoError> entries_with_chunk_hashes(const std::vector<Entry>& recorded,
    const std::map<std::string, std::vector<std::string>>& chunks, std::uint64_t chunk_size);

// The header of r(N+1), before the writer fills in its chunk size, counts,
// Merkle root and body: rN's (`previous`: its label, copy_of, exclusions),
// with revision N+1 superseding N, made at `created` on `host`, from the
// carrier `identity` at the archive root `root_subpath` on it, found at
// `root_hint`: the carrier this revision was read from (decision 0004).
ManifestHeader header_with_chunk_hashes(const ManifestHeader& previous, const Timestamp& created,
    const HostInfo& host, const VolumeInfo& identity, const std::string& root_subpath, const std::string& root_hint);

} // namespace va
