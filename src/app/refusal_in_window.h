#pragma once
// What the windows say when Register Archive or Verify Archive is refused
// because another operation that creates the archive's next revision is
// unfinished (decisions 0025, 0026, 0033): a registration, a relocation, or a
// FULL verify that adds chunk hashes. The command line names its commands;
// the windows name their own ways on in their place (decision 0033): Register
// Archive's "Resume an interrupted registration of this folder or volume",
// Verify Archive's "Resume interrupted run", and "Abandon..." in the Archive
// List; a relocation, which has no window, keeps its command line for
// continuing. While the operation that refuses still runs or is held up, as
// the Archive List judges its row from one read, they say so, and to wait for
// it: the window offers neither way on for it then.
#include "app/unfinished_operations.h"
#include "common/error.h"
#include "core/engine/add_chunk_hashes.h"
#include "core/engine/register_engine.h"
#include "core/engine/verify_engine.h"
#include "core/repository/archive_store.h"

#include <chrono>
#include <expected>
#include <string>

namespace va::app {

// The view that shows a refusal.
enum class RefusedIn { RegisterArchive, VerifyArchive };

// What `view` says, an English sentence translated where it is shown, of an
// operation refused because `refusing` is unfinished: the command line's
// refusal with the window's own ways on in place of its commands, and, while
// `refusing` has not stopped (`stopped` false), that it still runs and to
// wait for it.
std::string refusal_in_window(const RefusingOperation& refusing, RefusedIn view, bool stopped);

// What Register Archive says of a registration that does not begin: the
// failure's message, or, where another operation that creates the archive's
// next revision refuses it (RegisterFailure::refusing), refusal_in_window.
std::string register_message_in_window(const RegisterFailure& failure, bool stopped);

// What Verify Archive says of a verification that does not begin: the stop's
// message, or, where another operation that creates the archive's next
// revision refuses it (VerifyStop::refusing), refusal_in_window.
std::string stop_message_in_window(const VerifyStop& stop, bool stopped);

// Whether `refusing`, as the refusal found it, has stopped, as the Archive
// List would show it from one read (state_of_one_read), its attempt judged as
// `attempts` says (decision 0024): its record saying that it stopped, a
// verification recorded INCOMPLETE, the process its record names ended, or,
// where no process can be checked, its record silent for minutes. False while
// it runs or is held up, when the Archive List offers no Abandon... for it and
// a resume would be refused or take over an attempt that works. Reads whether
// a verification's run is recorded INCOMPLETE.
std::expected<bool, IoError> refusing_operation_stopped(ArchiveStore& store, const RefusingOperation& refusing,
    const AttemptCheck& attempts, std::chrono::sys_seconds wall_now);

} // namespace va::app
