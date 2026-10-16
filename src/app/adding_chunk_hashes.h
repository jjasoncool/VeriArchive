#pragma once
// Adding chunk hashes (decision 0033) as the GUI and the command line offer
// it: which stopped verification that adds chunk hashes Verify Archive's
// Resume continues.
#include "app/unfinished_operations.h"
#include "common/error.h"
#include "common/types.h"
#include "core/repository/archive_store.h"

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace va::app {

// A run that Verify Archive's Resume continues (R-104): its id, the root it
// was given, or none for an archive found by its uuid on the attached
// volumes, and its archive.
struct RunToResume {
    std::string run_id;
    std::optional<std::filesystem::path> root;
    Uuid archive_uuid;
};

// The stopped verification that adds chunk hashes to the archive
// `archive_uuid`, which Verify Archive's Resume continues once the archive is
// chosen in its list (decision 0033), also after the window that ran it was
// closed: the archive's unfinished one (unfinished_adding_verification), when
// the Archive List would show it stopped from one read (state_of_one_read),
// its attempt judged as `attempts` says (decision 0024). That is, recorded
// INCOMPLETE, its record saying that it stopped, the process its record names
// ended, or, where no process can be checked, its record silent for minutes.
// Nothing while it runs or may run, as a resume would then be refused or take
// over an attempt that works, nor when the archive has none. Found by the
// archive's uuid, without a root. Reads in one read transaction.
std::expected<std::optional<RunToResume>, IoError> resumable_adding_verification(
    ArchiveStore& store, const Uuid& archive_uuid, const AttemptCheck& attempts, std::chrono::sys_seconds wall_now);

// Which run Verify Archive's Resume continues: the run interrupted last in
// the window (`interrupted_here`), as before decision 0033, unless the
// archive chosen in its list has a stopped verification that adds chunk
// hashes (`chosen`, from resumable_adding_verification) and the run
// interrupted in the window is of another archive, or there is none; then
// the chosen archive's. So the window goes on with its own run while that
// run's archive is chosen, also a run that adds no chunk hashes, and with the
// chosen archive's verification that adds them otherwise.
std::optional<RunToResume> run_to_resume(
    const std::optional<RunToResume>& interrupted_here, const std::optional<RunToResume>& chosen);

} // namespace va::app
