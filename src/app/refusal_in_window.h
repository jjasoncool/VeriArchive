#pragma once
// What the window says when Register Archive, a verification, a relocation or
// the retirement of an archive started in Archives is refused because another
// operation that creates the archive's next revision is unfinished (decisions
// 0025, 0026, 0033, 0046): a registration, a relocation, or a FULL verify that
// adds chunk hashes. The command line names its commands; the window names
// its own ways on in their place (decisions 0033, 0046): first "Resume" and
// "Abandon..." on the operation's row under Unfinished operations in Jobs
// (decision 0022, point 7), then Register Archive, the registration's root
// chosen there, whose options then resume it ("Resume the interrupted
// registration"), or "Resume" in the archive's details, where its stopped
// verification or relocation shows; a registration or relocation made for an
// older revision, and a relocation whose new carrier differs, can only be
// abandoned. While the operation that refuses still runs or is held up, as
// Archives judges its row from one read, they say so, and to wait for it: the
// window offers no way on for it then. Where one read cannot tell, as for a
// record that says nothing of its state, they name the ways on.
#include "app/text.h"
#include "app/unfinished_operations.h"
#include "common/error.h"
#include "core/engine/add_chunk_hashes.h"
#include "core/engine/register_engine.h"
#include "core/engine/relocate_engine.h"
#include "core/engine/retire_archive.h"
#include "core/engine/verify_engine.h"
#include "core/repository/archive_store.h"

#include <chrono>
#include <expected>
#include <string>

namespace va::app {

// Where a refusal is shown: Register Archive, or Archives, where an archive's
// verifications start.
enum class RefusedIn { RegisterArchive, Archives };

// What `view` says, an English sentence translated where it is shown, of an
// operation refused because `refusing` is unfinished: the command line's
// refusal with the window's own ways on in place of its commands, and, while
// `refusing` runs (`runs`), that it still runs and to wait for it.
std::string refusal_in_window(const RefusingOperation& refusing, RefusedIn view, bool runs);

// What Register Archive says of a registration that does not begin: the
// failure's message, or, where another operation that creates the archive's
// next revision refuses it (RegisterFailure::refusing), refusal_in_window.
std::string register_message_in_window(const RegisterFailure& failure, bool runs);

// What Archives says of a verification that does not begin: the stop's
// message, or, where another operation that creates the archive's next
// revision refuses it (VerifyStop::refusing), refusal_in_window.
std::string stop_message_in_window(const VerifyStop& stop, bool runs);

// Whether `refusing`, as the refusal found it, still runs, as Archives
// would show it from one read (state_of_one_read), its attempt judged as
// `attempts` says (decision 0024): running, as its record said a moment ago or
// as the process its record names runs, or held up, that process running
// while its record has said nothing for minutes. Then Archives offers
// no Abandon... for it, and a resume would be refused or take over an attempt
// that works. False once it has stopped (its record saying so, a
// verification recorded INCOMPLETE, the process its record names ended, or,
// where no process can be checked, its record silent for minutes), and where
// one read cannot tell: a record that says nothing of its state, as a plan
// that a version before decision 0022 wrote without "updated" or a process,
// which Archives shows waiting for progress. Reads whether a
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

// What Archives says of a verification that does not begin: as
// stop_message_in_window says it, the operation that refuses it judged as
// Register Archive judges it (register_message_in_window). The window's one
// operation is then the verification refused, which wrote no record.
std::string stop_message_in_window(const VerifyStop& stop, ArchiveStore& store, IProcessTable& processes,
    const std::string& host, std::chrono::sys_seconds now);

// Whether the window says that `refusing` still runs: as
// refusing_operation_runs judges it, through `processes` on this computer
// (`host`) at `now`, with this process taken for one that runs no operation
// (the window's operation is the one refused); not where it cannot be judged,
// where the window names the ways on.
bool runs_as_the_window_judges(ArchiveStore& store, const RefusingOperation& refusing, IProcessTable& processes,
    const std::string& host, std::chrono::sys_seconds now);

// What the window says of a relocation that did not record its revision, in
// `text`'s language: where another operation, or the archive's own unfinished
// relocation, refuses it (RelocateFailure::refusing), refusal_in_window,
// `runs` as runs_as_the_window_judges says; where the relocation keeps its
// saved progress (RelocateFailure::keeps_progress: interrupted, its device no
// longer answering, or stopped by a failure, `cause`), that Resume under
// Unfinished operations continues it, in place of the command line's
// `archive relocate ... --resume`; a resume at a root where the new carrier is
// not attached now (RelocateFailure::elsewhere), that the relocation dialog
// resumes it at the root the operator gives there; one that another operation
// overtook before it began (RelocateFailure::overtaken), to relocate again;
// otherwise the failure's message.
std::string relocation_message_in_window(const Text& text, const RelocateFailure& failure, bool runs);

// What the window says of a retirement refused, in `text`'s language: where an
// operation that would give the archive a new revision is unfinished
// (RetireFailure::refusing), refusal_in_window, `runs` as above; otherwise the
// failure's message.
std::string retirement_message_in_window(const Text& text, const RetireFailure& failure, bool runs);

} // namespace va::app
