#pragma once
// What Archives says of each archive (decision 0046): a row of its list and
// the details of the archive selected. Never a bare status (R-21): a row
// gives the result of the last FULL verification with how long ago it was,
// and the details give that result with its mode, its local time and age,
// and the last verification of any mode where it is another; OVERDUE (R-105)
// with the limit that fired, the device alerts of the last verification
// (decision 0044), a retired archive, and what is wrong with a manifest that
// cannot be used (decision 0028). The media identity is the one the views
// show (displayed_identity): the sample one in the screenshot mode.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/text.h"
#include "app/unfinished_operations.h"
#include "common/config.h"
#include "common/error.h"
#include "core/engine/archive_status.h"
#include "core/repository/archive_store.h"
#include "gui/displayed_identity.h"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace va::gui {

// How a result, an archive or a mark is shown: in the theme's colour for
// success, warnings or errors, or in none.
enum class Tone { Neutral, Success, Warning, Error };

// The style class that gives `tone` its colour in the window's style
// (window.cpp): va-neutral, va-success, va-warning, va-error.
const char* tone_class(Tone tone);

// A few words in a rounded box: a result, or a mark beside it.
struct Pill {
    std::string words;
    Tone tone = Tone::Neutral;
    bool operator==(const Pill&) const = default;
};

// How long before `now` the UTC timestamp `then` was: "less than an hour
// ago", "5 hours ago", "1 day ago", "3 days ago" (up to 13), "2 weeks ago"
// (up to 8), "5 months ago" (up to 11), "1 year ago", "1 year 2 months ago"
// (up to 11 months);
// a moment after `now` (a clock set back) is "less than an hour ago". Empty
// where `then` is no timestamp.
std::string age_words(const app::Text& text, std::string_view then, std::chrono::sys_seconds now);

// How long a run took, from its start to its end: "8 s", "5 min 37 s",
// "12 h 44 min"; empty where either is no timestamp.
std::string duration_words(const app::Text& text, std::string_view started, std::string_view finished);

// A run's result with its mode, as its report's first line says it: "FULL
// VERIFY PASSED" (完整驗證 通過) for a FULL verification, "QUICK PASSED" for a
// mode that V1 does not run.
std::string result_with_mode(const app::Text& text, const VerifyRunRecord& run);

// PASSED Success, PASSED_WITH_WARNINGS Warning, INCOMPLETE Neutral (it claims
// nothing), FAILED and anything else Error.
Tone result_tone(std::string_view result);

// A run's result alone, as a pill in the tone of the result: PASSED (通過),
// PASSED_WITH_WARNINGS (通過（有警告）), FAILED (失敗), INCOMPLETE (未完成).
Pill result_pill(const app::Text& text, const VerifyRunRecord& run);

// An archive's row in the list of Archives.
struct ArchiveRow {
    Uuid archive_uuid;
    std::string label;          // display_label: with the start of its uuid where the label is shared (R-88)
    std::string volume_label;   // of its current revision's carrier; empty without a manifest to read it from
    // The device model and the capacity of that carrier, and "Last FULL
    // verify 3 days ago" once there is one, joined by " · ".
    std::string subtitle;
    // The status dot: Error for a last verification that FAILED, a device
    // alert, or a manifest that cannot be used; Warning for a last FULL
    // verification that PASSED_WITH_WARNINGS, or OVERDUE; Neutral for an
    // archive never verified; Success otherwise.
    Tone tone = Tone::Neutral;
    // The result of the last FULL verification; of the last verification of
    // another mode, with its mode, where there is no FULL one; or "Never
    // verified".
    Pill result;
    // Beside the result, each that holds: OVERDUE (Warning), the device
    // alerts of the last verification (app::alert_marks, Error), and the
    // archive's status where it is not REGISTERED, as `archive list` shows
    // it: RETIRED, REGISTRATION_INCOMPLETE (its first registration has not
    // finished).
    std::vector<Pill> marks;
    // What is wrong with the latest manifest, and how to fix it (decision
    // 0028): "Problem: ...", or "Manifest lost: ..." for a retired archive
    // whose latest manifest is lost (point 9), which needs nothing of the
    // operator any more and so is not shown as an error.
    std::string problem;
    bool problem_alerts = false;
};

