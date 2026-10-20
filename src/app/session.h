#pragma once
// Composition-root helper shared by the CLI and the GUI (R-05): resolves and
// opens the repository (HANDOFF §B7.4) and creates the platform services.
#include "app/app_context.h"
#include "app/text.h"
#include "core/engine/candidates.h"
#include "core/interfaces.h"
#include "core/repository/repository.h"

#include <functional>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace va::app {

struct SessionOptions {
    bool initialise = false;                          // repo init
    std::optional<std::filesystem::path> repository;  // --repository
    std::optional<std::filesystem::path> config;      // --config
    // Whether this process runs elevated (R-12): the platform's answer when
    // empty, as in the user interfaces; tests give their own.
    std::function<bool()> elevated;
    // repo rebuild-index: a database that is missing, empty, not a database
    // or damaged is put aside and made again (decision 0036).
    bool rebuild = false;
    // The moment the repository is opened at, whose local date names the
    // copy of a database kept before its upgrade (decision 0032): the system
    // clock's when not given. Tests give one.
    std::optional<std::chrono::sys_seconds> now;
};

struct Session {
    AppContext context;
    std::unique_ptr<IFileWriter> writer;
    std::vector<std::string> warnings;  // running elevated, repository uuid changed, ...
    // What opening the repository did that people are told once, where it is
    // opened: the upgrade of its database and where the copy from before it
    // is (schema_upgrade_note). English sentences, translated where shown.
    std::vector<std::string> notes;
    // The database that opening for repo rebuild-index put aside and made
    // again (decision 0036); nothing otherwise.
    std::optional<ReplacedDatabase> replaced_database;
};

// Why no session was opened, with the warnings that hold all the same:
// running elevated (R-12), which may be why, as the home folder and the config
// file then resolve otherwise (decision 0037); and what opening the repository
// did all the same that people are told once, as Session::notes: the upgrade
// of its database, which stays upgraded when the open fails after it, and
// where the copy from before it is (decision 0032). English sentences,
// translated where shown, the notes before the message.
struct SessionFailure {
    std::string message;
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
    // The RepositoryRoot that the open resolved before it failed, where it got
    // that far: Settings and `repo info` name the copies of its database kept
    // before upgrades there also when it cannot be opened (decision 0032), as
    // where the open cannot write the config file as it records the pinned
    // repository's uuid, or refuses a database that cannot be used or that a
    // later version upgraded: there a copy is the way back.
    // Nothing where the open resolved none, as for a repository that is not
    // configured or is unavailable.
    std::optional<RepositoryLayout> repository;
};

// Opens the repository, upgrading its database where it is of an older
// schema version, with the copy from before the upgrade named for the local
// date at options.now (decision 0032).
std::expected<Session, SessionFailure> open_session(const SessionOptions& options);

// R-12: the warning that this process runs elevated, where it does; for a
// command that opens no repository (`media list`).
std::optional<std::string> elevated_warning();

// The schema version of the database of the repository that open_session
// would open, when that open would upgrade it, keeping a copy of it first
// (decision 0032): known at once, without the write lock, also while another
// process upgrades the database, which that open then waits for. Nothing for
// a repository that it opens as it is, and one it cannot open, which it
// reports itself. The GUI shows its window with a notice before such an
// open, which takes a while for a large repository.
std::optional<int> schema_upgrade_ahead(const SessionOptions& options);

// What people are told once of `upgrade` (decision 0032): that the
// repository database was upgraded, which earlier versions of VeriArchive
// cannot open, and where its copy from before the upgrade is kept. An English
// sentence, translated where it is shown.
std::string schema_upgrade_note(const SchemaUpgrade& upgrade);

// Where each copy of the repository database kept before an upgrade is
// (decision 0032), as Settings and `repo info` show it for as long as the
// copy is in RepositoryRoot (copies_before_upgrade), and not only once, at
// the open that made it, with what the copy is for: "Copy of the database
// from before an upgrade: P (an earlier version of VeriArchive can open only
// this copy; you can delete it once you no longer need that version)", one
// line per copy, in `text`'s language. Nothing where there is none.
std::vector<std::string> copy_before_upgrade_lines(const Text& text, const RepositoryLayout& layout);

// Why no repository is open, as a window says it (decision 0032): Settings
// after "No repository is open.", and the window in place of its notice of an
// upgrade. The failure's message, then each warning that holds all the same
// ("Warning: ...", as running elevated, R-12), then each note of what the open
// did all the same ("Note: ..."), then, where the open resolved a RepositoryRoot
// (SessionFailure::repository), where each copy of its database kept before
// an upgrade is (copy_before_upgrade_lines), read from the files each time it
// is worded, as Settings names them while a repository is open. In `text`'s
// language.
std::string session_failure_in_window(const Text& text, const SessionFailure& failure);

// Operating system, host name and version for manifests and history.
HostInfo host_info(std::string app_version);

// The interface language (decision 0017): `explicit_code` (--lang), then the
// VERIARCHIVE_LANG environment variable, then the config file's "language",
// then the operating system's language, then English.
Language interface_language(std::optional<std::string> explicit_code,
    std::optional<std::filesystem::path> explicit_config);

// Remembers the language in the config file that interface_language() reads.
std::expected<void, std::string> save_language(Language language,
    std::optional<std::filesystem::path> explicit_config);

} // namespace va::app
