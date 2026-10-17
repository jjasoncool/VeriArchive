#pragma once
// What the windows say when Register Archive or Verify Archive is refused
// because another operation that creates the archive's next revision is
// unfinished (decisions 0025, 0026, 0033): a registration, a relocation, or a
// FULL verify that adds chunk hashes. The command line names its commands;
// the windows name their own ways on in their place (decision 0033): first
// "Resume" and "Abandon..." on the operation's row in the Archive List
// (decision 0022, point 7), then Register Archive's "Resume an interrupted
// registration of this folder or volume" or Verify Archive's "Resume
// interrupted run"; a relocation, which has no window and no Resume in the
// list, keeps its command line for continuing, and a registration made for an
// older revision can only be abandoned. While the operation that refuses still
// runs or is held up, as the Archive List judges its row from one read, they
// say so, and to wait for it: the window offers no way on for it then. Where
// one read cannot tell, as for a record that says nothing of its state, they
// name the ways on.
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
// `refusing` runs (`runs`), that it still runs and to wait for it.
std::string refusal_in_window(const RefusingOperation& refusing, RefusedIn view, bool runs);

// What Register Archive says of a registration that does not begin: the
// failure's message, or, where another operation that creates the archive's
// next revision refuses it (RegisterFailure::refusing), refusal_in_window.
std::string register_message_in_window(const RegisterFailure& failure, bool runs);

// What Verify Archive says of a verification that does not begin: the stop's
// message, or, where another operation that creates the archive's next
// revision refuses it (VerifyStop::refusing), refusal_in_window.
std::string stop_message_in_window(const VerifyStop& stop, bool runs);

// Whether `refusing`, as the refusal found it, still runs, as the Archive List
// would show it from one read (state_of_one_read), its attempt judged as
// `attempts` says (decision 0024): running, as its record said a moment ago or
// as the process its record names runs, or held up, that process running
// while its record has said nothing for minutes. Then the Archive List offers
// no Abandon... for it, and a resume would be refused or take over an attempt
// that works. False once it has stopped (its record saying so, a
// verification recorded INCOMPLETE, the process its record names ended, or,
// where no process can be checked, its record silent for minutes), and where
// one read cannot tell: a record that says nothing of its state, as a plan
// that a version before decision 0022 wrote without "updated" or a process,
// which the Archive List shows waiting for progress. Reads whether a
// verification's run is recorded INCOMPLETE.
std::expected<bool, IoError> refusing_operation_runs(ArchiveStore& store, const RefusingOperation& refusing,
    const AttemptCheck& attempts, std::chrono::sys_seconds wall_now);

// What Register Archive says of a registration that does not begin: as
// register_message_in_window says it, the operation that refuses it, if one
// does, judged by the window as refusing_operation_runs judges it, from the
// repository `store`, through this computer's process table `processes` on
// this computer (`host`, HostInfo::host), at the time `now`. The window's own
// process runs no operation of its own then: the window's one operation is
// the registration refused, which wrote no plan or record, so a plan or
// record that names this process is of an attempt that has ended (decision
// 0024), as a registration that the window cancelled and whose stop mark it
// could not write. Where it cannot be judged, as when the repository cannot
// be read, the window names its ways on.
std::string register_message_in_window(const RegisterFailure& failure, ArchiveStore& store,
    IProcessTable& processes, const std::string& host, std::chrono::sys_seconds now);

// What Verify Archive says of a verification that does not begin: as
// stop_message_in_window says it, the operation that refuses it judged as
// Register Archive judges it (register_message_in_window). The window's one
// operation is then the verification refused, which wrote no record.
std::string stop_message_in_window(const VerifyStop& stop, ArchiveStore& store, IProcessTable& processes,
    const std::string& host, std::chrono::sys_seconds now);

} // namespace va::app