ArchiveRow archive_row(const app::Text& text, const ArchiveStatus& status, IdentityShown shown,
    std::chrono::sys_seconds now);

// The banner at the top of an archive's details.
struct ResultBanner {
    Tone tone = Tone::Neutral;  // of the result in the heading
    // "FULL VERIFY PASSED": the result of the last FULL verification with
    // its mode; of the last verification of another mode where there is no
    // FULL one; or "Never verified".
    std::string heading;
    bool overdue = false;       // an OVERDUE pill beside the heading
    // Under it: when the last FULL verification ended, local time with its
    // offset and how long ago (decision 0017), or that there is none yet; the
    // last verification of another mode where it is a later one; each
    // overdue limit that fired; and that the archive is retired.
    std::vector<std::string> lines;
};

ResultBanner result_banner(const app::Text& text, const ArchiveStatus& status, const Policy& policy,
    std::chrono::sys_seconds now);

// The device alerts of the last verification (decision 0044), as its report
// gave them, one line each: the health that each device reported as the last
// run started (an INCOMPLETE one included), and a read of the last FULL
// verification markedly slower than the one it was compared with; the
// devices named as the views name them (displayed_devices). The same two
// sources as app::archive_alerts, which marks the archive's row. The slower
// read names its run (`slower_run`), whose result page charts where it read
// slower; a health alert names none.
struct AlertLine {
    std::string words;
    std::string slower_run;
    bool operator==(const AlertLine&) const = default;
};

std::vector<AlertLine> device_alert_lines(const app::Text& text, const ArchiveStatus& status, IdentityShown shown);

// A name and its value, a line of a group of the details. A value that is a
// path (a root) wraps at any character, filling its lines; any other between
// words where it can.
struct Field {
    std::string name;
    std::string value;
    bool path = false;
    bool operator==(const Field&) const = default;
};

// The group Device: the volume label and capacity of the current revision's
// carrier, and of each of its devices the model, bus and serial (or WWN), as
// displayed_identity shows them; the health that each device reported in the
// last run, and the read speed of the last FULL verification (with what it
// was compared with), where the run recorded them. None without a manifest
// to read the carrier from.
std::vector<Field> device_fields(const app::Text& text, const ArchiveStatus& status, IdentityShown shown);

// The group Content: the archive's root as its current revision recorded it
// (root_hint, "not known" without one), its files and their size, its
// revision with when it was recorded, whether that revision has chunk hashes,
// and the archive's UUID.
std::vector<Field> content_fields(const app::Text& text, const ArchiveStatus& status);

// An item of a menu of the details: whether it can be chosen now, and the
// line under its name, which says what it does or why it cannot be chosen.
struct Choice {
    bool offered = false;
    std::string note;
};

// Whether the menu of FULL verify lists "FULL verify, and add chunk hashes"
// (decision 0033) at all: not for an archive whose current revision has
// chunk hashes already, as almost every archive that V1.x registered has;
// for any other, offered or not as adding_choice says.
bool lists_adding_chunk_hashes(const ArchiveStatus& status);

// "FULL verify, and add chunk hashes" (decision 0033), in the menu of FULL
// verify: offered only where why_chunk_hashes_cannot_be_added gives no
// reason (`why_not`), and no verification of the archive that adds them is
// unfinished (`adding_unfinished`), which Resume or Abandon... finishes
// first; otherwise not, saying why.
Choice adding_choice(const app::Text& text, const std::expected<std::optional<std::string>, IoError>& why_not,
    bool adding_unfinished);

// "Create a new revision...", in the menu of the details: offered where the
// root of the archive is known (root_hint), which Register Archive is given.
Choice new_revision_choice(const app::Text& text, const ArchiveStatus& status);

// "Compare with another archive...": offered where the repository has
// another archive to compare with.
Choice compare_choice(const app::Text& text, std::size_t archives);

// A line of the group Copies (decision 0005, display only): an archive whose
// current revision records it as a copy of the archive shown, or the archive
// that the archive shown is recorded as a copy of.
struct CopyLink {
    Uuid other;
    std::string label;          // its display label; the start of its uuid where this repository has no such archive
    std::string volume_label;
    std::string how;            // "This archive is recorded as a copy of it", "Recorded as a copy of this archive"
    bool listed = false;        // in this repository, so the two can be compared
    Uuid reference;             // A of their comparison: the original
    Uuid copy;                  // B: the copy
};

// The copies of the archive `status` among `all`, and the archive it is a
// copy of first, as their current revisions record them.
std::vector<CopyLink> copy_links(const app::Text& text, const ArchiveStatus& status,
    const std::vector<ArchiveStatus>& all, IdentityShown shown);

// A run in the group Recent verifications of the details.
struct RunRow {
    std::string run_id;
    Pill result;
    std::string title;          // its local time with the offset, after its mode where it is not FULL
    std::string meta;           // the bytes read and matched, and how long it took
    // What the history notes of it (app::history_note), and its device
    // alerts (app::alert_marks, Error).
    std::vector<Pill> marks;
};

// The last `limit` of `runs` (an archive's history, oldest first, as
// ArchiveStore::verify_runs gives it), newest first.
std::vector<RunRow> recent_runs(const app::Text& text, const std::vector<VerifyRunRecord>& runs, std::size_t limit);

// What the details say of a stopped verification of the archive that Resume
// continues (`row`, from the latest read of the unfinished operations): how
// far it came, and that a resume does not read again what it read.
std::string stopped_verification_words(const app::Text& text, const app::OperationRow& row, bool adds_chunk_hashes);

// What the details say of a stopped relocation of the archive that Resume
// continues (decision 0046): where it reads, how far it came, and that a
// resume does not read again what matched.
std::string stopped_relocation_words(const app::Text& text, const app::OperationRow& row);

// What the details show of the unfinished verifications and relocations of
// `archive`, from the latest read of the unfinished operations (`watches`).
struct DetailsOperations {
    // The first that has stopped and that Resume continues, as its row under
    // Unfinished operations offers Resume (OperationRow::offers_resume), with
    // the words of the details' notice (stopped_verification_words,
    // stopped_relocation_words).
    std::optional<app::OperationRow> stopped;
    std::string words;
    // Whether a verification of the archive that adds chunk hashes is
    // unfinished, stopped or not: no second one is offered (decision 0033).
    bool adding_unfinished = false;
};

DetailsOperations details_operations(const app::Text& text, const std::vector<app::OperationWatch>& watches,
    const Uuid& archive, app::OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now);

// A FULL verification that the window ran and that was interrupted (R-104):
// its run, the root it was given (none for an archive found by its uuid), its
// archive, and whether it failed metadata differences, as its resume does.
struct InterruptedRun {
    std::string run_id;
    std::optional<std::filesystem::path> root;
    Uuid archive_uuid;
    bool strict_metadata = false;
};

// What Resume in the details of an archive runs for its stopped verification
// `stopped` (as the latest read lists it): the run that this window
// interrupted last (`here`), where it is that one, as the window ran it, at
// the root it was given or by its archive's uuid, failing metadata
// differences where it did or the policy says so; otherwise exactly what
// Resume on its row under Unfinished operations runs
// (app::verification_resume). A resumed run adds chunk hashes as the run it
// continues did.
VerifyRequest details_resume(
    const app::UnfinishedOperation& stopped, const std::optional<InterruptedRun>& here, const Config& config);

// Why the archive `label` is not verified again now: a job of the window
// waits or works on it already (decision 0045), and a second one would read
// its whole device once more. FULL verify of its details says it while its
// card shows.
std::string archive_has_a_job(const app::Text& text, const std::string& label);

// The toast once a verification ended: the archive and the result with its
// mode, "ARCH-0001: FULL VERIFY PASSED"; or, where it reached no result,
// "Not verified: <name>", whose reason its report gives.
std::string verified_toast(const app::Text& text, const std::string& label, const std::string& result);
std::string not_verified_toast(const app::Text& text, const std::string& name);

} // namespace va::gui
